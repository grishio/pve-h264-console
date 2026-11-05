// SPDX-License-Identifier: MIT
// Stream settings panel: encoder / rate control / smoothness (shared by all
// viewers of the VM, applied by pve-h264d) and display options (this browser).
//
// Only settings the user changed are remembered (localStorage) and pushed to
// the daemon on connect; everything else follows the daemon's config file.

const LS_KEY = 'pve-h264-enc';
const mbit = (k) => (k / 1000).toFixed(k % 1000 ? 1 : 0) + ' Mbit/s';

// Profiles: tested at 1080p60 on NVENC (mid-range NVIDIA GPU) - all hold 60 fps with
// 5-10 ms encode time. "Smooth video" uses ~20 % fewer bits than the
// defaults for the same content (better preset, AQ, quarter-res 2nd pass).
export const PROFILES = {
    lowbw: { label: 'Low bitrate', title: 'Slow links: 30 fps, ≤ 6 Mbit/s, best compression',
        s: { rc: 'vbr', quality: 28, max_kbps: 6000, min_kbps: 1000, abr: 1, preset: 'p6', tune: 'll',
            multipass: 'qres', spatial_aq: 1, aq_strength: 8, temporal_aq: 0, vbv_frames: 4, fps: 30,
            max_inflight: 0, refine_frames: 4 } },
    balanced: { label: 'Desktop', title: 'Lowest latency, sharp text (the defaults)',
        s: { rc: 'vbr', quality: 24, max_kbps: 25000, min_kbps: 1500, abr: 1, preset: 'p4', tune: 'ull',
            multipass: 'disabled', spatial_aq: 0, aq_strength: 8, temporal_aq: 0, vbv_frames: 6, fps: 60,
            max_inflight: 0, refine_frames: 4 } },
    video: { label: 'Video', title: '60 fps video / games, bitrate-efficient (~20–30 Mbit/s at 1080p)',
        s: { rc: 'vbr', quality: 26, max_kbps: 30000, min_kbps: 4000, abr: 1, preset: 'p5', tune: 'll',
            multipass: 'qres', spatial_aq: 1, aq_strength: 8, temporal_aq: 1, vbv_frames: 4, fps: 60,
            max_inflight: 0, refine_frames: 2 } },
    quality: { label: 'Max quality', title: 'Fast LAN: near-lossless look, 40–80 Mbit/s',
        s: { rc: 'vbr', quality: 19, max_kbps: 80000, min_kbps: 5000, abr: 1, preset: 'p6', tune: 'hq',
            multipass: 'fullres', spatial_aq: 1, aq_strength: 8, temporal_aq: 1, vbv_frames: 8, fps: 60,
            max_inflight: 0, refine_frames: 6 } },
};

const KBPS_STEPS = [1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000, 20000, 25000,
    30000, 40000, 50000, 60000, 80000, 100000, 150000, 200000];
const MIN_STEPS = [500, 1000, 1500, 2000, 3000, 4000, 5000, 8000, 10000, 15000, 20000];
const FPS_STEPS = [24, 30, 50, 60, 75, 90, 120, 144];
const NVENC = ['nvenc'], NV_X264 = ['nvenc', 'x264'];

// Encoder controls. only: backend families that use the setting.
const FIELDS = [
    { section: 'Rate control' },
    { key: 'rc', label: 'Mode', type: 'select', opts: [['vbr', 'Quality-targeted VBR'], ['cbr', 'Constant bitrate'],
        ['cqp', 'Constant QP']],
    help: 'VBR spends bits only where the picture needs them (most efficient). CBR sends a steady stream at the max bitrate. Constant QP ignores bitrate entirely.' },
    { key: 'quality', label: 'Quality', type: 'range', min: 10, max: 40,
        fmt: (v) => `${v} ${v <= 19 ? '· very high' : v <= 22 ? '· high' : v <= 25 ? '· normal' : v <= 29 ? '· lower' : '· low'}`,
        help: 'cq / crf / qp target. Lower = better picture, more bits. 23–27 suits video; ≤ 21 for crisp text.',
        when: (s) => s.rc !== 'cbr' },
    { key: 'max_kbps', label: 'Max bitrate', type: 'steps', steps: KBPS_STEPS, fmt: mbit,
        help: 'Ceiling for VBR, the rate for CBR. A 1080p60 video needs about 15–30 Mbit/s.', when: (s) => s.rc !== 'cqp' },
    { key: 'abr', label: 'Adaptive', type: 'check', text: 'lower the ceiling when the network queues',
        help: 'Cuts the ceiling once, to what the link carried, when frames start queueing; probes back up slowly. Every change costs a keyframe on NVENC, so turn it off on a LAN for a fixed ceiling.',
        when: (s) => s.rc !== 'cqp' },
    { key: 'min_kbps', label: 'Adaptive floor', type: 'steps', steps: MIN_STEPS, fmt: mbit,
        help: 'The adaptive ceiling never goes below this.', when: (s) => s.rc !== 'cqp' && s.abr },
    { key: 'vbv_frames', label: 'VBV buffer', type: 'range', min: 1, max: 30, fmt: (v) => `${v} frame${v > 1 ? 's' : ''}`,
        help: 'How far one frame may exceed the average. Small = even frame sizes (smooth on slow links); large = steadier quality on scene changes.',
        when: (s) => s.rc !== 'cqp' },
    { section: 'Encoder' },
    { key: 'preset', label: 'Preset', type: 'select', only: NV_X264,
        opts: [['p1', 'p1 · fastest'], ['p2', 'p2'], ['p3', 'p3'], ['p4', 'p4 · balanced'], ['p5', 'p5'], ['p6', 'p6'], ['p7', 'p7 · best compression']],
        help: 'Higher = better picture per bit, more GPU time per frame (1080p on a mid-range NVIDIA GPU: p2 ≈ 3.5 ms, p4 ≈ 5 ms, p6 ≈ 8 ms). x264 maps p1/p2 → ultrafast … p7 → medium.' },
    { key: 'tune', label: 'Tuning', type: 'select', only: NVENC,
        opts: [['ull', 'Ultra-low latency'], ['ll', 'Low latency'], ['hq', 'High quality']],
        help: 'All three stay at zero frame delay here (no B-frames, no lookahead). ll / hq make better rate-control decisions for video.' },
    { key: 'multipass', label: 'Two-pass', type: 'select', only: NVENC,
        opts: [['disabled', 'Off'], ['qres', 'Quarter resolution'], ['fullres', 'Full resolution']],
        help: 'A first pass over each frame guides bit allocation: better quality per bit, ~1–3 ms more encode time, no extra latency in frames.' },
    { key: 'spatial_aq', label: 'Spatial AQ', type: 'check', text: 'more bits for flat areas', only: NV_X264,
        help: 'Adaptive quantization: fewer banding / blocking artifacts in skies, gradients and dark scenes. Usually a win for video.' },
    { key: 'aq_strength', label: 'AQ strength', type: 'range', min: 1, max: 15, only: NV_X264, when: (s) => s.spatial_aq,
        help: '1 = gentle … 15 = aggressive (default 8).' },
    { key: 'temporal_aq', label: 'Temporal AQ', type: 'check', text: 'more bits for static regions', only: NVENC,
        help: 'Spends bits on parts of the picture that stay still, so they sharpen over time. Good for video with static overlays.' },
    { section: 'Smoothness' },
    { key: 'fps', label: 'Frame rate', type: 'steps', steps: FPS_STEPS, fmt: (v) => `${v} fps`,
        help: 'Ceiling; frames are only encoded when the screen changes.' },
    { key: 'max_inflight', label: 'Frames in flight', type: 'steps', steps: [0, 1, 2, 3, 4, 5, 6, 8],
        fmt: (v) => v ? String(v) : 'auto',
        help: 'Frames sent before the viewer confirms decoding. auto covers the measured round trip (2 on a LAN, 4–6 over WAN/VPN); a fixed 2 caps 60 fps at a 33 ms round trip.' },
    { key: 'refine_frames', label: 'Static refinement', type: 'range', min: 0, max: 10,
        fmt: (v) => v ? `${v} frames` : 'off',
        help: 'Extra frames after the screen stops changing, to sharpen it.' },
];

// Display: local to this browser, applied immediately.
const DPI_OPTS = [['0.5', '0.5× · large text, fewest pixels'], ['0.75', '0.75×'], ['css', '1× · CSS pixels (default)'],
    ['1.25', '1.25×'], ['1.5', '1.5×'], ['2', '2×'], ['native', 'Native device pixels']];

const $ = (id) => document.getElementById(id);
const store = {
    get(k, d) { try { const v = localStorage.getItem('pve-h264-' + k); return v === null ? d : v; } catch { return d; } },
    set(k, v) { try { localStorage.setItem('pve-h264-' + k, v); } catch { /* ignore */ } },
};

let srv = null;          // effective settings from the daemon (S_SETTINGS)
let ov = loadOverrides(); // what this browser changed
let sendFn = () => {};
let displayFn = () => {};
let audio = null;        // www/audio.js
let open = false;
const els = {};          // key -> { row, input, val }

function loadOverrides() {
    let o = {};
    try { o = JSON.parse(store.get('enc', '{}')) || {}; } catch { o = {}; }
    // migrate the old toolbar selects (bitrate / fps / quality)
    for (const [old, key] of [['bitrate', 'max_kbps'], ['fps', 'fps'], ['quality', 'quality']]) {
        const v = store.get(old, null);
        if (v !== null && o[key] === undefined) o[key] = parseInt(v, 10);
        try { localStorage.removeItem('pve-h264-' + old); } catch { /* ignore */ }
    }
    return o;
}
function saveOverrides() { store.set('enc', JSON.stringify(ov)); }

export function current() { return { server: srv, overrides: ov, dpi: dpiMode(), factor: dpiFactor() }; }
export function overrides() { return Object.keys(ov).length ? ov : null; }

export function dpiFactor() {
    const v = store.get('dpi', 'css');
    if (v === 'native') return window.devicePixelRatio || 1;
    if (v === 'css') return 1;
    const f = parseFloat(v);
    return f > 0 ? f : 1;
}
export function dpiMode() { return store.get('dpi', 'css'); }
export function smoothing() { return store.get('smooth', 'auto'); }

function change(key, value) {
    ov[key] = value;
    saveOverrides();
    sendFn({ [key]: value });
    markProfile();
}

function el(tag, props = {}, ...kids) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(props)) {
        if (k === 'class') e.className = v; else if (k in e) e[k] = v; else e.setAttribute(k, v);
    }
    for (const k of kids) if (k != null) e.append(k);
    return e;
}

function build() {
    const panel = $('panel');
    panel.replaceChildren();
    panel.append(el('div', { class: 'p-head' },
        el('strong', {}, 'Stream settings'),
        el('span', { class: 'p-enc', id: 'p-enc' }),
        el('button', { class: 'p-close', title: 'Close', onclick: () => toggle(false) }, '×')));

    const prof = el('div', { class: 'p-profiles' });
    for (const [id, p] of Object.entries(PROFILES)) {
        prof.append(el('button', { title: p.title, 'data-profile': id, onclick: () => applyProfile(id) }, p.label));
    }
    panel.append(prof);
    panel.append(el('div', { class: 'p-note' }, 'Encoder settings are shared by everyone watching this VM and apply within a frame or two (with a keyframe).'));

    let grid = null;
    for (const f of FIELDS) {
        if (f.section) {
            panel.append(el('div', { class: 'p-sec' }, f.section));
            grid = el('div', { class: 'p-grid' });
            panel.append(grid);
            continue;
        }
        const id = 'set-' + f.key;
        let input, val = null;
        if (f.type === 'select') {
            input = el('select', { id });
            for (const [v, t] of f.opts) input.add(new Option(t, v));
            input.addEventListener('change', () => change(f.key, input.value));
        } else if (f.type === 'check') {
            input = el('input', { id, type: 'checkbox' });
            input.addEventListener('change', () => change(f.key, input.checked ? 1 : 0));
        } else {
            const steps = f.type === 'steps';
            input = el('input', { id, type: 'range', min: steps ? 0 : f.min, max: steps ? f.steps.length - 1 : f.max, step: 1 });
            val = el('output', { class: 'p-val', htmlFor: id });
            const value = () => steps ? f.steps[+input.value] : +input.value;
            input.addEventListener('input', () => { val.textContent = f.fmt ? f.fmt(value()) : String(value()); });
            input.addEventListener('change', () => change(f.key, value()));
        }
        const label = el('label', { htmlFor: id, title: f.help || '' }, f.label);
        const ctl = el('div', { class: 'p-ctl' }, input, val,
            f.type === 'check' ? el('label', { htmlFor: id, class: 'p-text' }, f.text) : null);
        const help = el('div', { class: 'p-help' }, f.help || '');
        const row = el('div', { class: 'p-row' }, label, ctl, help);
        grid.append(row);
        els[f.key] = { f, row, input, val };
    }

    // display (local)
    panel.append(el('div', { class: 'p-sec' }, 'Display (this browser)'));
    const dg = el('div', { class: 'p-grid' });
    const dpi = el('select', { id: 'set-dpi' });
    for (const [v, t] of DPI_OPTS) dpi.add(new Option(t, v));
    dpi.value = dpiMode();
    if (dpi.selectedIndex < 0) dpi.value = 'css';
    const dpiInfo = el('span', { class: 'p-val', id: 'dpi-info' });
    dpi.addEventListener('change', () => { store.set('dpi', dpi.value); displayFn('dpi'); refreshDpiInfo(); });
    dg.append(el('div', { class: 'p-row' },
        el('label', { htmlFor: 'set-dpi' }, 'Guest resolution'),
        el('div', { class: 'p-ctl' }, dpi, dpiInfo),
        el('div', { class: 'p-help' }, 'Used by "Resize guest" (turned on when you change this). Below 1× renders fewer pixels: bigger text, fewer bits. Native matches your screen pixel-for-pixel on HiDPI displays; set the guest\'s own scaling (e.g. Windows 150–200 %) to keep text readable.')));
    const sm = el('select', { id: 'set-smooth' });
    sm.add(new Option('Smooth', 'auto')); sm.add(new Option('Pixelated (sharp edges)', 'pixelated'));
    sm.value = smoothing();
    sm.addEventListener('change', () => { store.set('smooth', sm.value); displayFn('smooth'); });
    dg.append(el('div', { class: 'p-row' },
        el('label', { htmlFor: 'set-smooth' }, 'Scaling filter'),
        el('div', { class: 'p-ctl' }, sm),
        el('div', { class: 'p-help' }, 'How the picture is filtered when it is scaled to the window.')));
    panel.append(dg);

    if (audio) {
        panel.append(el('div', { class: 'p-sec' }, 'Sound (this browser)'));
        const ag = el('div', { class: 'p-grid' });
        const ls = el('select', { id: 'set-lipsync' });
        ls.add(new Option('Off · lowest latency', '0'));
        ls.add(new Option('On · hold video for its audio', '1'));
        ls.value = audio.getLipSync() ? '1' : '0';
        ls.addEventListener('change', () => audio.setLipSync(ls.value === '1'));
        ag.append(el('div', { class: 'p-row' },
            el('label', { htmlFor: 'set-lipsync' }, 'Lip sync'),
            el('div', { class: 'p-ctl' }, ls),
            el('div', { class: 'p-help' }, 'Audio and video carry the same host timestamps. Off: every frame is drawn the moment it decodes and audio keeps the smallest buffer that doesn\'t stutter, usually 20–50 ms behind the picture (the stats show the offset). On: frames wait until their sound is heard, for videos and calls; the picture then lags by that much too.')));
        const vol = el('input', { id: 'set-volume', type: 'range', min: 0, max: 100, step: 1 });
        const vv = el('output', { class: 'p-val', htmlFor: 'set-volume' });
        vol.value = Math.round(audio.getVolume() * 100);
        vv.textContent = vol.value + ' %';
        vol.addEventListener('input', () => { vv.textContent = vol.value + ' %'; audio.setVolume(vol.value / 100); });
        ag.append(el('div', { class: 'p-row' },
            el('label', { htmlFor: 'set-volume' }, 'Volume'),
            el('div', { class: 'p-ctl' }, vol, vv),
            el('div', { class: 'p-help' }, 'Playback volume in this browser; the guest\'s own volume applies too. Mute with the Sound button.')));
        panel.append(ag);
    }

    panel.append(el('div', { class: 'p-foot' },
        el('button', { title: 'Forget this browser\'s settings and use the daemon\'s config file (/etc/pve-h264/h264d.conf)',
            onclick: () => { ov = {}; saveOverrides(); sendFn({ reset: 1 }); markProfile(); } }, 'Reset to defaults'),
        el('span', { class: 'dim', id: 'p-ovn' })));
    if (srv) render();
}

export function refreshDpiInfo() {
    const info = $('dpi-info');
    if (!info) return;
    const wrap = $('wrap');
    const f = dpiFactor();
    let w = Math.floor(wrap.clientWidth * f) & ~7, h = Math.floor(wrap.clientHeight * f) & ~1;
    // same clamp as pve-h264d: a guest framebuffer holds at most 16 MiB (BGRX)
    const MAX = 16 * 1024 * 1024 / 4;
    let clamped = false;
    if (w * h > MAX) {
        const s = Math.sqrt(MAX / (w * h));
        w = Math.floor(w * s) & ~7; h = Math.floor(h * s) & ~1; clamped = true;
    }
    info.textContent = `→ ${w}×${h}` + (clamped ? ' (16 MiB limit)'
        : dpiMode() === 'native' ? ` (${(window.devicePixelRatio || 1).toFixed(2)}×)` : '');
}

function applyProfile(id) {
    const p = PROFILES[id].s;
    ov = { ...ov, ...p };
    saveOverrides();
    sendFn({ ...p });
    markProfile();
}

function matches(p, s) { return s && Object.entries(p).every(([k, v]) => s[k] === v); }
function markProfile() {
    const cur = srv ? { ...srv, ...ov } : null;
    document.querySelectorAll('#panel [data-profile]').forEach((b) => {
        b.classList.toggle('active', matches(PROFILES[b.dataset.profile].s, cur));
    });
    const n = $('p-ovn');
    if (n) n.textContent = Object.keys(ov).length ? `${Object.keys(ov).length} setting(s) saved in this browser` : 'using the daemon\'s defaults';
}

function render() {
    if (!srv) return;
    const kind = srv.kind || '';
    for (const { f, row, input, val } of Object.values(els)) {
        const v = srv[f.key];
        if (v === undefined) continue;
        if (f.type === 'select') input.value = v;
        else if (f.type === 'check') input.checked = !!v;
        else if (f.type === 'steps') {
            let i = f.steps.indexOf(v);
            if (i < 0) i = f.steps.reduce((b, s, j) => Math.abs(s - v) < Math.abs(f.steps[b] - v) ? j : b, 0);
            input.value = i;
            val.textContent = f.fmt ? f.fmt(v) : String(v);
        } else { input.value = v; val.textContent = f.fmt ? f.fmt(v) : String(v); }
        const unsupported = f.only && kind && !f.only.includes(kind);
        const inactive = f.when && !f.when(srv);
        input.disabled = unsupported;
        row.classList.toggle('off', unsupported || inactive);
        row.title = unsupported ? `Not used by the ${kind} encoder` : inactive ? 'Not used with the current mode' : '';
    }
    const enc = $('p-enc');
    if (enc) enc.textContent = srv.encoder || '';
    markProfile();
}

export function applyServerSettings(s) {
    srv = s;
    if ($('panel').childElementCount) render();
}

export function toggle(force) {
    open = force === undefined ? !open : force;
    const panel = $('panel');
    if (open && !panel.childElementCount) build();
    panel.hidden = !open;
    $('settingsbtn').setAttribute('aria-expanded', String(open));
    $('settingsbtn').classList.toggle('active', open);
    if (open) { render(); refreshDpiInfo(); }
}
export function isOpen() { return open; }

export function initSettings({ send, onDisplay, audio: a }) {
    sendFn = send;
    audio = a || null;
    displayFn = onDisplay;
    $('settingsbtn').addEventListener('click', () => toggle());
}
