# PVE H.264 Console

**The Proxmox VE console, the way it should have been all along.**

If you've ever dragged a window around in a VM through noVNC and watched it smear,
stutter and lag behind your mouse, this project is for you. PVE H.264 Console streams
your VM's screen as hardware-encoded H.264 video, straight into the Proxmox web UI you
already use. You get smooth, sharp **1080p at 60 fps** with roughly **20–30 ms** of
latency, plus sound and a shared clipboard.

No client software. No extra ports. No QEMU fork. It's one more entry in the
**Console ▾** menu, right next to noVNC.

---

## ✨ What you get

- 🚀 **Fluid 1080p60.** Encoded on your host's GPU (NVIDIA NVENC or Intel/AMD VA-API, with
  CPU x264 as a fallback) and decoded on the browser's GPU via WebCodecs.
- ⚡ **Built for low latency.** No B-frames, no lookahead, no buffering, and frames drawn the
  moment they arrive. The bitrate adapts on its own, so a static screen costs almost nothing.
- 🔊 **Guest audio with optional lip sync.**
- 📋 **Clipboard sharing** with the guest: paste in, copy out.
- 🖥️ **The guest follows your window size,** including on Windows guests.
- 🔐 **Same login, same permissions.** It uses Proxmox's own authentication and the
  `VM.Console` privilege, over the same HTTPS port (8006).
- 🧩 **Plays nicely with Proxmox.** noVNC keeps working as a fallback, the integration
  re-applies itself after Proxmox updates, and one command uninstalls it cleanly.

## 📋 What you need

- **Proxmox VE 9.x**
- **An encoder:** an NVIDIA GPU with NVENC, or an Intel/AMD GPU with VA-API. A plain CPU
  works too, just less efficiently.
- **A modern browser** with WebCodecs: Chrome/Edge 94+, Safari 16.4+ or Firefox 130+
- **A LAN or VPN** connection to your Proxmox host
- **In a cluster:** install it on every node, the same version on each. You can then open any
  VM's console from whichever node's web UI you're logged in to
- **Recommended VM settings:** `vga: virtio` for 60 fps, and an audio device if you want sound

> Using an Intel iGPU? Proxmox doesn't ship the encoding driver by default. See
> [Intel iGPU (VA-API) setup](documentation.md#intel-igpu-va-api-setup) first.

## 🛠️ Getting started

**1. Clone and install** on your Proxmox host, as root:

```bash
git clone https://github.com/grishio/pve-h264-console.git
```

```bash
cd pve-h264-console && ./install.sh
```

The installer builds the daemon, installs the viewer, hooks it into Proxmox and restarts
the Proxmox web services. Running VMs aren't touched.

**2. Fully stop and start each VM** you want to use it with. A reboot from inside the
guest isn't enough, because QEMU's command line has to change.

**3. Reload the Proxmox web UI** in your browser.

**4. Open the console:** **VM → Console ▾ → H.264**. Click **Default** in the viewer's
toolbar if you want it to be your usual console.

To check that everything is hooked in:

```bash
pve-h264-patch status
```

To remove it completely and restore the stock Proxmox files:

```bash
./uninstall.sh
```

## 📚 Documentation

Everything else lives in [**documentation.md**](documentation.md):

- [Requirements in detail and Intel iGPU setup](documentation.md#6-install--update--uninstall)
- [Viewer controls](documentation.md#7-using-it) and [recommended VM settings](documentation.md#recommended-vm-settings)
- [Configuration options](documentation.md#8-configuration-etcpve-h264h264dconf)
- [How it works](documentation.md#3-architecture), [security model](documentation.md#security-model) and [performance numbers](documentation.md#10-performance-and-test-results)
- [Updates and uninstalling](documentation.md#updates)
- [Known limitations](documentation.md#11-known-limitations--todo) and [troubleshooting](documentation.md#12-troubleshooting)

## 🤖 How this was built

In the spirit of full transparency: **most of the code in this project, and most of its
documentation, was written by [Claude](https://www.anthropic.com/claude), Anthropic's AI
model, using Claude Code.**

The project was guided by me, [David Grishko](https://github.com/grishio). I set the goals,
made the design calls, tested it on real Proxmox hosts and guests, reported what broke,
and decided what shipped. Claude did most of the writing: the daemon, the viewer, the
Proxmox integration, the tests and these docs.

If you're reviewing or contributing, please keep that in mind and read the code with the
same care you'd give any other project you didn't write yourself.

## ⚠️ Disclaimer: use at your own risk

This is an independent, community project. It is **not affiliated with or endorsed by
Proxmox Server Solutions GmbH**.

To work, it **modifies files that belong to Proxmox VE packages** and changes how your VMs
are started. It has been tested carefully, and it's designed to be removable, but it is
provided **as is, without warranty of any kind**. The authors and contributors **assume no
responsibility or liability** for any damage, data loss, downtime, security issues or other
problems that result from installing or using it.

By using this project, you accept that you do so **entirely at your own risk**. Please try
it on a non-critical host first, and keep backups of anything you care about.

## 📄 License

Released under the [MIT License](LICENSE). Use it, change it, share it, sell it, build it
into something bigger: do whatever you like, just keep the copyright notice. Like the
disclaimer above, the license says the software comes with **no warranty** and the authors
are **not liable** for anything that happens when you use it.

Proxmox VE itself is licensed under the AGPL-3.0, and the libraries this project uses
(FFmpeg, GLib, Opus and others) have their own licenses. Those still apply to them.
