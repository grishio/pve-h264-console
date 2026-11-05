/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - guest audio: QEMU's D-Bus audio backend -> Opus / PCM -> viewers
 *
 *  QEMU  -audiodev dbus,id=X  +  -display dbus,p2p=yes,audiodev=X
 *     | org.qemu.Display1.Audio.RegisterOutListener(fd)  (on the display link)
 *     v  private p2p D-Bus connection, QEMU calls our AudioOutListener:
 *        Init / Fini / SetEnabled / SetVolume / Write(id, PCM)
 *  audio thread (own GMainContext, never blocks the display main loop)
 *     | swresample -> 48 kHz s16 stereo, guest volume, 10 ms frames
 *     | Opus (libopus via libavcodec, restricted low delay) and/or raw PCM,
 *     | only for the modes viewers asked for (C_AUDIO)
 *     v  S_AUDIO, timestamped with now_us() like video frames (cap_us)
 *
 * Timestamps: QEMU's backend is paced by its own real-time rate control and
 * calls Write() when a buffer is full, the moment a sound card would start
 * playing it. Each packet carries that host time for its first sample (kept
 * continuous between packets, slowly pulled towards the arrival times), on
 * the same CLOCK_MONOTONIC as the video frames' capture time. The viewer
 * lines both up on its side (www/audio.js), which is what lip sync needs;
 * muxing both into one container would add buffering and nothing else.
 */
#define _GNU_SOURCE
#include "pve-h264d.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#define AUDIO_PATH "/org/qemu/Display1/Audio"
#define AUDIO_IFACE "org.qemu.Display1.Audio"
#define OUT_LISTENER_PATH "/org/qemu/Display1/AudioOutListener"
#define OPUS_KBPS 128
#define FRAME_US (AUDIO_FRAME * 1000000LL / AUDIO_RATE)
#define FIFO_FRAMES (AUDIO_RATE / 2) /* 500 ms of converted audio */

/* ------------------------------------------------------------------ */
/* state shown to viewers (a_lock)                                     */
/* ------------------------------------------------------------------ */
static pthread_mutex_t a_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool available;
    bool playing;
    const char *reason; /* why not available */
    int src_rate, src_channels;
} st = {.reason = "starting"};
static atomic_int a_bytes;
static atomic_bool a_opus_ok = true;

char *audio_state_json(void)
{
    pthread_mutex_lock(&a_lock);
    char *s = g_strdup_printf(
        "{\"available\":%s,\"playing\":%s,\"reason\":\"%s\",\"rate\":%d,\"channels\":%d,"
        "\"src_rate\":%d,\"src_channels\":%d,\"opus\":%s,\"frame_ms\":%d}",
        st.available ? "true" : "false", st.playing ? "true" : "false",
        st.available ? "" : st.reason, AUDIO_RATE, AUDIO_CHANNELS, st.src_rate,
        st.src_channels, atomic_load(&a_opus_ok) ? "true" : "false",
        (int)(FRAME_US / 1000));
    pthread_mutex_unlock(&a_lock);
    return s;
}

int audio_kbps(void)
{
    return atomic_exchange(&a_bytes, 0) * 8 / 1000;
}

static void set_state(bool available, const char *reason)
{
    pthread_mutex_lock(&a_lock);
    bool changed = st.available != available || (!available && st.reason != reason);
    st.available = available;
    st.reason = reason;
    if (!available)
        st.playing = false;
    pthread_mutex_unlock(&a_lock);
    if (changed)
        audio_state_changed();
}

/* ------------------------------------------------------------------ */
/* voices (audio thread only)                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    bool used;
    uint64_t id;
    bool enabled;
    int bits, channels, bpf;
    bool is_signed, is_float, be;
    uint32_t freq;
    float gain[AUDIO_CHANNELS]; /* guest volume per output channel */
    float cur[AUDIO_CHANNELS];  /* gain applied now: ramps to gain (no clicks) */
    SwrContext *swr;
} Voice;

#define MAX_VOICES 8
static Voice voices[MAX_VOICES];
static uint64_t g_active;      /* voice whose data we forward */
static int64_t g_active_us;    /* its last Write */

static Voice *voice_find(uint64_t id)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].id == id)
            return &voices[i];
    return NULL;
}

static void voice_free(Voice *v)
{
    swr_free(&v->swr);
    memset(v, 0, sizeof(*v));
}

static void update_playing(void)
{
    bool playing = false;
    int rate = 0, ch = 0;
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].enabled) {
            playing = true;
            rate = (int)voices[i].freq;
            ch = voices[i].channels;
        }
    pthread_mutex_lock(&a_lock);
    bool changed = st.playing != playing || (playing && st.src_rate != rate);
    st.playing = playing;
    if (playing) {
        st.src_rate = rate;
        st.src_channels = ch;
    }
    pthread_mutex_unlock(&a_lock);
    if (changed)
        audio_state_changed();
}

static bool voice_init(Voice *v)
{
    enum AVSampleFormat f;
    if (v->is_float)
        f = v->bits == 64 ? AV_SAMPLE_FMT_DBL : AV_SAMPLE_FMT_FLT;
    else if (v->bits == 8)
        f = AV_SAMPLE_FMT_U8; /* signed 8 bit is flipped to unsigned first */
    else if (v->bits == 16)
        f = AV_SAMPLE_FMT_S16;
    else if (v->bits == 32)
        f = AV_SAMPLE_FMT_S32;
    else
        return false;
    if ((v->is_float && v->bits != 32 && v->bits != 64) || v->channels < 1 ||
        v->channels > 8 || v->freq < 8000 || v->freq > 192000 ||
        v->bpf != v->bits / 8 * v->channels)
        return false;
    AVChannelLayout in_l, out_l = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_default(&in_l, v->channels);
    swr_free(&v->swr);
    if (swr_alloc_set_opts2(&v->swr, &out_l, AV_SAMPLE_FMT_S16, AUDIO_RATE, &in_l, f,
                            (int)v->freq, 0, NULL) < 0 ||
        swr_init(v->swr) < 0) {
        swr_free(&v->swr);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* output: 10 ms frames of 48 kHz s16 stereo                            */
/* ------------------------------------------------------------------ */
static int16_t fifo[FIFO_FRAMES * AUDIO_CHANNELS];
static int fifo_n;        /* frames in fifo */
static int64_t fifo_ts;   /* host time of fifo[0] */
static bool fifo_restart = true; /* next Write starts a new stream: anchor fifo_ts */
static int64_t last_lag;  /* previous Write: arrival - its samples' time */
static int64_t hold_lag;  /* a late Write waits in the fifo: how late it was */
static GSource *hold_timer; /* ... until the next Write, or this fires */
#define LATE_US 20000     /* later than this: a stall or a gap (timer jitter is ~10 ms) */
#define CATCHUP_US 2000   /* the next Write this much less late: a stall catching up */
#define HOLD_MS 25        /* no Write within this: the late one starts after a gap */
#define GAP_US 1000000    /* off by more than this: re-anchor at once */

static void hold_end(void)
{
    if (hold_timer) {
        g_source_destroy(hold_timer);
        g_source_unref(hold_timer);
        hold_timer = NULL;
    }
}

static void fifo_reset(void)
{
    hold_end();
    fifo_n = 0;
    fifo_restart = true;
}
static uint32_t g_seq;
static AVCodecContext *g_opus;
static AVFrame *g_oframe;
static AVPacket *g_opkt;

static void opus_close(void)
{
    avcodec_free_context(&g_opus);
    av_frame_free(&g_oframe);
    av_packet_free(&g_opkt);
}

static bool opus_open(void)
{
    if (g_opus)
        return true;
    if (!atomic_load(&a_opus_ok))
        return false;
    const AVCodec *c = avcodec_find_encoder_by_name("libopus");
    if (!c) {
        LOG("audio: libavcodec has no libopus encoder, viewers get raw PCM");
        goto fail;
    }
    g_opus = avcodec_alloc_context3(c);
    g_opus->sample_rate = AUDIO_RATE;
    g_opus->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&g_opus->ch_layout, AUDIO_CHANNELS);
    g_opus->bit_rate = OPUS_KBPS * 1000;
    AVDictionary *o = NULL;
    av_dict_set(&o, "application", "lowdelay", 0); /* CELT only, 2.5 ms lookahead */
    av_dict_set(&o, "frame_duration", "10", 0);
    av_dict_set(&o, "vbr", "on", 0);
    int r = avcodec_open2(g_opus, c, &o);
    av_dict_free(&o);
    if (r < 0) {
        LOG("audio: opening the Opus encoder failed (%d), viewers get raw PCM", r);
        goto fail;
    }
    g_oframe = av_frame_alloc();
    g_oframe->nb_samples = AUDIO_FRAME;
    g_oframe->format = AV_SAMPLE_FMT_S16;
    g_oframe->sample_rate = AUDIO_RATE;
    av_channel_layout_copy(&g_oframe->ch_layout, &g_opus->ch_layout);
    g_opkt = av_packet_alloc();
    if (av_frame_get_buffer(g_oframe, 0) < 0)
        goto fail;
    DBG("audio: Opus encoder open (%d kbit/s, 10 ms frames)", OPUS_KBPS);
    return true;
fail:
    opus_close();
    atomic_store(&a_opus_ok, false);
    audio_state_changed();
    return false;
}

static void put_hdr(uint8_t *h, uint32_t seq, int64_t ts, int codec, int samples)
{
    uint64_t t = (uint64_t)ts;
    uint16_t n = (uint16_t)samples;
    memcpy(h, &seq, 4);
    memcpy(h + 4, &t, 8);
    h[12] = (uint8_t)codec;
    h[13] = AUDIO_CHANNELS;
    memcpy(h + 14, &n, 2);
}

static void emit(const int16_t *pcm, int64_t ts)
{
    int modes = audio_modes_wanted();
    uint8_t h[16];
    uint32_t seq = g_seq++;
    if (modes & (1 << AUDIO_PCM)) {
        put_hdr(h, seq, ts, AUDIO_PCM, AUDIO_FRAME);
        audio_send(AUDIO_PCM, h, sizeof(h), pcm, AUDIO_FRAME * AUDIO_CHANNELS * 2);
        atomic_fetch_add(&a_bytes, AUDIO_FRAME * AUDIO_CHANNELS * 2 + 21);
    }
    if (!(modes & (1 << AUDIO_OPUS))) {
        if (g_opus)
            opus_close(); /* nobody wants Opus any more */
        return;
    }
    if (!opus_open())
        return;
    if (av_frame_make_writable(g_oframe) < 0)
        return;
    memcpy(g_oframe->data[0], pcm, AUDIO_FRAME * AUDIO_CHANNELS * 2);
    g_oframe->pts = (int64_t)seq * AUDIO_FRAME;
    if (avcodec_send_frame(g_opus, g_oframe) < 0)
        return;
    while (avcodec_receive_packet(g_opus, g_opkt) == 0) {
        put_hdr(h, seq, ts, AUDIO_OPUS, AUDIO_FRAME);
        audio_send(AUDIO_OPUS, h, sizeof(h), g_opkt->data, (size_t)g_opkt->size);
        atomic_fetch_add(&a_bytes, g_opkt->size + 21);
        av_packet_unref(g_opkt);
    }
}

static void fifo_emit(void)
{
    int done = 0;
    while (fifo_n - done >= AUDIO_FRAME) {
        emit(fifo + (size_t)done * AUDIO_CHANNELS, fifo_ts);
        done += AUDIO_FRAME;
        fifo_ts += FRAME_US;
    }
    if (done) {
        memmove(fifo, fifo + (size_t)done * AUDIO_CHANNELS,
                (size_t)(fifo_n - done) * AUDIO_CHANNELS * 2);
        fifo_n -= done;
    }
}

/* nothing followed the late Write: its audio starts after a gap */
static gboolean hold_expired(gpointer ud)
{
    g_source_unref(hold_timer);
    hold_timer = NULL;
    fifo_ts += hold_lag;
    fifo_emit();
    return G_SOURCE_REMOVE;
}

/* Converted samples [fifo_n, fifo_n + n) arrived at host time t.
 *
 * The timestamps are a sample clock: each packet's ts is the previous one +
 * 10 ms, pulled gently (1/16 of the error per Write) towards the arrival
 * times, which jitter by about one QEMU audio timer tick. A Write more than
 * 20 ms late is either
 *  - a stall: QEMU's main loop (or this thread) was held up, e.g. by a
 *    CPU-bound x264 encode. No sample is missing and QEMU hands over the
 *    backlog faster than real time (two timer ticks per 10 ms buffer), so the
 *    next Write comes early. The timestamps stay continuous, so the viewer
 *    sees late packets, which is what its jitter buffer is for; or
 *  - a gap: the guest produced nothing for a while without stopping its
 *    stream. Samples are missing, the next Write comes on time, and the clock
 *    re-anchors at the arrival, so the viewer plays silence for the jump
 *    (www/audio-worklet.js) instead of draining its buffer.
 * The late Write's samples wait in the fifo until the next Write (5-10 ms)
 * tells which it is, so a gap's audio carries its new time from the start. */
static void fifo_commit(int n, int64_t t)
{
    int64_t end = fifo_ts + (int64_t)fifo_n * 1000000 / AUDIO_RATE;
    int64_t lag = t - end;
    if (fifo_restart || lag > GAP_US || lag < -GAP_US) {
        hold_end();
        fifo_ts += lag; /* new stream, long pause, or nonsense: re-anchor */
        fifo_restart = false;
    } else if (hold_timer) {
        if (lag >= hold_lag - CATCHUP_US)
            fifo_ts += lag; /* on time again after the late one: a gap */
        hold_end();         /* else a stall catching up: stays continuous */
    } else if (lag > LATE_US && !(last_lag > LATE_US && lag < last_lag - CATCHUP_US)) {
        hold_lag = last_lag = lag; /* late (and not still catching up): wait and see */
        fifo_n += n;
        hold_timer = g_timeout_source_new(HOLD_MS);
        g_source_set_callback(hold_timer, hold_expired, NULL, NULL);
        g_source_attach(hold_timer, g_main_context_get_thread_default());
        return;
    } else if (lag <= LATE_US) {
        fifo_ts += lag / 16; /* follow the arrival times, smoothly */
    }
    last_lag = lag;
    fifo_n += n;
    fifo_emit();
}

static void on_write(Voice *v, const uint8_t *data, size_t len, int64_t t)
{
    if (!v->swr || !v->enabled)
        return;
    if (!audio_modes_wanted()) {
        fifo_reset(); /* nobody listening */
        return;
    }
    /* Only one voice is forwarded (guests play through one stream at a
     * time). Another one takes over once the current one went quiet. */
    if (g_active != v->id) {
        if (t - g_active_us < 200000 && voice_find(g_active))
            return;
        DBG("audio: forwarding voice %llx", (unsigned long long)v->id);
        g_active = v->id;
        fifo_reset();
    }
    g_active_us = t;

    int n = (int)(len / (size_t)v->bpf);
    if (n <= 0)
        return;
    const uint8_t *in = data;
    uint8_t *tmp = NULL;
    bool swap = v->be != (G_BYTE_ORDER == G_BIG_ENDIAN) && v->bits > 8;
    bool flip = (!v->is_float && v->is_signed == (v->bits == 8));
    if (swap || flip) {
        tmp = g_memdup2(data, (size_t)n * v->bpf);
        int bs = v->bits / 8;
        for (size_t i = 0; i < (size_t)n * v->channels; i++) {
            uint8_t *s = tmp + i * bs;
            if (swap)
                for (int a = 0, b = bs - 1; a < b; a++, b--) {
                    uint8_t x = s[a]; s[a] = s[b]; s[b] = x;
                }
            if (flip) /* signed 8 bit <-> unsigned, unsigned 16/32 <-> signed */
                s[G_BYTE_ORDER == G_BIG_ENDIAN ? 0 : bs - 1] ^= 0x80;
        }
        in = tmp;
    }
    int room = FIFO_FRAMES - fifo_n;
    int want = swr_get_out_samples(v->swr, n);
    if (want > room) {
        /* can't happen while frames are drained right away; be safe */
        fifo_reset();
        room = FIFO_FRAMES;
    }
    uint8_t *out = (uint8_t *)(fifo + (size_t)fifo_n * AUDIO_CHANNELS);
    int got = swr_convert(v->swr, &out, room, &in, n);
    g_free(tmp);
    if (got <= 0)
        return;
    bool unity = true;
    for (int c = 0; c < AUDIO_CHANNELS; c++)
        unity &= v->gain[c] == 1.0f && v->cur[c] == 1.0f;
    if (!unity) {
        /* guest volume; a change glides over ~3 ms instead of stepping,
         * which would click while sound plays */
        int16_t *p = (int16_t *)out;
        for (int i = 0; i < got; i++)
            for (int c = 0; c < AUDIO_CHANNELS; c++) {
                float d = v->gain[c] - v->cur[c];
                v->cur[c] = fabsf(d) < 1e-4f ? v->gain[c] : v->cur[c] + d * (1.0f / 128);
                p[i * AUDIO_CHANNELS + c] =
                    (int16_t)lrintf(p[i * AUDIO_CHANNELS + c] * v->cur[c]);
            }
    }
    fifo_commit(got, t);
}

/* QEMU's HDA amplifiers have 74 steps of 1 dB, scaled to 0..255 */
static float vol_gain(uint8_t v)
{
    return v >= 255 ? 1.0f : v == 0 ? 0.0f : powf(10.0f, (v / 255.0f - 1.0f) * 74.0f / 20.0f);
}

/* ------------------------------------------------------------------ */
/* D-Bus AudioOutListener object                                        */
/* ------------------------------------------------------------------ */
static const char listener_xml[] =
    "<node>"
    " <interface name='org.qemu.Display1.AudioOutListener'>"
    "  <method name='Init'><arg type='t' direction='in'/><arg type='y' direction='in'/>"
    "   <arg type='b' direction='in'/><arg type='b' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='y' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='b' direction='in'/></method>"
    "  <method name='Fini'><arg type='t' direction='in'/></method>"
    "  <method name='SetEnabled'><arg type='t' direction='in'/><arg type='b' direction='in'/></method>"
    "  <method name='SetVolume'><arg type='t' direction='in'/><arg type='b' direction='in'/>"
    "   <arg type='ay' direction='in'/></method>"
    "  <method name='Write'><arg type='t' direction='in'/><arg type='ay' direction='in'/></method>"
    "  <property name='Interfaces' type='as' access='read'/>"
    " </interface>"
    "</node>";

static void listener_method(GDBusConnection *conn, const gchar *sender,
                            const gchar *path, const gchar *iface,
                            const gchar *method, GVariant *params,
                            GDBusMethodInvocation *inv, gpointer ud)
{
    if (!strcmp(method, "Write")) {
        int64_t t = now_us();
        guint64 id;
        g_autoptr(GVariant) data = NULL;
        g_variant_get(params, "(t@ay)", &id, &data);
        gsize len;
        const uint8_t *p = g_variant_get_fixed_array(data, &len, 1);
        Voice *v = voice_find(id);
        if (v)
            on_write(v, p, len, t);
    } else if (!strcmp(method, "Init")) {
        guint64 id;
        guchar bits, ch;
        gboolean is_signed, is_float, be;
        guint32 freq, bpf, bps;
        g_variant_get(params, "(tybbuyuub)", &id, &bits, &is_signed, &is_float, &freq,
                      &ch, &bpf, &bps, &be);
        Voice *v = voice_find(id);
        if (!v)
            for (int i = 0; i < MAX_VOICES && !v; i++)
                if (!voices[i].used)
                    v = &voices[i];
        if (v) {
            bool was_enabled = v->used && v->enabled;
            voice_free(v);
            *v = (Voice){.used = true, .id = id, .enabled = was_enabled, .bits = bits,
                         .channels = ch, .bpf = (int)bpf, .is_signed = is_signed,
                         .is_float = is_float, .be = be, .freq = freq,
                         .gain = {1.0f, 1.0f}, .cur = {1.0f, 1.0f}};
            bool ok = voice_init(v);
            LOG("audio: voice %llx: %u Hz, %u ch, %u bit %s%s%s",
                (unsigned long long)id, freq, ch, bits,
                is_float ? "float" : is_signed ? "signed" : "unsigned",
                be ? " big-endian" : "", ok ? "" : " (unsupported, ignored)");
            if (!ok)
                voice_free(v);
            update_playing();
        }
    } else if (!strcmp(method, "Fini")) {
        guint64 id;
        g_variant_get(params, "(t)", &id);
        Voice *v = voice_find(id);
        if (v) {
            voice_free(v);
            update_playing();
        }
    } else if (!strcmp(method, "SetEnabled")) {
        guint64 id;
        gboolean en;
        g_variant_get(params, "(tb)", &id, &en);
        Voice *v = voice_find(id);
        if (v) {
            DBG("audio: voice %llx %s", (unsigned long long)id, en ? "on" : "off");
            v->enabled = en;
            if (!en && g_active == id)
                fifo_reset();
            update_playing();
        }
    } else if (!strcmp(method, "SetVolume")) {
        guint64 id;
        gboolean mute;
        g_autoptr(GVariant) vol = NULL;
        g_variant_get(params, "(tb@ay)", &id, &mute, &vol);
        gsize n;
        const uint8_t *p = g_variant_get_fixed_array(vol, &n, 1);
        Voice *v = voice_find(id);
        if (v) {
            for (int c = 0; c < AUDIO_CHANNELS; c++)
                v->gain[c] = mute || n == 0 ? 0.0f : vol_gain(p[(size_t)c < n ? c : 0]);
            DBG("audio: voice %llx volume %s %d/%d", (unsigned long long)id,
                mute ? "muted" : "", n ? p[0] : -1, n > 1 ? p[1] : -1);
        }
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *listener_get_prop(GDBusConnection *conn, const gchar *sender,
                                   const gchar *path, const gchar *iface,
                                   const gchar *prop, GError **error, gpointer ud)
{
    if (!strcmp(prop, "Interfaces"))
        return g_variant_new_strv(NULL, 0);
    return NULL;
}

static const GDBusInterfaceVTable listener_vtable = {
    .method_call = listener_method,
    .get_property = listener_get_prop,
};

static void on_closed(GDBusConnection *c, gboolean remote, GError *e, gpointer ud)
{
    LOG("audio: QEMU closed the audio connection");
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used)
            voice_free(&voices[i]);
    set_state(false, "closed");
    g_main_loop_quit(ud);
}

/* ------------------------------------------------------------------ */
/* test mode: 1 kHz beep in the first 100 ms of every second, the same  */
/* moments the test pattern flashes (measures A/V sync end to end).     */
/* PVE_H264D_TEST=...:gaps[=ms] leaves out that much audio at 2.5 s of  */
/* every 3 s (a guest pausing without stopping its stream; default 300) */
/* and :stalls[=ms] holds it back and then hands it over at once, like  */
/* QEMU after its main loop was held up (default 80); :slow[=permille]  */
/* delivers that much less than 48 kHz, like QEMU with a 10 ms audio    */
/* timer period (default 24).                                           */
/* ------------------------------------------------------------------ */
static int64_t g_tone_start, g_tone_done;
static int g_tone_gap_ms, g_tone_stall_ms, g_tone_slow;

static int test_opt_ms(const char *test, const char *name, int def)
{
    const char *p = test ? strstr(test, name) : NULL;
    if (!p)
        return 0;
    int v = def;
    if (p[strlen(name)] == '=')
        v = atoi(p + strlen(name) + 1);
    return v > 0 && v <= 2000 ? v : def;
}

static gboolean tone_tick(gpointer ud)
{
    Voice *v = ud;
    int64_t t = now_us();
    int64_t ph3 = t % 3000000 - 2500000;
    if (g_tone_stall_ms && ph3 >= 0 && ph3 < g_tone_stall_ms * 1000LL)
        return G_SOURCE_CONTINUE; /* held back: handed over afterwards */
    int64_t rate = AUDIO_RATE * (1000 - g_tone_slow) / 1000; /* frames per host second */
    int64_t due = (t - g_tone_start) * rate / 1000000 - g_tone_done;
    if (due < AUDIO_FRAME)
        return G_SOURCE_CONTINUE;
    due -= due % AUDIO_FRAME;
    if (due > AUDIO_RATE / 4) {
        g_tone_done += due - AUDIO_FRAME; /* we were stalled: drop it, no burst */
        due = AUDIO_FRAME;
    }
    if (g_tone_gap_ms && ph3 >= 0 && ph3 < g_tone_gap_ms * 1000LL) {
        g_tone_done += due; /* like a guest pausing its stream: no Write */
        return G_SOURCE_CONTINUE;
    }
    int16_t *buf = g_new(int16_t, (size_t)due * 2);
    for (int64_t i = 0; i < due; i++) {
        /* the host time this sample is due (a backlog: in the past) */
        int64_t ts = g_tone_start + (g_tone_done + i) * 1000000 / rate;
        double ph = 2 * M_PI * 1000.0 * (double)(ts % 1000000) / 1e6;
        int16_t s = av_test_flash(ts) ? (int16_t)(9000 * sin(ph)) : 0;
        buf[i * 2] = buf[i * 2 + 1] = s;
    }
    g_tone_done += due;
    on_write(v, (uint8_t *)buf, (size_t)due * 4, t);
    g_free(buf);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* thread                                                              */
/* ------------------------------------------------------------------ */
static GDBusConnection *g_conn_qemu;

static bool register_listener(GMainLoop *loop)
{
    GError *err = NULL;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return false;
    GUnixFDList *fdl = g_unix_fd_list_new();
    int idx = g_unix_fd_list_append(fdl, sv[1], NULL);
    close(sv[1]);
    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
        g_conn_qemu, NULL, AUDIO_PATH, AUDIO_IFACE, "RegisterOutListener",
        g_variant_new("(h)", idx), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, fdl, NULL,
        NULL, &err);
    g_object_unref(fdl);
    if (!r) {
        bool nodev = g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
                     g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT) ||
                     g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE);
        LOG("audio: %s (%s)",
            nodev ? "none: the VM has no audio device on the D-Bus backend" : "registering failed",
            err->message);
        g_error_free(err);
        close(sv[0]);
        set_state(false, nodev ? "no-device" : "error");
        return false;
    }
    g_variant_unref(r);

    /* QEMU now waits (synchronously, in its main loop) for the handshake and
     * our Interfaces property: this thread answers, not the display thread */
    GSocket *gs = g_socket_new_from_fd(sv[0], &err);
    GDBusConnection *c = NULL;
    if (gs) {
        GSocketConnection *sc = g_socket_connection_factory_create_connection(gs);
        g_object_unref(gs);
        c = g_dbus_connection_new_sync(G_IO_STREAM(sc), NULL,
                                       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                           G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
                                       NULL, NULL, &err);
        g_object_unref(sc);
    }
    if (!c) {
        LOG("audio: listener connection: %s", err ? err->message : "?");
        g_clear_error(&err);
        set_state(false, "error");
        return false;
    }
    GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(listener_xml, NULL);
    guint oid = g_dbus_connection_register_object(c, OUT_LISTENER_PATH, ni->interfaces[0],
                                                  &listener_vtable, NULL, NULL, &err);
    g_dbus_node_info_unref(ni);
    if (!oid) {
        LOG("audio: register_object: %s", err->message);
        g_error_free(err);
        set_state(false, "error");
        return false;
    }
    g_signal_connect(c, "closed", G_CALLBACK(on_closed), loop);
    g_dbus_connection_set_exit_on_close(c, FALSE);
    g_dbus_connection_start_message_processing(c);
    LOG("audio: attached to the VM's D-Bus audio backend");
    set_state(true, "");
    return true;
}

static void *audio_thread(void *ud)
{
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    static Voice tone = {.used = true, .id = 1, .enabled = true, .bits = 16,
                         .channels = 2, .bpf = 4, .is_signed = true,
                         .freq = AUDIO_RATE, .gain = {1.0f, 1.0f},
                         .cur = {1.0f, 1.0f}};
    if (!g_conn_qemu) {
        voice_init(&tone);
        const char *test = getenv("PVE_H264D_TEST");
        g_tone_gap_ms = test_opt_ms(test, "gaps", 300);
        g_tone_stall_ms = test_opt_ms(test, "stalls", 80);
        g_tone_slow = test_opt_ms(test, "slow", 24);
        if (g_tone_slow > 100)
            g_tone_slow = 24;
        g_tone_start = now_us();
        GSource *s = g_timeout_source_new(5);
        g_source_set_callback(s, tone_tick, &tone, NULL);
        g_source_attach(s, ctx);
        g_source_unref(s);
        pthread_mutex_lock(&a_lock);
        st.available = st.playing = true;
        st.src_rate = AUDIO_RATE;
        st.src_channels = 2;
        pthread_mutex_unlock(&a_lock);
        audio_state_changed();
        LOG("audio: TEST MODE: 1 kHz beep in the first 100 ms of every second, "
            "every 3 s a %d ms gap, a %d ms stall, %d permille slow", g_tone_gap_ms,
            g_tone_stall_ms, g_tone_slow);
    } else if (!register_listener(loop)) {
        return NULL;
    }
    g_main_loop_run(loop);
    return NULL;
}

void audio_init(void *qemu_conn, bool enabled)
{
    if (!enabled) {
        set_state(false, "disabled");
        LOG("audio: disabled in the configuration (audio = 0)");
        return;
    }
    g_conn_qemu = qemu_conn;
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &a, audio_thread, NULL);
    pthread_attr_destroy(&a);
}
