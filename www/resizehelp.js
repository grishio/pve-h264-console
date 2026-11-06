// SPDX-License-Identifier: MIT
// "Fix resize" button + help panel, shown while the guest doesn't follow
// *Resize guest* (pve-h264d sends S_RESIZE_STATE: {ok,w,h,guest,os,qga,vdagent}).
//
// The steps follow the VM's Proxmox OS type (`ostype` from the VM config,
// read with the viewer's own permissions when the panel opens; the guest
// agent's answer is the fallback), plus what the config and the daemon know:
// display type, guest agent enabled / answering, spice-vdagent running.
// Linux guests get a desktop picker (remembered per VM), because only the
// desktop decides whether a resize hint is followed.
// Everything is built with textContent: nothing from the API or the daemon is
// parsed as HTML.

const $ = (id) => document.getElementById(id);
const store = {
    get(k, d) { try { const v = localStorage.getItem('pve-h264-' + k); return v === null ? d : v; } catch { return d; } },
    set(k, v) { try { localStorage.setItem('pve-h264-' + k, v); } catch { /* ignore */ } },
};

let api = null;   // { vmid, notice(text), focus(), closeOthers(), vmConfig() }
let state = { ok: true };
let open = false;
let cfg = null;   // VM config (null: not read yet / no permission)

// ---------------------------------------------------------------- guest-side snippets

// udev hook for wlroots-style compositors that keep the mode list from when
// the output appeared (Hyprland, sway): pass the kernel's new preferred mode
// explicitly. Runs as root from udev; the mode string is checked to be WxH
// before it goes into a compositor command.
const HOOK_SCRIPT = `#!/bin/sh
# /usr/local/bin/vm-resize: after a virtio-gpu resize hint (udev, as root),
# switch running Hyprland / sway sessions to the kernel's new preferred mode.
sleep 0.3
for m in /sys/class/drm/card*-Virtual-*/modes; do
    mode=$(head -n1 "$m" 2>/dev/null)
    case "$mode" in *[!0-9x]* | x* | *x | *x*x* | '') continue ;; *x*) ;; *) continue ;; esac
    out=\${m%/modes}; out=\${out##*/}; out=\${out#card*-}
    for sock in /run/user/*/hypr/*/.socket.sock; do
        [ -S "$sock" ] || continue
        dir=\${sock%/.socket.sock}
        export XDG_RUNTIME_DIR=/run/user/$(stat -c %u "$sock") HYPRLAND_INSTANCE_SIGNATURE=\${dir##*/}
        # Hyprland 0.55+ (Lua config) takes eval, older ones keyword: the
        # other one is a no-op, and neither reports that in its exit code
        /usr/bin/hyprctl eval "hl.monitor({ output = \\"$out\\", mode = \\"$mode\\", position = \\"auto\\", scale = \\"auto\\" })" >/dev/null 2>&1
        /usr/bin/hyprctl keyword monitor "$out,$mode,auto,auto" >/dev/null 2>&1
    done
    for sock in /run/user/*/sway-ipc.*.sock; do
        [ -S "$sock" ] && SWAYSOCK=$sock /usr/bin/swaymsg output "$out" mode "$mode" >/dev/null 2>&1
    done
done`;
const HOOK_INSTALL = `sudo tee /usr/local/bin/vm-resize >/dev/null <<'EOF'
${HOOK_SCRIPT}
EOF
sudo chmod 755 /usr/local/bin/vm-resize
echo 'ACTION=="change", SUBSYSTEM=="drm", KERNEL=="card*", RUN+="/usr/local/bin/vm-resize"' | sudo tee /etc/udev/rules.d/99-vm-resize.rules
sudo udevadm control --reload`;

// spice-vdagent install per distribution (guest agent's os id)
function pkgCmd(os) {
    if (/^(arch|manjaro|endeavouros|cachyos|garuda|omarchy)/.test(os)) return 'sudo pacman -S --needed spice-vdagent';
    if (/^(fedora|rhel|centos|rocky|almalinux|ol)/.test(os)) return 'sudo dnf install spice-vdagent';
    if (/^(opensuse|sles|sled)/.test(os)) return 'sudo zypper install spice-vdagent';
    if (/^(debian|ubuntu|linuxmint|pop|elementary|zorin|kali|raspbian|neon)/.test(os)) return 'sudo apt install spice-vdagent';
    return 'sudo apt install spice-vdagent      # Debian/Ubuntu/Mint\nsudo dnf install spice-vdagent      # Fedora\nsudo pacman -S spice-vdagent        # Arch\nsudo zypper install spice-vdagent   # openSUSE';
}

// ---------------------------------------------------------------- DOM helpers

function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
}
function p(...parts) {
    const e = el('p');
    for (const x of parts) e.append(typeof x === 'string' ? document.createTextNode(x) : x);
    return e;
}
function code(text) { return el('code', '', text); }

function codeBlock(text) {
    const box = el('div', 'r-code');
    const pre = el('pre', '', text);
    const btn = el('button', 'r-copy', 'Copy');
    btn.title = 'Copy to your clipboard';
    btn.onclick = async () => {
        try { await navigator.clipboard.writeText(text); api.notice('Copied to your clipboard'); }
        catch {
            const r = document.createRange(); r.selectNodeContents(pre);
            const s = getSelection(); s.removeAllRanges(); s.addRange(r);
            api.notice('Copy blocked by the browser: the text is selected, press Ctrl+C');
        }
    };
    box.append(pre, btn);
    return box;
}

// a checklist step: ok = true (done) / false (the problem) / null (can't tell)
function step(ok, title, ...body) {
    const li = el('li', 'r-step ' + (ok === true ? 'ok' : ok === false ? 'bad' : 'unk'));
    const mark = el('span', 'r-mark', ok === true ? '✓' : ok === false ? '✗' : '?');
    mark.title = ok === true ? 'OK' : ok === false ? 'This needs fixing' : "Can't be checked from the host";
    const head = el('div', 'r-step-head');
    head.append(mark, el('strong', '', title));
    li.append(head, ...body);
    return li;
}
function checklist(...steps) { const ol = el('ol', 'r-steps'); ol.append(...steps); return ol; }

// ---------------------------------------------------------------- what we know

const WIN_NAMES = { wxp: 'Windows XP', w2k: 'Windows 2000', w2k3: 'Windows 2003', w2k8: 'Windows 2008',
    wvista: 'Windows Vista', win7: 'Windows 7/2008r2', win8: 'Windows 8/2012/2012r2',
    win10: 'Windows 10/2016/2019', win11: 'Windows 11/2022/2025' };

function guestKind() {
    const t = cfg?.ostype;
    if (t) return /^w/.test(t) ? 'windows' : /^l2/.test(t) ? 'linux' : 'other';
    if (state.guest === 'windows') return 'windows';
    if (state.os) return 'linux'; // the agent answered and it isn't Windows
    return 'other';
}
function osTypeLabel() {
    const t = cfg?.ostype;
    if (!t) return cfg ? 'not set' : null;
    return WIN_NAMES[t] || (t === 'l26' ? 'Linux 2.6 – 7.x kernel' : t === 'l24' ? 'Linux 2.4' : t === 'solaris' ? 'Solaris' : 'Other');
}
// display type from the config: "virtio", "qxl", "std", ... (default std)
function vgaType() {
    if (!cfg) return null;
    return (cfg.vga || 'std').split(',')[0].replace(/^type=/, '');
}
function agentEnabled() {
    if (!cfg) return null;
    const a = String(cfg.agent ?? '0').split(',');
    return a.some((x) => x === '1' || x === 'enabled=1' || x === 'enabled=yes');
}
// Windows' driver folder on the virtio-win ISO
function winDriverDir() {
    const t = cfg?.ostype;
    return t === 'win11' ? 'viogpudo\\w11\\amd64' : t === 'win10' ? 'viogpudo\\w10\\amd64' : 'viogpudo\\<Windows version>\\amd64';
}

// ---------------------------------------------------------------- content

function displayStep(okTypes) {
    const vga = vgaType();
    const vm = api.vmid;
    return step(vga === null ? null : okTypes.includes(vga),
        `Display: ${okTypes.includes('qxl') ? 'VirtIO-GPU (or SPICE/QXL)' : 'VirtIO-GPU'}`,
        p(vga === null ? "Couldn't read the VM config (needs VM.Audit). Check Hardware → Display."
            : okTypes.includes(vga) ? `Hardware → Display is ${vga}.`
            : `Hardware → Display is ${vga}: that display can't be resized from the host. Set it to VirtIO-GPU, then shut the VM down and start it again (a reboot inside the guest isn't enough):`),
        ...(vga !== null && !okTypes.includes(vga) ? [codeBlock(`qm set ${vm} --vga virtio`)] : []));
}

function windowsHelp() {
    const vm = api.vmid;
    const en = agentEnabled();
    const answering = !!state.qga;
    return [
        p('Windows changes the resolution through the QEMU guest agent, in the logged-in user\'s session. Work through these in order:'),
        checklist(
            displayStep(['virtio']),
            step(null, 'VirtIO GPU driver installed in Windows',
                p('Open ', el('strong', '', 'Device Manager'), ' → ', el('strong', '', 'Display adapters'), '. It must show ',
                    el('strong', '', 'Red Hat VirtIO GPU DOD controller'), '. If it shows ', el('strong', '', 'Microsoft Basic Display Adapter'),
                    ', the driver is missing: attach the virtio-win ISO, right-click the adapter → Update driver → Browse my computer → pick the CD\'s ',
                    code(winDriverDir()), ' folder. Or run ', code('virtio-win-guest-tools.exe'), ' from the ISO, which installs it together with the guest agent.'),
                p('Device Manager, from Run (Win+R):'), codeBlock('devmgmt.msc')),
            step(en, 'QEMU guest agent enabled in Proxmox',
                ...(en === false
                    ? [p('Options → QEMU Guest Agent is off. Turn it on, then shut the VM down and start it again:'), codeBlock(`qm set ${vm} --agent 1`)]
                    : [p(en ? 'Options → QEMU Guest Agent is on.' : 'Check Options → QEMU Guest Agent (enabled).')])),
            step(answering, 'QEMU guest agent running in Windows',
                p(answering ? 'The guest agent answers.'
                    : 'The agent doesn\'t answer. Install it from the virtio-win ISO (virtio-win-guest-tools.exe, or guest-agent\\qemu-ga-x86_64.msi) and check that the service runs. In an administrator PowerShell:'),
                ...(answering ? [] : [codeBlock('Get-Service QEMU-GA')])),
            step(null, 'A user is logged in',
                p('The resolution can only be changed in a user session, not at the login screen or the lock screen.')),
        ),
        p(`Still nothing? On the Proxmox node: `, code(`journalctl -u pve-h264d@${vm} | grep -i resize`), ' — the "guest-exec resize:" lines say why.'),
    ];
}

const DESKTOPS = [
    ['gnome', 'GNOME'], ['kde', 'KDE Plasma'], ['hypr', 'Hyprland / Omarchy'], ['sway', 'sway'],
    ['x11', 'XFCE, MATE, Cinnamon, LXQt, i3 (X11)'], ['other', 'Other / not sure'],
];

function hookSection() {
    const d = el('details', 'r-sec');
    d.append(el('summary', '', 'Follow every resize automatically (udev hook)'),
        p('Installs /usr/local/bin/vm-resize and a udev rule that runs it after each resize: it reads the new size from /sys/class/drm and passes it to every running Hyprland and sway session. Run in a guest terminal (asks for your sudo password):'),
        codeBlock(HOOK_INSTALL),
        p('Remove it again with: ', code('sudo rm /usr/local/bin/vm-resize /etc/udev/rules.d/99-vm-resize.rules')));
    return d;
}

function desktopHelp(de) {
    const size = `${state.w}x${state.h}`;
    const os = state.os || '';
    const vd = state.vdagent;
    const vdStep = (why) => step(vd, 'spice-vdagent running in the guest',
        p(vd ? 'It runs.' : `It doesn't run. ${why} Install it and log out and in again (it starts with the session):`),
        ...(vd ? [] : [codeBlock(pkgCmd(os))]));
    switch (de) {
    case 'gnome': return [
        p('GNOME follows the resize on its own, on Wayland and on X11.'),
        checklist(
            step(null, 'Use the VirtIO-GPU output', p('Settings → Displays should list one display, "Virtual-1". If you picked a fixed resolution there, GNOME keeps it: pick the top (preferred) resolution again.')),
            vdStep('On an X11 session ("GNOME on Xorg") GNOME needs it.'),
        ),
    ];
    case 'kde': return [
        p('Plasma follows the resize on Wayland. On X11 it needs spice-vdagent.'),
        checklist(
            step(null, 'No fixed resolution saved', p('Plasma restores a resolution you set in System Settings → Display & Monitor. Reset the saved layouts, then log out and in again:'),
                codeBlock('rm -rf ~/.local/share/kscreen')),
            vdStep('An X11 session needs it.'),
        ),
    ];
    case 'hypr': return [
        p('Hyprland keeps the list of display modes it read at login, so "preferred" stays the old size. Pass it the new size explicitly. Run as your desktop user (not root or sudo), in a terminal inside the guest:'),
        codeBlock(`hyprctl eval 'hl.monitor({ output = "Virtual-1", mode = "${size}", position = "auto", scale = "auto" })'`),
        p('Hyprland before 0.55 (hyprlang config):'),
        codeBlock(`hyprctl keyword monitor Virtual-1,${size},auto,auto`),
        hookSection(),
    ];
    case 'sway': return [
        p("sway doesn't follow a resize on its own. Run in a terminal inside the guest:"),
        codeBlock(`swaymsg output Virtual-1 mode ${size}`),
        hookSection(),
    ];
    case 'x11': return [
        p('X11 desktops change the resolution through spice-vdagent, which the console asks for the new size.'),
        checklist(
            vdStep('This is what resizes X11 desktops.'),
            step(null, 'The system service runs too', p('spice-vdagent has a system part that must run as well:'), codeBlock('sudo systemctl enable --now spice-vdagentd')),
        ),
        p('By hand, for one size:'), codeBlock(`xrandr --output Virtual-1 --auto`),
    ];
    default: return [
        p('Whether a resize is followed depends on the desktop: GNOME and KDE Plasma follow it, Hyprland and sway need the udev hook (pick them above), X11 desktops need spice-vdagent.'),
        p('You can always untick Resize guest and set the resolution inside the guest instead.'),
    ];
    }
}

function linuxHelp() {
    const key = `resize-de-${api.vmid}`;
    let de = store.get(key, '');
    if (!DESKTOPS.some(([k]) => k === de)) de = '';
    const wrap = el('div');
    const chips = el('div', 'r-chips');
    const body = el('div');
    const pick = (k) => {
        de = k; store.set(key, k);
        for (const c of chips.children) c.classList.toggle('active', c.dataset.de === k);
        body.replaceChildren(...(k ? desktopHelp(k) : [p('Pick the guest\'s desktop above for the steps.')]));
    };
    for (const [k, label] of DESKTOPS) {
        const b = el('button', 'r-chip', label);
        b.dataset.de = k;
        b.onclick = () => pick(k);
        chips.append(b);
    }
    wrap.append(
        checklist(displayStep(['virtio', 'qxl'])),
        p(el('strong', '', 'Which desktop runs in the guest?')),
        chips, body);
    pick(de);
    return [wrap];
}

function otherHelp() {
    return [
        p(cfg ? "The VM's OS type isn't Windows or Linux, so there are no specific steps."
            : "Couldn't read the VM's OS type (no VM.Audit permission, and the QEMU guest agent doesn't answer)."),
        checklist(displayStep(['virtio', 'qxl'])),
        p('A guest resizes when its display driver follows the VirtIO-GPU size hint, or its spice agent sets the size. Otherwise untick Resize guest and set the resolution inside the guest.'),
        ...(!cfg ? [p('If this is a Windows or Linux guest, set Options → OS Type in Proxmox, or install the QEMU guest agent, to get the steps for it.')] : []),
    ];
}

function render() {
    const body = $('resizebody');
    if (state.ok) { body.replaceChildren(p('The guest follows the window size.')); return; }
    $('resizetitle').textContent = `Guest did not resize to ${state.w}x${state.h}`;
    const kind = guestKind();
    const info = [osTypeLabel() ? `OS type: ${osTypeLabel()}` : state.os ? `guest: ${state.os}` : 'OS type unknown',
        `guest agent: ${state.qga ? 'answering' : 'not answering'}`];
    if (kind !== 'windows') info.push(`spice-vdagent: ${state.vdagent ? 'running' : 'not running'}`);
    $('resizeinfo').textContent = info.join(' · ');
    body.replaceChildren(...(kind === 'windows' ? windowsHelp() : kind === 'linux' ? linuxHelp() : otherHelp()));
}

async function loadConfig() {
    try { cfg = await api.vmConfig(); }
    catch { cfg = null; } // no VM.Audit: fall back to what the daemon knows
}

async function toggle(force) {
    open = force === undefined ? !open : force;
    const pnl = $('resizepanel'), btn = $('resizehelpbtn');
    btn.classList.toggle('active', open);
    btn.setAttribute('aria-expanded', open);
    pnl.hidden = !open;
    if (!open) { api.focus(); return; }
    api.closeOthers();
    render();
    place();
    await loadConfig(); // fresh each time: the user may just have changed it
    if (open) { render(); place(); }
}
function place() {
    const pnl = $('resizepanel'), btn = $('resizehelpbtn');
    const wrapR = $('wrap').getBoundingClientRect(), r = btn.getBoundingClientRect();
    pnl.style.left = Math.max(6, Math.min(r.left - wrapR.left, wrapR.width - pnl.offsetWidth - 6)) + 'px';
}

function setFailed(failed) {
    $('resizehelpbtn').hidden = !failed;
    if (!failed && open) toggle(false);
}

export function close() { if (open) toggle(false); }
export function isOpen() { return open; }

// S_RESIZE_STATE from the daemon
export function onState(s) {
    const prev = state;
    state = s && typeof s === 'object' ? s : { ok: true };
    setFailed(!state.ok);
    // the daemon reports every failed request: say it once per size while
    // the button is up, not again for each retry of the same size
    if (!state.ok && (prev.ok || prev.w !== state.w || prev.h !== state.h))
        api.notice(`Guest did not resize to ${state.w}x${state.h}. Click "Fix resize" for how to fix it.`);
    if (open) render();
}

// the guest's display size changed (S_VIDEO_CONFIG): it may have followed
// after all, e.g. after the user ran the fix in the guest
export function onGuestSize(w, h) {
    if (!state.ok && w === state.w && h === state.h) { state = { ok: true }; setFailed(false); }
}

// Resize guest unticked or the connection dropped: nothing to fix right now
export function reset() { state = { ok: true }; setFailed(false); }

export function init(a) {
    api = a;
    $('resizehelpbtn').addEventListener('click', () => toggle());
    $('resizeclose').onclick = () => toggle(false);
    document.addEventListener('pointerdown', (ev) => {
        if (open && !$('resizepanel').contains(ev.target) && !$('resizehelpbtn').contains(ev.target)) toggle(false);
    }, true);
    document.addEventListener('keydown', (ev) => {
        if (open && ev.code === 'Escape') { toggle(false); ev.stopPropagation(); ev.preventDefault(); }
    }, true);
}
