// SPDX-License-Identifier: MIT
// "Fix resize" button + help panel, shown while the guest doesn't follow
// *Resize guest* (pve-h264d sends S_RESIZE_STATE: {ok,w,h,guest,os,qga,vdagent}).
// The steps depend on what the daemon could find out about the guest:
// Windows needs the QEMU guest agent and a logged-in user; Linux desktops
// either follow the virtio-gpu hint (GNOME, KDE) or need a nudge in the guest
// (Hyprland, sway), X11 desktops without that need spice-vdagent.
// Everything is built with textContent: nothing from the daemon is parsed as HTML.

const $ = (id) => document.getElementById(id);

let api = null;   // { vmid, notice(text), focus(), closeOthers() }
let state = { ok: true };
let open = false;

// --- guest-side snippets (Hyprland: re-apply the preferred mode on each hint)
const HYPR_SCRIPT = `#!/bin/sh
# /usr/local/bin/vm-resize: after a virtio-gpu resize hint, make every running
# Hyprland session switch to the new preferred mode (called by udev, as root).
sleep 0.5
for sock in /run/user/*/hypr/*/.socket.sock; do
    [ -S "$sock" ] || continue
    dir=\${sock%/.socket.sock}
    uid=$(stat -c %u "$sock")
    XDG_RUNTIME_DIR=/run/user/$uid HYPRLAND_INSTANCE_SIGNATURE=\${dir##*/} \\
        /usr/bin/hyprctl keyword monitor ,preferred,auto,auto >/dev/null 2>&1
done`;
const HYPR_INSTALL = `sudo tee /usr/local/bin/vm-resize >/dev/null <<'EOF'
${HYPR_SCRIPT}
EOF
sudo chmod 755 /usr/local/bin/vm-resize
echo 'ACTION=="change", SUBSYSTEM=="drm", KERNEL=="card*", RUN+="/usr/local/bin/vm-resize"' | sudo tee /etc/udev/rules.d/99-vm-resize.rules
sudo udevadm control --reload`;

function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
}

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

function section(title, open, ...children) {
    const d = el('details', 'r-sec');
    d.open = open;
    d.append(el('summary', '', title), ...children);
    return d;
}

function p(text) { return el('p', '', text); }

function steps(...items) {
    const ol = el('ol');
    for (const i of items) {
        const li = el('li');
        if (Array.isArray(i)) li.append(...i.map((x) => typeof x === 'string' ? document.createTextNode(x) : x));
        else li.textContent = i;
        ol.append(li);
    }
    return ol;
}

function windowsHelp(s) {
    const vm = api.vmid || '<vmid>';
    return [
        p('Windows changes resolution through the QEMU guest agent, inside the logged-in user\'s session.'),
        steps(
            [s.qga ? 'The QEMU guest agent answers. ' : 'The QEMU guest agent does not answer: install the virtio-win guest tools in Windows (they include the agent) and enable it on the host:',
                ...(s.qga ? [] : [codeBlock(`qm set ${vm} --agent 1`), 'then shut the VM down and start it again (a reboot inside Windows is not enough).'])],
            'Log in to Windows: the resolution can only be changed in a user session, not at the login screen.',
            ['The VM\'s display must be VirtIO-GPU:', codeBlock(`qm set ${vm} --vga virtio`)],
        ),
        p('Details: journalctl -u pve-h264d@' + vm + ' | grep resize (on the Proxmox node).'),
    ];
}

function linuxHelp(s) {
    const vm = api.vmid || '<vmid>';
    const size = `${s.w}x${s.h}`;
    const arch = /arch|omarchy|manjaro|endeavour/.test(s.os || '');
    return [
        p(`The guest was offered ${size}, but its desktop didn't switch to it. GNOME and KDE switch by themselves; Hyprland (Omarchy), sway and other wlroots desktops don't, and spice-vdagent can only change the resolution on X11.`),
        section('Hyprland (Omarchy)', true,
            p('Once, for this size: run in the guest'),
            codeBlock('hyprctl keyword monitor ,preferred,auto,auto'),
            p('Every time, automatically: install a udev hook that does this after each resize (run in a guest terminal):'),
            codeBlock(HYPR_INSTALL),
            p('If ~/.config/hypr/monitors.conf sets a fixed resolution, change it to "monitor=,preferred,auto,auto".'),
        ),
        section('sway', false,
            p('Run in the guest (the virtio-gpu output is usually Virtual-1; swaymsg -t get_outputs lists them):'),
            codeBlock(`swaymsg output Virtual-1 mode ${size}`),
        ),
        section('X11 desktops (XFCE, MATE, Cinnamon, i3, …)', !s.vdagent,
            p(s.vdagent ? 'spice-vdagent is running, but the desktop didn\'t take the size. Try:' : 'spice-vdagent is not running in the guest. Install it, enable it and log out and in again:'),
            ...(s.vdagent ? [codeBlock('xrandr --output Virtual-1 --auto')]
                : [codeBlock(arch ? 'sudo pacman -S spice-vdagent' : 'sudo apt install spice-vdagent    # Fedora: sudo dnf install spice-vdagent')]),
        ),
        section('GNOME / KDE', false,
            p('These follow the resize by themselves. If they don\'t, check that the VM\'s display is VirtIO-GPU (or QXL), then shut the VM down and start it again:'),
            codeBlock(`qm set ${vm} --vga virtio`),
        ),
        p('Or untick Resize guest and set the resolution inside the guest.'),
    ];
}

function render() {
    const body = $('resizebody');
    body.replaceChildren();
    if (state.ok) { body.append(p('The guest follows the window size.')); return; }
    $('resizetitle').textContent = `Guest did not resize to ${state.w}x${state.h}`;
    const os = state.guest === 'windows' ? 'Windows'
        : state.os ? `guest OS: ${state.os}` : 'guest OS unknown (no QEMU guest agent)';
    $('resizeinfo').textContent = `${os} · spice-vdagent: ${state.vdagent ? 'running' : 'not running'}`;
    body.append(...(state.guest === 'windows' ? windowsHelp(state) : linuxHelp(state)));
}

function toggle(force) {
    open = force === undefined ? !open : force;
    const pnl = $('resizepanel'), btn = $('resizehelpbtn');
    btn.classList.toggle('active', open);
    btn.setAttribute('aria-expanded', open);
    pnl.hidden = !open;
    if (!open) { api.focus(); return; }
    api.closeOthers();
    render();
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
    state = s && typeof s === 'object' ? s : { ok: true };
    setFailed(!state.ok);
    if (!state.ok) api.notice(`Guest did not resize to ${state.w}x${state.h}. Click "Fix resize" for how to fix it.`);
    else if (open) render();
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
