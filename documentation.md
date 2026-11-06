# PVE H.264 Console — Project Documentation

Low-latency, GPU-encoded (H.264, variable bitrate) console for Proxmox VE VMs,
added **next to** noVNC. Goal: fluid 1080p60 with the lowest possible latency,
opened from the normal PVE web UI with normal PVE permissions.

This document is the engineering reference for the project: design decisions,
every Proxmox file it touches, the wire protocol, the security model, test
results, troubleshooting and the change log. For a quick overview and setup,
start with the [README](README.md).

> Most of this project's code and of this document were written by Claude
> (Anthropic's AI model) under the direction of David Grishko, who set the goals,
> made the design decisions and did the real-world testing. See
> [How this was built](README.md#-how-this-was-built).

**Contents**

1. [Design goals](#1-design-goals) · 2. [Development environment](#2-development-environment) ·
3. [Architecture](#3-architecture) · 4. [Key technical findings](#4-key-technical-findings-and-fixes) ·
5. [Files](#5-files) · 6. [Install / update / uninstall](#6-install--update--uninstall) ·
7. [Using it](#7-using-it) · 8. [Configuration](#8-configuration-etcpve-h264h264dconf) ·
9. [Wire protocol and security](#9-wire-protocol-daemon--viewer-inside-the-websocket-byte-stream) ·
10. [Performance and test results](#10-performance-and-test-results) ·
11. [Known limitations](#11-known-limitations--todo) · 12. [Troubleshooting](#12-troubleshooting) ·
13. [Change log](#13-change-log)

---

## 1. Design goals

| Topic | Decision |
|---|---|
| Goal | Replace VNC with a smooth H.264 console, 1080p60, VBR, lowest latency |
| Encoder hardware | Pluggable: NVIDIA NVENC, Intel/AMD iGPU (VA-API), CPU x264 fallback |
| Network | LAN / VPN only → WebSocket through pveproxy + browser WebCodecs (no WebRTC/UDP) |
| UI | Added **beside** noVNC (noVNC stays as fallback); new "H.264" console option |
| Initial test guest | A disk-less VM (SeaBIOS screen), followed by real Windows guests |

## 2. Development environment

* Proxmox VE 9.1.1, kernel 6.17, QEMU `pve-qemu-kvm 10.1.2-3`, noVNC `novnc-pve 1.6.0-3`
  (later fully upgraded to Proxmox VE 9.2.25 / QEMU 11.1.3 / kernel 7.0, see change log)
* QEMU display backends available: `none`, `egl-headless`, `spice-app`, **`dbus`**
* A mid-range NVIDIA GPU with NVENC (proprietary DKMS driver); no Intel/AMD iGPU
* FFmpeg 7.1.5 (Debian) with `h264_nvenc`, `h264_vaapi`, `libx264`

## 3. Architecture

```
Browser  ── viewer page /?console=kvm&h264=1&vmid=V&node=N  (served by pveproxy)
   │  1. POST /api2/json/nodes/N/qemu/V/h264proxy        → {ticket, token[, relay]}  (pvedaemon, root)
   │  2. wss  /api2/json/nodes/N/qemu/V/h264websocket?vncticket=…[&relay=…]        (pveproxy)
   │  3. first message: one-time token
   ▼
pveproxy (TLS, PVE auth + VNC-ticket check, TCP_NODELAY) ── unix socket /run/pve-h264/V.sock
   ▼
pve-h264d@V.service   (one daemon per VM, root, started on demand, exits 30 s after last viewer)
   ├─ QMP getfd + add_client @dbus-display  → private p2p D-Bus connection to QEMU
   ├─ org.qemu.Display1 Listener: ScanoutMap (memfd, zero-copy) + UpdateMap damage,
   │  Scanout/Update copy fallback, CursorDefine/MouseSet; Keyboard/Mouse input
   ├─ encoder thread: damage-driven, unchanged-pixel suppression, ACK-paced,
   │  adaptive VBR ceiling, NVENC(CUDA dirty-rect) | NVENC(sysmem) | VA-API | x264
   ├─ SPS rewrite (VUI bitstream_restriction, max_num_reorder_frames=0)
   ├─ audio thread: Audio.RegisterOutListener → PCM → 48 kHz stereo → Opus 10 ms
   │  (or raw PCM), stamped with the same host clock as the video frames
   └─ fan-out of the single encoded stream to all authenticated viewers
QEMU  (-display dbus,p2p=yes[,audiodev=X] + -audiodev dbus,id=X added by the
       patched qemu-server, alongside the normal -vnc)
```

### VMs on another cluster node

Both API calls run on the node the browser is connected to (A), because A's
pveproxy serves the websocket and pveproxy can't forward a websocket upgrade to
another node. For a VM on node B, `h264proxy` does what stock noVNC's `vncproxy`
does for a remote VM and goes over the cluster's SSH:

```
Browser ── wss (A's pveproxy) ── /run/pve-h264/relay/<id>.sock ── h264relay task on A
             ssh root@B (cluster keys) ── socat ── /run/pve-h264/V.sock on B ── pve-h264d@V on B
```

1. On A: check that V's config is under `/etc/pve/nodes/B/`, then run
   `ssh root@B pve-h264-ctl prepare V`. On B the helper checks the VM runs with the
   D-Bus display, writes a one-time token, starts `pve-h264d@V` and prints the token.
   Only the vmid goes into the remote command line.
2. On A: create a one-shot unix socket `/run/pve-h264/relay/<id>.sock` (random id,
   `root:www-data 0660`) and a task worker (`h264relay`, shown in the task log) that
   accepts one connection within 10 s, unlinks the socket and pipes it to
   `ssh root@B socat - UNIX-CONNECT:/run/pve-h264/V.sock`. socat is a qemu-server
   dependency, so every node has it. The task ends with the stream.
3. The returned VNC ticket is bound to the relay id. `h264websocket` with `relay=<id>`
   relays to that socket, and B's daemon checks the token as usual.

On the same host the relay added about 0.5–1 ms of round trip (SSH encryption, socat),
plus about 0.2 s once per connection for the SSH handshake. Both ssh ends set
`TCP_NODELAY`. For a local VM nothing changes: no SSH, no relay, and the same ticket
and socket as before. Every node needs pve-h264-console installed, at the same
version. If B has none (or an old one), A reports that instead of failing obscurely.

### Why these choices
* **D-Bus display instead of patching QEMU**: available in stock pve-qemu,
  coexists with VNC, gives a shared-memory (memfd) framebuffer + damage
  rectangles + input methods. Survives pve-qemu updates.
* **One daemon per VM**: QEMU's p2p D-Bus mode accepts only *one* client
  connection at a time (`dbus_display_add_client()` replaces the object
  manager connection). So one process owns it, encodes once and fans out.
* **WebSocket + WebCodecs through pveproxy**: reuses PVE TLS, cookies, CSRF
  and VNC tickets; no extra ports; WebCodecs `optimizeForLatency` with no
  jitter buffer; frames are drawn to a `desynchronized` canvas the moment the
  decoder outputs them (no requestAnimationFrame, no media element buffering).
* **Application-level ACKs**: pveproxy's websocket relay has no backpressure,
  so TCP-level flow control can't be used. The viewer ACKs every decoded
  frame; the daemon keeps at most `max_inflight` (default 2) frames
  undecoded per viewer and otherwise *skips* (frames are "latest state", the
  next encode simply contains everything) → latency can never build up.

### Latency at a glance

```
 noVNC:   guest ──► VNC rectangles (CPU, tiled, lossless-ish) ──► browser canvas
 this:    guest ──► memfd framebuffer ──► NVENC / VA-API H.264 ──► WebCodecs (GPU decode)
```

| Problem | What this project does |
|---|---|
| NVENC's SPS doesn't say "no reordering", so browsers buffer several frames | Rewrites every SPS with VUI `max_num_reorder_frames = 0` |
| Uploading the full 8 MB 1080p frame to the GPU every time | Pinned shadow buffer; only the damaged rectangle goes to a persistent CUDA surface |
| QEMU reports damage even when pixels didn't change | Row-compares against the shadow and skips the encode |
| pveproxy's websocket relay has no backpressure and Nagle enabled | Per-frame ACK flow control (≤ 2 frames in flight by default); patches in `TCP_NODELAY` |
| QEMU's p2p D-Bus display allows only one client | One daemon per VM owns the connection and fans out the stream |

## 4. Key technical findings (and fixes)

1. **QEMU refresh timer**: the D-Bus listener never sets `update_interval`, so
   QEMU polls timer-driven devices (std VGA, cirrus, vmware, qxl-vga-mode) every
   30 ms → **~33 fps max with `vga: std`**. `virtio-gpu` pushes updates on the
   guest's `RESOURCE_FLUSH`, so it's not capped → **use `vga: virtio` for 60 fps**.
   (Lifting the cap for std VGA needs a 1-line QEMU patch, see §11.)
2. **`ScanoutMap` is synchronous** from QEMU's main loop → the daemon answers
   D-Bus on its main thread and never blocks it (encoding is in its own thread).
3. **No-reorder signalling (big latency fix)**: NVENC writes an SPS without VUI
   `bitstream_restriction`. Decoders (Chromium software *and* hardware
   H264Decoder) then assume reordering and hold frames in the DPB → several
   frames of extra latency, and on a static screen the last frames never
   appear. `daemon/sps.c` rewrites every SPS with `bitstream_restriction_flag=1,
   max_num_reorder_frames=0, max_dec_frame_buffering=num_ref_frames`.
   Verified with `ffmpeg -bsf:v trace_headers`.
4. **pveproxy does not set TCP_NODELAY** on websocket relays → Nagle could add
   tens of ms to small frames/input. Patched (`no_delay` on both legs; also
   benefits noVNC).
5. **Full-frame upload cost**: NVENC from system memory copies + DMAs the whole
   8 MB 1080p BGRX frame each time (~5.1 ms total per frame). New *nvenc-cuda*
   path keeps a persistent CUDA surface and uploads only the damaged rectangle
   from a page-locked shadow (libcuda via `dlopen`, no CUDA toolkit needed):
   ~1.0 ms upload + 2.6 ms encode in the half-screen-motion test; tiny changes
   cost almost nothing.
6. **Spurious damage**: QEMU VGA text mode reports damage every refresh even
   when nothing changed. The daemon compares damaged rows with its shadow and
   skips encoding unchanged frames.
7. **ABR must not punish slow decoders**: congestion (bitrate reduction) is
   only assumed when ACK delay *minus the viewer's reported decode time* rises
   ≥12 ms above its baseline while pacing is active. A slow decoder just gets a
   lower frame rate via ACK pacing.
8. **Windows ignores `SetUIInfo`**: the virtio-win GPU driver (viogpudo) does
   not follow host resize hints, and the SPICE server turns agent monitor
   configs into that same hint instead of forwarding them to the agent. The
   patch therefore puts the spice agent port (`com.redhat.spice.0`) on a
   `-chardev dbus` and the daemon speaks the vdagent protocol itself
   (`daemon/vdagent.c`): after the `SetUIInfo` hint it sends
   `VD_AGENT_MONITORS_CONFIG` to the guest's `spice-agent` (virtio-win guest
   tools), which switches the mode. `Chardev.Register` must be called
   asynchronously: QEMU makes a blocking `ScanoutMap` call into the daemon
   right after `RegisterListener`.
   **Update:** the spice agent turned out to be unable to switch modes on
   viogpudo at all (its WDDM path only uses the QXL escape; it reports
   success without changing anything). What does work is what viogpudo's
   own helper `viogpuap.exe` does after the driver's resolution event:
   `QueryDisplayConfig` + `SetDisplayConfig` on the source mode, in the
   user's session. `daemon/guestexec.c` does that without installing
   anything. A small PowerShell **listener** runs in the logged-in user's
   session: it defines the user32 calls in memory (Reflection.Emit, no
   compiler run), watches `C:\ProgramData\pve-h264\resize.req` and answers
   in a new `resize.ack.<seq>` per request (`resize.alive` exists while it
   runs). The Windows agent opens files without write sharing, so a file the
   daemon is polling can't be rewritten by the listener: each answer gets
   its own file, written once. Per resize the daemon only writes the request with the
   agent's `guest-file-*` commands (no process started in the guest) and
   polls the ack: ~0.1–0.3 s. The listener retries for ~1.5 s while the
   driver hasn't taken the new size yet (and drops a request as soon as a
   newer one arrives; the daemon stops waiting on it too), keeps one instance per session
   (named mutex), and exits after 30 min without requests (deleting its
   files). If `resize.alive` is missing 0.3 s after a request, or no answer
   comes within 2.5 s (listener died without cleaning up) (first resize, new logon, idle exit), `guest-exec`
   (SYSTEM) runs PowerShell that (re)registers a hidden scheduled task
   `pve-h264-resize` for the logged-in user (`TASK_LOGON_INTERACTIVE_TOKEN`,
   no password; `conhost --headless`, no window) and runs it; about 1–2 s.
   That task definition stays registered until the next start replaces it
   (deleting it could stop the running listener); remove it in the guest
   with `schtasks /delete /tn pve-h264-resize /f`. The daemon learns the
   guest OS at startup, so for Windows it starts this right after the hint
   instead of waiting a second. Each agent operation uses a fresh connection
   (the socket serves one client at a time and Proxmox uses it too). The spice agent port stays on
   D-Bus (harmless, and needed for QXL guests / a future clipboard).
   Two driver limits found in the viogpudo source: the agent can only switch
   to a mode the driver lists (its QXL custom-mode escape fails with
   `0xc0000206`), and the hint puts the size into the driver's single
   "custom" mode slot; that mode is refused if it needs more than the
   framebuffer segment sized at driver start (16 MiB unless a built-in mode
   needs more). Hence the size is sent in **CSS pixels** (a 2× HiDPI window
   asked for 3576x2254 = 32 MB) and clamped to 16 MiB. CSS pixels also make
   the guest's text the same size as the local desktop's on any client OS,
   since the guest's own display scaling can't be set from the host.
   **Linux guests** get only the hint: virtio-gpu reports the size as the
   connector's new preferred mode (with `hotplug_mode_update` set), and the
   compositor has to switch to it. Mutter (GNOME) and KWin (KDE) do; Hyprland
   (e.g. Omarchy), sway and other wlroots compositors pick a mode only when the
   output appears and ignore later mode-list changes. The spice-vdagent
   fallback can't help there: it sets modes with XRandR, which only works on
   X11. The daemon watches the scanout (15 s for Windows, 5 s otherwise; a
   compositor that follows the hint does so within a second or two) and, if
   the size didn't change, sends the viewers `S_RESIZE_STATE` (size, guest
   OS id from `guest-get-osinfo` reduced to `[a-z0-9._-]`, whether the QEMU
   guest agent and spice-vdagent answer; once per size, `{ok:true}` after a
   resize succeeds, and to a viewer that connects while it's failed). The
   viewer then shows a **Fix resize** button next to *Resize guest*. Its
   panel (`www/resizehelp.js`) reads the VM config with the viewer's own
   permissions (`GET …/config`, VM.Audit; without it, the guest agent's OS id)
   and picks the steps by the Proxmox **OS type**: Windows gets a checklist
   (display `virtio`, VirtIO GPU DOD driver in Device Manager, guest agent
   enabled in Proxmox and answering, a logged-in user), each marked ✓/✗ where
   the config or the daemon can tell. Linux gets the display check and a
   desktop picker (remembered per VM): GNOME and KDE follow by themselves
   (fixed resolutions saved in their settings, spice-vdagent on X11), Hyprland
   and sway need the exact size, because they keep the mode list from login
   (a one-off `hyprctl eval` / `swaymsg` command, and a udev hook that reads
   the kernel's new preferred mode from `/sys/class/drm/*/modes`, accepts only
   `WxH`, and passes it to every running session), X11 desktops need
   spice-vdagent (install command by distribution). Commands have Copy
   buttons. The button goes away when the guest reaches that size
   (`S_VIDEO_CONFIG`), when *Resize guest* is unticked, or on reconnect.
10. **libavcodec's NVENC reconfigure forces an IDR**: changing `rc_max_rate` /
   `bit_rate` at runtime makes FFmpeg call `nvEncReconfigureEncoder` with
   `resetEncoder = forceIDR = 1`, so *every* ABR step sent a full keyframe.
   On a slow link that keyframe queues, the old ABR (−25 % on any pacing,
   +15 %/s back, triggered by counting 2 ms wait spins) read that as more
   congestion, and the stream oscillated for many seconds before settling at
   60 fps. The ABR now changes the ceiling rarely and decisively (§8).
11. **Fixed ACK window capped the frame rate**: with 2 frames in flight, 60 fps
   needs an ACK round trip (network + decode) under 33 ms; at 40 ms it held
   43 fps, forever. The window is now sized per viewer from the measured
   round trip (`max_inflight = 0`, default), using 10 s minima so queueing
   can't inflate it, and stays at 2 for viewers whose decoder is slower than
   the frame rate.
9. **Bugs found and fixed during testing**: viewer decoder-reconfigure race
   (async `isConfigSupported` during rapid mode switches), frame-size check
   using padded `codedWidth`, heap overflow when the guest changed resolution
   while the encoder was reopening (ASan/UBSan clean afterwards).

12. **Clipboard through the spice agent** (`daemon/vdagent.c`). QEMU's D-Bus
   display has its own `Clipboard` interface, but it is fed by QEMU's
   built-in `qemu-vdagent` chardev, which would then have to own the agent port
   `com.redhat.spice.0` instead of the daemon (and it can't send the monitors
   config the Windows resize needs, §4.8). So the daemon plays the SPICE client's clipboard part, as
   spice-gtk does: announces `CLIPBOARD_BY_DEMAND` (+ `MAX_CLIPBOARD`, 1 MiB;
   not `CLIPBOARD_SELECTION`, so messages carry no selection byte and only the
   CLIPBOARD selection is shared). Guest copies → `GRAB` → we `REQUEST` the
   UTF-8 text right away (a browser can only be handed text, not asked later)
   → broadcast. Viewer paste → we `GRAB` for the guest and keep the text until
   a guest program pastes (`REQUEST` → `CLIPBOARD`); other requests get a
   `CLIPBOARD` of type NONE so the guest program doesn't hang. Line ends:
   guests announcing `GUEST_LINEEND_CRLF` (Windows) get CRLF, viewers always
   LF. Messages are split into 2 KiB chunks (`VD_AGENT_MAX_DATA_SIZE`) and
   queued (≤ 4 MiB) behind a writable watch, since the old single
   `send(MSG_DONTWAIT)` would have dropped anything bigger than the socket
   buffer. Oversized messages from the guest are skipped without losing sync.
   *Browser side*: browsers expose the clipboard only on a paste, so on
   Ctrl+V the viewer holds the V key back (no `preventDefault`), takes the text
   from the `paste` event, sends `CLIPBOARD`, waits 40 ms for the agent to take
   the guest clipboard, then sends the key (150 ms fallback if no paste event
   comes). pveproxy limits client websocket frames to 128 KiB and closes the
   connection otherwise; the viewer now splits big messages into 64 KiB frames.
   Tested with a Windows 11 guest (virtio-win spice agent, caps `0x46b7`):
   both directions, Unicode, CRLF ↔ LF, no echo, 896 KiB, and the real viewer
   in headless Chromium (Ctrl+V → `CLIPBOARD` before the V key; a guest copy
   lands in the browser clipboard). The VM had been started before the
   agent-port patch; its port was moved to a D-Bus chardev live through QMP
   (`device_del` the `virtserialport`, `chardev-remove vdagent`, `chardev-add`
   a `dbus` chardev named `com.redhat.spice.0`, `device_add` it back on
   `spice.0`), and the Windows agent reconnected on its own.

### 4.11 Audio and lip sync

* **Source**: QEMU's `dbus` audiodev, enabled for the D-Bus display with
  `-display dbus,…,audiodev=X`. On the p2p display link the daemon calls
  `org.qemu.Display1.Audio.RegisterOutListener(fd)`; QEMU then calls
  `Init/Fini/SetEnabled/SetVolume/Write` on our
  `/org/qemu/Display1/AudioOutListener` over that private connection. QEMU
  blocks its main loop while it sets the connection up (handshake + property
  read), so the listener lives on its own thread and GMainContext, never
  waiting on the display thread. QEMU paces `Write` by its own real-time rate
  control (480 frames = 10 ms per call by default) and does not wait for us.
* **Processing**: one voice at a time (another takes over after 200 ms of
  silence), swresample to 48 kHz s16 stereo (the patch asks QEMU for exactly
  that, so normally a pass-through), guest volume applied (QEMU HDA amps:
  0..255 ↔ 74 dB), 10 ms frames. Opus via libavcodec's libopus, `lowdelay`
  (CELT only, 2.5 ms lookahead), 128 kbit/s VBR (~60–90 kbit/s measured), only
  while a viewer wants it; raw PCM for viewers without an Opus `AudioDecoder`
  (1.5 Mbit/s). Not ACK-paced; packets keep flowing while video is paced.
* **Why not mux**: a container (MP4/MPEG-TS/WebM through MSE) only adds the
  player's buffering, 100 ms or more. Sync comes from timestamps, so the two
  streams share the WebSocket, each with its own decoder.
* **Timestamps**: video `capture_us` = when the encoder took the frame;
  audio `ts_us` = when its first sample "started playing" (the `Write`
  arrival, like a sound card starting a buffer). Both are CLOCK_MONOTONIC.
  Audio timestamps are a sample clock: +10 ms per packet, pulled by 1/16 of
  the error per `Write` towards the arrival times (which jitter by about one
  QEMU timer tick). A `Write` more than 20 ms late is either a **stall** or a
  **gap**:
  - **Stall:** QEMU's main loop or the audio thread was held up, e.g. by a
    CPU-bound x264 encode. No sample is missing, and QEMU hands over the
    backlog faster than real time, so the next `Write` comes early. The clock
    stays continuous, the viewer sees late packets, and its jitter buffer
    grows to cover them.
  - **Gap:** the guest produced nothing for a while without stopping its
    stream. Samples are missing and the next `Write` comes on time. The clock
    re-anchors at the arrival, and the viewer plays silence for the jump.

  The late `Write`'s samples wait in the fifo until the next `Write` (5–10 ms,
  at most 25 ms) shows which case it is, so after a gap the new audio carries
  its new time from its first packet. A new stream (`SetEnabled`, voice
  switch, first viewer) or anything more than 1 s off re-anchors at once.
* **QEMU audio timer** (`timer-period=5000` on the `-audiodev`, set by the
  qemu-server patch). QEMU's mixing buffer for the D-Bus backend is one
  480-frame (10 ms) buffer, and the audio timer re-arms itself one period
  *after* each run finishes. With the default 10 ms period it can never
  deliver 48 kHz: on the development host it averaged one `Write` per
  10.24 ms, i.e. **46.85 kHz, 2.4 % short**, and more under host CPU load.
  The viewer played that at 48 kHz, so its buffer drained, the rate control
  sat at −0.5 % and it ran dry every 3–5 s. A 5 ms period leaves 2× headroom,
  and QEMU's own rate control still holds the output to real time
  (48.00 kHz measured). This only takes effect after the VM is stopped and
  started again.
* **Opus decoding keeps the host times itself.** Chromium's `AudioDecoder`
  ignores the timestamps of all chunks after the first and counts decoded
  samples from there (checked in Chromium 156: a +5 s jump in the input came
  out unchanged as +10 ms). So every gap or re-anchor in the daemon was lost,
  and the decoded audio's host times fell further behind real time with each
  one. The decoder now gets gap-free virtual timestamps (cumulative samples,
  which it returns unchanged either way), and `audio.js` maps each output
  back to the packet's real `ts_us` through a small queue.
* **Viewer** (`www/audio.js`): `off = min(arrival − host time)` over the last
  5–10 s of both streams absorbs the clock offset and the network base delay
  (the same for both, so they cancel). Audio delay = time heard
  (`AudioContext.getOutputTimestamp()` + the worklet's reports of which host
  time it renders at which context time; includes the device's output
  latency) − (ts + off). Video delay = time drawn − (capture + off). The
  worklet plays as soon as `target` ms are buffered: target = jitter + 12 ms
  (min 30 ms; 60 ms for the first 10 s after sound is turned on, while
  nothing is measured yet and the decoders warm up) + a penalty of 15 ms per
  real dropout (decays 0.5 ms/s; dropouts in the first 3 s of a stream don't
  count). Jitter = spread (max − min) of host-time-to-buffer delays, measured
  where audio enters the buffer (after the decoder), over the last **30 s**:
  delay spikes recur every few seconds (a keyframe holding audio back on a
  slow link, browser GC, guest bursts), and a short memory shrinks the buffer
  between them, so it drops out every time. **Plus the output device's
  burst**: many devices pull audio in chunks rather than one 2.7 ms render
  quantum at a time (Windows ~10 ms, PulseAudio/PipeWire 10–25 ms,
  Bluetooth more, headless Chromium's fake device 85 ms = 4096 frames), so
  the buffer has to hold a whole pull on top of the jitter. The worklet
  measures it as the spread of (context time − wall clock) per second, worst
  of the last 10 s, and the target adds `burst − 3 ms` (shown as `device N
  ms` in the stats). Without it, a 60 ms buffer on an 85 ms-pull device
  dropped out every few seconds with only 23 ms of network jitter.
* **Playout** (`www/audio-worklet.js`): the worklet keeps the buffer at
  target by playing at most ±0.5 % slower/faster (~9 cents, inaudible),
  steering the *peak* fill of the last 1–2 s (the level right after on-time
  packets, so late packets don't move it) into [target, target + 6 ms]. That
  absorbs clock drift between the host and the browser's audio device (which
  otherwise slowly drains the buffer into a dropout every minute or two at a
  few hundred ppm) and lets the buffer grow without a gap when target rises.
  Above target + max(20 ms, target/2) it drains at up to +1 %; beyond
  target + 300 ms it jumps, crossfading old and new over 5 ms. A forward jump
  of ≥ 5 ms in the host timestamps (a gap in the guest's audio, see
  *Timestamps*) is filled with as much silence (max 300 ms), with the audio
  before it faded out and the audio after it faded in. That keeps the buffer
  level and the timing of what follows. Without it each gap drained the
  buffer for good, which the ±0.5 % rate control can only refill at 5 ms/s,
  and pre-gap and post-gap audio were butted together (a click). Running
  dry over such a gap is not counted as a dropout and adds no penalty.
  The worklet also measures the rate the source really delivers (frames
  written ÷ host time they cover, over 5–20 s) and plays at that rate when it
  is more than 0.2 % off. This covers VMs still running with QEMU's 10 ms
  timer, which deliver ~2.4 % too little. Shown as `source −2.4 %`; restart
  the VM to fix it at the source. In the first second after a (re)start, it
  jumps back to target once the buffer is more than 20 ms over it (300 ms
  later on): the rest of a backlog that arrives after a stall would otherwise
  keep audio, and with lip sync the video, a few hundred ms late for half a
  minute. All level
  changes are 5 ms gain ramps: it fades out so it reaches 0 exactly when the
  buffer runs dry, and back in when audio returns, so dropouts never click.
  Resampling (drift control, contexts not at 48 kHz) uses 4-point Hermite
  interpolation. Stats show `rate ±x %` while it is adjusting. Guest volume
  changes glide over ~3 ms in the daemon instead of stepping.
* **Lip sync** (*Sound* section of the settings panel): off by default (each
  frame drawn the moment it decodes). On: a decoded frame waits until its
  audio is heard (max 300 ms, frames superseded while waiting are skipped,
  timers corrected for their own lateness); if video is the later one, the
  audio buffer grows instead (max 200 ms). ACKs are sent at decode time, so a
  hold never looks like a slow viewer to the daemon's pacing.

## 5. Files

### In the repository
| Path | Purpose |
|---|---|
| `daemon/pve-h264d.c` | daemon: QMP attach, D-Bus listener, clients/protocol, encoder thread, ABR, test pattern |
| `daemon/encoder.c` | encoder backends (nvenc-cuda, nvenc sysmem, vaapi, x264) |
| `daemon/sps.c` | H.264 SPS rewriter (no-reorder VUI) |
| `daemon/guestexec.c` | Windows guest resize through qemu-guest-agent (SetDisplayConfig in the user's session) |
| `daemon/audio.c` | guest audio: D-Bus AudioOutListener (own thread), resampling, guest volume, Opus/PCM packets, test tone |
| `daemon/vdagent.c` | spice-vdagent client on the D-Bus agent port (guest resize for Windows, clipboard) |
| `daemon/probe.c` | `pve-h264d --probe`: test all encoder backends, explain failures |
| `daemon/pve-h264d.h`, `daemon/Makefile` | shared header (wire protocol constants), build |
| `www/index.html.tpl` | viewer page template (pveproxy injects user + CSRF token) |
| `www/h264.js` | viewer: WebSocket, parser, WebCodecs, input, toolbar, stats |
| `www/settings.js` | viewer: Stream settings panel (encoder profiles/tunables, guest resolution / DPI, scaling filter, lip sync, volume) |
| `www/audio.js` | viewer: audio decode (WebCodecs Opus / PCM), jitter buffer control, A/V delay measurement, lip sync |
| `www/audio-worklet.js` | viewer: AudioWorklet playout (ring buffer, rebuffering, drift catch-up) |
| `www/clipboard.js` | viewer: clipboard sharing (paste interception, local clipboard writes, Clipboard panel) |
| `www/resizehelp.js` | viewer: *Fix resize* button and help panel when the guest doesn't follow *Resize guest* |
| `www/keymap.js` | `KeyboardEvent.code` → QEMU qnum table, key combos |
| `www/h264.css` | viewer styles |
| `patches/pve-h264-patch` | idempotent patcher for the PVE files (apply/revert/status) |
| `helper/pve-h264-ctl` | node-local helper (root): `prepare <vmid>` checks the VM, creates a one-time token, starts the daemon. Called by `h264proxy`, over SSH for VMs on other nodes |
| `systemd/pve-h264d@.service` | per-VM unit (Type=notify, on demand) |
| `h264d.conf` | default config (installed to `/etc/pve-h264/h264d.conf` if absent) |
| `install.sh` / `uninstall.sh` | install/update, and full removal |
| `tools/` | test tools (latency probe, end-to-end API test, headless-browser test, clipboard test) |

### Installed on the host
| Path | Notes |
|---|---|
| `/usr/local/sbin/pve-h264d` | daemon binary |
| `/usr/local/sbin/pve-h264-patch` | patcher |
| `/usr/local/sbin/pve-h264-ctl` | helper called by `h264proxy` (locally and over cluster SSH) |
| `/usr/share/pve-h264-console/` | viewer files, served by pveproxy at `/h264/` |
| `/usr/share/doc/pve-h264-console/documentation.md` | this document (linked from the systemd unit) |
| `/etc/systemd/system/pve-h264d@.service` | unit |
| `/etc/pve-h264/h264d.conf` | configuration (kept on reinstall) |
| `/etc/apt/apt.conf.d/99pve-h264-console` | re-applies the patches after every dpkg run |
| `/run/pve-h264/` | `V.sock` (root:www-data 0660), `V.lock`, `tokens/V/<token>` (root 0700), `relay/<id>.sock` (dir root:www-data 0710, socket 0660; one-shot, VMs on other nodes) |

### Proxmox files patched (all pure insertions between `pve-h264-console BEGIN/END` markers)
| File | Change |
|---|---|
| `/usr/share/perl5/PVE/QemuServer.pm` | add `-display dbus,p2p=yes` after `-vnc`; `audio0` with the (default) `spice` driver: `-audiodev spice,…` → `dbus,…,timer-period=5000,out.frequency=48000,out.channels=2` plus `audiodev=` on the display (not with `audio = 0` in `h264d.conf`); spice agent port chardev `spicevmc` → `dbus` (all skipped for `virtio-gl` or user `args` containing `-display`) |
| `/usr/share/perl5/PVE/API2/Qemu.pm` | new API methods `h264proxy` (POST, VM.Console; runs on the browser's node, relays VMs on other nodes over cluster SSH) and `h264websocket` (GET) |
| `/usr/share/perl5/PVE/Service/pveproxy.pm` | serve `/h264/`; render viewer for `?console=kvm&h264=1`; append `-h264.<mtime>` to the UI script's `?ver=` cache-busting tag |
| `/usr/share/perl5/PVE/APIServer/AnyEvent.pm` | `TCP_NODELAY` on websocket proxy legs |
| `/usr/share/pve-manager/js/pvemanagerlib.js` | Console menu entry "H.264", `openH264Viewer`, default-viewer + Console-tab switch |

`pve-h264-patch status` shows each patch. If a future Proxmox version changes
an anchor, that single patch is reported and skipped (never half-applied).

Before applying, `pve-h264-patch` checks the host (also on its own: `pve-h264-patch check`;
`install.sh` runs it first and stops before installing anything). It needs:

* `pve-manager` 9.0 or newer;
* a QEMU (`/usr/bin/kvm`) that lists `dbus` under `-display help`, `-audiodev help` and
  `-chardev help`. Without them the qemu-server patch would make **every** VM fail to start;
* `PVE::AccessControl::assemble_vnc_ticket($username, $path, $port)` and
  `verify_vnc_ticket($ticket, $username, $path, $port, $noerr)`. The API patch binds a relay
  ticket to its relay id through `$port`; if that slot were `$noerr`, a bad ticket would pass.

If any check fails (or can't be run), nothing is applied and blocks left from an earlier
apply are removed, including when the apt hook runs after an upgrade or downgrade.
`pve-h264-patch status` lists the failed checks.

## 6. Install / update / uninstall

### Requirements

| | |
|---|---|
| Proxmox VE | 9.x — developed on 9.1.1 (QEMU 10.1), tested after a full upgrade to 9.2.25 (QEMU 11.1.3) |
| Encoder | NVIDIA GPU with NVENC + driver (`libcuda`, `libnvidia-encode`) — **or** Intel/AMD GPU with VA-API — **or** CPU (x264) |
| Browser | HTTPS + WebCodecs: Chrome/Edge ≥ 94, Safari ≥ 16.4, Firefox ≥ 130 |
| Network | LAN or VPN (plain WebSocket, no WebRTC) |
| VM | `vga: virtio` recommended for 60 fps (§4.1); `tablet: 1` (default) for an absolute mouse; an audio device (`audio0`, driver SPICE) for sound |

### Intel iGPU (VA-API) setup

Proxmox doesn't ship an Intel VA-API driver, and Debian's encoding-capable build
(`intel-media-va-driver-non-free`) is in the `non-free` component, which Proxmox doesn't
enable by default. Without it, the console falls back to CPU encoding (`libx264`).

1. Add `non-free` to the `Components:` line in `/etc/apt/sources.list.d/debian.sources`,
   so it reads `main contrib non-free non-free-firmware`.
2. Install the driver and the `vainfo` tool:

   ```bash
   apt update && apt install intel-media-va-driver-non-free vainfo
   ```

3. Check that the iGPU can encode H.264. Look for `VAProfileH264High` with
   `VAEntrypointEncSlice` or `VAEntrypointEncSliceLP`:

   ```bash
   vainfo --display drm --device /dev/dri/renderD128 | grep -i h264
   ```

4. Ask the console daemon which encoders work, and why the others don't:

   ```bash
   pve-h264d --probe
   ```

5. Restart a VM's daemon so it picks up the new encoder (the next viewer starts it again):

   ```bash
   systemctl stop pve-h264d@<vmid>
   ```

Newer iGPUs (about 11th-gen / Tiger Lake and later, including N100-class chips) encode
H.264 only in low-power (VDENC) mode; the daemon tries that automatically. The iGPU must
stay on the host: not bound to `vfio-pci` or passed through to a VM.

### Commands

```bash
cd pve-h264-console   # wherever you cloned the repository
./install.sh          # build, install, patch PVE, restart pveproxy/pvedaemon; safe to re-run
pve-h264-patch status # verify
./uninstall.sh        # revert patches, remove everything except /etc/pve-h264, verify files
./uninstall.sh --repair   # same, and reinstall the exact installed package versions if a file still differs
```
* The patcher refuses to touch a file whose BEGIN/END markers don't pair up
  (e.g. hand-edited) instead of guessing, and writes every file atomically
  (temp file + fsync + rename). Verified on copies of the real files: corrupted
  copies left byte-identical, good copies reverted to the exact package md5.
* After reverting, `uninstall.sh` checks the 5 files with `dpkg --verify` and
  for leftover markers. `--repair` reinstalls `qemu-server=<installed>`,
  `pve-manager=<installed>`, `libpve-http-server-perl=<installed>`; it is
  **pinned** because a plain `apt install --reinstall <pkg>` installs the newest
  available version, i.e. upgrades (see the 2026-11-04 incident below).
* If a patch can't be applied, `install.sh` now finishes and prints a warning
  instead of aborting halfway.
* Service reloads are **waited for**: `systemctl reload` returns at once while
  the old pveproxy/pvedaemon workers keep serving the old code for ~5 s
  (H.264 API calls got HTTP 501 in that window). The patcher now waits until
  every old worker has exited and new ones run.
* `pve-h264-patch gpu-check` (also run by the apt hook and `status`): if the
  NVIDIA driver is a DKMS module, warns about any installed kernel that has no
  NVIDIA module built (typically a new kernel installed without its headers)
  and prints the exact fix (`apt install proxmox-headers-<kver> && dkms
  autoinstall -k <kver>`). The hook can't install packages itself — dpkg holds
  the lock while it runs.
* After installing, **each VM must be fully stopped and started once** (a
  reboot from inside the guest is not enough) to get `-display dbus`.
* Browsers: a normal page reload loads the patched UI. pveproxy serves
  `pvemanagerlib.js?ver=<pve version>` with no `Cache-Control`, so browsers may
  keep a cached copy for days; the patch adds `-h264.<file mtime>` to that tag,
  which changes whenever the patched file changes (install, re-apply after an
  upgrade). The index page itself is sent `no-cache`.
* After `apt upgrade` the apt hook re-applies the patches automatically; if a
  pve-manager/qemu-server update changed the code, run `pve-h264-patch status`.

### Updates

* **Updating pve-h264-console itself:** `git pull` in the repository and run `./install.sh`
  again. It rebuilds and replaces the daemon and viewer and updates the patches in place;
  `/etc/pve-h264/h264d.conf` is kept. Open viewers pick up the new version on reload. A
  change to QEMU's command line (e.g. the audio timer in the 2026-11-05 audio fix) reaches
  a VM only after a full stop + start.
* **Proxmox updates** to `qemu-server`, `pve-manager` or `libpve-http-server-perl` replace
  the patched files. The apt hook `/etc/apt/apt.conf.d/99pve-h264-console` re-applies the
  patches automatically.
* **Changed Proxmox code:** if a release changed the code a patch hooks into, that single
  patch is skipped with a warning rather than half-applied. Proxmox and noVNC keep working;
  check with `pve-h264-patch status`.
* **QEMU updates** don't affect it, since QEMU is not modified. Tested across the
  QEMU 10.1 → 11.1 upgrade.
* **Tested for real:** a full `apt full-upgrade` from Proxmox 9.1 to 9.2. The hook
  re-applied every patch, the console kept working, and a later uninstall left the
  files byte-identical to the packages.
* **NVIDIA + new kernels:** with the DKMS driver, a new kernel needs its headers, or
  NVENC is missing after the reboot. The apt hook warns about this (`pve-h264-patch
  gpu-check`) and prints the exact `apt install proxmox-headers-… && dkms autoinstall`
  command.
* **Major Proxmox upgrades:** re-run `./install.sh`.
* **Package checks:** while installed, `dpkg --verify` reports the patched Proxmox files as
  modified. `./uninstall.sh` checks the files against `dpkg --verify` afterwards. If any
  still differ, `./uninstall.sh --repair` reinstalls the **exact installed versions** of the
  owning packages. A plain `apt install --reinstall <pkg>` would *upgrade* them. By hand:

  ```bash
  apt install --reinstall $(for p in qemu-server pve-manager libpve-http-server-perl; do echo "$p=$(dpkg-query -W -f='${Version}' $p)"; done)
  ```
* Build deps (installed): `build-essential pkg-config libavcodec-dev libavutil-dev libswscale-dev libglib2.0-dev libva-dev`.
* Dev/test tools installed during development (not needed at runtime):
  `git`, `nodejs` (JS syntax checks), `python3-gi`, `chromium` (headless tests).

## 7. Using it

### Viewer controls

| Control | What it does |
|---|---|
| Sound | mute / unmute this browser. Sound starts with the first click or key press in the console (browser autoplay rules). The tooltip says why there's none (e.g. no audio device) |
| Ctrl+Alt+Del / *Send key…* | key combos the browser would otherwise swallow |
| ⏻ Power | Start, Shutdown, Reboot, Pause, Resume, Hard Stop, Hard Reset (as in noVNC; needs `VM.PowerMgmt`). When the VM is off the viewer shows a **Start Now** prompt instead of an error, and connects by itself once the VM is running |
| ⚙ Stream | settings panel: profiles (Low bitrate / Desktop / Video / Max quality), rate control, bitrate ceiling, adaptive bitrate, preset, tuning, two-pass, AQ, fps, frames in flight (encoder settings are shared by all viewers of that VM); guest resolution / DPI, scaling filter, lip sync and volume (this browser) |
| scale | fit, fit without upscaling, 1:1 CSS px, or 1:1 device px |
| 📋 Clipboard | text clipboard shared with the guest: **Ctrl+V** (Shift+Insert) in the console pastes your local clipboard into the guest, and text copied in the guest lands on your local clipboard. Needs the spice agent in the guest (`spice-vdagent` on Linux, the virtio-win guest tools on Windows) and `vga: virtio` or `qxl`. Without the agent (BIOS, installers, login screens), the panel's **Type into guest** types the box's text as key presses (US keyboard layout in the guest; ~12 ms per key event, *Stop typing* cancels) |
| Resize guest | asks the guest to match the window size, times the panel's *Guest resolution* (0.5×–2×, or native device pixels for HiDPI) (virtio-gpu guests; Windows also needs the QEMU guest agent; Linux: GNOME/KDE follow, Hyprland/sway don't). If the guest doesn't follow, a **Fix resize** button appears with the steps for that guest |
| Capture mouse | relative-mouse guests only: lock the pointer on click (Esc releases); off = noVNC-style |
| Stats | fps, bitrate, encode/decode time, RTT, estimated latency |
| Default | use H.264 for this browser's Console tab and Console button |
| noVNC | switch back to the classic console |
| ⛶ | fullscreen; in Chromium it also captures Esc, Win and Alt+Tab |

### Details

* VM → **Console ▾ → H.264** opens the viewer in a new window.
* Viewer toolbar: Ctrl+Alt+Del, *Send key…* combos, **⏻ Power** (start /
  shutdown / reboot / pause / resume / hard stop / hard reset via
  `/status/<cmd>`; a stopped VM gets a centered *Start Now* prompt, and the
  viewer polls `/status/current` and connects when the VM comes up), **⚙ Stream** (settings
  panel, below), scaling (fit / fit without upscaling / 1:1 CSS px / 1:1 device
  px), **Resize guest** (the guest follows the
  window size; Windows via the spice agent, see below), Stats overlay, **Default** (use H.264 for this browser's
  Console tab and the Console button), **noVNC** (switch back), fullscreen
  (Chromium: also captures Esc/Win/Alt+Tab via Keyboard Lock).
* Mouse: absolute when the guest has an active USB tablet (normal OS). In
  relative mode (BIOS, VMs without `tablet`) the pointer is **not** captured by
  default — like noVNC, changes of the local pointer position are sent as
  relative motion (the guest's own acceleration may let its cursor drift from
  the local one). Tick **Capture mouse** to lock the pointer on click instead
  (exact, unbounded movement; Esc releases).
* **Stream settings panel** (⚙ Stream): profiles *Low bitrate*, *Desktop*
  (defaults), *Video* (bitrate-efficient 60 fps: p5/ll, quarter-res 2nd pass,
  spatial + temporal AQ, cq 26, ≤ 30 Mbit/s — ~20 % fewer bits than the
  defaults on the synthetic video source), *Max quality*; and every tunable of
  §8: rate-control mode, quality, max bitrate, adaptive on/off + floor, VBV,
  preset, tuning, two-pass, spatial/temporal AQ + strength, fps, frames in
  flight, static refinement. Encoder settings are shared by all viewers of the
  VM; only values a user changed are kept in that browser (localStorage) and
  re-sent on connect; *Reset to defaults* returns to the config file. Options
  the running backend doesn't use are greyed out; an option the GPU rejects
  reverts the tuning to safe defaults with a notice instead of killing the
  daemon.
  Display (per browser): **Guest resolution** — multiplier for *Resize guest*:
  0.5× … 2× of the window's CSS size, or **native device pixels** (sharp on
  HiDPI; set the guest's own scaling for readable text). Clamped to the 16 MiB
  guest framebuffer (shown in the panel). Follows moves between screens with
  different scale factors. Scaling filter smooth / pixelated.
* **📋 Clipboard** (text, both directions, through the spice agent in the guest;
  details §4.12): Ctrl+V / Shift+Insert (Cmd+V) in the console first sends the
  local clipboard to the guest, then the key; text copied in the guest is
  written to the local clipboard. Chromium with clipboard access allowed also
  syncs whenever the window gets the focus. The panel shows the shared text
  and has *Send to guest* (the box), *Send my clipboard*, *Copy here* and
  *Sync automatically* (per browser). Unavailable (greyed button, reason in the
  panel): no spice agent port (`vga` must be `virtio`/`qxl`; VM started before
  the install), agent not running in the guest, or `clipboard = off`.
* Keyboard: layout independent (physical scancodes); guest layout applies.
* Requirements: HTTPS (PVE default) and a WebCodecs browser
  (Chrome/Edge ≥ 94, Safari ≥ 16.4, Firefox ≥ 130).
* Permissions: same as noVNC — `VM.Console` on `/vms/<vmid>`.

### Recommended VM settings
* `vga: virtio` (virtio-gpu) for 60 fps and guest resize; std VGA is capped at ~33 fps (see §4.1).
* Keep `tablet: 1` (default) for absolute mouse.
* Windows guests: install the virtio-win GPU driver (viogpudo) for virtio-gpu,
  and the QEMU guest agent (virtio-win guest tools, `agent: 1`) for guest
  resize. The daemon then starts a small PowerShell listener in the
  logged-in user's session through the agent (`guest_exec_resize`, §8) that
  switches the display to the window size; at the login screen nobody is logged in, so Windows keeps its
  size until someone logs in. The VM needs
  one full stop + start after installing/updating pve-h264-console so the agent
  port moves to D-Bus. Side effect: Proxmox's SPICE console (virt-viewer) no
  longer has an agent for that VM (no SPICE clipboard / SPICE-side resize).
* Sound: an audio device (`audio0: device=ich9-intel-hda,driver=spice`, in
  the GUI *Hardware → Add → Audio Device*) and one full stop + start. Its
  backend then goes to the H.264 console instead of SPICE (§4.11); keep it on
  SPICE with `audio = 0`. `driver=none` VMs are left alone.

## 8. Configuration (`/etc/pve-h264/h264d.conf`)

| Key | Default | Meaning |
|---|---|---|
| `backend` | `auto` | `auto` = nvenc(cuda) → nvenc(sysmem) → vaapi → x264; or force `nvenc`, `nvenc-sysmem`, `vaapi`, `x264` |
| `nvenc_preset` | `p4` | p1 (fastest) … p7 (best compression); 1080p on a mid-range NVIDIA GPU: p2 ≈ 3.5 ms, p4 ≈ 5 ms, p6 ≈ 8 ms. x264: p1/p2 = ultrafast … p7 = medium |
| `rc` | `vbr` | `vbr` (quality-targeted, capped by `max_kbps`), `cbr`, `cqp` |
| `tune` | `ull` | NVENC `ull` / `ll` / `hq` — all with zero frame delay (no B-frames, no lookahead) |
| `multipass` | `disabled` | NVENC `qres` / `fullres`: first pass per frame for better bit allocation, +1–3 ms |
| `spatial_aq` / `aq_strength` / `temporal_aq` | `0` / `8` / `0` | adaptive quantization (NVENC; x264 spatial only) |
| `abr` | `1` | adaptive ceiling on network queueing; `0` = fixed at `max_kbps` (no IDRs from bitrate changes) |
| `gpu` | `0` | NVIDIA GPU index |
| `vaapi_device` | empty | e.g. `/dev/dri/renderD129`; empty = probe renderD128–135 |
| `fps` | `60` | frame-rate ceiling (frames only encoded on change) |
| `max_kbps` / `min_kbps` | `25000` / `1500` | VBR ceiling and ABR floor |
| `quality` | `24` | cq/crf/qvbr target (lower = better) |
| `vbv_frames` | `6` | VBV buffer in frames at max rate |
| `max_inflight` | `0` | undecoded frames allowed per viewer before pacing; `0` = auto (round trip ÷ frame interval + 1, 2…8) |
| `refine_frames` | `4` | extra frames after the screen goes static (sharpen) |
| `idle_exit_s` | `30` | daemon lifetime without viewers |
| `audio` | `1` | guest audio to viewers. Read by the patched qemu-server at VM start (`0` = leave the VM's audio on SPICE) and by the daemon |

Viewers can change every key from `nvenc_preset` to `refine_frames` at
runtime from the Stream settings panel (applies to all viewers of that VM,
until the daemon exits).

**ABR** (1 Hz): per viewer, *queue* = this second's average network delay
(ACK delay − the viewer's decode time) minus the 10 s minimum. If it exceeds
one frame interval (≥ 10 ms) in a second without a keyframe, the ceiling is
cut once to min(85 % of itself, 85 % of the bitrate actually sent), then held
2 s. It probes up ×1.3 after 5 calm seconds only while the encoder uses
> 70 % of it, and jumps back to `max_kbps` after 8 calm seconds of not
using it. Measured with a simulated 20 Mbit/s, 20 ms link and 25 Mbit/s video:
two cuts, 60 fps within ~4 s (before: a cut every other second plus a keyframe
each time, 13 s to settle). Unconstrained LAN: no changes at all. Restart a VM's daemon: `systemctl stop pve-h264d@<vmid>`
(next viewer starts it again). Debug logging: `PVE_H264D_DEBUG=1` or `-v`.
Logs: `journalctl -u pve-h264d@<vmid>`.

`guest_exec_resize = 1` (default) — Windows guests: apply *Resize guest*
through qemu-guest-agent (see §4.8). `0` turns it off; the daemon then never
runs commands in the guest.

`clipboard = both` (default) — text clipboard sharing through the spice agent
(§4.12): `to-guest` only lets viewers paste into the guest, `from-guest` only
lets guest copies reach the viewers, `off` disables it (the clipboard
capability is then not announced to the agent at all). Read at daemon start.

## 9. Wire protocol (daemon ⇄ viewer, inside the websocket byte stream)

Every message: `u32 len (LE, = 1 + payload) | u8 type | payload`.

| Dir | Type | Payload |
|---|---|---|
| C→S | `0x01 AUTH` | one-time token (hex) — must be first, within 10 s |
| C→S | `0x02 KEY` | `u8 down, u32 qnum` |
| C→S | `0x03 MOUSE_ABS` | `u32 x, u32 y` (framebuffer pixels) |
| C→S | `0x04 MOUSE_BTN` | `u8 down, u8 button` (0 L,1 M,2 R,3/4 wheel up/down,5 side,6 extra,7/8 hwheel) |
| C→S | `0x05 MOUSE_REL` | `i32 dx, i32 dy` |
| C→S | `0x06 ACK` | `u32 frame_id, u32 decode_us` |
| C→S | `0x07 CONFIG` | JSON, any subset of `rc, quality, max_kbps, min_kbps, abr, fps, preset, tune, multipass, spatial_aq, aq_strength, temporal_aq, vbv_frames, max_inflight, refine_frames`, or `reset: 1` (config-file values); invalid values are ignored |
| C→S | `0x08 KEYFRAME` | request IDR |
| C→S | `0x09 PING` | `f64` echoed as PONG |
| C→S | `0x0A RESIZE` | `u32 w, u32 h` in CSS pixels; clamped to 16 MiB of BGRX, debounced 150 ms → `Console.SetUIInfo`; Windows guest (known from the agent) → guest-agent listener `SetDisplayConfig` right away; others: if the scanout hasn't changed 1 s later → spice agent monitors config; outcome logged within 15 s |
| S→C | `0x81 HELLO` | JSON (proto, vmid, mouse_abs, defaults) |
| S→C | `0x82 VIDEO_CONFIG` | JSON `{codec, width, height, coded_width, coded_height, encoder}` |
| S→C | `0x83 FRAME` | `u32 id, u8 flags(1=key), u32 encode_us, u64 capture_us, Annex-B H.264` |
| S→C | `0x84 CURSOR` | `i32 w,h,hot_x,hot_y` + RGBA |
| S→C | `0x85 CURSOR_POS` | `i32 x, i32 y, u8 visible` |
| S→C | `0x86 PONG`, `0x87 STATS` (JSON, 1 Hz; incl. `window`, `queue_ms`, `base_ms`, `paced_ms`), `0x88 ERROR` (text), `0x89 MOUSE_MODE` (`u8 abs`), `0x8A DISPLAY_OFF` | |
| S→C | `0x8B SETTINGS` | JSON: effective settings (CONFIG keys) + `kind` (nvenc/vaapi/x264) + `encoder`; sent after HELLO, on every change and encoder reopen |
| S→C | `0x8C NOTICE` | text, informational (e.g. tuning rejected by the GPU, reverted) |
| C→S | `0x0B AUDIO` | `u8 mode`: 0 off, 1 Opus, 2 PCM. Off by default; the viewer turns it on once its AudioContext runs |
| S→C | `0x8D AUDIO_STATE` | JSON `{available, playing, reason, rate, channels, src_rate, src_channels, opus, frame_ms}`; after HELLO and on changes. `reason`: `no-device`, `disabled`, `closed`, `error` |
| S→C | `0x8E AUDIO` | `u32 seq, u64 ts_us, u8 codec (1 Opus, 2 PCM), u8 channels, u16 samples` + one Opus packet or `s16le` interleaved; 10 ms (480 samples at 48 kHz); `ts_us` = host time of the first sample, same clock as FRAME's `capture_us`. Not ACK-paced |
| C→S | `0x0C CLIPBOARD` | UTF-8 text (LF line ends, ≤ 1 MiB): the viewer's clipboard; the daemon grabs the guest clipboard for it. Larger than one websocket frame: the viewer splits the stream into ≤ 64 KiB frames (pveproxy drops frames > 128 KiB) |
| S→C | `0x8F CLIPBOARD` | `u8 flags, UTF-8 text`: the guest's clipboard (LF). flags bit 0 = *replay*: the latest shared text, sent on connect and to other viewers when one pastes; shown in the panel, not written to the local clipboard |
| S→C | `0x90 CLIPBOARD_STATE` | JSON `{available, to_guest, from_guest, max, reason}`; after HELLO and on every change (agent starts/stops) |
| S→C | `0x91 RESIZE_STATE` | JSON `{ok:false, w, h, guest ("windows"/"other"/"unknown"), os, qga, vdagent}` when the guest didn't follow *Resize guest* (once per size; also after HELLO while failed), `{ok:true}` once a resize works again |

STATS also carries `audio_kbps`.

### Security model

Opening a stream takes three checks:

1. A valid Proxmox login.
2. The `VM.Console` privilege on `/vms/<vmid>`. The `h264proxy` API call checks it and
   issues a VNC-style ticket bound to the user and the VM.
3. A **one-time token** that only the root-run API call can create (written to the
   root-only token directory). The daemon checks it on its unix socket
   (`root:www-data`, mode 0660).

pveproxy checks the login and ticket before relaying anything. Tokens expire after
60 seconds and are deleted when used. No network ports are opened.

For a VM on another cluster node (§3), the browser's node reaches the VM's node over
the cluster's root SSH, as noVNC does for remote VMs. Only the numeric vmid is put into
the remote command. The node must be a cluster member, and the VM's config must be on
it. The relay socket has a random name, only www-data (pveproxy) can open it, it takes
exactly one connection within 10 s, and it is unlinked when that connection arrives. The
ticket is bound to the relay id, so a relay ticket only opens that relay and a local
ticket opens no relay. The VM's node still checks the one-time token.

## 10. Performance and test results

### Performance summary

Measured on the development host (a mid-range NVIDIA GPU, Proxmox VE 9.1) with a
synthetic 1920×1080 test pattern moving at 60 Hz (a dragged window plus a scrolling
terminal):

| Metric | Result |
|---|---|
| Frame rate | steady **60 fps** (16.9 ms frame interval) |
| Capture → client (p50) | **5.3 ms** with NVENC + CUDA dirty-rect (6.4 ms from system memory) |
| GPU upload + encode | ≈ 1.0 ms + 2.6 ms (half the screen changing every frame) |
| Round trip through pveproxy (TLS) | 0.6 – 1.1 ms |
| Bitrate | ≈ 3 – 5 Mbit/s for that content; near zero on a static screen |

**Estimated glass-to-glass latency** at 1080p60 with a browser that decodes on its GPU:
**about 20–30 ms** (breakdown below). It's an estimate built from the measured parts,
not an end-to-end measurement.

### Testing tools

```bash
# daemon-only latency, on the host, with a synthetic 1080p60 source
systemctl set-environment PVE_H264D_TEST=1920x1080
python3 tools/h264-latency.py <vmid> 5
systemctl unset-environment PVE_H264D_TEST
```

```bash
# full path: login → h264proxy → wss through pveproxy → frames
PVE_USER=root@pam PVE_PASS=... python3 tools/h264-e2e.py <vmid> 5
# cluster: VM on another node, through this node's relay (prints relay=…)
PVE_NODE=<vm's node> PVE_USER=root@pam PVE_PASS=... python3 tools/h264-e2e.py <vmid> 5
```

```bash
# clipboard both ways with a Windows guest (spice agent + qemu-guest-agent), via pveproxy
python3 tools/h264-clipboard-test.py <vmid>
```

`tools/h264-browser-test.py` drives a headless Chromium through the real viewer page. Its
modes are `view`, `esc`, `perf`, `ui`, `mouse`, `settings` and `audio` (sound on, lip sync
per `LIPSYNC=0/1`, audio stats every 5 s for `SECS` seconds).

The test source's tone can imitate a guest's audio problems. Every 3 s, `:gaps[=ms]` leaves
out that much audio (default 300) and `:stalls[=ms]` holds it back, then hands it over at
once (default 80), e.g. `PVE_H264D_TEST=2560x1260:gaps=40`.

### Test log (development host)

| Test | Result |
|---|---|
| D-Bus attach via QMP, memfd ScanoutMap | works, zero-copy (`src memfd`) |
| SeaBIOS screen → NVENC → decode (ffmpeg) | correct image |
| Full path as non-root PVEVMUser (login → h264proxy → wss via pveproxy) | works; setup ≈ 134 ms; ping RTT through pveproxy ≈ 0.6–1.1 ms |
| Headless Chromium viewer (WebCodecs) | live SeaBIOS picture; **Esc from the browser opened the SeaBIOS boot menu** (keyboard path verified) |
| PVE UI | Console menu shows "H.264" for VMs; Console tab / Console button switch to H.264 when "Default" is set |
| Security | bogus token → "authentication failed" + close; bad ticket → HTTP 401 |
| Lifecycle | VM stop mid-stream → viewer gets "VM display connection closed"; stopped VM → clear API error; VM started pre-install → "restart the VM" message |
| Two viewers | shared encode, each gets an IDR on join |
| ASan/UBSan build (x264 and NVENC paths, rapid mode switches) | no memory errors |
| Synthetic 1080p60 motion (`PVE_H264D_TEST=1920x1080`), instant-ACK client | steady 60 fps (16.9 ms interval); capture→client p50 **5.3 ms** (nvenc-cuda) vs 6.4 ms (sysmem); ~3–5 Mbit/s VBR for that content |
| Headless Chromium 1080p | limited to ~45–50 fps by *software* decode on the host CPU (≈20–30 ms/frame); real browsers decode on the GPU (≈1–4 ms) |

Expected glass-to-glass at 1080p60 with a GPU-decoding browser on the LAN:
≈ half a frame waiting for the next guest update + ~3.5 ms encode + ~1 ms
network/relay + ~2–4 ms decode + up to one display refresh ≈ **20–30 ms**
(vs. typically 60–150+ ms for noVNC at this resolution).

Not verified on the development host (no hardware/OS available): VA-API on an Intel iGPU, absolute
mouse and HW cursor with a real guest OS, real-GPU browser decode timings,
virtio-gpu guest at 1080p60.

**Audio (2026-11-04)**, headless Chromium 156 through pveproxy, synthetic
source (`PVE_H264D_TEST=1920x1080`: the test pattern flashes a white box and
a 1 kHz tone beeps in the first 100 ms of every host second):

| Check | Result |
|---|---|
| Real QEMU 11.1.3 (test VM + `audio0: ich9-intel-hda`) | command line accepted, `audio: attached`, voice `Init` 48 kHz / 2 ch / s16; re-attaches after daemon restarts; without `audio0`: `no-device` |
| Opus | ~60–90 kbit/s, loud packets exactly at host ms 0–90 of each second (peak 0.30 for a 0.27 tone) |
| PCM fallback (`AudioDecoder` removed) | 1.5 Mbit/s, plays |
| A/V offset, lip sync off | audio +145…+175 ms later: headless Chromium's fake output device reports 128 ms output + 43 ms base latency (real desktop devices: ~10–40 ms) |
| A/V offset, lip sync on | −5…−13 ms (video held ~180 ms) |

Dropouts (stutter), 60 s each, `1920x1080:video` source (~25 Mbit/s), headless
Chromium decoding in software at ~35 fps (a busy main thread: harsher than a
desktop browser). "Link" = pveproxy → browser throttled to 40 Mbit/s with
`tc tbf` on `lo`, so large video frames hold audio packets back:

| Case | First version (2–4 s jitter memory) | Now |
|---|---|---|
| unthrottled | – | 0 dropouts, buffer 43 ms avg |
| 40 Mbit/s link | 5 | 2 (one spike at 18 s), buffer 57 ms |
| link + 70 ms main-thread stall every 6 s | 7 | 2 (first stall, one at 18 s), buffer 110 ms |

The remaining dropouts are the first occurrence of a bigger delay spike; the
buffer then covers it for 30 s.

Re-run after the device-burst and rate-control changes (same setup, 60 s
each; this Chromium's fake output device pulls 4096 frames = 85 ms at a time,
measured in the worklet):

| Case | Before (jitter-only target, speed-up only) | Now |
|---|---|---|
| unthrottled | 0 (4 in another 20 s run) | 0, target 79 ms |
| 40 Mbit/s link | 3 | 0, target 95 ms |
| link + 70 ms main-thread stall every 6 s | 2 | 0, target 137 ms |

The higher targets are the 85 ms device pull being covered; a desktop
device's 10–25 ms pull adds correspondingly less. An offline harness (the
worklet driven with synthetic packet timing) also showed: device clock
300 ppm fast → 1 dropout in 2 min before, 0 now (rate −0.05 %); target
30 → 70 ms mid-stream → 1 dropout before, 0 now; a 300 ms+ jump clicked
before (2nd difference 0.46), now crossfaded (0.005, same as the tone). Audio decode still runs on the page's main
thread, so a long main-thread stall can still cause one.

**Audio timestamps and real-guest rate (2026-11-05)**, headless Chromium
156, lip sync on, 40–60 s each, x264 forced (`backend = x264`), test pattern
at 2560×1260 unless noted. *Before* = the code before this fix, *after* =
now:

| Case | Before | After |
|---|---|---|
| test tone, 300 ms gap every 3 s (`:gaps`) | A/V and "audio delay" grow 300 ms per gap (+4.9 s after 50 s); jitter 3 s; 2 dropouts counted | A/V −20…+49 ms, jitter 59–73 ms, 0 dropouts |
| test tone, 40 ms gap every 3 s (`:gaps=40`) | – | every gap played as silence, A/V −14…−9 ms, 0 underruns |
| test tone, 80 ms stall + backlog every 3 s (`:stalls`) | – | no silence inserted (continuous), jitter ~107 ms, A/V −17…−5 ms, 0 dropouts |
| test tone 2.4 % slow (`:slow=24`, an unrestarted VM) | – | `source −2.4 %`, 0 dropouts after the first 5 s, A/V −22…−7 ms |
| Ubuntu 26.04 live guest, `speaker-test` 440 Hz, 10 ms QEMU timer | QEMU 46.85 kHz; rate −0.5 %, buffer below target, dropout every 3–5 s | – |
| same guest, `timer-period=5000` | – | QEMU 48.00 kHz; 0 dropouts, jitter 24–30 ms, A/V −18…−9 ms |
| same, QEMU starved (6 busy loops at nice −10 on 6 cores, viewer reniced) | – | 2 dropouts in 60 s (first 78 ms and 235 ms QEMU stalls), A/V −18…−13 ms, +81…+111 after the 235 ms stall (lip-sync hold limit of 300 ms with headless Chromium's 170 ms output latency) |

Chromium's `AudioDecoder` timestamps checked directly: 100 Opus packets with
a +5 s jump at packet 50 came out with the jump removed (outputs
first input + 10 ms each).

Not yet verified: a desktop browser and its audio device, a cluster relay
(the relay is a plain SSH + socat byte pipe, and the user's stats showed rtt 1.9 ms /
queue 1 ms through it).

## 11. Known limitations / TODO

* **std VGA ≈33 fps** cap (QEMU 30 ms timer). Fix options: use `vga: virtio`;
  or rebuild pve-qemu with a patch setting `ddl->dcl.update_interval = 16`
  (ms) in `ui/dbus-listener.c:dbus_display_listener_new()`.
* **Cluster**: VMs on other nodes are relayed over the cluster's SSH (§3), which adds
  a little latency and SSH CPU time on both nodes. Install the same version on every node.
* `virtio-gl` (VirGL) VMs are skipped (they already use `-display egl-headless`).
* VA-API path written per FFmpeg API but untested on real Intel/AMD hardware.
* Clipboard: browsers only give a page the clipboard on a paste, so the browser's
  clipboard reaches the guest when you press Ctrl+V in the console (or, in Chromium
  after allowing clipboard access once, whenever the window gets the focus). Safari and
  Firefox may only write a guest copy to the local clipboard right after a click or key
  press in the viewer; otherwise the Clipboard button lights up, and a click copies it.
* Audio is playback only
  (no microphone; QEMU's `AudioInListener` would be the way).
* Audio: only one guest voice is forwarded at a time (guests play through one
  stream); a VM's audio goes either to this console or to SPICE, not both.
* Lip sync can't see the guest's own audio buffering (it happens before QEMU).
* Audio shares the main thread with video decode/drawing on the way to the
  worklet; a WebSocket in a Worker that feeds the worklet directly would make
  sound immune to main-thread stalls.
* *Type into guest* maps characters for a US keyboard layout (other guest
  layouts get the wrong symbols; characters a US keyboard can't type are
  skipped), a guest with Caps Lock on gets inverted letter case, and it is
  paced at one key event per 12 ms, so long texts take a while (it stops
  when the console is hidden or disconnects).
* Clipboard: text only (no images/files), CLIPBOARD selection only (not X11
  PRIMARY), ≤ 1 MiB; needs the spice agent in the guest.
* Multi-monitor: only console 0.
* Config changes from one viewer apply to all viewers of that VM.
* `nvenc_preset`, `backend` only via config file (daemon restart).

## 12. Troubleshooting

Sound: the *Sound* button's tooltip says why there is none. `journalctl -u
pve-h264d@<vmid> | grep audio`: `attached` = OK; `none: the VM has no audio
device on the D-Bus backend` = no `audio0`, `driver=none`, `audio = 0`, or the
VM was started before the update (`qm showcmd <vmid> | grep -o 'audiodev [^ ]*'`
should show `dbus`). Browsers start audio only after a click or key press in
the page. The Stats overlay shows the codec, buffer, underruns and the A/V
offset.

| Symptom | Check |
|---|---|
| "started before the H.264 console was installed" | full stop + start of the VM; `qm showcmd <vmid> \| grep -o -- '-display [^ ]*'` |
| No "H.264" menu entry | `pve-h264-patch status` (all `[x]`?); in the browser console `typeof PVE.Utils.openH264Viewer` should be `"function"` — if not, the browser runs a cached UI (hard-refresh; fixed for installs that include the cache-tag patch) or the page comes from another cluster node without pve-h264-console |
| "not installed (or is too old) on node 'X'" | the VM runs on node X, which has no `/usr/local/sbin/pve-h264-ctl`: run `./install.sh` on X (same version on every node) |
| VM on another node: connects, then closes at once | task log of the `h264relay` task on the node you're connected to (`ssh`/`socat` errors); on the VM's node `journalctl -u pve-h264d@<vmid>`; check `ssh root@<node>` works from the node you're connected to (`pvecm updatecerts` repairs cluster SSH keys) |
| "failed to start H.264 console daemon" | `journalctl -u pve-h264d@<vmid>` |
| Clipboard unavailable / not working | the panel (📋 Clipboard) shows the reason; `journalctl -u pve-h264d@<vmid> \| grep -i "clipboard\|vdagent"`. "no spice agent port" = `vga` isn't virtio/qxl, or the VM was started before the install (stop + start); "not running" = start the spice agent in the guest (Windows: *spice-agent* service from the virtio-win guest tools; Linux: `spice-vdagent`, needs an X11/XWayland session for the clipboard). Pasting does nothing: the browser must allow clipboard access for the PVE page; the panel's box always works (paste into it, *Send to guest*) |
| "Guest did not resize to WxH" / **Fix resize** button | the button's panel checks the VM config (OS type, display, guest agent) and the agents and lists the steps for that guest. Hyprland (Omarchy) and sway keep the mode list from login, so "preferred" stays the old size: the guest has the new size as the first line of `/sys/class/drm/card*-Virtual-1/modes`, and `hyprctl eval 'hl.monitor({ output = "Virtual-1", mode = "WxH", position = "auto", scale = "auto" })'` (Hyprland 0.55+, Lua config; `hyprctl keyword` is rejected there) or `swaymsg output Virtual-1 mode WxH` switches to it. The panel's udev hook (`/usr/local/bin/vm-resize`) does that after every resize. Run `hyprctl` as the desktop user, not root (root has no `HYPRLAND_INSTANCE_SIGNATURE`) |
| Windows guest doesn't resize | `journalctl -u pve-h264d@<vmid> \| grep -i "resize\|vdagent"`: `guest-exec resize:` lines give the reason (agent not responding, nobody logged in, Win32 error from SetDisplayConfig); for the spice agent path: "not on D-Bus" = VM started before the update (stop + start); no "guest agent connected" = `spice-agent` service not running in the guest; guest log `No suitable mode found` = the driver doesn't list that size (hint not applied yet, or larger than its framebuffer) |
| Sound stutters / clicks; Stats show `source −2.x %` or `rate −0.5 %` with dropouts | the VM still runs with QEMU's default 10 ms audio timer, which delivers ~2–3 % too few samples: stop + start the VM (`qm showcmd <vmid> \| grep -o 'audiodev [^ ]*'` should contain `timer-period=5000`). Jitter of hundreds of ms on a busy host = QEMU stalls under CPU load (§4.11) |
| Black screen / waiting for video | viewer Stats overlay; `PVE_H264D_DEBUG=1`; check the browser console for decoder errors |
| Low fps | `vga: std` cap (§4.1); viewer decode time in Stats (software decode?); "paced" counter = viewer/network can't keep up |
| Encoder falls back to x264 | run `pve-h264d --probe` (lists render nodes, installed VA drivers and why each backend fails). Intel: install `intel-media-va-driver-non-free` (enable Debian `non-free`), check `vainfo --display drm --device /dev/dri/renderD128`; the iGPU must not be bound to vfio/passed through. NVIDIA: driver/`libcuda` and, after a kernel update, `pve-h264-patch gpu-check` |

## 13. Change log

* 2026-11-04 — Initial implementation: daemon (D-Bus capture, NVENC/VA-API/x264,
  ACK pacing, ABR), browser viewer, PVE API/UI integration, patcher with apt
  hook, systemd unit. Fixes during testing: SPS no-reorder rewrite, CUDA
  dirty-rect upload, unchanged-damage suppression, decoder reconfigure race,
  resolution-change heap overflow, ABR decode/network separation, token dir
  permissions. Tested on a disk-less std-VGA test VM.
* 2026-11-04 — **Intel iGPU fell back to x264** on another host. VA-API
  path hardened: tries the regular and the **low-power (VDENC)** H.264
  entrypoint (Gen11+/Tiger Lake+ iGPUs such as N100 only have low-power, which
  FFmpeg never selects by itself), rate control QVBR → VBR → CQP, records every
  failure with the DRM driver and VA driver name, and hints at the missing
  package (`intel-media-va-driver-non-free`, Debian `non-free`, not enabled on
  Proxmox by default). New `pve-h264d --probe`. Not testable on the development
  host (no Intel/AMD GPU). Also fixed a Makefile ordering bug introduced in this change
  that would have dropped `-O2`.
* 2026-11-04 — **Browser-cache bug** found on a second host: patches
  applied (`pve-h264-patch status` all `[x]`) but no menu entry, because the
  browser kept the cached `pvemanagerlib.js?ver=9.x` (no Cache-Control from
  pveproxy, unchanged URL). On the development host it was hidden by the Proxmox
  version change during the upgrade. Fix: 11th patch tags the URL with the
  patched file's mtime. Revert restores the stock URL; `dpkg --verify` clean.
* 2026-11-04 — **Full upgrade test** (`apt full-upgrade`, 142 packages, to
  Proxmox VE 9.2: pve-manager 9.2.25, qemu-server 9.2.14, pve-qemu-kvm 11.1.3,
  proxmox-kernel 7.0.14-28 installed but not booted yet). Results:
  - the apt hook re-applied all 10 patches to the new files; Perl/JS compile,
    services healthy, web UI up;
  - H.264 console works unchanged on **QEMU 11.1.3** (D-Bus display interfaces
    identical to 10.1, memfd capture, NVENC CUDA path, keyboard/mouse, UI menu
    and Console-tab default on the 9.2 web UI);
  - live `uninstall.sh` on 9.2: all 5 files byte-identical to the packages
    (`dpkg --verify` clean); reinstall OK;
  - **found and fixed:** (1) reload race → HTTP 501 for ~5 s after install or
    hook re-apply; (2) kernel 7.0 arrived without headers, so the NVIDIA DKMS
    module wasn't built — NVENC would have vanished after reboot. Installed
    `proxmox-headers-7.0` and built it (`dkms autoinstall -k <kver>`; the
    driver compiles fine on 7.0); added `gpu-check`.
  Rebooted into kernel 7.0: NVIDIA module loads, NVENC OK, daemon 1080p60
  test pattern at 60 fps with capture→client p50 4.7 ms (5.1 ms on 6.17), the
  test VM's display streams normally; all 10 patches, services and web UI
  healthy.
* 2026-11-04 — Hardened install/uninstall (marker validation, atomic writes,
  dpkg verification, pinned `--repair`, install no longer aborts on a failed
  patch). **Lesson learned while testing:** simulating an update with
  `apt-get install --reinstall qemu-server pve-manager libpve-http-server-perl`
  *without* version pins upgraded those packages and 46 dependencies
  (pve-manager 9.1.1→9.2.25, qemu-server 9.0.30→9.2.14, pve-qemu-kvm
  10.1.2→11.1.3, …), leaving a partial upgrade — hence the pinned `--repair`.
  The host stayed healthy. Upside: the apt
  hook re-applied all 10 patches cleanly onto the PVE 9.2 files — a real
  version-update test. Running VMs still use the old QEMU 10.1 binary until
  restarted; QEMU 11 with the daemon is untested.
* 2026-11-04 — Relative mouse no longer captures the pointer by default
  (noVNC-style position deltas); pointer lock moved behind the new
  "Capture mouse" toolbar option. Browser test tool gained a `mouse` action.
* 2026-11-04 — A pve-qemu-kvm rebuild with the D-Bus refresh patch (§11) was
  started and then **cancelled** (too heavy to maintain as a custom QEMU build).
  Nothing was installed: stock `pve-qemu-kvm` remains. The std VGA
  ~33 fps cap stays; use `vga: virtio` for 60 fps.
* 2026-11-04 — **Windows guest resize.** The viewer's resize requests reached
  QEMU (`SetUIInfo`) but the Windows virtio-gpu driver ignores them (the test
  VM stayed at 1280x800). Sending a monitor config through QEMU's SPICE server
  doesn't help either: QEMU intercepts it and turns it into the same hint. New
  12th patch moves the spice agent port to a `-chardev dbus`; the daemon
  (`daemon/vdagent.c`) registers on it and sends the size to the guest's
  `spice-agent` directly. Resize requests are debounced (one per drag), and
  the viewer also requests a resize on connect when *Resize guest* is on.
  Tested against a throwaway QEMU and a simulated agent; not yet tested with
  a real Windows guest (needs a VM restart).
* 2026-11-04 — First real test (HiDPI 2× client, Windows guest): the agent got
  the request but Windows stayed put; the agent log showed its QXL escape
  failing and `No suitable mode found` for 3576x2254. Fixes: resize in CSS
  pixels instead of device pixels (any client OS), clamp to the 16 MiB
  framebuffer, ask the agent only if the hint alone didn't resize the guest,
  and verify against the scanout size with retries. Also fixed the agent
  reply log (`VD_AGENT_SUCCESS` is 1, not 0).
* 2026-11-04 — Second test (Windows guest): resizing only worked at boot (the driver
  takes QEMU's size when it starts); afterwards the spice agent answered
  "success" without switching. Reading the agent source showed it can only
  add modes on QXL. New `daemon/guestexec.c` resizes Windows through
  qemu-guest-agent with `SetDisplayConfig` in the user's session (see §4.8),
  new config key `guest_exec_resize`. Verified against a fake guest agent
  (protocol, script encoding, 13 K command line); not yet on a real guest.
* 2026-11-04 — Guest-agent resize worked on the Windows guest; made it faster. A resident
  PowerShell listener in the user's session (no `Add-Type` compile: P/Invoke
  via Reflection.Emit) takes requests through a file the daemon writes with
  `guest-file-*`, so a resize no longer starts processes in the guest
  (~0.1–0.3 s instead of ~3–4 s). The daemon detects the guest OS at startup
  and, for Windows, skips the 1 s wait after the hint; debounce 250 → 150 ms.
  Verified against a fake agent with a simulated listener (first resize,
  fast path 88 ms, restart after idle exit); not yet on a real guest.
* 2026-11-04 — Fixed 5–10 s lags with the listener (log from the Windows guest: the
  switched on time, but the answer never arrived, so the daemon waited 8 s
  and only then sent the next size). The agent opens files without write
  sharing, so the listener's write to `resize.ack` failed while the daemon
  was polling it (silently, no retry), which then triggered needless
  listener restarts. Now one ack file per request, written with retries;
  `resize.alive` tells the daemon quickly whether a listener runs; request
  writes are retried while the listener reads the file; superseded requests
  are dropped on both sides. Verified against a fake agent that enforces
  the same locking (steady state 100–140 ms, burst ends on the last size,
  idle exit → immediate restart, crashed listener → 3.7 s once).
* 2026-11-04 — **Encoder controls, DPI, faster ramp to 60 fps.** Viewer
  Stream settings panel (`www/settings.js`) with profiles and every encoder
  tunable (rate-control mode, preset, tuning, two-pass, spatial/temporal AQ,
  VBV, adaptive bitrate on/off, frames in flight, refinement); guest
  resolution multiplier / native device pixels for *Resize guest*; scaling
  1:1 device px; pixelated filter. Daemon: new keys (§8), `0x8B SETTINGS` /
  `0x8C NOTICE`, auto ACK window from the measured round trip (40 ms RTT:
  43 → 60 fps), ABR redesigned around libavcodec's forced IDR on NVENC bitrate
  changes (§4.10), jitter-tolerant frame pacing (an ideal 1/fps timeline that
  allows a quarter interval early), safe fallback when the GPU rejects a
  tuning combination, `paced_ms` replaces the misleading `skipped` count.
  Fixed a pre-existing race: a config change during an encoder open was lost.
  Default preset p2 → p4. Test source `PVE_H264D_TEST=WxH:video` (full-screen
  panning texture, ~25 Mbit/s at 1080p cq24). Browser test mode `settings`
  (`DPR=2` emulates HiDPI).
* 2026-11-04 — **Guest audio with lip sync.** qemu-server patch moves the
  VM's `audio0` backend from SPICE to QEMU's D-Bus audio and hands it to the
  D-Bus display (`audio = 0` in `h264d.conf` keeps SPICE). New
  `daemon/audio.c`: AudioOutListener on its own thread, resampling, guest
  volume, Opus (libopus, 10 ms, low delay) or raw PCM per viewer, host
  timestamps on the video clock; protocol `0x0B AUDIO`, `0x8D AUDIO_STATE`,
  `0x8E AUDIO`. Viewer: *Sound* button (mute, reason when unavailable),
  WebCodecs Opus decode, AudioWorklet player with an adaptive jitter buffer,
  A/V offset in the stats, optional lip sync (hold video for its audio, or
  buffer audio for late video), volume. Test mode beeps and flashes once a
  second. Build dependency: `libswresample-dev`. See §4.11 and §10.
* 2026-11-04 — **Fewer audio dropouts.** The jitter buffer remembered spikes
  for only 2–4 s and its post-dropout margin faded in 10 s, so recurring
  spikes (keyframes on a slow link, GC pauses) caused a dropout each time.
  Now: jitter measured where audio enters the buffer, as the delay spread
  over 30 s; 60 ms start-up floor; 15 ms per dropout, decaying over ~30 s;
  2 ms fades at dropouts; catch-up limited to 1 % speed. Stats show jitter
  and a running dropout count. Measured 5 → 2 and 7 → 2 dropouts per minute
  in the throttled-link tests (§10).
* 2026-11-04 — **No clicks, no drift dropouts, device bursts covered.**
  Audio devices that pull in chunks (85 ms in headless Chromium, 10–25 ms on
  typical desktops) were not accounted for, so the buffer ran dry inside a
  pull even with low jitter; the worklet now measures the burst and target
  adds it. Rate control in both directions (±0.5 %, on the peak fill)
  replaces speed-up-only catch-up, so clock drift and a growing target no
  longer end in a dropout. Click-free 5 ms gain ramps at every
  underrun/restart, crossfaded jumps, Hermite resampling, guest volume
  ramps in the daemon. Results in §10.
* 2026-11-04 — **Clipboard sharing** (text, both ways) between the viewer's
  browser and the guest through the spice agent (§4.12): `C_CLIPBOARD` /
  `S_CLIPBOARD` / `S_CLIPBOARD_STATE` (§9), config `clipboard` (§8), viewer
  Clipboard button + panel (`www/clipboard.js`), Ctrl+V paste interception,
  `tools/h264-clipboard-test.py`. Also: vdagent writes are now chunked and
  queued (were limited to a 64-byte payload), viewer messages larger than
  64 KiB are split into several websocket frames (pveproxy's 128 KiB frame
  limit), and the daemon accepts client messages up to 1 MiB + 64.
* 2026-11-05 — **Cluster support**: opening the console of a VM on another node
  no longer needs the PVE UI opened on that node. `h264proxy` no longer uses
  `proxyto` and runs on the browser's node, and VMs on other nodes are relayed over
  the cluster's SSH like noVNC's (`pve-h264-ctl prepare` on the VM's node + a one-shot
  `h264relay` task, ticket bound to the relay id). The token/daemon start logic moved
  from the API patch into `pve-h264-ctl`, and local VMs behave as before. Tested on a
  single node by forcing the relay path over an SSH loopback: e2e stream, headless
  Chromium viewer, ticket binding (wrong or missing relay → 401, bad syntax → 400, used
  socket can't be reopened), socket permissions, 10 s accept timeout, relay cleanup.
  `tools/h264-e2e.py` takes `PVE_NODE`, and both test tools take `PVE_TICKET`.
* 2026-11-05 — **Audio: A/V offsets of minutes, "jitter" of seconds, clicks
  and dropouts with real guests.** Three causes:
  (1) Chromium's `AudioDecoder` ignores the timestamps of all chunks after
  the first and counts decoded samples, so every gap or re-anchor in the
  daemon's timestamps was lost. The host times of decoded audio fell behind
  by the total of all gaps since the decoder opened (156 s on one VM, 4.6 s
  of "jitter" on another), which broke the A/V offset, lip sync (video held
  300 ms) and the jitter buffer (target stuck at 250 ms). The decoder now
  gets gap-free virtual timestamps, and `audio.js` maps outputs back to the
  real ones.
  (2) QEMU's D-Bus audio backend delivered ~2.4 % fewer samples than 48 kHz
  with its default 10 ms timer: one 480-frame buffer per tick, and the
  timer re-arms a period after each run. Worse under CPU load. The buffer
  drained, the rate control sat at −0.5 %, and it ran dry every few seconds.
  The qemu-server patch now adds `timer-period=5000` (48.00 kHz measured),
  which needs a VM stop + start. The worklet also follows the source's real
  rate, measured from the timestamps (`source −x %` in the stats), for VMs
  not restarted yet.
  (3) Gaps in the guest's audio drained the buffer for good and butted
  pre-gap and post-gap audio together (a click). The daemon now tells stalls
  (QEMU catches up: continuous timestamps) from gaps (re-anchor) by holding
  a late `Write` until the next one. The worklet plays a timestamp gap as
  faded silence, trims a stall's backlog right after a restart, and running
  dry over a source gap no longer counts as a dropout.
  Test source options `:gaps[=ms]`, `:stalls[=ms]`, `:slow[=‰]`; browser
  test mode `audio`. Results in §10. The cluster relay was not involved.
* 2026-11-05 — **Host compatibility check.** Nothing stopped an install on an
  older Proxmox VE: on a QEMU without D-Bus display/audio/chardev backends the
  qemu-server patch would have made every VM fail to start, and an older
  ticket API could have taken the relay id as `$noerr`. `pve-h264-patch`
  now checks pve-manager ≥ 9.0, QEMU's D-Bus backends and the ticket function
  signatures before applying (`check` command, also run first by
  `install.sh`); on failure it applies nothing and removes earlier blocks.
  Tested on 9.2.25 (passes, files byte-identical after re-apply) and with
  simulated failures (QEMU without dbus, failing or missing QEMU, old ticket
  signatures, pve-manager 8.4, no pve-manager) on copies of the patched files.
* 2026-11-05 — **Resize guest: say when the guest doesn't follow.** Linux
  guests on Hyprland (Omarchy), sway and other wlroots compositors ignore the
  virtio-gpu resize hint, and spice-vdagent can only switch modes on X11, so
  *Resize guest* silently did nothing (the daemon logged it after 15 s). The
  daemon now tells the viewers the likely cause (Windows:
  guest agent / logged-in user; others: compositor ignores hints, plus whether
  spice-vdagent runs), once per size, after 5 s for non-Windows guests
  (15 s for Windows). The viewer shows a **Fix resize** button next to
  *Resize guest* (new `S_RESIZE_STATE` 0x91 instead of a plain notice) whose
  panel gives the steps for that guest with Copy buttons (`www/resizehelp.js`).
  Docs: §4.8 Linux note, troubleshooting row with a Hyprland udev workaround.
* 2026-11-05 — **Clipboard: Type into guest.** The Clipboard panel can type
  its text into the guest as key presses, for guests without the spice agent
  (BIOS, installers, login screens, no `spice-vdagent`). Characters are mapped
  for a US layout (Shift as needed, CRLF → Enter, others skipped and counted),
  sent one key event per 12 ms (the guest's keyboard queue is short: USB HID
  holds 16 events), with progress and *Stop typing* on the button; longer
  than 2000 characters asks first. Hiding the console or a disconnect stops
  it and releases held keys. Tested in headless Chromium (event sequence).
* 2026-11-06 — **Fix resize panel by OS type.** The panel reads the VM config
  and picks the steps by the Proxmox OS type instead of only the guest
  agent's answer: Windows gets a checklist (display, VirtIO GPU driver in
  Device Manager with the virtio-win ISO folder for that Windows version,
  guest agent enabled / answering, logged-in user), Linux a desktop picker
  (GNOME, KDE Plasma, Hyprland/Omarchy, sway, X11 desktops). Found on
  Omarchy (Hyprland 0.56.2): `hyprctl keyword` is rejected by the Lua config
  parser, and `hl.monitor` with `mode = "preferred"` keeps the old size
  because Hyprland doesn't re-read the mode list; the exact size works. The
  udev hook now reads the size from `/sys/class/drm` and uses `eval` (plus
  `keyword` for older Hyprland, and sway). The hook itself is not yet
  verified end to end on a guest.
