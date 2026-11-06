/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - low latency H.264 console daemon for Proxmox VE
 *
 * One instance per VM (systemd: pve-h264d@<vmid>.service).
 *
 *  QEMU (-display dbus,p2p=yes)
 *     ^ QMP getfd + add_client @dbus-display   (attach at runtime)
 *     | org.qemu.Display1 (p2p D-Bus)
 *     |   Console.RegisterListener -> our Listener object receives
 *     |   ScanoutMap (memfd, zero copy) / Update* damage / cursor
 *  pve-h264d
 *     | encoder thread: damage-driven, ACK-paced, adaptive VBR, HW encode
 *     v
 *  /run/pve-h264/<vmid>.sock  <- pveproxy websocket relay <- browser
 *
 * See ../documentation.md for the protocol and design notes.
 */
#define _GNU_SOURCE
#include "pve-h264d.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <libavutil/log.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

int g_verbose = 0;

#define RUN_DIR "/run/pve-h264"
#define CONF_FILE "/etc/pve-h264/h264d.conf"
#define CONSOLE_PATH "/org/qemu/Display1/Console_0"
#define LISTENER_PATH "/org/qemu/Display1/Listener"

int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char backend[16];
    char nvenc_preset[8];
    char rc[8];          /* vbr | cbr | cqp */
    char tune[8];        /* nvenc: ull | ll | hq */
    char multipass[12];  /* nvenc: disabled | qres | fullres */
    char vaapi_device[64];
    int gpu;
    int fps;
    int max_kbps;
    int min_kbps;
    int quality;
    int vbv_frames;
    int max_inflight;    /* 0 = auto: enough to cover the viewer's round trip */
    int refine_frames;
    int spatial_aq;
    int aq_strength;
    int temporal_aq;
    int abr;             /* adaptive bitrate ceiling on network congestion */
    int idle_exit_s;
    int guest_exec_resize;
    int audio;           /* forward guest audio (D-Bus audiodev) to viewers */
    char clipboard[12];  /* both | to-guest | from-guest | off */
} Config;

static Config cfg = {
    .backend = "auto",
    .nvenc_preset = "p4",
    .rc = "vbr",
    .tune = "ull",
    .multipass = "disabled",
    .vaapi_device = "",
    .gpu = 0,
    .fps = 60,
    .max_kbps = 25000,
    .min_kbps = 1500,
    .quality = 24,
    .vbv_frames = 6,
    .max_inflight = 0,
    .refine_frames = 4,
    .spatial_aq = 0,
    .aq_strength = 8,
    .temporal_aq = 0,
    .abr = 1,
    .idle_exit_s = 30,
    .guest_exec_resize = 1,
    .audio = 1,
    .clipboard = "both",
};

static Config cfg_defaults; /* cfg after config file + command line */

static void load_config(void)
{
    FILE *f = fopen(CONF_FILE, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char k[64], v[128];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %127s", k, v) != 2)
            continue;
/* overlong values are rejected (default kept) rather than silently truncated */
#define S(name) if (!strcmp(k, #name)) { \
            if (strlen(v) < sizeof(cfg.name)) memcpy(cfg.name, v, strlen(v) + 1); \
            else LOG("config: value for '%s' too long (max %zu), ignored", k, sizeof(cfg.name) - 1); \
            continue; }
#define I(name) if (!strcmp(k, #name)) { cfg.name = atoi(v); continue; }
        S(backend) S(nvenc_preset) S(vaapi_device) S(rc) S(tune) S(multipass)
        S(clipboard)
        I(gpu) I(fps) I(max_kbps) I(min_kbps) I(quality) I(vbv_frames)
        I(max_inflight) I(refine_frames) I(idle_exit_s) I(guest_exec_resize) I(audio)
        I(spatial_aq) I(aq_strength) I(temporal_aq) I(abr)
#undef S
#undef I
        LOG("config: unknown key '%s'", k);
    }
    fclose(f);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool one_of(const char *v, const char *const *opts)
{
    for (; *opts; opts++)
        if (!strcmp(v, *opts))
            return true;
    return false;
}

static const char *const RC_OPTS[] = {"vbr", "cbr", "cqp", NULL};
static const char *const TUNE_OPTS[] = {"ull", "ll", "hq", NULL};
static const char *const MP_OPTS[] = {"disabled", "qres", "fullres", NULL};
static const char *const PRESET_OPTS[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7", NULL};

static const char *const CLIP_OPTS[] = {"both", "to-guest", "from-guest", "off", NULL};

static int clip_mode(void)
{
    if (!strcmp(cfg.clipboard, "both")) return CLIP_TO_GUEST | CLIP_FROM_GUEST;
    if (!strcmp(cfg.clipboard, "to-guest")) return CLIP_TO_GUEST;
    if (!strcmp(cfg.clipboard, "from-guest")) return CLIP_FROM_GUEST;
    return 0;
}

static void config_sanitize(void)
{
    cfg.fps = clampi(cfg.fps, 5, 144);
    cfg.max_kbps = clampi(cfg.max_kbps, 1000, 200000);
    cfg.min_kbps = clampi(cfg.min_kbps, 500, cfg.max_kbps);
    cfg.quality = clampi(cfg.quality, 1, 51);
    cfg.vbv_frames = clampi(cfg.vbv_frames, 1, 120);
    cfg.max_inflight = clampi(cfg.max_inflight, 0, 16);
    cfg.refine_frames = clampi(cfg.refine_frames, 0, 30);
    cfg.aq_strength = clampi(cfg.aq_strength, 1, 15);
    cfg.spatial_aq = !!cfg.spatial_aq;
    cfg.temporal_aq = !!cfg.temporal_aq;
    cfg.abr = !!cfg.abr;
    if (!one_of(cfg.clipboard, CLIP_OPTS)) {
        LOG("config: clipboard = '%s' is not both|to-guest|from-guest|off, "
            "using off", cfg.clipboard);
        snprintf(cfg.clipboard, sizeof(cfg.clipboard), "off");
    }
#define CHK(f, opts, def) if (!one_of(cfg.f, opts)) { LOG("config: invalid %s '%s', using %s", #f, cfg.f, def); snprintf(cfg.f, sizeof(cfg.f), "%s", def); }
    CHK(rc, RC_OPTS, "vbr")
    CHK(tune, TUNE_OPTS, "ull")
    CHK(multipass, MP_OPTS, "disabled")
    CHK(nvenc_preset, PRESET_OPTS, "p4")
#undef CHK
}

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */
static int g_vmid;
static GMainLoop *g_loop;
static GDBusConnection *g_qemu;    /* main p2p display connection */
static GDBusConnection *g_lconn;   /* listener connection (QEMU -> us) */
static atomic_bool g_quit;
static char g_sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

/* framebuffer state, protected by fb_lock */
static pthread_mutex_t fb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fb_cond;
static struct {
    const uint8_t *data;   /* pixel (0,0) */
    void *map_base;        /* mmap'ed memfd (ScanoutMap) */
    size_t map_len;
    uint8_t *owned;        /* private copy (Scanout/Update fallback) */
    size_t owned_len;
    int w, h, stride;
    uint32_t fmt;          /* pixman format */
    bool valid;
    bool mapped;
    bool dirty;
    int dx0, dy0, dx1, dy1; /* damage bbox, exclusive end */
} fb;

static bool g_force_idr;     /* fb_lock */
static bool g_reopen;        /* fb_lock: encoder params changed */
static int g_cur_kbps;       /* fb_lock: adaptive bitrate ceiling */
static atomic_int g_authed;  /* number of authenticated clients */
static atomic_bool g_mouse_abs = true;

/* cursor cache (main thread only) */
static GBytes *g_cursor_msg;
static char *g_video_config_json; /* fb_lock */
static char g_enc_kind[8] = "";     /* fb_lock: backend family of the open encoder */
static char g_enc_name[112] = "";   /* fb_lock */

/* stats (encoder thread writes, timer reads under fb_lock) */
static struct {
    int frames, bytes, keyframes, unchanged;
    int64_t enc_us_sum;
    int64_t paced_us; /* time a ready frame waited for viewer ACKs */
    int last_fps, last_kbps, last_paced_ms;
    double last_enc_ms;
} st;

/* ------------------------------------------------------------------ */
/* clients                                                             */
/* ------------------------------------------------------------------ */
#define ACK_RING 512
typedef struct Client {
    int fd;
    int id;
    atomic_int ref;
    atomic_bool dead;
    bool authed;
    GByteArray *rbuf;
    guint watch;
    int64_t connected_us;
    pthread_mutex_t wlock;
    /* flow control (clients_lock) */
    uint32_t last_sent, last_acked;
    int64_t last_ack_us;
    bool need_key, paused;
    int64_t sent_us[ACK_RING];
    double ack_ms, decode_ms, rtt_ms;
    /* auto in-flight window + congestion signal (clients_lock). Per second:
     * minimum network delay (ACK delay minus the viewer's decode time) and
     * minimum decode time; kept for 10 s so queueing can't inflate them. */
#define RTT_HIST 10
    double net_min_ms, dec_min_ms, net_sum_ms;
    int net_n;
    double net_hist[RTT_HIST], dec_hist[RTT_HIST];
    int hist_pos, hist_n;
    int window;      /* frames allowed in flight (auto mode) */
    double queue_ms; /* last second: avg network delay above the base */
    /* input state (main thread) */
    uint8_t keys[64]; /* bitset of pressed qnums (0..511) */
    uint8_t buttons;
    atomic_int audio_mode; /* AUDIO_OFF | AUDIO_OPUS | AUDIO_PCM */
} Client;

static GPtrArray *g_clients;  /* Client*, protected by clients_lock */
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_client_id = 1;
static int64_t g_last_client_us;
static atomic_uint g_frame_id; /* written by encoder thread */

static void client_unref(Client *c)
{
    if (atomic_fetch_sub(&c->ref, 1) == 1) {
        close(c->fd);
        g_byte_array_unref(c->rbuf);
        pthread_mutex_destroy(&c->wlock);
        free(c);
    }
}

static bool client_sendv(Client *c, uint8_t type, const void *a, size_t al,
                         const void *b, size_t bl)
{
    if (atomic_load(&c->dead))
        return false;
    uint8_t hdr[5];
    uint32_t len = (uint32_t)(1 + al + bl);
    memcpy(hdr, &len, 4); /* little endian host */
    hdr[4] = type;
    struct iovec iov[3] = {{hdr, 5}, {(void *)a, al}, {(void *)b, bl}};
    int n = 3;
    pthread_mutex_lock(&c->wlock);
    struct iovec *v = iov;
    while (n > 0) {
        struct msghdr mh = {.msg_iov = v, .msg_iovlen = n};
        ssize_t w = sendmsg(c->fd, &mh, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            atomic_store(&c->dead, true);
            pthread_mutex_unlock(&c->wlock);
            DBG("client %d: write failed: %s", c->id, strerror(errno));
            return false;
        }
        while (n > 0 && (size_t)w >= v->iov_len) {
            w -= v->iov_len;
            v++;
            n--;
        }
        if (n > 0) {
            v->iov_base = (uint8_t *)v->iov_base + w;
            v->iov_len -= w;
        }
    }
    pthread_mutex_unlock(&c->wlock);
    return true;
}

static bool client_send(Client *c, uint8_t type, const void *p, size_t l)
{
    return client_sendv(c, type, p, l, NULL, 0);
}

static void client_send_str(Client *c, uint8_t type, const char *s)
{
    client_send(c, type, s, strlen(s));
}

/* snapshot authenticated clients with a reference held */
static GPtrArray *clients_snapshot(void)
{
    GPtrArray *a = g_ptr_array_new();
    pthread_mutex_lock(&clients_lock);
    for (guint i = 0; i < g_clients->len; i++) {
        Client *c = g_clients->pdata[i];
        if (c->authed && !atomic_load(&c->dead)) {
            atomic_fetch_add(&c->ref, 1);
            g_ptr_array_add(a, c);
        }
    }
    pthread_mutex_unlock(&clients_lock);
    return a;
}

static void clients_release(GPtrArray *a)
{
    for (guint i = 0; i < a->len; i++)
        client_unref(a->pdata[i]);
    g_ptr_array_unref(a);
}

static void broadcast(uint8_t type, const void *p, size_t l)
{
    GPtrArray *a = clients_snapshot();
    for (guint i = 0; i < a->len; i++)
        client_send(a->pdata[i], type, p, l);
    clients_release(a);
}

static void broadcast_str(uint8_t type, const char *s)
{
    broadcast(type, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* audio fan-out (called from the audio thread)                        */
/* ------------------------------------------------------------------ */
int audio_modes_wanted(void)
{
    int m = 0;
    pthread_mutex_lock(&clients_lock);
    for (guint i = 0; i < g_clients->len; i++) {
        Client *c = g_clients->pdata[i];
        if (c->authed && !atomic_load(&c->dead))
            m |= 1 << atomic_load(&c->audio_mode);
    }
    pthread_mutex_unlock(&clients_lock);
    return m & ~(1 << AUDIO_OFF);
}

/* Audio is not ACK-paced: 10 ms packets are small, and they must keep
 * flowing while video frames are skipped for a slow viewer. */
void audio_send(int mode, const void *hdr, size_t hl, const void *d, size_t dl)
{
    GPtrArray *a = clients_snapshot();
    for (guint i = 0; i < a->len; i++) {
        Client *c = a->pdata[i];
        if (atomic_load(&c->audio_mode) == mode)
            client_sendv(c, S_AUDIO, hdr, hl, d, dl);
    }
    clients_release(a);
}

void audio_state_changed(void)
{
    char *js = audio_state_json();
    broadcast_str(S_AUDIO_STATE, js);
    g_free(js);
}

/* test mode: the first 100 ms of every second (host clock) beep and flash */
bool av_test_flash(int64_t t_us)
{
    return t_us % 1000000 < 100000;
}

/* ------------------------------------------------------------------ */
/* Clipboard (vdagent.c talks to the guest; main thread)               */
/* ------------------------------------------------------------------ */
static GByteArray *g_clip_last; /* latest shared text, replayed to new viewers */

static void clip_remember(const char *text, size_t len)
{
    if (!g_clip_last)
        g_clip_last = g_byte_array_new();
    g_byte_array_set_size(g_clip_last, 0);
    uint8_t flags = 0;
    g_byte_array_append(g_clip_last, &flags, 1);
    g_byte_array_append(g_clip_last, (const uint8_t *)text, len);
}

static void clip_send(Client *c, const char *text, size_t len, uint8_t flags)
{
    client_sendv(c, S_CLIPBOARD, &flags, 1, text, len);
}

void clipboard_from_guest(const char *text, size_t len)
{
    clip_remember(text, len);
    GPtrArray *a = clients_snapshot();
    guint n = a->len;
    for (guint i = 0; i < n; i++)
        clip_send(a->pdata[i], text, len, 0);
    clients_release(a);
    LOG("clipboard: guest -> %u viewer(s), %zu bytes", n, len);
}

void clipboard_state_changed(void)
{
    char *js = vdagent_clipboard_state_json();
    LOG("clipboard: %s", js);
    broadcast_str(S_CLIPBOARD_STATE, js);
    g_free(js);
}

static void clipboard_from_viewer(Client *c, const char *text, size_t len)
{
    if (!g_utf8_validate(text, len, NULL)) {
        LOG("client %d: clipboard text is not valid UTF-8, ignored", c->id);
        return;
    }
    if (!vdagent_clipboard_set(text, len)) {
        DBG("client %d: clipboard not available, %zu bytes ignored", c->id, len);
        return;
    }
    LOG("clipboard: viewer %d -> guest, %zu bytes", c->id, len);
    /* other viewers see it in their clipboard panel (not copied locally) */
    if (clip_mode() & CLIP_FROM_GUEST) {
        clip_remember(text, len);
        GPtrArray *a = clients_snapshot();
        for (guint i = 0; i < a->len; i++)
            if (a->pdata[i] != c)
                clip_send(a->pdata[i], text, len, CLIP_FLAG_REPLAY);
        clients_release(a);
    }
}

static void clipboard_hello(Client *c)
{
    char *js = vdagent_clipboard_state_json();
    client_send_str(c, S_CLIPBOARD_STATE, js);
    g_free(js);
    if (g_clip_last && g_clip_last->len > 1 && (clip_mode() & CLIP_FROM_GUEST))
        clip_send(c, (const char *)g_clip_last->data + 1, g_clip_last->len - 1,
                  CLIP_FLAG_REPLAY);
}

/* ------------------------------------------------------------------ */
/* QEMU input helpers (fire and forget)                                */
/* ------------------------------------------------------------------ */
static void qemu_call(const char *iface, const char *method, GVariant *args)
{
    if (!g_qemu || g_dbus_connection_is_closed(g_qemu)) {
        if (args)
            g_variant_unref(g_variant_ref_sink(args));
        return;
    }
    GDBusMessage *m =
        g_dbus_message_new_method_call(NULL, CONSOLE_PATH, iface, method);
    if (args)
        g_dbus_message_set_body(m, args);
    g_dbus_message_set_flags(m, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
    g_dbus_connection_send_message(g_qemu, m, G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                   NULL, NULL);
    g_object_unref(m);
}

static void key_event(Client *c, bool down, uint32_t qnum)
{
    if (qnum >= 512)
        return;
    bool was = c->keys[qnum >> 3] & (1 << (qnum & 7));
    if (down)
        c->keys[qnum >> 3] |= (1 << (qnum & 7));
    else {
        if (!was)
            return; /* never pressed by this client */
        c->keys[qnum >> 3] &= ~(1 << (qnum & 7));
    }
    qemu_call("org.qemu.Display1.Keyboard", down ? "Press" : "Release",
              g_variant_new("(u)", qnum));
}

static void button_event(Client *c, bool down, uint32_t btn)
{
    if (btn > 8)
        return;
    if (btn < 8) {
        if (down)
            c->buttons |= 1 << btn;
        else
            c->buttons &= ~(1 << btn);
    }
    qemu_call("org.qemu.Display1.Mouse", down ? "Press" : "Release",
              g_variant_new("(u)", btn));
}

static void client_release_input(Client *c)
{
    for (uint32_t q = 0; q < 512; q++)
        if (c->keys[q >> 3] & (1 << (q & 7)))
            key_event(c, false, q);
    for (uint32_t b = 0; b < 8; b++)
        if (c->buttons & (1 << b))
            button_event(c, false, b);
}

/* ------------------------------------------------------------------ */
/* framebuffer / pixel formats                                         */
/* ------------------------------------------------------------------ */
#define PF(bpp, type, a, r, g, b)                                             \
    (((uint32_t)(bpp) << 24) | ((type) << 16) | ((a) << 12) | ((r) << 8) |    \
     ((g) << 4) | (b))
#define PIX_x8r8g8b8 PF(32, 2, 0, 8, 8, 8)
#define PIX_a8r8g8b8 PF(32, 2, 8, 8, 8, 8)
#define PIX_x8b8g8r8 PF(32, 3, 0, 8, 8, 8)
#define PIX_a8b8g8r8 PF(32, 3, 8, 8, 8, 8)
#define PIX_b8g8r8x8 PF(32, 8, 0, 8, 8, 8)
#define PIX_b8g8r8a8 PF(32, 8, 8, 8, 8, 8)
#define PIX_r8g8b8x8 PF(32, 9, 0, 8, 8, 8)
#define PIX_r8g8b8a8 PF(32, 9, 8, 8, 8, 8)
#define PIX_r8g8b8 PF(24, 2, 0, 8, 8, 8)
#define PIX_b8g8r8 PF(24, 3, 0, 8, 8, 8)
#define PIX_r5g6b5 PF(16, 2, 0, 5, 6, 5)
#define PIX_x1r5g5b5 PF(16, 2, 0, 5, 5, 5)

static int fmt_bpp(uint32_t fmt) { return (int)(fmt >> 24); }

/* Copy rect (x,y,w,h) from the guest surface into the BGRX shadow.
 * Returns false if the shadow already had identical pixels (QEMU reports
 * damage for unchanged areas, e.g. VGA text mode every refresh). */
static bool copy_rect_bgrx(uint8_t *dst, int ds, const uint8_t *src, int ss,
                           uint32_t fmt, int x, int y, int w, int h)
{
    int bpp = fmt_bpp(fmt) / 8;
    bool changed = false;
    uint8_t *tmp = NULL;
    for (int row = y; row < y + h; row++) {
        const uint8_t *s = src + (size_t)row * ss + (size_t)x * bpp;
        uint8_t *d = dst + (size_t)row * ds + (size_t)x * 4;
        if (fmt == PIX_x8r8g8b8 || fmt == PIX_a8r8g8b8) {
            if (memcmp(d, s, (size_t)w * 4)) {
                memcpy(d, s, (size_t)w * 4);
                changed = true;
            }
            continue;
        }
        /* converting formats: convert into a scratch row, then compare */
        uint8_t *final = d;
        if (!tmp)
            tmp = malloc((size_t)w * 4);
        d = tmp;
        switch (fmt) {
        case PIX_x8b8g8r8:
        case PIX_a8b8g8r8: /* bytes R G B X */
            for (int i = 0; i < w; i++, s += 4, d += 4) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 0;
            }
            break;
        case PIX_b8g8r8x8:
        case PIX_b8g8r8a8: /* bytes X R G B */
            for (int i = 0; i < w; i++, s += 4, d += 4) {
                d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = 0;
            }
            break;
        case PIX_r8g8b8x8:
        case PIX_r8g8b8a8: /* bytes X B G R */
            for (int i = 0; i < w; i++, s += 4, d += 4) {
                d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = 0;
            }
            break;
        case PIX_r8g8b8: /* bytes B G R */
            for (int i = 0; i < w; i++, s += 3, d += 4) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0;
            }
            break;
        case PIX_b8g8r8: /* bytes R G B */
            for (int i = 0; i < w; i++, s += 3, d += 4) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 0;
            }
            break;
        case PIX_r5g6b5:
            for (int i = 0; i < w; i++, s += 2, d += 4) {
                uint16_t p = (uint16_t)(s[0] | (s[1] << 8));
                d[0] = (uint8_t)(((p & 0x1f) * 527 + 23) >> 6);
                d[1] = (uint8_t)((((p >> 5) & 0x3f) * 259 + 33) >> 6);
                d[2] = (uint8_t)((((p >> 11) & 0x1f) * 527 + 23) >> 6);
                d[3] = 0;
            }
            break;
        case PIX_x1r5g5b5:
            for (int i = 0; i < w; i++, s += 2, d += 4) {
                uint16_t p = (uint16_t)(s[0] | (s[1] << 8));
                d[0] = (uint8_t)(((p & 0x1f) * 527 + 23) >> 6);
                d[1] = (uint8_t)((((p >> 5) & 0x1f) * 527 + 23) >> 6);
                d[2] = (uint8_t)((((p >> 10) & 0x1f) * 527 + 23) >> 6);
                d[3] = 0;
            }
            break;
        default:
            memset(d, 0x40, (size_t)w * 4);
        }
        if (memcmp(final, tmp, (size_t)w * 4)) {
            memcpy(final, tmp, (size_t)w * 4);
            changed = true;
        }
    }
    free(tmp);
    return changed;
}

static bool fmt_supported(uint32_t fmt)
{
    switch (fmt) {
    case PIX_x8r8g8b8: case PIX_a8r8g8b8: case PIX_x8b8g8r8: case PIX_a8b8g8r8:
    case PIX_b8g8r8x8: case PIX_b8g8r8a8: case PIX_r8g8b8x8: case PIX_r8g8b8a8:
    case PIX_r8g8b8: case PIX_b8g8r8: case PIX_r5g6b5: case PIX_x1r5g5b5:
        return true;
    }
    return false;
}

/* fb_lock held */
static void fb_damage(int x, int y, int w, int h)
{
    if (!fb.valid)
        return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > fb.w) x1 = fb.w;
    if (y1 > fb.h) y1 = fb.h;
    if (x >= x1 || y >= y1)
        return;
    if (!fb.dirty) {
        fb.dx0 = x; fb.dy0 = y; fb.dx1 = x1; fb.dy1 = y1;
        fb.dirty = true;
    } else {
        if (x < fb.dx0) fb.dx0 = x;
        if (y < fb.dy0) fb.dy0 = y;
        if (x1 > fb.dx1) fb.dx1 = x1;
        if (y1 > fb.dy1) fb.dy1 = y1;
    }
    pthread_cond_signal(&fb_cond);
}

/* fb_lock held */
static void fb_release(void)
{
    if (fb.map_base)
        munmap(fb.map_base, fb.map_len);
    fb.map_base = NULL;
    fb.map_len = 0;
    fb.data = NULL;
    fb.valid = false;
    fb.mapped = false;
}

/* ------------------------------------------------------------------ */
/* D-Bus Listener object (QEMU calls us)                               */
/* ------------------------------------------------------------------ */
static const char listener_xml[] =
    "<node>"
    " <interface name='org.qemu.Display1.Listener'>"
    "  <method name='Scanout'><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='ay' direction='in'/></method>"
    "  <method name='Update'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='u' direction='in'/><arg type='ay' direction='in'/></method>"
    "  <method name='ScanoutDMABUF'><arg type='h' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='t' direction='in'/><arg type='b' direction='in'/></method>"
    "  <method name='UpdateDMABUF'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='Disable'/>"
    "  <method name='MouseSet'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='i' direction='in'/></method>"
    "  <method name='CursorDefine'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='ay' direction='in'/></method>"
    "  <property name='Interfaces' type='as' access='read'/>"
    " </interface>"
    " <interface name='org.qemu.Display1.Listener.Unix.Map'>"
    "  <method name='ScanoutMap'><arg type='h' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='u' direction='in'/></method>"
    "  <method name='UpdateMap'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    " </interface>"
    "</node>";

static void on_scanout(GVariant *params)
{
    guint32 w, h, stride, fmt;
    g_autoptr(GVariant) data = NULL;
    g_variant_get(params, "(uuuu@ay)", &w, &h, &stride, &fmt, &data);
    gsize len;
    const uint8_t *p = g_variant_get_fixed_array(data, &len, 1);
    if (len < (gsize)stride * h || !fmt_supported(fmt)) {
        LOG("scanout: bad data len=%zu stride=%u h=%u fmt=0x%x", (size_t)len,
            stride, h, fmt);
        return;
    }
    pthread_mutex_lock(&fb_lock);
    fb_release();
    if (fb.owned_len < len) {
        free(fb.owned);
        fb.owned = malloc(len);
        fb.owned_len = len;
    }
    memcpy(fb.owned, p, len);
    fb.data = fb.owned;
    fb.w = (int)w; fb.h = (int)h; fb.stride = (int)stride; fb.fmt = fmt;
    fb.valid = true;
    fb_damage(0, 0, fb.w, fb.h);
    pthread_mutex_unlock(&fb_lock);
    DBG("Scanout %ux%u stride %u fmt 0x%x (copy)", w, h, stride, fmt);
}

static void on_update(GVariant *params)
{
    gint32 x, y, w, h;
    guint32 stride, fmt;
    g_autoptr(GVariant) data = NULL;
    g_variant_get(params, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &fmt, &data);
    gsize len;
    const uint8_t *p = g_variant_get_fixed_array(data, &len, 1);
    pthread_mutex_lock(&fb_lock);
    if (!fb.valid || fb.mapped || fmt != fb.fmt || x < 0 || y < 0 ||
        x + w > fb.w || y + h > fb.h || len < (gsize)stride * (gsize)h) {
        pthread_mutex_unlock(&fb_lock);
        return;
    }
    int bpp = fmt_bpp(fmt) / 8;
    for (int r = 0; r < h; r++)
        memcpy(fb.owned + (size_t)(y + r) * fb.stride + (size_t)x * bpp,
               p + (size_t)r * stride, (size_t)w * bpp);
    fb_damage(x, y, w, h);
    pthread_mutex_unlock(&fb_lock);
}

static const char *on_scanout_map(GDBusMethodInvocation *inv, GVariant *params)
{
    gint32 handle;
    guint32 offset, w, h, stride, fmt;
    g_variant_get(params, "(huuuuu)", &handle, &offset, &w, &h, &stride, &fmt);
    GUnixFDList *fdl =
        g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(inv));
    if (!fdl)
        return "no fd list";
    int fd = g_unix_fd_list_get(fdl, handle, NULL);
    if (fd < 0)
        return "bad fd";
    if (!fmt_supported(fmt)) {
        close(fd);
        return "unsupported pixel format";
    }
    size_t len = (size_t)offset + (size_t)stride * h;
    void *base = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED)
        return "mmap failed";
    pthread_mutex_lock(&fb_lock);
    fb_release();
    fb.map_base = base;
    fb.map_len = len;
    fb.data = (const uint8_t *)base + offset;
    fb.w = (int)w; fb.h = (int)h; fb.stride = (int)stride; fb.fmt = fmt;
    fb.valid = true;
    fb.mapped = true;
    fb_damage(0, 0, fb.w, fb.h);
    pthread_mutex_unlock(&fb_lock);
    DBG("ScanoutMap %ux%u stride %u fmt 0x%x offset %u (zero-copy)", w, h,
        stride, fmt, offset);
    return NULL;
}

static void on_cursor_define(GVariant *params)
{
    gint32 w, h, hx, hy;
    g_autoptr(GVariant) data = NULL;
    g_variant_get(params, "(iiii@ay)", &w, &h, &hx, &hy, &data);
    gsize len;
    const uint8_t *p = g_variant_get_fixed_array(data, &len, 1);
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || len < (gsize)w * h * 4)
        return;
    size_t n = 16 + (size_t)w * h * 4;
    uint8_t *m = malloc(n);
    int32_t hdr[4] = {w, h, hx, hy};
    memcpy(m, hdr, 16);
    /* QEMUCursor: uint32 0xAARRGGBB native endian -> bytes B G R A (LE) */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        m[16 + i * 4 + 0] = p[i * 4 + 2];
        m[16 + i * 4 + 1] = p[i * 4 + 1];
        m[16 + i * 4 + 2] = p[i * 4 + 0];
        m[16 + i * 4 + 3] = p[i * 4 + 3];
    }
    if (g_cursor_msg)
        g_bytes_unref(g_cursor_msg);
    g_cursor_msg = g_bytes_new_take(m, n);
    broadcast(S_CURSOR, m, n);
}

static void listener_method(GDBusConnection *conn, const gchar *sender,
                            const gchar *path, const gchar *iface,
                            const gchar *method, GVariant *params,
                            GDBusMethodInvocation *inv, gpointer ud)
{
    const char *err = NULL;
    if (!strcmp(method, "UpdateMap")) {
        gint32 x, y, w, h;
        g_variant_get(params, "(iiii)", &x, &y, &w, &h);
        pthread_mutex_lock(&fb_lock);
        fb_damage(x, y, w, h);
        pthread_mutex_unlock(&fb_lock);
    } else if (!strcmp(method, "ScanoutMap")) {
        err = on_scanout_map(inv, params);
    } else if (!strcmp(method, "Update")) {
        on_update(params);
    } else if (!strcmp(method, "Scanout")) {
        on_scanout(params);
    } else if (!strcmp(method, "Disable")) {
        pthread_mutex_lock(&fb_lock);
        fb_release();
        pthread_mutex_unlock(&fb_lock);
        broadcast(S_DISPLAY_OFF, NULL, 0);
        DBG("Disable");
    } else if (!strcmp(method, "MouseSet")) {
        gint32 x, y, on;
        g_variant_get(params, "(iii)", &x, &y, &on);
        uint8_t m[9];
        memcpy(m, &x, 4);
        memcpy(m + 4, &y, 4);
        m[8] = on ? 1 : 0;
        broadcast(S_CURSOR_POS, m, sizeof(m));
    } else if (!strcmp(method, "CursorDefine")) {
        on_cursor_define(params);
    } else {
        err = "not supported";
    }
    if (err)
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR,
                                              G_IO_ERROR_NOT_SUPPORTED, "%s", err);
    else
        g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *listener_get_prop(GDBusConnection *conn, const gchar *sender,
                                   const gchar *path, const gchar *iface,
                                   const gchar *prop, GError **error, gpointer ud)
{
    if (!strcmp(prop, "Interfaces")) {
        const gchar *ifs[] = {"org.qemu.Display1.Listener.Unix.Map", NULL};
        return g_variant_new_strv(ifs, -1);
    }
    return NULL;
}

static const GDBusInterfaceVTable listener_vtable = {
    .method_call = listener_method,
    .get_property = listener_get_prop,
};

/* ------------------------------------------------------------------ */
/* QMP: hand QEMU one end of a socketpair as a D-Bus display client    */
/* ------------------------------------------------------------------ */
static int qmp_readline(int fd, char *buf, size_t n)
{
    size_t i = 0;
    while (i + 1 < n) {
        ssize_t r = read(fd, buf + i, 1);
        if (r <= 0)
            return -1;
        if (buf[i] == '\n')
            break;
        i++;
    }
    buf[i] = 0;
    return (int)i;
}

/* read until a non-event reply; returns 0 on {"return":...} */
static int qmp_reply(int fd, char *buf, size_t n)
{
    for (;;) {
        if (qmp_readline(fd, buf, n) < 0)
            return -1;
        if (strstr(buf, "\"event\""))
            continue;
        return strstr(buf, "\"return\"") ? 0 : -1;
    }
}

static int qmp_attach_dbus(int vmid, int client_fd, char *err, size_t errlen)
{
    char path[108], buf[4096];
    snprintf(path, sizeof(path), "/var/run/qemu-server/%d.qmp", vmid);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    struct timeval tv = {.tv_sec = 15};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        snprintf(err, errlen, "connect %s: %s (VM not running?)", path,
                 strerror(errno));
        close(fd);
        return -1;
    }
    /* greeting (QEMU only accepts once other QMP users disconnect) */
    if (qmp_readline(fd, buf, sizeof(buf)) < 0)
        goto fail;
    const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
    if (write(fd, caps, strlen(caps)) < 0 || qmp_reply(fd, buf, sizeof(buf)))
        goto fail;

    const char *getfd =
        "{\"execute\":\"getfd\",\"arguments\":{\"fdname\":\"pve-h264d\"}}\n";
    char cmsgbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {(void *)getfd, strlen(getfd)};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cmsgbuf, .msg_controllen = sizeof(cmsgbuf)};
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &client_fd, sizeof(int));
    if (sendmsg(fd, &mh, 0) < 0 || qmp_reply(fd, buf, sizeof(buf)))
        goto fail;

    const char *add =
        "{\"execute\":\"add_client\",\"arguments\":{\"protocol\":\"@dbus-display\","
        "\"fdname\":\"pve-h264d\"}}\n";
    if (write(fd, add, strlen(add)) < 0 || qmp_reply(fd, buf, sizeof(buf))) {
        snprintf(err, errlen,
                 "add_client failed (VM started without -display dbus? restart "
                 "the VM): %.200s", buf);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
fail:
    snprintf(err, errlen, "QMP error: %.200s", buf);
    close(fd);
    return -1;
}

static void on_qemu_closed(GDBusConnection *c, gboolean remote, GError *e,
                           gpointer ud)
{
    LOG("QEMU display connection closed (VM stopped?)");
    broadcast_str(S_ERROR, "VM display connection closed (VM stopped or reset)");
    atomic_store(&g_quit, true);
    g_main_loop_quit(g_loop);
}

static void on_mouse_props(GDBusConnection *c, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *signal, GVariant *params, gpointer ud)
{
    const gchar *ifname;
    g_autoptr(GVariant) changed = NULL;
    g_autoptr(GVariant) inval = NULL;
    g_variant_get(params, "(&s@a{sv}@as)", &ifname, &changed, &inval);
    if (strcmp(ifname, "org.qemu.Display1.Mouse"))
        return;
    gboolean abs;
    if (g_variant_lookup(changed, "IsAbsolute", "b", &abs)) {
        atomic_store(&g_mouse_abs, abs);
        uint8_t m = abs ? 1 : 0;
        broadcast(S_MOUSE_MODE, &m, 1);
        LOG("mouse mode: %s", abs ? "absolute" : "relative");
    }
}

static GDBusConnection *dbus_over_fd(int fd, GDBusConnectionFlags flags,
                                     GError **err)
{
    GSocket *gs = g_socket_new_from_fd(fd, err);
    if (!gs)
        return NULL;
    GSocketConnection *sc = g_socket_connection_factory_create_connection(gs);
    g_object_unref(gs);
    GDBusConnection *c = g_dbus_connection_new_sync(G_IO_STREAM(sc), NULL,
                                                    flags, NULL, NULL, err);
    g_object_unref(sc);
    return c;
}

static int qemu_connect(int vmid)
{
    char err[512] = "";
    GError *gerr = NULL;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return -1;
    if (qmp_attach_dbus(vmid, sv[1], err, sizeof(err)) < 0) {
        LOG("%s", err);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    close(sv[1]);
    g_qemu = dbus_over_fd(sv[0], G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                          &gerr);
    if (!g_qemu) {
        LOG("D-Bus connect: %s", gerr->message);
        return -1;
    }
    g_signal_connect(g_qemu, "closed", G_CALLBACK(on_qemu_closed), NULL);
    g_dbus_connection_set_exit_on_close(g_qemu, FALSE);

    /* console sanity */
    GVariant *v = g_dbus_connection_call_sync(
        g_qemu, NULL, CONSOLE_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.qemu.Display1.Console", "Type"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &gerr);
    if (!v) {
        LOG("console 0 not available: %s", gerr->message);
        return -1;
    }
    g_autoptr(GVariant) inner = NULL;
    g_variant_get(v, "(v)", &inner);
    LOG("console 0 type: %s", g_variant_get_string(inner, NULL));
    g_variant_unref(v);

    v = g_dbus_connection_call_sync(
        g_qemu, NULL, CONSOLE_PATH, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.qemu.Display1.Mouse", "IsAbsolute"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (v) {
        g_autoptr(GVariant) b = NULL;
        g_variant_get(v, "(v)", &b);
        atomic_store(&g_mouse_abs, g_variant_get_boolean(b));
        g_variant_unref(v);
    }
    g_dbus_connection_signal_subscribe(
        g_qemu, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        CONSOLE_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_mouse_props, NULL, NULL);

    /* register our listener */
    int lv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, lv) < 0)
        return -1;
    GUnixFDList *fdl = g_unix_fd_list_new();
    int idx = g_unix_fd_list_append(fdl, lv[1], NULL);
    close(lv[1]);
    v = g_dbus_connection_call_with_unix_fd_list_sync(
        g_qemu, NULL, CONSOLE_PATH, "org.qemu.Display1.Console",
        "RegisterListener", g_variant_new("(h)", idx), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, fdl, NULL, NULL, &gerr);
    g_object_unref(fdl);
    if (!v) {
        LOG("RegisterListener: %s", gerr->message);
        return -1;
    }
    g_variant_unref(v);

    g_lconn = dbus_over_fd(lv[0],
                           G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
                           &gerr);
    if (!g_lconn) {
        LOG("listener connection: %s", gerr->message);
        return -1;
    }
    GDBusNodeInfo *ni = g_dbus_node_info_new_for_xml(listener_xml, &gerr);
    if (!ni) {
        LOG("xml: %s", gerr->message);
        return -1;
    }
    for (int i = 0; ni->interfaces[i]; i++) {
        if (!g_dbus_connection_register_object(g_lconn, LISTENER_PATH,
                                               ni->interfaces[i], &listener_vtable,
                                               NULL, NULL, &gerr)) {
            LOG("register_object: %s", gerr->message);
            return -1;
        }
    }
    g_dbus_node_info_unref(ni);
    g_signal_connect(g_lconn, "closed", G_CALLBACK(on_qemu_closed), NULL);
    g_dbus_connection_set_exit_on_close(g_lconn, FALSE);
    g_dbus_connection_start_message_processing(g_lconn);
    LOG("attached to VM %d display (mouse %s)", vmid,
        atomic_load(&g_mouse_abs) ? "absolute" : "relative");
    vdagent_attach(g_qemu, clip_mode());
    guestexec_init(vmid, cfg.guest_exec_resize);
    audio_init(g_qemu, cfg.audio);
    return 0;
}

/* ------------------------------------------------------------------ */
/* synthetic test source (PVE_H264D_TEST=WxH): measures the full        */
/* encode->network->browser path at high resolution without a guest OS  */
/* ------------------------------------------------------------------ */
static int g_test_w, g_test_h;
static bool g_test_video;

/* "WxH:video": full-screen panning texture with moving shading, roughly
 * what a 60 fps video or game puts through the encoder (every pixel changes,
 * sub-pixel motion, so motion compensation never matches exactly). */
static void *testvideo_thread(void *arg)
{
    int W = g_test_w, H = g_test_h, S = W * 4;
    const int PAD = 256, TW = W + 2 * PAD + 1, TH = H + 2 * PAD + 1;
    uint8_t *tex = malloc((size_t)TW * TH * 3);
    uint32_t seed = 7;
    uint8_t *l32 = malloc(((TW >> 5) + 1) * ((TH >> 5) + 1) * 3);
    uint8_t *l8 = malloc(((TW >> 3) + 1) * ((TH >> 3) + 1) * 3);
    for (size_t i = 0; i < (size_t)((TW >> 5) + 1) * ((TH >> 5) + 1) * 3; i++)
        l32[i] = (uint8_t)((seed = seed * 1103515245 + 12345) >> 16);
    for (size_t i = 0; i < (size_t)((TW >> 3) + 1) * ((TH >> 3) + 1) * 3; i++)
        l8[i] = (uint8_t)((seed = seed * 1103515245 + 12345) >> 16);
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++)
            for (int ch = 0; ch < 3; ch++) {
                int a = l32[(((y >> 5) * ((TW >> 5) + 1)) + (x >> 5)) * 3 + ch];
                int b = l8[(((y >> 3) * ((TW >> 3) + 1)) + (x >> 3)) * 3 + ch];
                seed = seed * 1103515245 + 12345;
                int c = (seed >> 16) & 63;
                tex[((size_t)y * TW + x) * 3 + ch] = (uint8_t)((a * 5 + b * 2) / 8 + c / 2);
            }
    free(l32);
    free(l8);
    uint8_t shade[512];
    for (int i = 0; i < 512; i++)
        shade[i] = (uint8_t)(160 + 95 * sin(i * 2 * M_PI / 512));

    pthread_mutex_lock(&fb_lock);
    fb_release();
    fb.owned = calloc((size_t)S, H);
    fb.owned_len = (size_t)S * H;
    fb.data = fb.owned; fb.w = W; fb.h = H; fb.stride = S; fb.fmt = PIX_x8r8g8b8;
    fb.valid = true; fb.mapped = false;
    fb_damage(0, 0, W, H);
    pthread_mutex_unlock(&fb_lock);

    for (int64_t n = 0; !atomic_load(&g_quit); n++) {
        int64_t t0 = now_us();
        double a = n / 60.0;
        double ox = PAD + (PAD - 1) * sin(a * 0.9), oy = PAD + (PAD - 1) * sin(a * 0.6 + 1);
        int ix = (int)ox, iy = (int)oy;
        int fx = (int)((ox - ix) * 256);
        for (int y = 0; y < H; y++) {
            const uint8_t *r0 = tex + ((size_t)(y + iy) * TW + ix) * 3;
            uint8_t *d = fb.owned + (size_t)y * S;
            int sh = shade[(y / 2 + n * 3) & 511]; /* moving light bands */
            int k0 = (256 - fx) * sh, k1 = fx * sh;
            for (int x = 0; x < W; x++, r0 += 3, d += 4) {
                d[0] = (uint8_t)((r0[0] * k0 + r0[3] * k1) >> 16);
                d[1] = (uint8_t)((r0[1] * k0 + r0[4] * k1) >> 16);
                d[2] = (uint8_t)((r0[2] * k0 + r0[5] * k1) >> 16);
            }
        }
        pthread_mutex_lock(&fb_lock);
        fb_damage(0, 0, W, H);
        pthread_mutex_unlock(&fb_lock);
        int64_t sl = 1000000 / 60 - (now_us() - t0);
        if (sl > 0)
            usleep((useconds_t)sl);
    }
    free(tex);
    return NULL;
}

static void *testpattern_thread(void *arg)
{
    int W = g_test_w, H = g_test_h, S = W * 4;
    pthread_mutex_lock(&fb_lock);
    fb_release();
    fb.owned = malloc((size_t)S * H);
    fb.owned_len = (size_t)S * H;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = fb.owned + (size_t)y * S + x * 4;
            p[0] = (uint8_t)(64 + x * 128 / W); p[1] = (uint8_t)(40 + y * 100 / H);
            p[2] = 30; p[3] = 0;
        }
    fb.data = fb.owned; fb.w = W; fb.h = H; fb.stride = S; fb.fmt = PIX_x8r8g8b8;
    fb.valid = true; fb.mapped = false;
    fb_damage(0, 0, W, H);
    pthread_mutex_unlock(&fb_lock);

    const int ww = W / 4, wh = H / 3;           /* "window" being dragged */
    const int tx = W / 20, ty = H / 2, tw = W / 2, th = H / 3; /* terminal */
    uint32_t seed = 1;
    int px = 0, py = 0;
    for (int64_t n = 0; !atomic_load(&g_quit); n++) {
        int64_t t0 = now_us();
        uint8_t *b = fb.owned; /* drawn unlocked, like a guest writing VRAM */
        /* scroll terminal up by 4 rows, add a new line of "text" */
        for (int y = ty; y < ty + th - 4; y++)
            memmove(b + (size_t)y * S + tx * 4, b + (size_t)(y + 4) * S + tx * 4, (size_t)tw * 4);
        for (int y = ty + th - 4; y < ty + th; y++)
            for (int x = tx; x < tx + tw; x++) {
                seed = seed * 1103515245 + 12345;
                uint8_t v = ((x / 8 + (int)(n / 4)) % 7 && ((seed >> 16) & 3)) ? 220 : 20;
                uint8_t *p = b + (size_t)y * S + x * 4;
                p[0] = p[1] = p[2] = v; p[3] = 0;
            }
        /* restore background where the window was, draw it at new pos */
        double a = n / 60.0;
        int nx = (int)((W - ww) * (0.5 + 0.45 * sin(a * 1.3)));
        int ny = (int)((H / 2 - wh) * (0.5 + 0.45 * sin(a * 1.9)));
        for (int y = py; y < py + wh; y++)
            for (int x = px; x < px + ww; x++) {
                uint8_t *p = b + (size_t)y * S + x * 4;
                p[0] = (uint8_t)(64 + x * 128 / W); p[1] = (uint8_t)(40 + y * 100 / H);
                p[2] = 30;
            }
        for (int y = ny; y < ny + wh; y++)
            for (int x = nx; x < nx + ww; x++) {
                uint8_t *p = b + (size_t)y * S + x * 4;
                bool title = y - ny < 28, txt = ((x - nx) / 9 + (y - ny) / 14) % 5 != 0 && (y - ny) % 14 < 9;
                p[0] = title ? 200 : txt ? 60 : 240; p[1] = title ? 120 : txt ? 60 : 240;
                p[2] = title ? 40 : txt ? 60 : 240;
            }
        /* A/V sync marker: a white box while the test tone beeps */
        const int fs = H / 8, fx0 = W - fs - W / 40, fy0 = H / 40;
        bool flash = av_test_flash(now_us());
        for (int y = fy0; y < fy0 + fs; y++)
            for (int x = fx0; x < fx0 + fs; x++) {
                uint8_t *p = b + (size_t)y * S + x * 4;
                p[0] = p[1] = p[2] = flash ? 255 : 0;
            }
        pthread_mutex_lock(&fb_lock);
        fb_damage(px < nx ? px : nx, py < ny ? py : ny,
                  abs(nx - px) + ww, abs(ny - py) + wh);
        fb_damage(tx, ty, tw, th);
        fb_damage(fx0, fy0, fs, fs);
        pthread_mutex_unlock(&fb_lock);
        px = nx; py = ny;
        int64_t sl = 1000000 / 60 - (now_us() - t0);
        if (sl > 0)
            usleep((useconds_t)sl);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* encoder thread                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    GPtrArray *clients;
    uint32_t id;
    int64_t cap_us;
    int64_t enc_start;
    bool config_pending;
    int w, h, cw, ch;
    const char *enc_name;
} FrameCtx;

static void make_codec_string(const uint8_t *d, size_t n, char *out, size_t outl)
{
    snprintf(out, outl, "avc1.64002a");
    for (size_t i = 0; i + 4 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1f) == 7 &&
            i + 6 < n) {
            snprintf(out, outl, "avc1.%02x%02x%02x", d[i + 4], d[i + 5], d[i + 6]);
            return;
        }
    }
}

static void on_packet(const uint8_t *data, size_t len, bool key, void *opaque)
{
    FrameCtx *fc = opaque;
    int64_t enc_us = now_us() - fc->enc_start;
    uint8_t *fixed = NULL;
    if (key) {
        /* signal "no reordering" so decoders output every frame immediately */
        size_t fl;
        if ((fixed = h264_fix_sps(data, len, &fl))) {
            data = fixed;
            len = fl;
        }
    }

    if (key && fc->config_pending) {
        char codec[32], json[512];
        make_codec_string(data, len, codec, sizeof(codec));
        snprintf(json, sizeof(json),
                 "{\"codec\":\"%s\",\"width\":%d,\"height\":%d,"
                 "\"coded_width\":%d,\"coded_height\":%d,\"encoder\":\"%s\"}",
                 codec, fc->w, fc->h, fc->cw, fc->ch, fc->enc_name);
        pthread_mutex_lock(&fb_lock);
        g_free(g_video_config_json);
        g_video_config_json = g_strdup(json);
        pthread_mutex_unlock(&fb_lock);
        for (guint i = 0; i < fc->clients->len; i++)
            client_send_str(fc->clients->pdata[i], S_VIDEO_CONFIG, json);
        fc->config_pending = false;
        LOG("stream: %s", json);
    }

    uint8_t hdr[17];
    uint32_t id = fc->id;
    uint32_t eus = (uint32_t)enc_us;
    uint64_t cap = (uint64_t)fc->cap_us;
    memcpy(hdr, &id, 4);
    hdr[4] = key ? FRAME_FLAG_KEY : 0;
    memcpy(hdr + 5, &eus, 4);
    memcpy(hdr + 9, &cap, 8);

    int64_t t = now_us();
    for (guint i = 0; i < fc->clients->len; i++) {
        Client *c = fc->clients->pdata[i];
        pthread_mutex_lock(&clients_lock);
        bool skip = c->paused || (c->need_key && !key);
        if (!skip) {
            c->need_key = false;
            c->last_sent = id;
            c->sent_us[id % ACK_RING] = t;
        }
        pthread_mutex_unlock(&clients_lock);
        if (!skip)
            client_sendv(c, S_FRAME, hdr, sizeof(hdr), data, len);
    }
    st.frames++;
    st.bytes += (int)len;
    st.enc_us_sum += enc_us;
    if (key)
        st.keyframes++;
    free(fixed);
}

/* fb_lock held. true if every active client has room for another frame. */
static bool flow_ok(int64_t now)
{
    bool ok = true;
    pthread_mutex_lock(&clients_lock);
    for (guint i = 0; i < g_clients->len; i++) {
        Client *c = g_clients->pdata[i];
        if (!c->authed || c->paused || c->need_key || atomic_load(&c->dead))
            continue;
        uint32_t inflight = c->last_sent - c->last_acked;
        if ((int)inflight < (cfg.max_inflight ? cfg.max_inflight : c->window))
            continue;
        if (now - c->last_ack_us > 1500000) {
            /* stalled viewer: stop feeding it, resync with IDR on recovery */
            c->paused = true;
            LOG("client %d stalled (%u frames in flight), pausing", c->id,
                inflight);
            continue;
        }
        ok = false;
    }
    pthread_mutex_unlock(&clients_lock);
    return ok;
}

static void timed_wait_us(int64_t us)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ns = ts.tv_nsec + us * 1000;
    ts.tv_sec += ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;
    pthread_cond_timedwait(&fb_cond, &fb_lock, &ts);
}

static char *settings_json(void);

static gboolean settings_broadcast_idle(gpointer ud)
{
    pthread_mutex_lock(&fb_lock);
    char *js = settings_json();
    pthread_mutex_unlock(&fb_lock);
    broadcast_str(S_SETTINGS, js);
    g_free(js);
    return G_SOURCE_REMOVE;
}

static gboolean notice_idle(gpointer ud)
{
    broadcast_str(S_NOTICE, ud);
    g_free(ud);
    return G_SOURCE_REMOVE;
}

/* fb_lock held. true if any tunable differs from the always-safe defaults */
static bool tuning_is_custom(void)
{
    return strcmp(cfg.rc, "vbr") || strcmp(cfg.tune, "ull") ||
           strcmp(cfg.multipass, "disabled") || cfg.spatial_aq || cfg.temporal_aq ||
           strcmp(cfg.nvenc_preset, "p2");
}

static void tuning_reset(void)
{
    snprintf(cfg.rc, sizeof(cfg.rc), "vbr");
    snprintf(cfg.tune, sizeof(cfg.tune), "ull");
    snprintf(cfg.multipass, sizeof(cfg.multipass), "disabled");
    snprintf(cfg.nvenc_preset, sizeof(cfg.nvenc_preset), "p2");
    cfg.spatial_aq = cfg.temporal_aq = 0;
}

static void *encoder_thread(void *arg)
{
    Encoder *enc = NULL;
    int src_w = 0, src_h = 0, enc_kbps = 0;
    int64_t last_frame = 0;
    int64_t next_due = 0;      /* ideal time of the next frame at cfg.fps */
    int64_t blocked_since = 0; /* a frame is ready but viewers' windows are full */
    int refine_left = 0;
    char err[512];

    pthread_mutex_lock(&fb_lock);
    while (!atomic_load(&g_quit)) {
        bool active = fb.valid && atomic_load(&g_authed) > 0;
        if (!active || (!fb.dirty && !g_force_idr && refine_left <= 0)) {
            if (!active && enc && atomic_load(&g_authed) == 0) {
                /* nobody watching: free the encoder session */
                encoder_close(enc);
                enc = NULL;
            }
            timed_wait_us(200000);
            continue;
        }
        int64_t now = now_us();
        int64_t interval = 1000000 / cfg.fps;
        /* Frames follow an ideal 1/fps timeline and may come up to a quarter
         * interval early, so guest frames that arrive with a little jitter
         * aren't merged into one (which would show up as a dropped frame). */
        int64_t due = next_due - interval / 4;
        if (!fb.dirty && !g_force_idr)
            due = last_frame + 60000; /* quality refinement frames for static content */
        if (now < due) {
            timed_wait_us(due - now);
            continue;
        }
        if (!flow_ok(now)) {
            if (!blocked_since)
                blocked_since = now;
            timed_wait_us(2000); /* woken early by ACKs */
            continue;
        }
        if (blocked_since) {
            st.paced_us += now - blocked_since;
            blocked_since = 0;
        }

        if (!enc || g_reopen || src_w != fb.w || src_h != fb.h) {
            encoder_close(enc);
            /* string params point into a copy: cfg may change while unlocked */
            Config pc = cfg;
            g_reopen = false; /* a change while unlocked sets it again */
            EncoderParams p = {
                .backend = pc.backend, .vaapi_device = pc.vaapi_device,
                .width = fb.w, .height = fb.h, .fps = pc.fps,
                .max_kbps = g_cur_kbps, .quality = pc.quality,
                .vbv_frames = pc.vbv_frames, .nvenc_preset = pc.nvenc_preset,
                .gpu = pc.gpu, .rc = pc.rc, .tune = pc.tune, .multipass = pc.multipass,
                .spatial_aq = pc.spatial_aq, .aq_strength = pc.aq_strength,
                .temporal_aq = pc.temporal_aq,
            };
            pthread_mutex_unlock(&fb_lock);
            enc = encoder_open(&p, err, sizeof(err));
            pthread_mutex_lock(&fb_lock);
            if (!enc && tuning_is_custom()) {
                /* a viewer picked options this GPU/driver doesn't support:
                 * fall back to the defaults instead of dying */
                LOG("encoder: open failed with custom tuning (%s), retrying with defaults", err);
                char *msg = g_strdup_printf("Encoder rejected these settings, reverted to "
                                            "defaults: %.300s", err);
                tuning_reset();
                g_idle_add(notice_idle, msg);
                g_idle_add(settings_broadcast_idle, NULL);
                g_reopen = true;
                g_force_idr = true;
                continue;
            }
            if (!enc) {
                LOG("%s", err);
                pthread_mutex_unlock(&fb_lock);
                broadcast_str(S_ERROR, err);
                atomic_store(&g_quit, true);
                g_main_loop_quit(g_loop);
                return NULL;
            }
            /* the size the encoder was really opened with; fb may have
             * changed while fb_lock was released -> loop reopens */
            src_w = p.width;
            src_h = p.height;
            enc_kbps = p.max_kbps;
            g_force_idr = true;
            snprintf(g_enc_kind, sizeof(g_enc_kind), "%s", encoder_kind(enc));
            snprintf(g_enc_name, sizeof(g_enc_name), "%s", encoder_name(enc));
            g_idle_add(settings_broadcast_idle, NULL);
            fb_damage(0, 0, fb.w, fb.h);
            if (!fb.valid || fb.w != src_w || fb.h != src_h)
                continue;
        }
        if (enc_kbps != g_cur_kbps) {
            encoder_set_bitrate(enc, g_cur_kbps);
            enc_kbps = g_cur_kbps;
        }

        bool was_dirty = false;
        int rect[4] = {0, 0, 0, 0};
        if (fb.dirty) {
            int stride, cw0, ch0;
            uint8_t *dst = encoder_input(enc, &stride);
            encoder_coded_size(enc, &cw0, &ch0);
            /* defensive: never write outside the encoder's input frame */
            int x1 = fb.dx1 < cw0 ? fb.dx1 : cw0, y1 = fb.dy1 < ch0 ? fb.dy1 : ch0;
            if (x1 > fb.dx0 && y1 > fb.dy0) {
                was_dirty = copy_rect_bgrx(dst, stride, fb.data, fb.stride, fb.fmt,
                                           fb.dx0, fb.dy0, x1 - fb.dx0, y1 - fb.dy0);
                rect[0] = fb.dx0; rect[1] = fb.dy0;
                rect[2] = x1 - fb.dx0; rect[3] = y1 - fb.dy0;
            }
            fb.dirty = false;
            if (!was_dirty && !g_force_idr && refine_left <= 0) {
                st.unchanged++;
                continue; /* damage without pixel change: nothing to send */
            }
        }
        bool idr = g_force_idr;
        g_force_idr = false;
        int cw, ch;
        encoder_coded_size(enc, &cw, &ch);
        FrameCtx fc = {
            .id = atomic_fetch_add(&g_frame_id, 1) + 1, .cap_us = now, .config_pending = idr,
            .w = src_w, .h = src_h, .cw = cw, .ch = ch,
            .enc_name = encoder_name(enc),
        };
        pthread_mutex_unlock(&fb_lock);

        fc.clients = clients_snapshot();
        fc.enc_start = now_us();
        int r = encoder_encode(enc, idr, was_dirty ? rect : NULL, on_packet, &fc);
        clients_release(fc.clients);
        if (r < 0)
            LOG("encode error %d", r);

        pthread_mutex_lock(&fb_lock);
        last_frame = now;
        next_due = next_due < now - interval ? now + interval : next_due + interval;
        if (was_dirty)
            refine_left = cfg.refine_frames;
        else if (refine_left > 0)
            refine_left--;
    }
    pthread_mutex_unlock(&fb_lock);
    encoder_close(enc);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* stats + adaptive bitrate (main loop, 1 Hz)                          */
/* ------------------------------------------------------------------ */
static int g_calm_secs, g_hold_secs;

/* fb_lock held */
static char *settings_json(void)
{
    return g_strdup_printf(
        "{\"rc\":\"%s\",\"quality\":%d,\"max_kbps\":%d,\"min_kbps\":%d,\"abr\":%d,"
        "\"fps\":%d,\"preset\":\"%s\",\"tune\":\"%s\",\"multipass\":\"%s\","
        "\"spatial_aq\":%d,\"aq_strength\":%d,\"temporal_aq\":%d,\"vbv_frames\":%d,"
        "\"max_inflight\":%d,\"refine_frames\":%d,\"kind\":\"%s\",\"encoder\":\"%s\"}",
        cfg.rc, cfg.quality, cfg.max_kbps, cfg.min_kbps, cfg.abr, cfg.fps,
        cfg.nvenc_preset, cfg.tune, cfg.multipass, cfg.spatial_aq, cfg.aq_strength,
        cfg.temporal_aq, cfg.vbv_frames, cfg.max_inflight, cfg.refine_frames,
        g_enc_kind, g_enc_name);
}

static gboolean stats_tick(gpointer ud)
{
    pthread_mutex_lock(&fb_lock);
    st.last_fps = st.frames;
    st.last_kbps = st.bytes * 8 / 1000;
    st.last_paced_ms = (int)(st.paced_us / 1000);
    st.last_enc_ms = st.frames ? (double)st.enc_us_sum / st.frames / 1000.0 : 0;

    /* Per viewer, from the last 10 s of per-second minima:
     *  - window: frames in flight needed to cover one round trip (network +
     *    decode) at the target frame rate, plus one for jitter. A fixed
     *    window of 2 caps 60 fps at a 33 ms round trip (WAN, VPN, Wi-Fi).
     *  - queue: how far this second's average network delay is above the
     *    base, i.e. queueing on the path -> the ABR signal. */
    double interval_ms = 1000.0 / cfg.fps;
    double ack_ms = 0, dec_ms = 0, queue_ms = 0, base_ms = 0;
    int window = 0;
    pthread_mutex_lock(&clients_lock);
    for (guint i = 0; i < g_clients->len; i++) {
        Client *c = g_clients->pdata[i];
        if (!c->authed)
            continue;
        if (c->ack_ms > ack_ms) {
            ack_ms = c->ack_ms;
            dec_ms = c->decode_ms;
        }
        if (c->net_n > 0) {
            c->net_hist[c->hist_pos] = c->net_min_ms;
            c->dec_hist[c->hist_pos] = c->dec_min_ms;
            c->hist_pos = (c->hist_pos + 1) % RTT_HIST;
            if (c->hist_n < RTT_HIST)
                c->hist_n++;
        }
        if (c->hist_n > 0) {
            double nb = 1e9, db = 1e9;
            for (int k = 0; k < c->hist_n; k++) {
                if (c->net_hist[k] < nb) nb = c->net_hist[k];
                if (c->dec_hist[k] < db) db = c->dec_hist[k];
            }
            /* a decoder slower than the frame rate gains nothing from more
             * frames in flight; they would only wait in its queue */
            int w = db > interval_ms ? 2 : (int)ceil((nb + db) / interval_ms) + 1;
            c->window = clampi(w, 2, 8);
            c->queue_ms = c->net_n >= 5 ? c->net_sum_ms / c->net_n - nb : 0;
            if (c->queue_ms > queue_ms) {
                queue_ms = c->queue_ms;
                base_ms = nb;
            }
        }
        int eff = cfg.max_inflight ? cfg.max_inflight : c->window;
        if (eff > window)
            window = eff;
        c->net_min_ms = c->dec_min_ms = 1e9;
        c->net_sum_ms = 0;
        c->net_n = 0;
    }
    pthread_mutex_unlock(&clients_lock);

    /* Adaptive bitrate ceiling. Only *network* queueing lowers it (the
     * viewer's decode time is excluded): a slow decoder is handled by the ACK
     * window, and a lower bitrate would just cost quality.
     * Every change is expensive: libavcodec's NVENC reconfigure forces an IDR,
     * and a big keyframe on a slow link is exactly what causes queueing. So
     * the ceiling moves rarely and decisively: one cut straight below the
     * measured throughput, a hold while that IDR drains, probing upwards only
     * while the ceiling actually limits the encoder, and one jump back to the
     * maximum once it has been idle for a while. Seconds with a keyframe are
     * not judged (one big frame always queues briefly). */
    double thr = interval_ms > 10 ? interval_ms : 10;
    int prev = g_cur_kbps;
    bool binding = st.last_kbps > g_cur_kbps * 7 / 10;
    if (!cfg.abr) {
        g_cur_kbps = cfg.max_kbps;
    } else if (g_hold_secs > 0) {
        g_hold_secs--;
    } else if (st.keyframes == 0 && st.frames > 0 && queue_ms > thr) {
        int cut = (int)(g_cur_kbps * 0.85), fit = (int)(st.last_kbps * 0.85);
        g_cur_kbps = cut < fit ? cut : fit;
        g_calm_secs = 0;
        g_hold_secs = 2;
    } else if (st.keyframes == 0 && queue_ms < thr / 2 && g_cur_kbps < cfg.max_kbps) {
        g_calm_secs++;
        if (binding && g_calm_secs >= 5) {
            g_cur_kbps = (int)(g_cur_kbps * 1.3) + 200;
            g_calm_secs = 0;
            g_hold_secs = 2;
        } else if (!binding && g_calm_secs >= 8) {
            g_cur_kbps = cfg.max_kbps;
            g_calm_secs = 0;
        }
    }
    g_cur_kbps = clampi(g_cur_kbps, cfg.min_kbps, cfg.max_kbps);
    if (g_cur_kbps != prev)
        DBG("abr: %d -> %d kbps (queue %.1f ms, base %.1f ms, used %d kbps)", prev,
            g_cur_kbps, queue_ms, base_ms, st.last_kbps);

    char json[1024];
    int len = snprintf(json, sizeof(json),
        "{\"fps\":%d,\"kbps\":%d,\"enc_ms\":%.2f,\"cap_kbps\":%d,\"max_kbps\":%d,"
        "\"target_fps\":%d,\"quality\":%d,\"paced_ms\":%d,\"keyframes\":%d,"
        "\"width\":%d,\"height\":%d,\"capture\":\"%s\",\"viewers\":%d,"
        "\"mouse_abs\":%s,\"ack_ms\":%.2f,\"dec_ms\":%.2f,\"unchanged\":%d,"
        "\"window\":%d,\"queue_ms\":%.1f,\"base_ms\":%.1f,\"audio_kbps\":%d}",
        st.last_fps, st.last_kbps, st.last_enc_ms, g_cur_kbps, cfg.max_kbps,
        cfg.fps, cfg.quality, st.last_paced_ms, st.keyframes, fb.w, fb.h,
        !fb.valid ? "none" : fb.mapped ? "memfd" : "copy",
        atomic_load(&g_authed), atomic_load(&g_mouse_abs) ? "true" : "false",
        ack_ms, dec_ms, st.unchanged, window, queue_ms, base_ms, audio_kbps());
    st.frames = st.bytes = st.keyframes = st.unchanged = 0;
    st.enc_us_sum = st.paced_us = 0;
    pthread_cond_signal(&fb_cond);
    pthread_mutex_unlock(&fb_lock);
    if (len > 0 && atomic_load(&g_authed) > 0)
        broadcast(S_STATS, json, (size_t)len);

    /* idle exit */
    if (atomic_load(&g_authed) == 0 && g_clients->len == 0 &&
        now_us() - g_last_client_us > (int64_t)cfg.idle_exit_s * 1000000) {
        LOG("no viewers for %ds, exiting", cfg.idle_exit_s);
        atomic_store(&g_quit, true);
        g_main_loop_quit(g_loop);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* client protocol                                                     */
/* ------------------------------------------------------------------ */
static bool token_valid(const char *tok, size_t len)
{
    if (len < 32 || len > 128)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!g_ascii_isxdigit(tok[i]))
            return false;
    char path[256];
    snprintf(path, sizeof(path), RUN_DIR "/tokens/%d/%.*s", g_vmid, (int)len, tok);
    struct stat sb;
    if (stat(path, &sb) < 0)
        return false;
    unlink(path); /* one-time use */
    time_t now = time(NULL);
    return now - sb.st_mtime <= 60 && now >= sb.st_mtime - 5;
}

static int json_int(const char *js, const char *key, int *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p)
        return 0;
    *out = (int)v;
    return 1;
}

/* "key":"value" with a short [a-z0-9_-] value */
static int json_str(const char *js, const char *key, char *out, size_t outl)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p++ != '"')
        return 0;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outl) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
            return 0;
        out[i++] = *p++;
    }
    if (*p != '"')
        return 0;
    out[i] = 0;
    return 1;
}

/* fb_lock held; values are validated by config_sanitize() afterwards */
static void config_apply_json(const char *js)
{
    int v;
    char sv[16];
    if (json_int(js, "reset", &v) && v) {
        /* back to the config file's values (everything a viewer can set) */
        Config d = cfg_defaults;
#define R(f) memcpy(&cfg.f, &d.f, sizeof(cfg.f));
        R(max_kbps) R(min_kbps) R(fps) R(quality) R(vbv_frames) R(max_inflight)
        R(refine_frames) R(spatial_aq) R(aq_strength) R(temporal_aq) R(abr)
        R(rc) R(tune) R(multipass) R(nvenc_preset)
#undef R
    }
#define JI(key, field) if (json_int(js, key, &v)) cfg.field = v;
/* invalid choices are ignored (the current value stays) */
#define JS(key, field, opts) \
    if (json_str(js, key, sv, sizeof(sv)) && one_of(sv, opts) && strlen(sv) < sizeof(cfg.field)) \
        memcpy(cfg.field, sv, strlen(sv) + 1);
    JI("max_kbps", max_kbps) JI("min_kbps", min_kbps) JI("fps", fps)
    JI("quality", quality) JI("vbv_frames", vbv_frames) JI("max_inflight", max_inflight)
    JI("refine_frames", refine_frames) JI("spatial_aq", spatial_aq)
    JI("aq_strength", aq_strength) JI("temporal_aq", temporal_aq) JI("abr", abr)
    JS("rc", rc, RC_OPTS) JS("tune", tune, TUNE_OPTS) JS("multipass", multipass, MP_OPTS)
    JS("preset", nvenc_preset, PRESET_OPTS)
#undef JI
#undef JS
}

static uint32_t g_resize_failed_w, g_resize_failed_h; /* 0 = no failure */
static void resize_state_json(char *buf, size_t n);
static bool scanout_is(uint32_t w, uint32_t h);

static void client_authed(Client *c)
{
    c->authed = true;
    atomic_fetch_add(&g_authed, 1);
    char hello[512];
    snprintf(hello, sizeof(hello),
             "{\"proto\":%d,\"vmid\":%d,\"mouse_abs\":%s,\"max_kbps\":%d,"
             "\"fps\":%d,\"quality\":%d,\"backend\":\"%s\",\"client_id\":%d}",
             PROTO_VERSION, g_vmid, atomic_load(&g_mouse_abs) ? "true" : "false",
             cfg.max_kbps, cfg.fps, cfg.quality, cfg.backend, c->id);
    client_send_str(c, S_HELLO, hello);
    char *aj = audio_state_json();
    client_send_str(c, S_AUDIO_STATE, aj);
    g_free(aj);
    if (g_cursor_msg) {
        gsize n;
        const void *d = g_bytes_get_data(g_cursor_msg, &n);
        client_send(c, S_CURSOR, d, n);
    }
    clipboard_hello(c);
    /* still failing: the guest may have reached the size since (fixed by
     * hand in the guest), then there is nothing to report */
    if (g_resize_failed_w && !scanout_is(g_resize_failed_w, g_resize_failed_h)) {
        char js[256];
        resize_state_json(js, sizeof(js));
        client_send_str(c, S_RESIZE_STATE, js);
    }
    pthread_mutex_lock(&clients_lock);
    c->need_key = true;
    c->last_sent = c->last_acked = atomic_load(&g_frame_id);
    c->last_ack_us = now_us();
    pthread_mutex_unlock(&clients_lock);
    pthread_mutex_lock(&fb_lock);
    if (g_video_config_json)
        client_send_str(c, S_VIDEO_CONFIG, g_video_config_json);
    char *sj = settings_json();
    client_send_str(c, S_SETTINGS, sj);
    g_free(sj);
    g_force_idr = true;
    pthread_cond_signal(&fb_cond);
    pthread_mutex_unlock(&fb_lock);
    LOG("client %d authenticated (%d viewers)", c->id, atomic_load(&g_authed));
}

/* Guest resize. Browsers send a request per resize event while the window is
 * dragged; act only once it settles (RESIZE_DEBOUNCE_MS).
 *  1. Console.SetUIInfo: virtio-gpu raises a display event. Linux guests
 *     resize on their own; the Windows driver (viogpudo) only offers the size
 *     as its "custom" mode and leaves switching to a user-session program.
 *  2. Ask the guest to switch: for a known Windows guest right away
 *     (SetDisplayConfig through qemu-guest-agent, guestexec.c; its listener
 *     retries until the driver has the new mode), otherwise only if the
 *     scanout hasn't changed a second later (spice agent, which works for
 *     QXL, and the guest agent path).
 *  3. Watch the scanout for up to RESIZE_WATCH_S (Windows) or
 *     RESIZE_WATCH_OTHER_S (others: a compositor that follows the hint does
 *     so within a second or two) and log the outcome; on failure the viewers
 *     get an S_NOTICE with the likely cause.
 * viogpudo sizes its framebuffer segment once at driver start: 16 MiB unless
 * a built-in mode needs more. Windows refuses larger modes, so clamp to that. */
#define RESIZE_MAX_PIXELS (16u * 1024 * 1024 / 4)
#define RESIZE_WATCH_S 15
#define RESIZE_WATCH_OTHER_S 5
#define RESIZE_DEBOUNCE_MS 150
static uint32_t g_resize_w, g_resize_h;
static guint g_resize_timer;
static int g_resize_checks;
static bool g_resize_asked;

static bool scanout_is(uint32_t w, uint32_t h)
{
    pthread_mutex_lock(&fb_lock);
    bool ok = fb.valid && (uint32_t)fb.w == w && (uint32_t)fb.h == h;
    pthread_mutex_unlock(&fb_lock);
    return ok;
}

/* What the viewers need to explain a failed resize (their help panel picks
 * the steps): the size, the guest OS as the guest agent reports it, and which
 * agents answer. All values are numbers, booleans or the sanitized OS id. */
static void resize_state_json(char *buf, size_t n)
{
    if (!g_resize_failed_w) {
        snprintf(buf, n, "{\"ok\":true}");
        return;
    }
    const char *os = guestexec_os_id();
    snprintf(buf, n,
             "{\"ok\":false,\"w\":%u,\"h\":%u,\"guest\":\"%s\",\"os\":\"%s\","
             "\"qga\":%s,\"vdagent\":%s}",
             g_resize_failed_w, g_resize_failed_h,
             !*os ? "unknown" : guestexec_is_windows() ? "windows" : "other", os,
             *os ? "true" : "false", vdagent_agent_connected() ? "true" : "false");
}

/* Tell the viewers the guest didn't follow, after every failed request: a
 * viewer may have dropped the last one (Resize guest unticked and ticked
 * again, the guest reached the size in between). The viewer doesn't repeat
 * a message it is already showing. */
static void resize_failed(uint32_t w, uint32_t h)
{
    LOG("guest did not resize to %ux%u", w, h);
    g_resize_failed_w = w;
    g_resize_failed_h = h;
    char js[256];
    resize_state_json(js, sizeof(js));
    broadcast_str(S_RESIZE_STATE, js);
}

static void resize_ok(void)
{
    if (!g_resize_failed_w)
        return;
    g_resize_failed_w = g_resize_failed_h = 0;
    broadcast_str(S_RESIZE_STATE, "{\"ok\":true}");
}

static gboolean resize_check(gpointer ud)
{
    g_resize_timer = 0;
    if (scanout_is(g_resize_w, g_resize_h)) {
        if (g_resize_asked)
            LOG("guest resized to %ux%u", g_resize_w, g_resize_h);
        resize_ok();
        return G_SOURCE_REMOVE;
    }
    if (!g_resize_asked) {
        g_resize_asked = true;
        vdagent_request_size(g_resize_w, g_resize_h);
        guestexec_request_size(g_resize_w, g_resize_h);
    } else if (++g_resize_checks >=
               (guestexec_is_windows() ? RESIZE_WATCH_S : RESIZE_WATCH_OTHER_S)) {
        resize_failed(g_resize_w, g_resize_h);
        return G_SOURCE_REMOVE;
    }
    g_resize_timer = g_timeout_add(1000, resize_check, NULL);
    return G_SOURCE_REMOVE;
}

static gboolean resize_fire(gpointer ud)
{
    uint32_t a = g_resize_w, b = g_resize_h;
    guint16 wmm = (guint16)(a * 254 / 960), hmm = (guint16)(b * 254 / 960);
    qemu_call("org.qemu.Display1.Console", "SetUIInfo",
              g_variant_new("(qqiiuu)", wmm, hmm, 0, 0, a, b));
    LOG("guest resize hint %ux%u", a, b);
    g_resize_checks = 0;
    g_resize_asked = guestexec_is_windows();
    if (g_resize_asked)
        guestexec_request_size(a, b);
    g_resize_timer = g_timeout_add(1000, resize_check, NULL);
    return G_SOURCE_REMOVE;
}

static void resize_request(uint32_t w, uint32_t h)
{
    if ((uint64_t)w * h > RESIZE_MAX_PIXELS) {
        double s = sqrt((double)RESIZE_MAX_PIXELS / ((double)w * h));
        uint32_t nw = (uint32_t)(w * s) & ~7u, nh = (uint32_t)(h * s) & ~1u;
        DBG("resize %ux%u exceeds the 16 MiB framebuffer, using %ux%u", w, h, nw, nh);
        w = nw;
        h = nh;
    }
    g_resize_w = w;
    g_resize_h = h;
    if (g_resize_timer)
        g_source_remove(g_resize_timer);
    g_resize_timer = g_timeout_add(RESIZE_DEBOUNCE_MS, resize_fire, NULL);
}

static bool handle_msg(Client *c, uint8_t type, const uint8_t *p, size_t n)
{
    if (!c->authed) {
        if (type != C_AUTH || !token_valid((const char *)p, n)) {
            LOG("client %d: authentication failed", c->id);
            client_send_str(c, S_ERROR, "authentication failed");
            return false;
        }
        client_authed(c);
        return true;
    }
    uint32_t a, b;
    switch (type) {
    case C_KEY:
        if (n >= 5) {
            memcpy(&a, p + 1, 4);
            key_event(c, p[0] != 0, a);
        }
        break;
    case C_MOUSE_ABS:
        if (n >= 8) {
            memcpy(&a, p, 4);
            memcpy(&b, p + 4, 4);
            pthread_mutex_lock(&fb_lock);
            int w = fb.w, h = fb.h;
            pthread_mutex_unlock(&fb_lock);
            if (w > 0 && h > 0) {
                if (a >= (uint32_t)w) a = (uint32_t)w - 1;
                if (b >= (uint32_t)h) b = (uint32_t)h - 1;
                qemu_call("org.qemu.Display1.Mouse", "SetAbsPosition",
                          g_variant_new("(uu)", a, b));
            }
        }
        break;
    case C_MOUSE_BTN:
        if (n >= 2)
            button_event(c, p[0] != 0, p[1]);
        break;
    case C_MOUSE_REL:
        if (n >= 8) {
            int32_t dx, dy;
            memcpy(&dx, p, 4);
            memcpy(&dy, p + 4, 4);
            qemu_call("org.qemu.Display1.Mouse", "RelMotion",
                      g_variant_new("(ii)", dx, dy));
        }
        break;
    case C_ACK:
        if (n >= 8) {
            memcpy(&a, p, 4);
            memcpy(&b, p + 4, 4);
            int64_t t = now_us();
            bool resync = false;
            pthread_mutex_lock(&clients_lock);
            if ((int32_t)(a - c->last_acked) > 0 && (int32_t)(c->last_sent - a) >= 0) {
                c->last_acked = a;
                c->last_ack_us = t;
                double ms = (t - c->sent_us[a % ACK_RING]) / 1000.0;
                c->ack_ms = c->ack_ms ? c->ack_ms * 0.9 + ms * 0.1 : ms;
                c->decode_ms = c->decode_ms ? c->decode_ms * 0.9 + b / 10000.0 : b / 1000.0;
                if (b > 0) { /* 0 = not decoded (dropped / straggler) */
                    double dec = b / 1000.0, net = ms > dec ? ms - dec : 0;
                    if (net < c->net_min_ms) c->net_min_ms = net;
                    if (dec < c->dec_min_ms) c->dec_min_ms = dec;
                    c->net_sum_ms += net;
                    c->net_n++;
                    if (c->hist_n == 0 && c->net_n == 4 && !cfg.max_inflight) {
                        /* first round trips of a new viewer: size the window
                         * now instead of at the next stats tick */
                        double iv = 1000.0 / cfg.fps;
                        int w = c->dec_min_ms > iv ? 2 : (int)ceil((c->net_min_ms + c->dec_min_ms) / iv) + 1;
                        c->window = clampi(w, 2, 8);
                    }
                }
            }
            if (c->paused && c->last_acked == c->last_sent) {
                c->paused = false;
                c->need_key = true;
                resync = true;
            }
            pthread_mutex_unlock(&clients_lock);
            if (resync) {
                LOG("client %d recovered, resync with IDR", c->id);
                pthread_mutex_lock(&fb_lock);
                g_force_idr = true;
                pthread_mutex_unlock(&fb_lock);
            }
            /* wake the encoder if it is waiting for ACKs (no lock needed:
             * it only ever waits with a short timeout) */
            pthread_cond_signal(&fb_cond);
        }
        break;
    case C_CONFIG: {
        char *js = g_strndup((const char *)p, n);
        pthread_mutex_lock(&fb_lock);
        Config old = cfg;
        config_apply_json(js);
        config_sanitize();
        if (cfg.max_kbps != old.max_kbps || cfg.abr != old.abr)
            g_cur_kbps = cfg.max_kbps;
        g_cur_kbps = clampi(g_cur_kbps, cfg.min_kbps, cfg.max_kbps);
        /* everything but the bitrate needs a new encoder session (+ IDR) */
        if (cfg.fps != old.fps || cfg.quality != old.quality ||
            cfg.vbv_frames != old.vbv_frames || strcmp(cfg.rc, old.rc) ||
            strcmp(cfg.tune, old.tune) || strcmp(cfg.multipass, old.multipass) ||
            strcmp(cfg.nvenc_preset, old.nvenc_preset) || cfg.spatial_aq != old.spatial_aq ||
            cfg.temporal_aq != old.temporal_aq ||
            (cfg.spatial_aq && cfg.aq_strength != old.aq_strength))
            g_reopen = true;
        char *sj = settings_json();
        pthread_cond_signal(&fb_cond);
        pthread_mutex_unlock(&fb_lock);
        LOG("client %d config: %s", c->id, js);
        broadcast_str(S_SETTINGS, sj);
        g_free(sj);
        g_free(js);
        break;
    }
    case C_KEYFRAME:
        pthread_mutex_lock(&clients_lock);
        c->need_key = true;
        pthread_mutex_unlock(&clients_lock);
        pthread_mutex_lock(&fb_lock);
        g_force_idr = true;
        pthread_cond_signal(&fb_cond);
        pthread_mutex_unlock(&fb_lock);
        break;
    case C_PING:
        client_send(c, S_PONG, p, n);
        break;
    case C_RESIZE:
        if (n >= 8) {
            memcpy(&a, p, 4);
            memcpy(&b, p + 4, 4);
            if (a >= 320 && b >= 200 && a <= 8192 && b <= 8192) {
                DBG("client %d: guest resize request %ux%u", c->id, a, b);
                resize_request(a, b);
            }
        }
        break;
    case C_AUDIO:
        if (n >= 1) {
            int m = p[0] <= AUDIO_PCM ? p[0] : AUDIO_OFF;
            if (atomic_exchange(&c->audio_mode, m) != m)
                LOG("client %d: audio %s", c->id,
                    m == AUDIO_OPUS ? "on (opus)" : m == AUDIO_PCM ? "on (pcm)" : "off");
        }
        break;
    case C_CLIPBOARD:
        clipboard_from_viewer(c, (const char *)p, n);
        break;
    default:
        DBG("client %d: unknown message 0x%02x", c->id, type);
    }
    return true;
}

static void client_remove(Client *c)
{
    if (c->watch) {
        g_source_remove(c->watch);
        c->watch = 0;
    }
    client_release_input(c);
    atomic_store(&c->dead, true);
    shutdown(c->fd, SHUT_RDWR);
    pthread_mutex_lock(&clients_lock);
    g_ptr_array_remove(g_clients, c);
    pthread_mutex_unlock(&clients_lock);
    if (c->authed)
        atomic_fetch_sub(&g_authed, 1);
    g_last_client_us = now_us();
    LOG("client %d disconnected (%d viewers)", c->id, atomic_load(&g_authed));
    client_unref(c);
}

static gboolean on_client_io(gint fd, GIOCondition cond, gpointer ud)
{
    Client *c = ud;
    uint8_t buf[16384];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            c->watch = 0;
            client_remove(c);
            return G_SOURCE_REMOVE;
        }
        if (r < 0)
            break;
        g_byte_array_append(c->rbuf, buf, (guint)r);
    }
    while (c->rbuf->len >= 5) {
        uint32_t len;
        memcpy(&len, c->rbuf->data, 4);
        if (len < 1 || len > PROTO_MAX_CLIENT_MSG) {
            LOG("client %d: bad frame length %u", c->id, len);
            c->watch = 0;
            client_remove(c);
            return G_SOURCE_REMOVE;
        }
        if (c->rbuf->len < 4 + len)
            break;
        bool ok = handle_msg(c, c->rbuf->data[4], c->rbuf->data + 5, len - 1);
        g_byte_array_remove_range(c->rbuf, 0, 4 + len);
        if (!ok) {
            c->watch = 0;
            client_remove(c);
            return G_SOURCE_REMOVE;
        }
    }
    if (atomic_load(&c->dead)) {
        c->watch = 0;
        client_remove(c);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean auth_timeout(gpointer ud)
{
    int id = GPOINTER_TO_INT(ud);
    pthread_mutex_lock(&clients_lock);
    Client *found = NULL;
    for (guint i = 0; i < g_clients->len; i++) {
        Client *c = g_clients->pdata[i];
        if (c->id == id && !c->authed)
            found = c;
    }
    pthread_mutex_unlock(&clients_lock);
    if (found) {
        LOG("client %d: auth timeout", id);
        client_remove(found);
    }
    return G_SOURCE_REMOVE;
}

/* periodically reap clients whose writer saw an error */
static gboolean reap_tick(gpointer ud)
{
    for (;;) {
        Client *dead = NULL;
        pthread_mutex_lock(&clients_lock);
        for (guint i = 0; i < g_clients->len; i++) {
            Client *c = g_clients->pdata[i];
            if (atomic_load(&c->dead))
                dead = c;
        }
        pthread_mutex_unlock(&clients_lock);
        if (!dead)
            break;
        client_remove(dead);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_accept(gint lfd, GIOCondition cond, gpointer ud)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return G_SOURCE_CONTINUE;
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    struct timeval tv = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    Client *c = calloc(1, sizeof(*c));
    c->fd = fd;
    c->id = g_next_client_id++;
    atomic_init(&c->ref, 1);
    c->rbuf = g_byte_array_new();
    c->connected_us = now_us();
    c->net_min_ms = c->dec_min_ms = 1e9;
    c->window = 3; /* until the first round trips are measured */
    pthread_mutex_init(&c->wlock, NULL);
    pthread_mutex_lock(&clients_lock);
    g_ptr_array_add(g_clients, c);
    pthread_mutex_unlock(&clients_lock);
    c->watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_client_io, c);
    g_timeout_add_seconds(10, auth_timeout, GINT_TO_POINTER(c->id));
    g_last_client_us = now_us();
    DBG("client %d connected", c->id);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */
static int listen_socket(int vmid)
{
    mkdir(RUN_DIR, 0711);
    chmod(RUN_DIR, 0711);
    int pl = snprintf(g_sock_path, sizeof(g_sock_path), RUN_DIR "/%d.sock", vmid);
    if (pl < 0 || (size_t)pl >= sizeof(g_sock_path)) {
        LOG("socket path too long");
        g_sock_path[0] = 0;
        return -1;
    }
    unlink(g_sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        LOG("socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    memcpy(sa.sun_path, g_sock_path, (size_t)pl + 1);
    mode_t old = umask(0177);
    int r = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old);
    if (r < 0 || listen(fd, 16) < 0) {
        LOG("bind %s: %s", g_sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    /* pveproxy runs as www-data */
    struct group *gr = getgrnam("www-data");
    if (gr && chown(g_sock_path, 0, gr->gr_gid) == 0)
        chmod(g_sock_path, 0660);
    return fd;
}

static void sd_notify_ready(void)
{
    const char *p = getenv("NOTIFY_SOCKET");
    if (!p || !*p)
        return;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    size_t l = strlen(p);
    if (l >= sizeof(sa.sun_path))
        return;
    memcpy(sa.sun_path, p, l);
    if (sa.sun_path[0] == '@')
        sa.sun_path[0] = 0;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    sendto(fd, "READY=1", 7, 0, (struct sockaddr *)&sa,
           (socklen_t)(offsetof(struct sockaddr_un, sun_path) + l));
    close(fd);
}

static gboolean on_signal(gpointer ud)
{
    atomic_store(&g_quit, true);
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: pve-h264d [-v] [-b backend] [-f fps] [-m max_kbps] [-q quality] <vmid>\n"
            "       pve-h264d --probe     test all encoder backends and explain failures\n"
            "  backend: auto | nvenc | vaapi | x264   (config: " CONF_FILE ")\n");
    exit(2);
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    load_config();
    /* libav chatter (e.g. x264 end-of-stream stats) only in verbose mode */
    av_log_set_level(getenv("PVE_H264D_DEBUG") ? AV_LOG_INFO : AV_LOG_WARNING);
    if (argc >= 2 && !strcmp(argv[1], "--probe")) {
        config_sanitize();
        EncoderParams p = {
            .backend = cfg.backend, .vaapi_device = cfg.vaapi_device,
            .fps = clampi(cfg.fps, 5, 144), .max_kbps = clampi(cfg.max_kbps, 1000, 200000),
            .quality = clampi(cfg.quality, 1, 51), .vbv_frames = clampi(cfg.vbv_frames, 1, 120),
            .nvenc_preset = cfg.nvenc_preset, .gpu = cfg.gpu, .rc = cfg.rc,
            .tune = cfg.tune, .multipass = cfg.multipass, .spatial_aq = cfg.spatial_aq,
            .aq_strength = cfg.aq_strength, .temporal_aq = cfg.temporal_aq,
        };
        return probe_main(&p);
    }
    int opt;
    while ((opt = getopt(argc, argv, "vb:f:m:q:")) != -1) {
        switch (opt) {
        case 'v': g_verbose = 1; av_log_set_level(AV_LOG_INFO); break;
        case 'b': snprintf(cfg.backend, sizeof(cfg.backend), "%s", optarg); break;
        case 'f': cfg.fps = atoi(optarg); break;
        case 'm': cfg.max_kbps = atoi(optarg); break;
        case 'q': cfg.quality = atoi(optarg); break;
        default: usage();
        }
    }
    if (optind >= argc)
        usage();
    g_vmid = atoi(argv[optind]);
    if (g_vmid < 100 || g_vmid > 999999999)
        usage();
    if (getenv("PVE_H264D_DEBUG"))
        g_verbose = 1;
    config_sanitize();
    cfg_defaults = cfg;
    g_cur_kbps = cfg.max_kbps;

    /* single instance per VM */
    mkdir(RUN_DIR, 0711);
    char lockp[128];
    snprintf(lockp, sizeof(lockp), RUN_DIR "/%d.lock", g_vmid);
    int lockfd = open(lockp, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        LOG("another pve-h264d for VM %d is running", g_vmid);
        sd_notify_ready();
        return 0;
    }

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&fb_cond, &ca);
    g_clients = g_ptr_array_new();
    g_loop = g_main_loop_new(NULL, FALSE);

    LOG("pve-h264d starting for VM %d: backend=%s fps=%d max=%dkbps q=%d rc=%s "
        "preset=%s/%s multipass=%s aq=%d/%d taq=%d vbv=%df inflight=%s abr=%d",
        g_vmid, cfg.backend, cfg.fps, cfg.max_kbps, cfg.quality, cfg.rc,
        cfg.nvenc_preset, cfg.tune, cfg.multipass, cfg.spatial_aq, cfg.aq_strength,
        cfg.temporal_aq, cfg.vbv_frames,
        cfg.max_inflight ? (char[4]){'0' + cfg.max_inflight / 10, '0' + cfg.max_inflight % 10, 0} : "auto",
        cfg.abr);

    const char *test = getenv("PVE_H264D_TEST");
    pthread_t tth;
    if (test && sscanf(test, "%dx%d", &g_test_w, &g_test_h) == 2 &&
        g_test_w >= 64 && g_test_h >= 64 && g_test_w <= 7680 && g_test_h <= 4320) {
        g_test_video = strstr(test, "video") != NULL;
        LOG("TEST MODE: synthetic %dx%d@60 %s source instead of VM display", g_test_w,
            g_test_h, g_test_video ? "video" : "desktop");
        pthread_create(&tth, NULL, g_test_video ? testvideo_thread : testpattern_thread, NULL);
        audio_init(NULL, cfg.audio);
    } else if (qemu_connect(g_vmid) < 0) {
        return 1;
    }
    int lfd = listen_socket(g_vmid);
    if (lfd < 0)
        return 1;
    g_unix_fd_add(lfd, G_IO_IN, on_accept, NULL);
    g_timeout_add_seconds(1, stats_tick, NULL);
    g_timeout_add(500, reap_tick, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_last_client_us = now_us();

    pthread_t th;
    pthread_create(&th, NULL, encoder_thread, NULL);
    sd_notify_ready();
    g_main_loop_run(g_loop);

    atomic_store(&g_quit, true);
    pthread_mutex_lock(&fb_lock);
    pthread_cond_broadcast(&fb_cond);
    pthread_mutex_unlock(&fb_lock);
    pthread_join(th, NULL);
    unlink(g_sock_path);
    LOG("exit");
    return 0;
}
