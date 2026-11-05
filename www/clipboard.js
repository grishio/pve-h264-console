// SPDX-License-Identifier: MIT
// Clipboard sharing between this browser and the guest (UTF-8 text), through
// the spice agent in the guest (pve-h264d: daemon/vdagent.c).
//
// Browser -> guest. Browsers only hand out the clipboard on a paste, so:
//  - Ctrl+V / Shift+Insert / Cmd+V in the console: the V (Insert) key is held
//    back and not preventDefault()ed, the browser fires a 'paste' event with
//    its clipboard, we send that text to the guest (which takes it as its own
//    clipboard), then forward the key, so the guest program pastes it.
//  - When the window gets the focus again and the page may read the clipboard
//    (Chromium, after "Allow" once): read it and send it if it changed, so a
//    paste from the guest's own menus has it too.
//  - The panel: type or paste text and send it.
// Guest -> browser: when something in the guest copies text, the daemon
// fetches it and we write it to the local clipboard. Without a recent click
// or key press some browsers refuse; then it waits for the next one (and the
// panel shows it).

const $ = (id) => document.getElementById(id);
const store = {
    get(k, d) { try { const v = localStorage.getItem('pve-h264-' + k); return v === null ? d : v; } catch { return d; } },
    set(k, v) { try { localStorage.setItem('pve-h264-' + k, v); } catch { /* ignore */ } },
};

const FLAG_REPLAY = 1;    // S_CLIPBOARD sent on connect: show only
const PASTE_WAIT_MS = 150; // for the paste event, then the key goes anyway
const GRAB_LEAD_MS = 40;   // let the agent take the clipboard before the key

let api = null;             // { send(text), keyEvent(down, code) }
let state = { available: false, to_guest: false, from_guest: false, max: 1 << 20, reason: 'not connected' };
let shared = null;          // latest text either way (what a guest paste gives)
let pendingWrite = null;    // guest text not yet in the local clipboard
let pendingKey = null;      // { code, up, timer } held-back paste key
let open = false;
const enc = new TextEncoder();

const auto = () => store.get('clip-auto', '1') === '1';
const canSend = () => state.available && state.to_guest;

function setShared(text) {
    shared = text;
    const ta = $('cliptext');
    if (ta && document.activeElement !== ta) ta.value = text;
}

function updateUi() {
    const btn = $('clipbtn');
    if (!btn) return;
    btn.classList.toggle('unavail', !state.available);
    btn.classList.toggle('pending', !!pendingWrite);
    btn.title = !state.available ? `Clipboard: ${state.reason}`
        : pendingWrite ? 'The guest copied text: click to put it on your clipboard'
        : 'Clipboard shared with the guest' + (state.to_guest && state.from_guest ? '' : state.to_guest ? ' (to the guest only)' : ' (from the guest only)');
    if (!open) return;
    const st = $('clipstate');
    st.textContent = !state.available ? state.reason
        : [state.to_guest && 'to the guest', state.from_guest && 'from the guest'].filter(Boolean).join(' and ');
    st.classList.toggle('err', !state.available);
    $('clipsend').disabled = !canSend();
    $('clippaste').disabled = !canSend() || !navigator.clipboard?.readText;
    $('clipcopy').disabled = !navigator.clipboard?.writeText;
}

// --- browser -> guest
function sendText(text, why) {
    if (!canSend() || typeof text !== 'string') return false;
    if (text === shared) return false; // the guest already has it
    const n = enc.encode(text).length;
    if (n > state.max) {
        api.notice(`Clipboard not sent: ${(n / 1048576).toFixed(1)} MiB is more than the ${(state.max / 1048576).toFixed(0)} MiB limit`);
        return false;
    }
    api.send(text);
    setShared(text);
    console.debug(`clipboard -> guest (${why}, ${n} bytes)`);
    return true;
}

function isPasteKey(ev) {
    const k = (ev.key || '').toLowerCase();
    return (k === 'v' && (ev.ctrlKey || ev.metaKey) && !ev.altKey) ||
        (ev.code === 'Insert' && ev.shiftKey && !ev.ctrlKey);
}

function flushKey(delay) {
    const p = pendingKey;
    if (!p) return;
    clearTimeout(p.timer);
    pendingKey = null;
    const go = () => { api.keyEvent(true, p.code); if (p.up) api.keyEvent(false, p.code); };
    if (delay) setTimeout(go, delay); else go();
}

// keydown/keyup from the console (h264.js). true = handled here: the caller
// must neither forward nor preventDefault() it (the browser's paste follows).
export function onKeyDown(ev) {
    retryWrite();
    if (pendingKey) {
        if (ev.code === pendingKey.code) { ev.preventDefault(); return true; } // auto-repeat
        flushKey(0);
    }
    if (!auto() || !canSend() || ev.repeat || !isPasteKey(ev)) return false;
    pendingKey = { code: ev.code, up: false, timer: setTimeout(() => flushKey(0), PASTE_WAIT_MS) };
    return true;
}
export function onKeyUp(ev) {
    if (pendingKey && ev.code === pendingKey.code) { pendingKey.up = true; ev.preventDefault(); return true; }
    return false;
}
export function reset() {
    if (pendingKey) clearTimeout(pendingKey.timer);
    pendingKey = null;
}

function onPaste(ev) {
    if (!pendingKey) return; // a paste into the panel's text box etc.
    ev.preventDefault();
    const text = ev.clipboardData?.getData('text/plain');
    flushKey(text && sendText(text, 'paste') ? GRAB_LEAD_MS : 0);
}

async function syncOnFocus() {
    // a guest copy still waiting for the local clipboard is newer
    if (!auto() || !canSend() || !navigator.clipboard?.readText || pendingWrite !== null) return;
    try {
        // only where reading needs no prompt (Chromium, permission granted)
        const p = await navigator.permissions.query({ name: 'clipboard-read' });
        if (p.state !== 'granted') return;
        const text = await navigator.clipboard.readText();
        if (text && pendingWrite === null) sendText(text, 'focus');
    } catch { /* unsupported or not focused */ }
}

// --- guest -> browser
async function writeLocal(text) {
    try {
        await navigator.clipboard.writeText(text);
        if (pendingWrite === text) pendingWrite = null;
        updateUi();
        return true;
    } catch {
        return false;
    }
}
function retryWrite() {
    // inside a user gesture: browsers that refused before allow it now
    if (pendingWrite !== null) writeLocal(pendingWrite);
}

export function onGuestText(text, flags) {
    setShared(text);
    if (flags & FLAG_REPLAY || !auto() || !navigator.clipboard?.writeText) return;
    pendingWrite = text;
    updateUi();
    writeLocal(text);
}

export function onState(s) {
    state = { ...state, ...s };
    if (!state.available) reset();
    updateUi();
}

export function onDisconnect() {
    reset();
    onState({ available: false, reason: 'not connected' });
}

// --- panel
function toggle(force) {
    open = force === undefined ? !open : force;
    const p = $('clippanel'), btn = $('clipbtn');
    btn.classList.toggle('active', open);
    btn.setAttribute('aria-expanded', open);
    p.hidden = !open;
    if (!open) { api.focus(); return; }
    api.closeOthers();
    const wrapR = $('wrap').getBoundingClientRect(), r = btn.getBoundingClientRect();
    p.style.left = Math.max(6, Math.min(r.left - wrapR.left, wrapR.width - p.offsetWidth - 6)) + 'px';
    if (shared !== null) $('cliptext').value = shared;
    updateUi();
}
export function close() { if (open) toggle(false); }
export function isOpen() { return open; }

export function init(a) {
    api = a;
    document.addEventListener('paste', onPaste, true);
    window.addEventListener('focus', () => { retryWrite(); syncOnFocus(); });
    document.addEventListener('pointerdown', (ev) => {
        if (open && !$('clippanel').contains(ev.target) && !$('clipbtn').contains(ev.target)) toggle(false);
        if (!$('clipbtn').contains(ev.target)) retryWrite();
    }, true);
    document.addEventListener('keydown', (ev) => {
        if (open && ev.code === 'Escape') { toggle(false); ev.stopPropagation(); ev.preventDefault(); }
    }, true);

    $('clipbtn').addEventListener('click', () => {
        if (pendingWrite !== null && !open) { writeLocal(pendingWrite); return; }
        toggle();
    });
    $('clipclose').onclick = () => toggle(false);
    const ac = $('clipauto');
    ac.checked = auto();
    ac.onchange = () => { store.set('clip-auto', ac.checked ? '1' : '0'); if (!ac.checked) { pendingWrite = null; updateUi(); } };
    $('clipsend').onclick = () => {
        const t = $('cliptext').value;
        if (t === shared) { api.notice('The guest has this text already'); return; }
        if (sendText(t, 'panel')) api.notice('Sent to the guest clipboard');
    };
    $('clippaste').onclick = async () => {
        try {
            const t = await navigator.clipboard.readText();
            $('cliptext').value = t;
            if (sendText(t, 'panel')) api.notice('Your clipboard was sent to the guest');
            else if (t === shared) api.notice('The guest has this text already');
        } catch (e) {
            api.notice('The browser did not allow reading the clipboard: paste into the box with Ctrl+V instead');
        }
    };
    $('clipcopy').onclick = async () => {
        if (await writeLocal($('cliptext').value)) api.notice('Copied to your clipboard');
        else api.notice('The browser did not allow writing the clipboard');
    };
    updateUi();
}

// for tests
export function debugState() { return { state, shared, pendingWrite, pendingKey: !!pendingKey, auto: auto() }; }
