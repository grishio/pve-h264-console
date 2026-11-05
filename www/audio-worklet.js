// SPDX-License-Identifier: MIT
// PVE H.264 console: audio playout (AudioWorklet, audio rendering thread).
//
// A ring buffer of 48 kHz stereo float samples fed by the main thread
// (www/audio.js), played with a small jitter buffer:
//  - (re)starts once `target` ms are buffered,
//  - keeps the buffer at target by playing slightly slower or faster
//    (at most ±0.5 %, ~9 cents, inaudible; up to +1 % to drain a burst).
//    The level it steers is the *peak* fill of the last 1-2 s (the fill right
//    after on-time packets), so late packets don't make it speed up or slow
//    down, while clock drift between the host and this audio device, or a
//    target that grew, is absorbed without a dropout,
//  - jumps ahead only beyond target + 300 ms, crossfading old and new (in
//    the first second after a (re)start already beyond target + 20 ms: the
//    rest of a backlog that arrives after a stall would otherwise leave the
//    buffer, and the A/V offset, a few hundred ms too high for half a minute),
//  - a gap in the host timestamps (the guest produced nothing for a while,
//    pve-h264d's clock jumped) is played as silence, faded out and in, so
//    the buffer keeps its level and the audio after it its timing; before,
//    every such gap drained the buffer for good, the rate control could only
//    refill it at 0.5 % and the next few gaps ran it dry,
//  - measures how bursty the device pulls audio (some render 10-85 ms in one
//    go instead of a quantum every 2.7 ms): the buffer has to hold a whole
//    burst on top of the network jitter, so the main thread adds it to target,
//  - every level change is a gain ramp (5 ms): it fades out just before the
//    buffer runs dry and back in when it refills, so dropouts never click.
//  - plays at the rate the source really delivers, measured from the host
//    timestamps over 5-20 s (frames written / host time they cover). A
//    VM started before timer-period=5000 was added to its -audiodev gets
//    ~2-3 % fewer samples than 48 kHz from QEMU; the ±0.5 % above can't
//    follow that and ran dry every few seconds. Shown as "source" in the stats.
// Resampling (drift control, a context not at 48 kHz) uses 4-point Hermite
// interpolation, which keeps the treble that linear interpolation dulls.
// Every chunk carries the host timestamp (pve-h264d, CLOCK_MONOTONIC) of its
// first sample; reports say which host time is being rendered at which
// context time, so the main thread knows the end-to-end audio delay.
const SRC_RATE = 48000;
const RING = SRC_RATE * 2; // 2 s
const FADE = 240;          // 5 ms ramps: no clicks at dropouts / restarts / skips
const MS = SRC_RATE / 1000;

class H264AudioPlayer extends AudioWorkletProcessor {
    constructor() {
        super();
        this.L = new Float32Array(RING);
        this.R = new Float32Array(RING);
        this.w = 0;          // absolute write position (frames)
        this.r = 0;          // absolute read position (fractional)
        this.chunks = [];    // { start, ts } host time of frame `start`
        this.target = 30 * MS;
        this.playing = false;
        this.ratio = SRC_RATE / sampleRate; // context may not run at 48 kHz
        this.g = 0;          // output gain ramp, 0..1
        this.xr = 0;         // crossfade (skip): old read position
        this.xf = 0;         //   frames of crossfade left
        this.speed = 1;
        this.peakCur = 0;    // max fill after a push, this window / the last
        this.peakPrev = 0;
        this.peakSince = 0;
        this.lead = { lo: Infinity, hi: -Infinity, since: 0, win: new Float64Array(10), pos: 0 };
        this.burst = 0;      // ms: worst render burst (device pull) of the last 10 s
        this.under = 0;
        this.skips = 0;
        this.end = 0;        // host time where the last chunk ends (µs)
        this.fadeIn = 0;     // frames of the next chunk still to fade in
        this.gaps = 0;
        this.hist = [];      // [w, end] about once a second: the source's real rate
        this.histAt = 0;
        this.src = 1;        // source rate / 48 kHz
        this.lastReport = 0;
        this.startAt = -1;   // context time playback last (re)started
        this.port.onmessage = (e) => {
            const m = e.data;
            if (m.l) this.push(m);
            else if (m.target !== undefined) this.target = Math.max(240, Math.round(m.target * MS));
            else if (m.reset) { this.w = this.r = 0; this.chunks = []; this.playing = false; this.g = 0; this.xf = 0; this.end = 0; this.hist = []; }
        };
    }

    // a gap of k frames before the next chunk: fade out what is buffered, then silence
    silence(k) {
        const f = Math.min(FADE, Math.floor(this.w - this.r) - 3);
        for (let i = 0; i < f; i++) {
            const p = (this.w - f + i) % RING, g = (f - 1 - i) / f;
            this.L[p] *= g; this.R[p] *= g;
        }
        for (let i = 0; i < k; i++) {
            const p = (this.w + i) % RING;
            this.L[p] = this.R[p] = 0;
        }
        this.chunks.push({ start: this.w, ts: this.end });
        this.w += k;
        this.fadeIn = FADE;
        this.gaps++;
    }

    push(m) {
        const n = m.l.length;
        if (this.end && m.ts) {
            const gap = Math.round((m.ts - this.end) * SRC_RATE / 1e6);
            // 5 ms+: pve-h264d's clock only moves by < 1.5 ms otherwise
            if (gap >= 5 * MS && gap <= 300 * MS && this.playing && this.w + gap + n - this.r < RING - 512) this.silence(gap);
            else if (Math.abs(gap) >= 5 * MS) this.hist = []; // frames and host time part ways: start over
        }
        if (m.ts) this.end = m.ts + n * 1e6 / SRC_RATE;
        if (this.w + n - this.r > RING - 512) { // overflow (renderer stalled ~2 s): keep the newest
            this.r = this.w + n - this.target;
            this.g = 0; this.xf = 0;
            this.skips++;
        }
        for (let i = 0; i < n; i++) {
            const k = (this.w + i) % RING;
            this.L[k] = m.l[i];
            this.R[k] = m.r[i];
        }
        for (let i = 0; this.fadeIn > 0 && i < n; i++, this.fadeIn--) {
            const k = (this.w + i) % RING, g = 1 - this.fadeIn / FADE;
            this.L[k] *= g; this.R[k] *= g;
        }
        this.chunks.push({ start: this.w, ts: m.ts });
        this.w += n;
        if (m.ts && currentTime - this.histAt >= 1) this.measureSource();
        const fill = this.w - this.r;
        if (fill > this.peakCur) this.peakCur = fill;
    }

    tsAt(pos) {
        const c = this.chunks;
        while (c.length > 1 && c[1].start <= pos) c.shift();
        if (!c.length) return 0;
        return c[0].ts + (pos - c[0].start) * 1e6 / SRC_RATE;
    }

    // 4-point Hermite at absolute position pos (needs pos - 1 .. pos + 2)
    sample(buf, pos) {
        const p = Math.floor(pos), x = pos - p;
        const b = buf[p % RING];
        if (x === 0) return b;
        const a = buf[(p + RING - 1) % RING], c = buf[(p + 1) % RING], d = buf[(p + 2) % RING];
        const c1 = 0.5 * (c - a), c2 = a - 2.5 * b + 2 * c - 0.5 * d, c3 = 0.5 * (d - a) + 1.5 * (b - c);
        return ((c3 * x + c2) * x + c1) * x + b;
    }

    rate() {
        // drift / target changes: steer the peak fill of the last 1-2 s into
        // [target, target + 6 ms]; a burst far above target drains faster
        if (currentTime - this.peakSince >= 1) {
            this.peakPrev = this.peakCur;
            this.peakCur = this.w - this.r;
            this.peakSince = currentTime;
        }
        const pe = Math.max(this.peakCur, this.peakPrev) - this.target;
        let s = 1;
        if (pe < 0) s = 1 - Math.min(0.005, -pe / SRC_RATE * 0.25);
        else if (pe > 6 * MS) s = 1 + Math.min(0.005, (pe - 6 * MS) / SRC_RATE * 0.25);
        const excess = this.w - this.r - this.target;
        const slack = Math.max(20 * MS, this.target * 0.5);
        if (excess > slack) s = Math.max(s, 1 + Math.min(0.01, (excess - slack) / SRC_RATE * 0.1));
        this.speed = s;
    }

    // frames per host second over the last 5-20 s, relative to 48 kHz
    measureSource() {
        this.histAt = currentTime;
        const h = this.hist;
        h.push([this.w, this.end]);
        if (h.length > 21) h.shift();
        if (h.length < 6) return;
        const est = (h[h.length - 1][0] - h[0][0]) / ((h[h.length - 1][1] - h[0][1]) * SRC_RATE / 1e6);
        if (est > 0.9 && est < 1.1) this.src = Math.abs(est - 1) > 0.002 ? est : 1; // else: clock drift, for rate()
    }

    // How far rendering runs ahead of the wall clock varies by one device
    // pull: spread (max - min) per second, worst of the last 10 s.
    measureBursts() {
        const ld = this.lead;
        const wall = typeof performance !== 'undefined' ? performance.now() : Date.now();
        const lead = currentTime * 1000 - wall;
        if (lead < ld.lo) ld.lo = lead;
        if (lead > ld.hi) ld.hi = lead;
        if (ld.hi - ld.lo > this.burst) this.burst = ld.hi - ld.lo; // grow at once
        if (currentTime - ld.since >= 1) {
            if (ld.since) {
                ld.win[ld.pos] = ld.hi - ld.lo;
                ld.pos = (ld.pos + 1) % ld.win.length;
                this.burst = Math.max(...ld.win);
            }
            ld.lo = ld.hi = lead; ld.since = currentTime;
        }
    }

    process(inputs, outputs) {
        this.measureBursts();
        const out = outputs[0];
        const oL = out[0], oR = out[1] || out[0];
        const n = oL.length;
        let ts = 0;
        if (!this.playing && this.w - this.r >= this.target) {
            this.playing = true;
            this.g = 0; this.xf = 0;
            if (this.w - this.r > this.target + 480) this.r = this.w - this.target; // start right at target
            this.peakCur = this.peakPrev = this.w - this.r;
            this.peakSince = currentTime;
            this.startAt = currentTime;
        }
        if (!this.playing) {
            oL.fill(0);
            if (oR !== oL) oR.fill(0);
        } else {
            const jump = currentTime - this.startAt < 1 ? 20 * MS : 300 * MS;
            if (this.w - this.r - this.target > jump && !this.xf) {
                this.xr = this.r; this.xf = FADE; // jump ahead, crossfading
                this.r = this.w - this.target;
                this.peakCur = this.peakPrev = this.target;
                this.skips++;
            }
            this.rate();
            const step = this.ratio * this.src * this.speed;
            ts = this.tsAt(this.r);
            const L = this.L, R = this.R;
            let i = 0;
            for (; i < n; i++) {
                // output frames still in the buffer (Hermite reads up to r + 2)
                const left = (this.w - 2 - this.r) / step;
                if (left < 1) break;
                // ramp: down so it reaches 0 exactly when the data runs out,
                // otherwise back up towards 1
                if (left < FADE * this.g) this.g = left / FADE;
                else if (this.g < 1) this.g = Math.min(1, this.g + 1 / FADE);
                let l = this.sample(L, this.r), r = this.sample(R, this.r);
                if (this.xf) {
                    const k = this.xf / FADE;
                    l = l * (1 - k) + this.sample(L, this.xr) * k;
                    r = r * (1 - k) + this.sample(R, this.xr) * k;
                    this.xr += step; this.xf--;
                }
                oL[i] = l * this.g;
                oR[i] = r * this.g;
                this.r += step;
            }
            if (i < n) { // underrun (already faded out): silence, rebuffer
                for (; i < n; i++) oL[i] = oR[i] = 0;
                this.playing = false;
                this.g = 0; this.xf = 0;
                this.under++;
            }
        }
        if (currentTime - this.lastReport >= 0.04) {
            this.lastReport = currentTime;
            this.port.postMessage({ ts, ctime: currentTime, playing: this.playing,
                fill: (this.w - this.r) * 1000 / SRC_RATE, under: this.under, skips: this.skips, gaps: this.gaps,
                speed: this.speed, src: this.src, burst: this.burst, peak: Math.max(this.peakCur, this.peakPrev) * 1000 / SRC_RATE });
        }
        return true;
    }
}

registerProcessor('h264-audio-player', H264AudioPlayer);
