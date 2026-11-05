// SPDX-License-Identifier: MIT
// PVE H.264 console viewer.
//
// Pipeline: WebSocket (pveproxy relay) -> length-prefixed messages ->
// WebCodecs VideoDecoder (hardware, optimizeForLatency, Annex B) ->
// draw to a desynchronized canvas immediately on decoder output.
// Every decoded frame is ACKed so the server can pace itself (no queueing).
import { QNUM, COMBOS } from './keymap.js?v=1';
// same cache-busting version as this file (the page template sets it)
const S_ = await import('./settings.js' + new URL(import.meta.url).search);
const A = await import('./audio.js' + new URL(import.meta.url).search);
const CB = await import('./clipboard.js' + new URL(import.meta.url).search);

const C = { AUTH: 1, KEY: 2, MOUSE_ABS: 3, MOUSE_BTN: 4, MOUSE_REL: 5, ACK: 6,
    CONFIG: 7, KEYFRAME: 8, PING: 9, RESIZE: 10, CLIPBOARD: 0x0c };
const S = { HELLO: 0x81, VIDEO_CONFIG: 0x82, FRAME: 0x83, CURSOR: 0x84,
    CURSOR_POS: 0x85, PONG: 0x86, STATS: 0x87, ERROR: 0x88, MOUSE_MODE: 0x89,
    DISPLAY_OFF: 0x8a, SETTINGS: 0x8b, NOTICE: 0x8c, AUDIO_STATE: 0x8d, AUDIO: 0x8e,
    CLIPBOARD: 0x8f, CLIPBOARD_STATE: 0x90 };

const params = new URLSearchParams(location.search);
const vmid = params.get('vmid');
const node = params.get('node');
const vmname = params.get('vmname') || '';
const embedded = window.self !== window.top;

const $ = (id) => document.getElementById(id);
const canvas = $('screen');
const ctx2d = canvas.getContext('2d', { alpha: false, desynchronized: true });
const statusEl = $('status');
const statsEl = $('stats');

const store = {
    get(k, d) { try { const v = localStorage.getItem('pve-h264-' + k); return v === null ? d : v; } catch { return d; } },
    set(k, v) { try { localStorage.setItem('pve-h264-' + k, v); } catch { /* ignore */ } },
};

let ws = null;
let decoder = null;
let decoderCfg = null;
let videoW = 0, videoH = 0;
let waitKey = true;
let mouseAbs = true;
let serverStats = {};
let reconnectDelay = 1000;
let fatal = false;
let connected = false;
const decodeStart = new Map();
const pressed = new Set();
const local = { frames: 0, bytes: 0, decMsSum: 0, decN: 0, rtt: 0, fps: 0, kbps: 0, decMs: 0, dropped: 0, encMs: 0 };

// ---------------------------------------------------------------- utils
function setStatus(text, isError) {
    statusEl.textContent = text || '';
    statusEl.classList.toggle('error', !!isError);
    statusEl.style.display = text ? 'block' : 'none';
}

function send(type, payload) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const len = payload ? payload.byteLength : 0;
    const b = new Uint8Array(5 + len);
    new DataView(b.buffer).setUint32(0, 1 + len, true);
    b[4] = type;
    if (len) b.set(payload, 5);
    // pveproxy drops the connection on websocket frames > 128 KiB; the
    // daemon reads a byte stream, so big messages (clipboard) can be split
    const MAXF = 64 * 1024;
    for (let o = 0; o < b.length; o += MAXF) ws.send(b.length <= MAXF ? b : b.subarray(o, o + MAXF));
}
function sendU32s(type, ...vals) {
    const a = new DataView(new ArrayBuffer(vals.length * 4));
    vals.forEach((v, i) => a.setUint32(i * 4, v >>> 0, true));
    send(type, new Uint8Array(a.buffer));
}
function sendI32s(type, ...vals) {
    const a = new DataView(new ArrayBuffer(vals.length * 4));
    vals.forEach((v, i) => a.setInt32(i * 4, v | 0, true));
    send(type, new Uint8Array(a.buffer));
}
function sendStr(type, s) { send(type, new TextEncoder().encode(s)); }

// ---------------------------------------------------------------- decoder
// Probed once; configure() must stay synchronous so it is ordered with decode().
let hwAccel = 'no-preference';
let hwFailed = false;
async function probeHardwareDecode() {
    try {
        const s = await VideoDecoder.isConfigSupported({ codec: 'avc1.64002a', codedWidth: 1920,
            codedHeight: 1080, optimizeForLatency: true, hardwareAcceleration: 'prefer-hardware' });
        hwAccel = s.supported ? 'prefer-hardware' : 'no-preference';
    } catch { hwAccel = 'no-preference'; }
}

function configureDecoder(cfg) {
    decoderCfg = cfg;
    decodeStart.clear();
    flushHeld(false);
    videoW = cfg.width; videoH = cfg.height;
    canvas.width = videoW; canvas.height = videoH;
    layout();
    const conf = { codec: cfg.codec, codedWidth: cfg.coded_width, codedHeight: cfg.coded_height,
        optimizeForLatency: true, hardwareAcceleration: hwFailed ? 'no-preference' : hwAccel };
    if (!decoder || decoder.state === 'closed')
        decoder = new VideoDecoder({ output: onFrame, error: onDecodeError });
    try {
        decoder.configure(conf); // queued after any pending decode() of the old stream
    } catch (e) {
        console.warn('configure failed', e);
        hwFailed = true;
        decoder = new VideoDecoder({ output: onFrame, error: onDecodeError });
        decoder.configure({ ...conf, hardwareAcceleration: 'no-preference' });
    }
    waitKey = true;
    serverEncoder = cfg.encoder;
    decodeMode = conf.hardwareAcceleration === 'prefer-hardware' ? 'GPU decode' : 'decode: browser default';
}
let serverEncoder = '', decodeMode = '';

function onFrame(frame) {
    const id = frame.timestamp;
    // decoders may pad codedWidth/Height for alignment; compare the display size
    if (Math.abs(frame.displayWidth - (decoderCfg?.coded_width ?? 0)) > 1 ||
        Math.abs(frame.displayHeight - (decoderCfg?.coded_height ?? 0)) > 1) {
        console.debug('dropping straggler frame', frame.displayWidth, frame.displayHeight);
        frame.close(); // straggler from before a resolution change
        decodeStart.delete(id);
        sendU32s(C.ACK, id, 0);
        return;
    }
    const meta = decodeStart.get(id);
    decodeStart.delete(id);
    const decUs = meta !== undefined ? Math.round((performance.now() - meta.t0) * 1000) : 0;
    local.decMsSum += decUs / 1000; local.decN++;
    // ACK on decode, not on draw: a lip-sync hold must not look like a slow
    // viewer to the daemon's pacing
    sendU32s(C.ACK, id, decUs);
    const hold = meta ? A.holdMs(meta.cap) : 0;
    if (hold > 1 || held.length) holdFrame(frame, meta ? meta.cap : 0, performance.now() + Math.max(0, hold));
    else drawFrame(frame, meta ? meta.cap : 0);
}

function drawFrame(frame, cap) {
    // crop away encoder padding (coded size may be rounded up to even)
    ctx2d.drawImage(frame, 0, 0, videoW, videoH, 0, 0, videoW, videoH);
    frame.close();
    local.frames++;
    if (cap) A.videoDrawn(cap);
    if (statusEl.dataset.waiting) { delete statusEl.dataset.waiting; setStatus(''); }
}

// lip sync: decoded frames waiting until their audio is heard (in order)
const held = [];
let heldTimer = null;
let heldLate = 0; // how late timers fire (busy main thread): wake up that much earlier
function holdFrame(frame, cap, due) {
    held.push({ frame, cap, due, wake: due - heldLate });
    while (held.length > 20) { const f = held.shift(); drawFrame(f.frame, f.cap); }
    drawHeld();
}
function drawHeld() {
    clearTimeout(heldTimer); heldTimer = null;
    const now = performance.now();
    // draw only the newest frame that is due; older due ones are superseded
    let last = -1;
    for (let i = 0; i < held.length && held[i].wake <= now + 1; i++) last = i;
    for (let i = 0; i < last; i++) held[i].frame.close();
    if (last >= 0) {
        const f = held[last];
        held.splice(0, last + 1);
        heldLate = heldLate * 0.9 + Math.max(-5, Math.min(30, now - f.due + heldLate)) * 0.1;
        drawFrame(f.frame, f.cap);
    }
    if (held.length) heldTimer = setTimeout(drawHeld, Math.max(0, held[0].wake - now));
}
function flushHeld(draw) {
    clearTimeout(heldTimer); heldTimer = null;
    const last = held.length - 1;
    held.forEach((f, i) => { if (draw && i === last) drawFrame(f.frame, f.cap); else f.frame.close(); });
    held.length = 0;
}

function onDecodeError(e) {
    console.warn('decoder error', e);
    if (hwAccel === 'prefer-hardware' && !hwFailed && local.frames === 0) hwFailed = true;
    decoder = null; // closed by the error
    if (decoderCfg) { configureDecoder(decoderCfg); send(C.KEYFRAME); }
}

function onVideoFrame(dv, off, len) {
    const id = dv.getUint32(off, true);
    const key = dv.getUint8(off + 4) & 1;
    local.encMs = local.encMs * 0.9 + dv.getUint32(off + 5, true) / 10000;
    const cap = Number(dv.getBigUint64(off + 9, true));
    A.observeVideo(cap);
    const data = new Uint8Array(dv.buffer, dv.byteOffset + off + 17, len - 17);
    local.bytes += data.byteLength;
    if (!decoder || decoder.state !== 'configured') { sendU32s(C.ACK, id, 0); return; }
    if (waitKey && !key) { sendU32s(C.ACK, id, 0); local.dropped++; return; }
    waitKey = false;
    // the server's ACK window bounds the queue; more means the decoder is behind
    if (decoder.decodeQueueSize > Math.max(4, (serverStats.window || 2) + 2)) {
        // decoder can't keep up: drop until next keyframe instead of building latency
        local.dropped++; waitKey = true; sendU32s(C.ACK, id, 0); send(C.KEYFRAME); return;
    }
    decodeStart.set(id, { t0: performance.now(), cap });
    try {
        decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: id, data }));
    } catch (e) {
        console.warn(e); waitKey = true; sendU32s(C.ACK, id, 0); send(C.KEYFRAME);
    }
}

// ---------------------------------------------------------------- stream parser
let rbuf = new Uint8Array(1 << 20);
let rlen = 0;
function onData(ab) {
    const chunk = new Uint8Array(ab);
    if (rlen + chunk.length > rbuf.length) {
        const n = new Uint8Array(Math.max(rbuf.length * 2, rlen + chunk.length));
        n.set(rbuf.subarray(0, rlen));
        rbuf = n;
    }
    rbuf.set(chunk, rlen);
    rlen += chunk.length;
    const dv = new DataView(rbuf.buffer);
    let pos = 0;
    while (rlen - pos >= 5) {
        const len = dv.getUint32(pos, true);
        if (rlen - pos < 4 + len) break;
        handleMsg(rbuf[pos + 4], dv, pos + 5, len - 1);
        pos += 4 + len;
    }
    if (pos > 0) { rbuf.copyWithin(0, pos, rlen); rlen -= pos; }
}

const td = new TextDecoder();
function str(dv, off, len) { return td.decode(new Uint8Array(dv.buffer, dv.byteOffset + off, len)); }

function handleMsg(type, dv, off, len) {
    switch (type) {
    case S.FRAME: onVideoFrame(dv, off, len); break;
    case S.AUDIO: A.onPacket(dv, off, len); break;
    case S.AUDIO_STATE: A.serverState(JSON.parse(str(dv, off, len))); break;
    case S.VIDEO_CONFIG: configureDecoder(JSON.parse(str(dv, off, len))); break;
    case S.HELLO: {
        const h = JSON.parse(str(dv, off, len));
        mouseAbs = h.mouse_abs;
        connected = true; reconnectDelay = 1000;
        setStatus('Waiting for video…'); statusEl.dataset.waiting = 1;
        applyCursorMode();
        sendConfig(S_.overrides());
        if ($('autoresize').checked) requestGuestResize();
        A.connected();
        break;
    }
    case S.STATS: serverStats = JSON.parse(str(dv, off, len)); break;
    case S.SETTINGS: S_.applyServerSettings(JSON.parse(str(dv, off, len))); break;
    case S.NOTICE: showNotice(str(dv, off, len)); break;
    case S.CLIPBOARD: CB.onGuestText(str(dv, off + 1, len - 1), dv.getUint8(off)); break;
    case S.CLIPBOARD_STATE: CB.onState(JSON.parse(str(dv, off, len))); break;
    case S.PONG: {
        const t = dv.getFloat64(off, true);
        const rtt = performance.now() - t;
        local.rtt = local.rtt ? local.rtt * 0.7 + rtt * 0.3 : rtt;
        break;
    }
    case S.CURSOR: setCursor(dv, off, len); break;
    case S.CURSOR_POS: cursorVisible = dv.getUint8(off + 8) !== 0; applyCursorMode(); break;
    case S.MOUSE_MODE: mouseAbs = dv.getUint8(off) !== 0; relLast = null; applyCursorMode(); break;
    case S.DISPLAY_OFF: setStatus('Guest display is off'); break;
    case S.ERROR: {
        const m = str(dv, off, len);
        setStatus(m, true);
        if (/authentication|encoder|no encoder/i.test(m)) fatal = true;
        break;
    }
    }
}

// ---------------------------------------------------------------- connection
async function api(method, path, body) {
    const opts = { method, credentials: 'same-origin', headers: {} };
    if (method !== 'GET') {
        opts.headers.CSRFPreventionToken = (window.PVE && PVE.CSRFPreventionToken) || '';
        if (body) { opts.headers['Content-Type'] = 'application/x-www-form-urlencoded'; opts.body = new URLSearchParams(body); }
    }
    const r = await fetch('/api2/json' + path, opts);
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.message || (j.errors && JSON.stringify(j.errors)) || r.statusText);
    return j.data;
}

let retryTimer = null;
function retryIn(ms, fn = connect) { clearTimeout(retryTimer); retryTimer = setTimeout(fn, ms); }

async function connect() {
    clearTimeout(retryTimer);
    if (ws) return; // already connected / connecting
    if (!window.VideoDecoder) {
        setStatus('This browser has no WebCodecs (needs HTTPS and a recent Chrome/Edge/Safari/Firefox). Use noVNC instead.', true);
        return;
    }
    // like noVNC: if the guest is off, offer to start it instead of failing
    const st = await refreshVmState();
    if (st && st.status !== 'running') { showStartPrompt(st); return; }
    hideStartPrompt();
    setStatus('Connecting…');
    let info;
    try {
        info = await api('POST', `/nodes/${node}/qemu/${vmid}/h264proxy`);
    } catch (e) {
        if (/not running/i.test(e.message)) { showStartPrompt({ status: 'stopped' }); return; }
        setStatus('Failed to start H.264 console: ' + e.message, true);
        scheduleReconnect(true);
        return;
    }
    if (ws) return;
    // relay: the VM runs on another cluster node; this node relays the stream
    const relay = info.relay ? `&relay=${encodeURIComponent(info.relay)}` : '';
    const url = `wss://${location.host}/api2/json/nodes/${node}/qemu/${vmid}/h264websocket?vncticket=${encodeURIComponent(info.ticket)}${relay}`;
    ws = new WebSocket(url, ['binary']);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => sendStr(C.AUTH, info.token);
    ws.onmessage = (ev) => onData(ev.data);
    ws.onclose = () => {
        ws = null; connected = false;
        releaseAll();
        A.disconnected();
        flushHeld(false);
        CB.onDisconnect();
        if (decoder && decoder.state !== 'closed') decoder.close();
        decoder = null;
        if (!fatal) scheduleReconnect(false);
    };
    ws.onerror = () => {};
}

function scheduleReconnect(slow) {
    if (fatal) return;
    const d = slow ? Math.max(reconnectDelay, 3000) : reconnectDelay;
    if (!statusEl.classList.contains('error')) setStatus(`Disconnected — reconnecting in ${Math.round(d / 1000)}s…`);
    retryIn(d);
    reconnectDelay = Math.min(reconnectDelay * 2, 10000);
}

function sendConfig(obj) {
    if (obj) sendStr(C.CONFIG, JSON.stringify(obj));
}

let noticeTimer = null;
function showNotice(text) {
    setStatus(text);
    clearTimeout(noticeTimer);
    noticeTimer = setTimeout(() => { if (statusEl.textContent === text) setStatus(''); }, 8000);
}

// ---------------------------------------------------------------- power control (as in noVNC)
const POWER = [
    // cmd, label, enabled when (status/current) ..., danger
    ['start', 'Start', (s) => s.status !== 'running'],
    ['shutdown', 'Shutdown', (s) => s.status === 'running'],
    ['reboot', 'Reboot', (s) => s.status === 'running' && s.qmpstatus !== 'paused'],
    null,
    ['suspend', 'Pause', (s) => s.status === 'running' && s.qmpstatus !== 'paused'],
    ['resume', 'Resume', (s) => s.status === 'running' && s.qmpstatus === 'paused'],
    null,
    ['stop', 'Hard Stop', (s) => s.status === 'running', true],
    ['reset', 'Hard Reset', (s) => s.status === 'running', true],
];
let vmState = null;       // last /status/current answer
let vmStateDenied = false; // no VM.Audit: we can't see the state, so allow everything
let startPending = false;

async function refreshVmState() {
    if (vmStateDenied) return null;
    try {
        vmState = await api('GET', `/nodes/${node}/qemu/${vmid}/status/current`);
    } catch (e) {
        // 403 (no VM.Audit) or a transient error: just try to connect, as noVNC does
        if (/permission/i.test(e.message)) vmStateDenied = true;
        vmState = null;
    }
    return vmState;
}

function describeState(s) {
    if (!s) return 'state unknown';
    if (s.status === 'running') return s.qmpstatus === 'paused' ? 'paused' : 'running';
    if (s.lock === 'suspended') return 'hibernated';
    return s.status;
}

function showStartPrompt(st) {
    setStatus('');
    if (ctx2d && canvas.width) { ctx2d.fillStyle = '#000'; ctx2d.fillRect(0, 0, canvas.width, canvas.height); }
    const hib = st && st.lock === 'suspended';
    $('startinfo').textContent = startPending ? 'Starting…' : hib ? 'Guest is hibernated' : 'Guest not running';
    $('startbtn').lastChild.textContent = hib ? ' Resume Now' : ' Start Now';
    $('startbtn').disabled = startPending;
    $('startdlg').hidden = false;
    // keep watching: connect by itself once the VM is started from elsewhere
    retryIn(startPending ? 1000 : 3000);
}
function hideStartPrompt() { $('startdlg').hidden = true; }

async function powerCommand(cmd, label, ask = true) {
    togglePowerMenu(false);
    if (ask && !confirm(`Do you really want to ${label.toLowerCase()} VM ${vmid}${vmname ? ' (' + vmname + ')' : ''}?`)) {
        canvas.focus();
        return;
    }
    if (cmd === 'start') { startPending = true; $('startbtn').disabled = true; setStatus(''); $('startinfo').textContent = 'Starting…'; }
    try {
        await api('POST', `/nodes/${node}/qemu/${vmid}/status/${cmd}`);
        if (cmd !== 'start') showNotice(`VM command '${label}' sent`);
    } catch (e) {
        if (!(cmd === 'start' && /already running/i.test(e.message))) {
            startPending = false;
            if (!$('startdlg').hidden) showStartPrompt(vmState);
            showNotice(`${label} failed: ${e.message}`);
            return;
        }
    }
    if (cmd === 'start') {
        // the start task runs asynchronously: poll until QEMU is up, then connect
        const t0 = performance.now();
        while (performance.now() - t0 < 60000) {
            const s = await refreshVmState();
            if (!s || s.status === 'running') break;
            await new Promise((r) => setTimeout(r, 700));
        }
        startPending = false;
        reconnectDelay = 1000;
        if (!ws) connect();
    } else {
        canvas.focus();
    }
}

function renderPowerMenu() {
    const m = $('powermenu');
    m.textContent = '';
    const s = vmState;
    const head = document.createElement('div');
    head.className = 'pm-state';
    head.textContent = `VM ${vmid}: ${vmStateDenied ? 'state unknown' : describeState(s)}`;
    m.append(head);
    for (const it of POWER) {
        if (!it) { m.append(document.createElement('hr')); continue; }
        const [cmd, label, enabled, danger] = it;
        const b = document.createElement('button');
        b.textContent = label;
        b.setAttribute('role', 'menuitem');
        b.dataset.cmd = cmd;
        if (danger) b.className = 'danger';
        b.disabled = s ? !enabled(s) : false;
        b.onclick = () => powerCommand(cmd, label);
        m.append(b);
    }
}

let powerOpen = false;
async function togglePowerMenu(force) {
    powerOpen = force === undefined ? !powerOpen : force;
    const m = $('powermenu'), btn = $('powerbtn');
    btn.classList.toggle('active', powerOpen);
    btn.setAttribute('aria-expanded', powerOpen);
    if (!powerOpen) { m.hidden = true; return; }
    if (S_.isOpen()) S_.toggle(false);
    CB.close();
    const wrapR = $('wrap').getBoundingClientRect(), r = btn.getBoundingClientRect();
    m.style.left = Math.max(6, r.left - wrapR.left) + 'px';
    renderPowerMenu();
    m.hidden = false;
    await refreshVmState();
    if (powerOpen) renderPowerMenu();
}

function initPower() {
    $('powerbtn').addEventListener('click', () => togglePowerMenu());
    $('startbtn').addEventListener('click', () => powerCommand('start', 'Start', false));
    document.addEventListener('pointerdown', (ev) => {
        if (powerOpen && !$('powermenu').contains(ev.target) && !$('powerbtn').contains(ev.target)) togglePowerMenu(false);
    }, true);
    document.addEventListener('keydown', (ev) => {
        if (powerOpen && ev.code === 'Escape') { togglePowerMenu(false); ev.stopPropagation(); ev.preventDefault(); }
    }, true);
}

// ---------------------------------------------------------------- layout / cursor
function layout() {
    if (!videoW) return;
    const wrap = $('wrap');
    const W = wrap.clientWidth, H = wrap.clientHeight;
    const mode = $('scale').value;
    const dpr = window.devicePixelRatio || 1;
    let s;
    if (mode === '1') s = 1;
    else if (mode === 'dev') s = 1 / dpr; // one guest pixel per screen pixel
    else {
        s = Math.min(W / videoW, H / videoH);
        // "no upscale" means no upscaling on the screen: at a native-DPI guest
        // resolution that is 1/dpr in CSS pixels
        if (mode === 'down') s = Math.min(S_.dpiMode() === 'native' ? 1 / dpr : 1, s);
    }
    // exact sizes for 1:1 so fractional DPRs still map pixel-for-pixel
    const exact = mode === '1' || mode === 'dev';
    canvas.style.width = (exact ? videoW * s : Math.round(videoW * s)) + 'px';
    canvas.style.height = (exact ? videoH * s : Math.round(videoH * s)) + 'px';
    canvas.style.imageRendering = S_.smoothing() === 'pixelated' ? 'pixelated' : 'auto';
}
window.addEventListener('resize', () => {
    layout(); S_.refreshDpiInfo();
    if ($('autoresize').checked) requestGuestResize();
});
// moving the window to a screen with another scale factor (or browser zoom)
function watchDpr() {
    const mq = matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
    mq.addEventListener('change', () => {
        layout(); S_.refreshDpiInfo();
        if ($('autoresize').checked && S_.dpiMode() === 'native') requestGuestResize();
        watchDpr();
    }, { once: true });
}
watchDpr();

let hwCursor = null;
let cursorVisible = true;
function setCursor(dv, off, len) {
    const w = dv.getInt32(off, true), h = dv.getInt32(off + 4, true);
    const hx = dv.getInt32(off + 8, true), hy = dv.getInt32(off + 12, true);
    const px = new Uint8ClampedArray(dv.buffer, dv.byteOffset + off + 16, w * h * 4);
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(px), w, h), 0, 0);
    hwCursor = `url(${c.toDataURL()}) ${hx} ${hy}, default`;
    applyCursorMode();
}
function applyCursorMode() {
    const locked = document.pointerLockElement === canvas;
    // Always show the user's native cursor; hide it only while pointer-locked in relative mode.
    canvas.style.cursor = (!mouseAbs && locked) ? 'none' : 'default';
    $('mousemode').textContent = mouseAbs ? 'mouse: abs'
        : locked ? 'mouse: captured (Esc releases)'
        : captureMouse ? 'mouse: rel – click to capture' : 'mouse: rel';
}

// ---------------------------------------------------------------- input
function keyEvent(down, code) {
    const q = QNUM[code];
    if (q === undefined) return false;
    if (down) pressed.add(code); else if (!pressed.delete(code)) return true;
    const b = new DataView(new ArrayBuffer(5));
    b.setUint8(0, down ? 1 : 0); b.setUint32(1, q, true);
    send(C.KEY, new Uint8Array(b.buffer));
    return true;
}
function releaseAll() {
    CB.reset();
    for (const code of [...pressed]) keyEvent(false, code);
    for (let b = 0; b < 8; b++) if (buttons & (1 << b)) { sendBtn(false, b); }
    buttons = 0;
}

function inputActive(ev) {
    // don't steal keys from the toolbar's form controls
    const t = ev.target;
    return !(t && (t.tagName === 'SELECT' || t.tagName === 'INPUT' || t.tagName === 'BUTTON' ||
        t.tagName === 'TEXTAREA'));
}
document.addEventListener('keydown', (ev) => {
    if (!inputActive(ev) || !connected || powerOpen) return;
    if (CB.onKeyDown(ev)) return; // paste key: held until the browser's paste event
    if (keyEvent(true, ev.code)) ev.preventDefault();
}, true);
document.addEventListener('keyup', (ev) => {
    if (!inputActive(ev) || !connected) return;
    if (CB.onKeyUp(ev)) return;
    if (keyEvent(false, ev.code)) ev.preventDefault();
}, true);
window.addEventListener('blur', releaseAll);
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });

let buttons = 0;
const BTN = [0, 1, 2, 5, 6]; // browser button -> qemu (left, middle, right, side, extra)
function sendBtn(down, qb) {
    send(C.MOUSE_BTN, new Uint8Array([down ? 1 : 0, qb]));
}
function fbPos(ev) {
    const r = canvas.getBoundingClientRect();
    const x = Math.floor((ev.clientX - r.left) * videoW / r.width);
    const y = Math.floor((ev.clientY - r.top) * videoH / r.height);
    return [Math.max(0, Math.min(videoW - 1, x)), Math.max(0, Math.min(videoH - 1, y))];
}
let lastMove = 0, pendingMove = null;
function flushMove() {
    if (pendingMove) { sendU32s(C.MOUSE_ABS, pendingMove[0], pendingMove[1]); pendingMove = null; }
    flushRel();
}
// Relative-mouse guests without pointer lock (noVNC style): turn changes of
// the local pointer position into relative motion, in framebuffer pixels.
// The guest's own acceleration may make its cursor drift from the local one.
let captureMouse = false;
let relLast = null, relAccX = 0, relAccY = 0;
function fbPosF(ev) {
    const r = canvas.getBoundingClientRect();
    return [(ev.clientX - r.left) * videoW / r.width, (ev.clientY - r.top) * videoH / r.height];
}
function flushRel() {
    const dx = Math.trunc(relAccX), dy = Math.trunc(relAccY);
    if (dx || dy) { sendI32s(C.MOUSE_REL, dx, dy); relAccX -= dx; relAccY -= dy; }
}
function throttledFlush() {
    // use the highest-resolution events available, but don't send >250 Hz
    const now = performance.now();
    if (now - lastMove >= 4) { lastMove = now; flushMove(); } else setTimeout(flushMove, 4);
}
canvas.addEventListener('pointermove', (ev) => {
    if (!connected || !videoW) return;
    if (!mouseAbs) {
        if (document.pointerLockElement === canvas) {
            if (ev.movementX || ev.movementY) sendI32s(C.MOUSE_REL, ev.movementX, ev.movementY);
            return;
        }
        const p = fbPosF(ev);
        if (relLast) { relAccX += p[0] - relLast[0]; relAccY += p[1] - relLast[1]; }
        relLast = p;
        throttledFlush();
        return;
    }
    pendingMove = fbPos(ev);
    throttledFlush();
});
canvas.addEventListener('pointerenter', (ev) => { relLast = videoW ? fbPosF(ev) : null; });
canvas.addEventListener('pointerleave', () => { flushRel(); relLast = null; });
canvas.addEventListener('pointerdown', (ev) => {
    if (S_.isOpen()) S_.toggle(false);
    if (!connected) return;
    canvas.focus();
    if (!mouseAbs && captureMouse && document.pointerLockElement !== canvas) {
        canvas.requestPointerLock({ unadjustedMovement: true })?.catch?.(() => canvas.requestPointerLock());
        return;
    }
    if (mouseAbs) { pendingMove = fbPos(ev); flushMove(); }
    const qb = BTN[ev.button];
    if (qb === undefined) return;
    buttons |= 1 << qb; sendBtn(true, qb);
    canvas.setPointerCapture(ev.pointerId);
    ev.preventDefault();
});
canvas.addEventListener('pointerup', (ev) => {
    const qb = BTN[ev.button];
    if (qb === undefined || !(buttons & (1 << qb))) return;
    buttons &= ~(1 << qb); sendBtn(false, qb);
    ev.preventDefault();
});
canvas.addEventListener('contextmenu', (ev) => ev.preventDefault());
let wheelAcc = 0, wheelAccX = 0;
canvas.addEventListener('wheel', (ev) => {
    if (!connected) return;
    ev.preventDefault();
    const k = ev.deltaMode === 1 ? 33 : ev.deltaMode === 2 ? 400 : 1;
    wheelAcc += ev.deltaY * k; wheelAccX += ev.deltaX * k;
    const step = 50;
    while (Math.abs(wheelAcc) >= step) {
        const b = wheelAcc < 0 ? 3 : 4; sendBtn(true, b); sendBtn(false, b);
        wheelAcc -= Math.sign(wheelAcc) * step;
    }
    while (Math.abs(wheelAccX) >= step) {
        const b = wheelAccX < 0 ? 7 : 8; sendBtn(true, b); sendBtn(false, b);
        wheelAccX -= Math.sign(wheelAccX) * step;
    }
}, { passive: false });
document.addEventListener('pointerlockchange', applyCursorMode);

// ---------------------------------------------------------------- audio button
const AUDIO_REASON = {
    'no-device': 'This VM has no audio device. Add one (Hardware → Add → Audio Device, driver SPICE), then shut the VM down and start it again.',
    disabled: 'Audio is turned off on this host (audio = 0 in /etc/pve-h264/h264d.conf).',
    closed: 'QEMU closed the audio connection.',
    error: 'The audio connection to QEMU failed (see journalctl -u pve-h264d@<vmid>).',
    starting: 'Connecting…',
};
function audioUi(st) {
    const b = $('audiobtn');
    const on = st.available && st.pref;
    b.classList.toggle('active', on && st.running && st.playing);
    b.classList.toggle('muted', !on);
    $('audio-on').style.display = on ? '' : 'none';
    $('audio-off').style.display = on ? 'none' : '';
    b.title = !st.supported ? 'Sound needs a browser with AudioWorklet support'
        : !st.available ? 'No sound: ' + (AUDIO_REASON[st.reason] || st.reason || 'not available')
        : !st.pref ? 'Sound is muted in this browser (click to unmute)'
        : !st.running ? 'Sound is on: it starts with your first click or key press in the console'
        : `Sound on (${st.mode === 1 ? 'Opus' : 'PCM'}${st.playing ? ', guest playing' : ''}); click to mute`;
}
function initAudio() {
    A.init({ send, onUi: audioUi });
    $('audiobtn').addEventListener('click', () => {
        const st = A.uiState();
        if (!st.available) { showNotice('No sound: ' + (AUDIO_REASON[st.reason] || st.reason)); return; }
        A.toggle(); canvas.focus();
    });
}

// ---------------------------------------------------------------- toolbar
function sendCombo(codes) {
    for (const c of codes) keyEvent(true, c);
    for (const c of [...codes].reverse()) keyEvent(false, c);
}
function requestGuestResize() {
    const wrap = $('wrap');
    // Default: CSS pixels, not device pixels: on a HiDPI screen (Mac Retina,
    // 150-200 % Windows/Linux scaling) the guest then runs at 100 % scaling
    // with text the same size as the local desktop's, instead of a huge mode
    // with tiny text (the guest can't be told to scale itself). The "Guest
    // resolution" setting multiplies this (native = device pixels).
    const f = S_.dpiFactor();
    const w = Math.floor(wrap.clientWidth * f) & ~7;
    const h = Math.floor(wrap.clientHeight * f) & ~1;
    sendU32s(C.RESIZE, w, h);
}

function initToolbar() {
    const ar = $('autoresize');
    const combos = $('combos');
    COMBOS.forEach(([label], i) => combos.add(new Option(label, i)));
    combos.addEventListener('change', () => {
        if (combos.value !== '') sendCombo(COMBOS[combos.value][1]);
        combos.value = ''; canvas.focus();
    });
    $('cad').onclick = () => { sendCombo(['ControlLeft', 'AltLeft', 'Delete']); canvas.focus(); };
    $('fullscreen').onclick = async () => {
        if (document.fullscreenElement) { await document.exitFullscreen(); return; }
        await document.documentElement.requestFullscreen();
        // capture Esc/Win/Alt+Tab etc. while fullscreen (Chromium)
        try { await navigator.keyboard?.lock(); } catch { /* ignore */ }
    };
    document.addEventListener('fullscreenchange', () => { setTimeout(layout, 50); if (!document.fullscreenElement) navigator.keyboard?.unlock?.(); });

    const scale = $('scale'), scaleDef = embedded ? 'fit' : 'down';
    scale.value = store.get('scale', scaleDef);
    if (scale.selectedIndex < 0) scale.value = scaleDef;
    scale.addEventListener('change', () => { store.set('scale', scale.value); layout(); canvas.focus(); });

    initPower();
    initAudio();
    CB.init({
        send: (text) => sendStr(C.CLIPBOARD, text),
        keyEvent,
        notice: showNotice,
        focus: () => canvas.focus(),
        closeOthers: () => { if (S_.isOpen()) S_.toggle(false); togglePowerMenu(false); },
    });
    S_.initSettings({
        audio: A,
        send: (obj) => sendConfig(obj),
        onDisplay: (what) => {
            layout();
            if (what === 'dpi') {
                // a guest resolution only matters when the guest is resized
                if (!ar.checked) { ar.checked = true; store.set('autoresize', '1'); }
                requestGuestResize();
            }
        },
    });
    const st = $('showstats');
    st.checked = store.get('stats', '1') === '1';
    statsEl.style.display = st.checked ? 'block' : 'none';
    st.onchange = () => { store.set('stats', st.checked ? '1' : '0'); statsEl.style.display = st.checked ? 'block' : 'none'; };

    const cap = $('capture');
    captureMouse = cap.checked = store.get('capture', '0') === '1';
    cap.onchange = () => {
        captureMouse = cap.checked; store.set('capture', cap.checked ? '1' : '0');
        if (!captureMouse && document.pointerLockElement) document.exitPointerLock();
        applyCursorMode(); canvas.focus();
    };

    ar.checked = store.get('autoresize', '0') === '1';
    ar.onchange = () => { store.set('autoresize', ar.checked ? '1' : '0'); if (ar.checked) requestGuestResize(); };

    const def = $('defconsole');
    def.checked = store.get('default', '0') === '1';
    def.onchange = () => store.set('default', def.checked ? '1' : '0');

    $('novnc').onclick = () => {
        const q = new URLSearchParams({ console: 'kvm', novnc: 1, vmid, vmname, node, resize: 'scale' });
        location.href = '/?' + q.toString();
    };
    document.title = `${vmid}${vmname ? ' (' + vmname + ')' : ''} - H.264 Console`;
    $('title').textContent = `VM ${vmid}${vmname ? ' – ' + vmname : ''}`;
}

// ---------------------------------------------------------------- stats
setInterval(() => {
    local.fps = local.frames; local.kbps = Math.round(local.bytes * 8 / 1000);
    local.decMs = local.decN ? local.decMsSum / local.decN : 0;
    local.frames = 0; local.bytes = 0; local.decMsSum = 0; local.decN = 0;
    if (ws && ws.readyState === WebSocket.OPEN) {
        const b = new DataView(new ArrayBuffer(8)); b.setFloat64(0, performance.now(), true);
        send(C.PING, new Uint8Array(b.buffer));
    }
    if (statsEl.style.display === 'none') return;
    const s = serverStats;
    const frameMs = 1000 / Math.max(1, s.target_fps || 60);
    // capture->glass estimate: avg wait for next capture tick (half a frame) +
    // encode + one-way network + decode + avg wait for vsync (half a frame)
    const est = frameMs / 2 + local.encMs + local.rtt / 2 + local.decMs + 8;
    statsEl.textContent =
        `${videoW}×${videoH}  ${local.fps} fps  ${(local.kbps / 1000).toFixed(1)} Mbit/s\n` +
        `${serverEncoder} → ${decodeMode}\n` +
        `encode ${local.encMs.toFixed(1)} ms  decode ${local.decMs.toFixed(1)} ms  rtt ${local.rtt.toFixed(1)} ms\n` +
        `est. latency ≈ ${est.toFixed(0)} ms\n` +
        `cap ${(s.cap_kbps / 1000 || 0).toFixed(1)}/${(s.max_kbps / 1000 || 0).toFixed(0)} Mbit/s  ` +
        `window ${s.window ?? '-'}  queue ${(s.queue_ms ?? 0).toFixed(0)} ms` +
        (s.keyframes ? `  key ${s.keyframes}` : '') + '\n' +
        `src ${s.capture || '-'}  viewers ${s.viewers ?? '-'}` +
        (s.paced_ms ? `  waited for ACKs ${s.paced_ms} ms/s` : '') + (local.dropped ? `  dropped ${local.dropped}` : '');
    const al = A.statsLine();
    if (al) statsEl.textContent += '\n' + al;
    local.dropped = 0;
}, 1000);

// ---------------------------------------------------------------- toolbar auto-hide (fullscreen)
let hideTimer = null;
function showBar() {
    document.body.classList.remove('bar-hidden');
    clearTimeout(hideTimer);
    if (document.fullscreenElement) hideTimer = setTimeout(() => document.body.classList.add('bar-hidden'), 2500);
}
document.addEventListener('mousemove', (ev) => { if (ev.clientY < 40) showBar(); });
document.addEventListener('fullscreenchange', showBar);

if (!vmid || !node) {
    setStatus('missing vmid/node parameter', true);
} else {
    initToolbar();
    probeHardwareDecode().then(connect);
    canvas.focus();
}

// debug hook for automated tests / the browser console
window.__h264 = {
    get decoderState() { return decoder ? decoder.state : 'none'; },
    get queue() { return decoder ? decoder.decodeQueueSize : -1; },
    get waitKey() { return waitKey; },
    get config() { return decoderCfg; },
    get hwAccel() { return hwAccel + (hwFailed ? ' (failed)' : ''); },
    get settings() { return S_.current(); },
    get vmState() { return vmState; },
    get startPrompt() { return !$('startdlg').hidden; },
    get held() { return held.length; },
    audio: A.debug,
    get clipboard() { return CB.debugState(); },
    local,
};
