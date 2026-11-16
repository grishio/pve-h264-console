/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - guest audio carried inside the H.264 bitstream.
 *
 * Each 10 ms audio packet becomes one SEI NAL unit (nal_unit_type 6) with a
 * single user_data_unregistered message (payloadType 5): a 16-byte UUID that
 * marks it as ours, then a version byte, the 16-byte audio header of S_AUDIO
 * (u32 seq, u64 ts_us, u8 codec, u8 channels, u16 samples) and the Opus
 * packet or s16le PCM. Decoders are required to skip SEI they don't know, so
 * the stream stays plain, playable H.264; the viewer takes these NAL units
 * out again before it decodes the picture.
 */
#include <string.h>

#include "pve-h264d.h"

/* 3f0b8a6e-5c21-4d97-b4e2-91a7c6d05f18 */
const uint8_t H264_AUDIO_SEI_UUID[16] = {
    0x3f, 0x0b, 0x8a, 0x6e, 0x5c, 0x21, 0x4d, 0x97,
    0xb4, 0xe2, 0x91, 0xa7, 0xc6, 0xd0, 0x5f, 0x18,
};

typedef struct {
    uint8_t *p;
    size_t n;
    int zeros; /* consecutive 0x00 bytes just written */
} Rbsp;

/* write one RBSP byte, inserting an emulation prevention byte (0x03) where
 * the payload would otherwise contain a start code prefix */
static inline void put(Rbsp *r, uint8_t b)
{
    if (r->zeros >= 2 && b <= 3) {
        r->p[r->n++] = 3;
        r->zeros = 0;
    }
    r->p[r->n++] = b;
    r->zeros = b ? 0 : r->zeros + 1;
}

static void put_bytes(Rbsp *r, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put(r, d[i]);
}

size_t h264_sei_audio(uint8_t *out, size_t cap, const uint8_t *hdr, size_t hl,
                      const uint8_t *d, size_t dl)
{
    size_t psize = sizeof(H264_AUDIO_SEI_UUID) + 1 + hl + dl;
    /* start code + header + size bytes + payload with worst-case escaping */
    if (cap < 5 + psize / 255 + 2 + (psize + 2) * 3 / 2 + 1)
        return 0;
    static const uint8_t start[5] = {0, 0, 0, 1, 6}; /* nal_ref_idc 0, type 6 */
    memcpy(out, start, sizeof(start));
    Rbsp r = {.p = out, .n = sizeof(start)};
    put(&r, 5); /* user_data_unregistered */
    size_t s = psize;
    for (; s >= 255; s -= 255)
        put(&r, 255);
    put(&r, (uint8_t)s);
    put_bytes(&r, H264_AUDIO_SEI_UUID, sizeof(H264_AUDIO_SEI_UUID));
    put(&r, H264_AUDIO_SEI_VERSION);
    put_bytes(&r, hdr, hl);
    put_bytes(&r, d, dl);
    put(&r, 0x80); /* rbsp_stop_one_bit + alignment; also: never ends in 0x00 */
    return r.n;
}

/* Offset of the start code of the first slice (VCL NAL types 1-5), where SEI
 * has to go in an access unit; n if there is none. */
size_t h264_first_vcl(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        int t = d[i + 3] & 0x1f;
        if (t >= 1 && t <= 5)
            return i > 0 && d[i - 1] == 0 ? i - 1 : i;
        i += 2;
    }
    return n;
}
