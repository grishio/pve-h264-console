// SPDX-License-Identifier: MIT
// PVE H.264 console: guest audio and lip sync (main thread).
//
// pve-h264d sends 10 ms audio packets (Opus, or raw PCM for browsers without
// an Opus AudioDecoder) on the same WebSocket as the video. Both carry the
// host time (CLOCK_MONOTONIC, µs) at which the daemon got them from QEMU:
// a video frame its capture time, an audio packet the time its first sample
// started "playing". Nothing is muxed; this is what keeps them in sync:
//
//  clock   off = min(arrival - host time) over the last 5-10 s, for both
//          streams. Unknown network delay and clock offset are folded into
//          it; they are the same for audio and video, so they cancel out.
//  audio   delay = (when a sample is heard) - (its host time + off), from
//          the worklet's reports and AudioContext.getOutputTimestamp().
//  video   delay = (when a frame is drawn) - (its capture time + off).
//
// Lip sync off (default): video is drawn the moment it decodes, audio keeps
// the smallest jitter buffer that doesn't underrun; the A/V offset is only
// shown in the stats. Lip sync on: decoded frames wait until their audio is
// heard (audio usually lags by its jitter buffer), and if video is the later
// one, the audio buffer grows to match it.
const MODE = { OFF: 0, OPUS: 1, PCM: 2 };
const C_AUDIO = 0x0b;
const RATE = 48000;

const store = {
    get(k, d) { try { const v = localStorage.getItem('pve-h264-' + k); return v === null ? d : v; } catch { return d; } },
    set(k, v) { try { localStorage.setItem('pve-h264-' + k, v); } catch { /* ignore */ } },
};

let sendFn = () => {};
let uiFn = () => {};
let server = { available: false, reason: 'starting' };
let wsUp = false;
let pref = store.get('audio', '1') === '1';
let volume = Math.min(1, Math.max(0, parseFloat(store.get('audio-vol', '1')) || 0));
let lipSync = store.get('lipsync', '0') === '1';
let opusOk = null; // AudioDecoder can decode Opus (probed)

let ctx = null, node = null, gain = null, ctxStarting = null;
let decoder = null;
// Opus chunks in the decoder: { vts, ts }. Chromium's AudioDecoder ignores
// the timestamps of all chunks but the first and counts samples from there,
// so every gap in the guest's audio (silence, a stall, the daemon
// re-anchoring) used to be lost and the host times drifted further behind
// with each one (A/V offsets of minutes, "jitter" of seconds). The decoder
// gets gap-free virtual timestamps instead (cumulative samples, which either
// behaviour hands back unchanged) and the host time is looked up here.
const decQ = [];
let decSamples = 0, decLast = null;
let sentMode = -1;

// clock + delay estimates (ms, performance.now() timebase)
const clk = { cur: Infinity, prev: Infinity, since: 0 };
// jitter: worst one-way delay excess per second, for the last 30 s. Spikes
// recur (keyframes holding audio back on a slow link, browser GC pauses,
// guest bursts): a short memory would shrink the buffer between them and
// drop out every time.
// Measured where audio enters the buffer (after the decoder), as the spread
// (max - min) of host-time-to-buffer delays.
const JIT_SECS = 30;
const jit = { hi: new Float64Array(JIT_SECS).fill(-Infinity), lo: new Float64Array(JIT_SECS).fill(Infinity),
    pos: 0, cur: -Infinity, curLo: Infinity, since: 0, peak: 0, min: Infinity };
const dropLog = []; // recent dropouts, for the debug hook
let aDelay = 0, aDelayAt = 0;      // audio: host time -> heard
let vDelay = 0, vNat = 0, vAt = 0; // video: host time -> drawn / -> decoded
let extra = 0;                     // lip sync: added audio buffering
let penalty = 0;                   // after real underruns
let target = 30, sentTarget = 0;
let fill = 0, lastUnder = 0, underruns = 0, underTotal = 0, lastPacket = 0, packets = 0, bytes = 0;
let streamStart = 0; // packets (re)started flowing: startup dropouts don't count
let pktEnd = 0;                      // host time (µs) where the last packet ends
const srcGaps = [];                  // when the source itself paused (recent)
const pendUnder = [];                // underruns not judged yet: { at, flowing }
let gapsPlayed = 0;                  // worklet: timestamp gaps played as silence
let audioOnAt = 0;   // sound turned on: no jitter history yet, start safe
let workletPlaying = false;
let speed = 1, peakFill = 0;       // worklet: playback rate (drift control), peak fill
let srcRate = 1;                   // worklet: source rate / 48 kHz, from the timestamps
let burst = 0;                     // worklet: ms the output device pulls in one go

function clockObserve(hostUs, now) {
    const o = now - hostUs / 1000;
    if (now - clk.since > 5000) { clk.prev = clk.cur; clk.cur = o; clk.since = now; }
    else if (o < clk.cur) clk.cur = o;
}
function clockOff() { return Math.min(clk.cur, clk.prev); }

// ------------------------------------------------------------------ video side
export function observeVideo(capUs) { clockObserve(capUs, performance.now()); }

// ms a decoded frame should wait for its audio (0 = draw now); also records
// the frame's natural (unheld) delay
export function holdMs(capUs) {
    const now = performance.now();
    const base = capUs / 1000 + clockOff();
    const d = now - base;
    vNat = vAt && now - vAt < 1000 ? vNat * 0.9 + d * 0.1 : d;
    if (!lipSync || !audioActive()) return 0;
    return Math.min(300, base + aDelay - now);
}

export function videoDrawn(capUs) {
    const now = performance.now();
    const d = now - (capUs / 1000 + clockOff());
    vDelay = vAt && now - vAt < 1000 ? vDelay * 0.9 + d * 0.1 : d;
    vAt = now;
}

function audioActive() { return workletPlaying && performance.now() - aDelayAt < 300; }

// ------------------------------------------------------------------ packets
export function onPacket(dv, off, len) {
    const now = performance.now();
    const seq = dv.getUint32(off, true); // eslint-disable-line no-unused-vars
    const ts = Number(dv.getBigUint64(off + 4, true));
    const codec = dv.getUint8(off + 12);
    const ch = dv.getUint8(off + 13);
    const n = dv.getUint16(off + 14, true);
    const data = new Uint8Array(dv.buffer, dv.byteOffset + off + 16, len - 16);
    clockObserve(ts, now);
    if (now - lastPacket > 500) { streamStart = now; pktEnd = 0; }
    // the source itself paused (the guest stopped its stream, QEMU stalled):
    // the daemon's timestamps jump. Running dry over such a gap is not a
    // dropout the buffer could have prevented.
    if (pktEnd && ts - pktEnd > 15000) { srcGaps.push(now); if (srcGaps.length > 8) srcGaps.shift(); }
    pktEnd = ts + n * 1e6 / RATE;
    lastPacket = now; packets++; bytes += len;
    if (!node || sentMode === MODE.OFF) return;
    if (codec === MODE.PCM) {
        const l = new Float32Array(n), r = new Float32Array(n);
        const s = new DataView(data.buffer, data.byteOffset, data.byteLength);
        for (let i = 0; i < n; i++) {
            l[i] = s.getInt16(i * ch * 2, true) / 32768;
            r[i] = s.getInt16((i * ch + (ch > 1 ? 1 : 0)) * 2, true) / 32768;
        }
        post(l, r, ts);
    } else if (codec === MODE.OPUS) {
        if (!decoder || decoder.state === 'closed') openDecoder();
        const vts = Math.round(decSamples * 1e6 / RATE);
        decSamples += n;
        decQ.push({ vts, ts });
        if (decQ.length > 200) decQ.shift(); // decoder stuck: don't grow forever
        try {
            decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: vts, data }));
        } catch (e) { console.warn('audio decode', e); decoder = null; }
    }
}

function post(l, r, ts) {
    const now = performance.now();
    node.port.postMessage({ l, r, ts }, [l.buffer, r.buffer]);
    if (!ts) return;
    const d = now - ts / 1000; // + an unknown constant, cancelled by max - min
    if (now - jit.since >= 1000) {
        jit.hi[jit.pos] = jit.cur; jit.lo[jit.pos] = jit.curLo;
        jit.pos = (jit.pos + 1) % JIT_SECS;
        jit.cur = -Infinity; jit.curLo = Infinity; jit.since = now;
        jit.min = Math.min(...jit.lo);
    }
    if (d < jit.curLo) jit.curLo = d;
    if (d < jit.min) jit.min = d;
    if (d > jit.cur) jit.cur = d;
    const spread = Math.max(jit.cur, ...jit.hi) - jit.min;
    if (Number.isFinite(spread) && Math.abs(spread - jit.peak) >= 1) {
        const grew = spread > jit.peak;
        jit.peak = spread;
        if (grew) updateTarget();
    }
}

// host time of a decoded chunk from its virtual timestamp
function hostTs(vts) {
    while (decQ.length && decQ[0].vts < vts - 2000) decQ.shift(); // chunks the decoder dropped
    let e = null;
    if (decQ.length && decQ[0].vts <= vts + 2000) e = decQ.shift();
    else if (decLast) e = decLast; // not ours (e.g. a decoder flushing extra output): extrapolate
    if (!e) return 0;
    decLast = e;
    return e.ts + (vts - e.vts);
}

function openDecoder() {
    decQ.length = 0; decSamples = 0; decLast = null;
    decoder = new AudioDecoder({
        output: (ad) => {
            const n = ad.numberOfFrames, ts = hostTs(ad.timestamp);
            const l = new Float32Array(n), r = new Float32Array(n);
            try {
                ad.copyTo(l, { planeIndex: 0, format: 'f32-planar' });
                ad.copyTo(r, { planeIndex: ad.numberOfChannels > 1 ? 1 : 0, format: 'f32-planar' });
            } catch {
                // no format conversion in copyTo: interleaved f32 / s16
                const k = ad.numberOfChannels, size = ad.allocationSize({ planeIndex: 0 });
                const buf = new ArrayBuffer(size);
                ad.copyTo(buf, { planeIndex: 0 });
                const f = ad.format.startsWith('s16') ? new Int16Array(buf) : new Float32Array(buf);
                const sc = f instanceof Int16Array ? 1 / 32768 : 1;
                for (let i = 0; i < n; i++) { l[i] = f[i * k] * sc; r[i] = f[i * k + (k > 1 ? 1 : 0)] * sc; }
            }
            ad.close();
            if (node && sentMode !== MODE.OFF) post(l, r, ts);
        },
        error: (e) => { console.warn('audio decoder error', e); decoder = null; },
    });
    decoder.configure({ codec: 'opus', sampleRate: RATE, numberOfChannels: 2 });
}

// ------------------------------------------------------------------ worklet reports
function onReport(m) {
    const now = performance.now();
    fill = m.fill;
    workletPlaying = m.playing;
    if (m.speed) { speed = m.speed; peakFill = m.peak; }
    if (m.src) srcRate = m.src;
    if (m.burst !== undefined && Math.abs(m.burst - burst) >= 1) { burst = m.burst; updateTarget(); }
    if (m.gaps !== undefined) gapsPlayed = m.gaps;
    if (m.under > lastUnder) {
        pendUnder.push({ at: now, flowing: now - lastPacket < 60 && now - streamStart > 3000 });
        lastUnder = m.under;
    }
    // an underrun while packets keep coming means the buffer is too small;
    // running dry while the source itself paused does not (its timestamps
    // jump, which pve-h264d may only decide up to 30 ms after the first
    // packet that follows, so each one is judged a little later)
    while (pendUnder.length && now - pendUnder[0].at > 300) {
        const u = pendUnder.shift();
        if (u.flowing && !srcGaps.some((g) => Math.abs(g - u.at) < 300)) {
            penalty = Math.min(penalty + 15, 100); underruns++; underTotal++;
            dropLog.push({ at: Math.round(u.at - streamStart), target: Math.round(target), jitter: Math.round(jit.peak) });
            if (dropLog.length > 20) dropLog.shift();
        }
    }
    if (m.playing && m.ts) {
        let heard;
        const o = ctx.getOutputTimestamp ? ctx.getOutputTimestamp() : null;
        if (o && o.contextTime > 0 && o.performanceTime > 0) heard = o.performanceTime + (m.ctime - o.contextTime) * 1000;
        else heard = now + (m.ctime - ctx.currentTime) * 1000 + ((ctx.outputLatency || 0) + (ctx.baseLatency || 0)) * 1000;
        const d = heard - (m.ts / 1000 + clockOff());
        aDelay = now - aDelayAt < 500 ? aDelay * 0.8 + d * 0.2 : d;
        aDelayAt = now;
    }
    // lip sync: video later than audio -> buffer more audio (the other way
    // round, frames are held in holdMs)
    if (lipSync && audioActive() && now - vAt < 500) {
        const want = Math.max(0, Math.min(200, vNat - (aDelay - extra)));
        extra += (want - extra) * 0.1;
    } else if (!lipSync || now - vAt > 2000) {
        extra = 0;
    }
    updateTarget();
}

function updateTarget() {
    // worst recent jitter + a packet and a render quantum or two; for the
    // first 10 s (decoder warm-up, nothing measured yet) at least 60 ms.
    // Plus what the output device takes in one pull beyond a quantum: the
    // buffer must still hold that much when a late packet is due.
    const floor = performance.now() - audioOnAt < 10000 ? 60 : 30;
    const dev = Math.min(150, Math.max(0, burst - 3));
    target = Math.max(floor, Math.min(250, jit.peak + 12 + penalty)) + dev + extra;
    if (node && Math.abs(target - sentTarget) >= 2) {
        sentTarget = target;
        node.port.postMessage({ target });
    }
}
setInterval(() => { penalty = Math.max(0, penalty - 0.5); }, 1000);

// ------------------------------------------------------------------ context / mode
async function probeOpus() {
    if (!window.AudioDecoder) return false;
    try {
        const s = await AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: RATE, numberOfChannels: 2 });
        return !!s.supported;
    } catch { return false; }
}

// must run inside a user gesture the first time (autoplay policy)
function startContext() {
    if (!window.AudioContext || !window.AudioWorkletNode) return;
    if (!ctx) {
        try { ctx = new AudioContext({ latencyHint: 'interactive', sampleRate: RATE }); }
        catch { ctx = new AudioContext({ latencyHint: 'interactive' }); }
        ctx.onstatechange = update;
        const url = new URL('./audio-worklet.js' + new URL(import.meta.url).search, import.meta.url);
        ctxStarting = ctx.audioWorklet.addModule(url).then(() => {
            node = new AudioWorkletNode(ctx, 'h264-audio-player', { numberOfInputs: 0, outputChannelCount: [2] });
            node.port.onmessage = (e) => onReport(e.data);
            gain = ctx.createGain();
            gain.gain.value = volume;
            node.connect(gain).connect(ctx.destination);
            sentTarget = 0;
            updateTarget();
            ctxStarting = null;
            update();
        }).catch((e) => { console.warn('audio worklet', e); ctxStarting = null; });
    }
    if (ctx.state === 'suspended') ctx.resume().catch(() => {});
}

function wantedMode() {
    if (!wsUp || !pref || !server.available || !node || ctx.state !== 'running') return MODE.OFF;
    return opusOk && server.opus !== false ? MODE.OPUS : MODE.PCM;
}

function update() {
    const m = wantedMode();
    if (m !== sentMode && wsUp) {
        sendFn(C_AUDIO, new Uint8Array([m]));
        if (m !== MODE.OFF && sentMode <= MODE.OFF) audioOnAt = performance.now();
        sentMode = m;
        if (m !== MODE.OPUS && decoder) { try { decoder.close(); } catch { /* closed */ } decoder = null; }
        if (m === MODE.OFF && node) node.port.postMessage({ reset: 1 });
    }
    uiFn(uiState());
}

export function uiState() {
    return {
        available: server.available, reason: server.reason, pref,
        supported: !!(window.AudioContext && window.AudioWorkletNode),
        running: !!(ctx && ctx.state === 'running' && node), playing: !!server.playing,
        mode: sentMode > 0 ? sentMode : 0,
    };
}

export function init({ send, onUi }) {
    sendFn = send;
    uiFn = onUi;
    probeOpus().then((ok) => { opusOk = ok; update(); });
    // the browser only lets audio start from a user gesture: any click or
    // key press in the console counts
    const gesture = () => { if (pref && server.available !== false) startContext(); };
    document.addEventListener('pointerdown', gesture, true);
    document.addEventListener('keydown', gesture, true);
    update();
}

export function serverState(s) {
    server = s;
    update();
}
export function connected() { wsUp = true; sentMode = -1; update(); }
export function disconnected() {
    wsUp = false; sentMode = MODE.OFF;
    if (node) node.port.postMessage({ reset: 1 });
    if (decoder) { try { decoder.close(); } catch { /* closed */ } decoder = null; }
    uiFn(uiState());
}

export function toggle() {
    pref = !pref;
    store.set('audio', pref ? '1' : '0');
    if (pref) startContext(); // a click: allowed to start audio
    update();
}
export function setVolume(v) {
    volume = Math.min(1, Math.max(0, v));
    store.set('audio-vol', String(volume));
    if (gain) gain.gain.value = volume;
}
export function getVolume() { return volume; }
export function setLipSync(on) { lipSync = !!on; store.set('lipsync', lipSync ? '1' : '0'); if (!lipSync) extra = 0; updateTarget(); }
export function getLipSync() { return lipSync; }

export function statsLine() {
    const kbps = Math.round(bytes * 8 / 1000);
    const pk = packets;
    bytes = 0; packets = 0;
    if (!server.available) return '';
    if (sentMode <= 0) return pref ? (server.playing ? 'audio: click the console to start sound' : '') : 'audio: muted';
    if (!pk && !audioActive()) return `audio ${sentMode === MODE.OPUS ? 'opus' : 'pcm'}: guest silent`;
    let s = `audio ${sentMode === MODE.OPUS ? 'opus' : 'pcm'} ${kbps} kbit/s  buffer ${fill.toFixed(0)}/${target.toFixed(0)} ms`;
    s += `  jitter ${jit.peak.toFixed(0)} ms`;
    if (burst > 6) s += `  device ${burst.toFixed(0)} ms`;
    if (Math.abs(speed - 1) >= 0.0005) s += `  rate ${speed > 1 ? '+' : ''}${((speed - 1) * 100).toFixed(1)} %`;
    if (srcRate !== 1) s += `  source ${srcRate > 1 ? '+' : ''}${((srcRate - 1) * 100).toFixed(1)} %`;
    if (underTotal) s += `  dropouts ${underTotal}${underruns ? ' (+' + underruns + ')' : ''}`;
    if (audioActive() && performance.now() - vAt < 1000) {
        const av = aDelay - vDelay;
        s += `\nA/V ${av >= 0 ? '+' : ''}${av.toFixed(0)} ms (${Math.abs(av) < 15 ? 'in sync' : av > 0 ? 'audio later' : 'video later'})` +
            `  lip sync ${lipSync ? 'on' : 'off'}  audio delay ${aDelay.toFixed(0)} ms`;
    }
    underruns = 0;
    return s;
}

export const debug = {
    get aDelay() { return aDelay; }, get vDelay() { return vDelay; }, get vNat() { return vNat; },
    get extra() { return extra; }, get target() { return target; }, get fill() { return fill; },
    get active() { return audioActive(); }, get mode() { return sentMode; }, get opusOk() { return opusOk; },
    get underTotal() { return lastUnder; }, get dropLog() { return dropLog; }, get dropouts() { return underTotal; }, get jitter() { return jit.peak; }, get penalty() { return penalty; },
    get speed() { return speed; }, get srcRate() { return srcRate; }, get burst() { return burst; }, get peakFill() { return peakFill; },
    get ctxState() { return ctx ? ctx.state : 'none'; }, get server() { return server; },
    get clockOff() { return clockOff(); }, get gapsPlayed() { return gapsPlayed; }, get srcGaps() { return srcGaps.length; },
    get latency() {
        if (!ctx) return null;
        const o = ctx.getOutputTimestamp ? ctx.getOutputTimestamp() : {};
        return { output: ctx.outputLatency, base: ctx.baseLatency, rate: ctx.sampleRate,
            ctxAhead: (ctx.currentTime - (o.contextTime || 0)) * 1000, perfAge: performance.now() - (o.performanceTime || 0) };
    },
};
