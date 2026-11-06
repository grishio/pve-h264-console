/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - low latency H.264 console daemon for Proxmox VE
 * Shared declarations.
 */
#ifndef PVE_H264D_H
#define PVE_H264D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern int g_verbose;

#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define DBG(fmt, ...)                                                          \
    do {                                                                       \
        if (g_verbose)                                                         \
            fprintf(stderr, "[dbg] " fmt "\n", ##__VA_ARGS__);                 \
    } while (0)

int64_t now_us(void);

/* ------------------------------------------------------------------ */
/* Wire protocol (both directions): [u32 len LE][u8 type][payload]     */
/* len counts type byte + payload.                                     */
/* ------------------------------------------------------------------ */
#define PROTO_VERSION 1
#define CLIPBOARD_MAX (1024 * 1024) /* bytes of clipboard text, each way */
#define PROTO_MAX_CLIENT_MSG (CLIPBOARD_MAX + 64)

enum {
    /* client -> server */
    C_AUTH = 0x01,      /* utf8 one-time token */
    C_KEY = 0x02,       /* u8 down, u32 qnum */
    C_MOUSE_ABS = 0x03, /* u32 x, u32 y (framebuffer pixels) */
    C_MOUSE_BTN = 0x04, /* u8 down, u8 button (qemu numbering) */
    C_MOUSE_REL = 0x05, /* i32 dx, i32 dy */
    C_ACK = 0x06,       /* u32 frame_id, u32 decode_us */
    C_CONFIG = 0x07,    /* utf8 json, any subset of the settings keys */
    C_KEYFRAME = 0x08,  /* (empty) request IDR */
    C_PING = 0x09,      /* f64 client timestamp, echoed back */
    C_RESIZE = 0x0A,    /* u32 w, u32 h : SetUIInfo hint + spice agent */
    C_AUDIO = 0x0B,     /* u8 mode: AUDIO_OFF | AUDIO_OPUS | AUDIO_PCM */
    C_CLIPBOARD = 0x0C, /* utf8 text: the viewer's clipboard, for the guest */
};

enum {
    /* server -> client */
    S_HELLO = 0x81,        /* utf8 json */
    S_VIDEO_CONFIG = 0x82, /* utf8 json {codec,width,height,coded_width,...} */
    S_FRAME = 0x83,        /* u32 id, u8 flags, u32 encode_us, u64 cap_us, annexb */
    S_CURSOR = 0x84,       /* i32 w,h,hot_x,hot_y, RGBA bytes */
    S_CURSOR_POS = 0x85,   /* i32 x, i32 y, u8 visible */
    S_PONG = 0x86,         /* f64 echoed client timestamp */
    S_STATS = 0x87,        /* utf8 json */
    S_ERROR = 0x88,        /* utf8 message, connection will close */
    S_MOUSE_MODE = 0x89,   /* u8 absolute */
    S_DISPLAY_OFF = 0x8A,  /* (empty) guest display disabled */
    S_SETTINGS = 0x8B,     /* utf8 json: effective encoder settings */
    S_NOTICE = 0x8C,       /* utf8 message, informational (connection stays) */
    S_AUDIO_STATE = 0x8D,  /* utf8 json {available,playing,rate,channels,reason} */
    S_AUDIO = 0x8E,        /* u32 seq, u64 ts_us, u8 codec, u8 channels,
                              u16 samples, data (opus packet | s16le interleaved) */
    S_CLIPBOARD = 0x8F,    /* u8 flags, utf8 text: the guest's clipboard */
    S_CLIPBOARD_STATE = 0x90, /* utf8 json {available,to_guest,from_guest,max,reason} */
    S_RESIZE_STATE = 0x91, /* utf8 json {ok,w,h,guest,os,qga,vdagent}: guest
                              didn't follow Resize guest (ok:false) or does again */
};

#define FRAME_FLAG_KEY 0x01
/* S_CLIPBOARD flags */
#define CLIP_FLAG_REPLAY 0x01 /* sent on connect: show it, don't copy it locally */

/* clipboard directions (config "clipboard") */
enum { CLIP_TO_GUEST = 1, CLIP_FROM_GUEST = 2 };

/* audio modes (C_AUDIO) = codec byte of S_AUDIO */
enum { AUDIO_OFF = 0, AUDIO_OPUS = 1, AUDIO_PCM = 2 };
#define AUDIO_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_FRAME 480 /* samples per packet: 10 ms */

/* ------------------------------------------------------------------ */
/* Encoder abstraction (encoder.c)                                     */
/* ------------------------------------------------------------------ */
typedef struct Encoder Encoder;

typedef struct {
    const char *backend;      /* "auto", "nvenc", "nvenc-sysmem", "vaapi", "x264" */
    const char *vaapi_device; /* NULL/"" = probe /dev/dri/renderD12x */
    int width, height;        /* source size (may be odd) */
    int fps;                  /* nominal frame rate for rate control */
    int max_kbps;             /* VBR ceiling / CBR rate */
    int quality;              /* 1..51 target quality (cq / crf / qp) */
    int vbv_frames;           /* VBV buffer size in frames at max rate */
    const char *nvenc_preset; /* p1..p7 (x264: mapped to ultrafast..medium) */
    int gpu;                  /* nvenc gpu index */
    /* NULL / 0 = defaults (vbr, ull, no multipass, no AQ) */
    const char *rc;           /* "vbr" (quality-targeted, capped), "cbr", "cqp" */
    const char *tune;         /* nvenc: "ull", "ll", "hq" */
    const char *multipass;    /* nvenc: "disabled", "qres", "fullres" */
    int spatial_aq;           /* nvenc spatial AQ / x264 aq-mode 1 */
    int aq_strength;          /* 1..15 (nvenc; x264: /8) */
    int temporal_aq;          /* nvenc only */
} EncoderParams;

typedef void (*EncoderPacketCb)(const uint8_t *data, size_t len, bool key,
                                void *opaque);

Encoder *encoder_open(const EncoderParams *p, char *err, size_t errlen);
/* Writable, persistent BGRX (AV_PIX_FMT_BGR0) frame of coded size. */
uint8_t *encoder_input(Encoder *e, int *stride);
/* rect = {x,y,w,h} of pixels changed in the shadow since the last call, or
 * NULL if nothing changed (IDR / refinement re-encode of the same picture). */
int encoder_encode(Encoder *e, bool force_idr, const int *rect,
                   EncoderPacketCb cb, void *opaque);
void encoder_set_bitrate(Encoder *e, int max_kbps);
const char *encoder_name(const Encoder *e);
const char *encoder_kind(const Encoder *e); /* "nvenc", "vaapi", "x264" */
void encoder_coded_size(const Encoder *e, int *w, int *h);
void encoder_close(Encoder *e);

/* probe.c */
int probe_main(const EncoderParams *base);

/* vdagent.c (main thread only); conn is the GDBusConnection to QEMU */
void vdagent_attach(void *conn, int clip_mode);
void vdagent_request_size(uint32_t w, uint32_t h);
bool vdagent_agent_connected(void);
/* offer a viewer's clipboard text (LF line ends) to the guest */
bool vdagent_clipboard_set(const char *text, size_t len);
char *vdagent_clipboard_state_json(void); /* g_free() */

/* pve-h264d.c, called by vdagent.c (main thread) */
void clipboard_from_guest(const char *text, size_t len);
void clipboard_state_changed(void);

/* guestexec.c: Windows guest resize via qemu-guest-agent (own thread) */
void guestexec_init(int vmid, bool enabled);
void guestexec_request_size(uint32_t w, uint32_t h);
bool guestexec_is_windows(void); /* known Windows guest, feature enabled */
/* guest-get-osinfo "id" ([a-z0-9._-] only), "" while unknown (agent not
 * answering, or guest_exec_resize = 0). Any thread. */
const char *guestexec_os_id(void);

/* audio.c: guest audio from QEMU's D-Bus audio backend (own thread).
 * Timestamps are now_us() (CLOCK_MONOTONIC), the clock video cap_us uses. */
void audio_init(void *qemu_conn, bool enabled); /* NULL conn: test tone */
char *audio_state_json(void);                   /* g_free() */
int audio_kbps(void);                           /* last second, all modes */

/* pve-h264d.c, for audio.c (thread safe) */
int audio_modes_wanted(void);                   /* bitmask 1 << AUDIO_* */
void audio_send(int mode, const void *hdr, size_t hl, const void *d, size_t dl);
void audio_state_changed(void);
bool av_test_flash(int64_t t_us);               /* test mode: beep + flash */

/* sps.c */
uint8_t *h264_fix_sps(const uint8_t *pkt, size_t n, size_t *outlen);

#endif
