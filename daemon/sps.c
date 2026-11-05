/* SPDX-License-Identifier: MIT */
/*
 * H.264 SPS rewriter: force VUI bitstream_restriction with
 * max_num_reorder_frames = 0.
 *
 * Without it decoders (Chromium's FFmpeg software path and its hardware
 * H264Decoder) must assume frame reordering and hold decoded pictures in the
 * DPB until it fills -> several frames of added latency, and on a static
 * screen the last frames are never shown at all. NVENC does not write the
 * flag, so we patch every SPS we emit. Only IDR packets carry an SPS, so the
 * cost is negligible.
 */
#include "pve-h264d.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *d;
    size_t n, pos; /* bit position */
    bool err;
} BR;

static unsigned br_u(BR *b, int bits)
{
    unsigned v = 0;
    while (bits--) {
        if (b->pos >= b->n * 8) {
            b->err = true;
            return 0;
        }
        v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static unsigned br_ue(BR *b)
{
    int z = 0;
    while (!b->err && br_u(b, 1) == 0 && z < 32)
        z++;
    return z ? ((1u << z) - 1 + br_u(b, z)) : 0;
}

static int br_se(BR *b)
{
    unsigned k = br_ue(b);
    return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

typedef struct {
    uint8_t *d;
    size_t cap, pos;
} BW;

static void bw_u(BW *w, unsigned v, int bits)
{
    while (bits--) {
        if ((w->pos >> 3) >= w->cap)
            return;
        if ((w->pos & 7) == 0)
            w->d[w->pos >> 3] = 0;
        if ((v >> bits) & 1)
            w->d[w->pos >> 3] |= (uint8_t)(1 << (7 - (w->pos & 7)));
        w->pos++;
    }
}

static void bw_ue(BW *w, unsigned v)
{
    unsigned x = v + 1;
    int len = 0;
    for (unsigned t = x; t > 1; t >>= 1)
        len++;
    bw_u(w, 0, len);
    bw_u(w, x, len + 1);
}

static void skip_scaling_list(BR *b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next != 0)
            next = (last + br_se(b) + 256) % 256;
        last = next == 0 ? last : next;
    }
}

static void skip_hrd(BR *b)
{
    unsigned cnt = br_ue(b) + 1;
    br_u(b, 4);
    br_u(b, 4);
    for (unsigned i = 0; i < cnt && !b->err && i < 32; i++) {
        br_ue(b);
        br_ue(b);
        br_u(b, 1);
    }
    br_u(b, 20);
}

/* rbsp (no emulation prevention, starting at the NAL header byte) ->
 * new rbsp in out; returns length or 0 on parse failure. */
static size_t fix_sps_rbsp(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    BR b = {in, n, 8, false}; /* skip NAL header */
    unsigned profile = br_u(&b, 8);
    br_u(&b, 16); /* constraints + level */
    br_ue(&b);    /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        unsigned chroma = br_ue(&b);
        if (chroma == 3)
            br_u(&b, 1);
        br_ue(&b);
        br_ue(&b);
        br_u(&b, 1);
        if (br_u(&b, 1)) {
            for (int i = 0; i < (chroma == 3 ? 12 : 8); i++)
                if (br_u(&b, 1))
                    skip_scaling_list(&b, i < 6 ? 16 : 64);
        }
    }
    br_ue(&b); /* log2_max_frame_num */
    unsigned poc = br_ue(&b);
    if (poc == 0) {
        br_ue(&b);
    } else if (poc == 1) {
        br_u(&b, 1);
        br_se(&b);
        br_se(&b);
        unsigned c = br_ue(&b);
        for (unsigned i = 0; i < c && i < 256 && !b.err; i++)
            br_se(&b);
    }
    unsigned max_refs = br_ue(&b);
    br_u(&b, 1);
    br_ue(&b);
    br_ue(&b);
    if (!br_u(&b, 1)) /* frame_mbs_only */
        br_u(&b, 1);
    br_u(&b, 1);
    if (br_u(&b, 1)) {
        br_ue(&b);
        br_ue(&b);
        br_ue(&b);
        br_ue(&b);
    }
    size_t vui_flag_pos = b.pos;
    bool vui = br_u(&b, 1);
    size_t cut; /* copy bits [0,cut), then write our tail */
    if (vui) {
        if (br_u(&b, 1) && br_u(&b, 8) == 255)
            br_u(&b, 32);
        if (br_u(&b, 1))
            br_u(&b, 1);
        if (br_u(&b, 1)) {
            br_u(&b, 4);
            if (br_u(&b, 1))
                br_u(&b, 24);
        }
        if (br_u(&b, 1)) {
            br_ue(&b);
            br_ue(&b);
        }
        if (br_u(&b, 1))
            br_u(&b, 65);
        bool nal = br_u(&b, 1);
        if (nal)
            skip_hrd(&b);
        bool vcl = br_u(&b, 1);
        if (vcl)
            skip_hrd(&b);
        if (nal || vcl)
            br_u(&b, 1);
        br_u(&b, 1); /* pic_struct_present */
        cut = b.pos; /* position of bitstream_restriction_flag */
    } else {
        cut = vui_flag_pos;
    }
    if (b.err)
        return 0;

    BW w = {out, cap, 0};
    BR c = {in, n, 0, false};
    for (size_t i = 0; i < cut; i++)
        bw_u(&w, br_u(&c, 1), 1);
    if (!vui) {
        bw_u(&w, 1, 1); /* vui_parameters_present_flag */
        /* aspect_ratio_info, overscan_info, video_signal_type, chroma_loc,
         * timing_info, nal_hrd, vcl_hrd, pic_struct_present: all 0 */
        bw_u(&w, 0, 8);
    }
    bw_u(&w, 1, 1);    /* bitstream_restriction_flag */
    bw_u(&w, 1, 1);    /* motion_vectors_over_pic_boundaries_flag */
    bw_ue(&w, 2);      /* max_bytes_per_pic_denom */
    bw_ue(&w, 1);      /* max_bits_per_mb_denom */
    bw_ue(&w, 16);     /* log2_max_mv_length_horizontal */
    bw_ue(&w, 16);     /* log2_max_mv_length_vertical */
    bw_ue(&w, 0);      /* max_num_reorder_frames */
    bw_ue(&w, max_refs ? max_refs : 1); /* max_dec_frame_buffering */
    bw_u(&w, 1, 1);    /* rbsp_stop_one_bit */
    while (w.pos & 7)
        bw_u(&w, 0, 1);
    return w.pos >> 3;
}

static size_t unescape(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 2 < n && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 3) {
            out[o++] = 0;
            out[o++] = 0;
            i += 2;
            continue;
        }
        out[o++] = in[i];
    }
    return o;
}

static size_t escape(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            out[o++] = 3;
            zeros = 0;
        }
        out[o++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

static size_t next_start(const uint8_t *d, size_t n, size_t from, size_t *sc_len)
{
    for (size_t i = from; i + 3 <= n; i++) {
        if (d[i] == 0 && d[i + 1] == 0) {
            if (d[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 4 <= n && d[i + 2] == 0 && d[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    *sc_len = 0;
    return n;
}

/* Returns a malloc'ed packet with patched SPS (and sets *outlen), or NULL if
 * the packet has no SPS / could not be parsed (caller sends the original). */
uint8_t *h264_fix_sps(const uint8_t *pkt, size_t n, size_t *outlen)
{
    size_t sc, start = next_start(pkt, n, 0, &sc);
    while (start < n) {
        size_t nal = start + sc;
        size_t sc2, end = next_start(pkt, n, nal, &sc2);
        if (nal < n && (pkt[nal] & 0x1f) == 7) {
            size_t len = end - nal;
            uint8_t *raw = malloc(len + 64), *fixed = malloc(len + 64),
                    *esc = malloc(2 * len + 128);
            size_t rl = unescape(pkt + nal, len, raw);
            size_t fl = fix_sps_rbsp(raw, rl, fixed, len + 64);
            uint8_t *res = NULL;
            if (fl) {
                size_t el = escape(fixed, fl, esc);
                res = malloc(n - len + el);
                memcpy(res, pkt, nal);
                memcpy(res + nal, esc, el);
                memcpy(res + nal + el, pkt + end, n - end);
                *outlen = n - len + el;
            }
            free(raw);
            free(fixed);
            free(esc);
            return res;
        }
        start = end;
        sc = sc2;
    }
    return NULL;
}
