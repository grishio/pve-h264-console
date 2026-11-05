/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - encoder backends via libavcodec:
 *
 *   nvenc-cuda : NVENC fed from a persistent CUDA surface. Only the damaged
 *                rectangle is uploaded (pinned host shadow -> device), so a
 *                small screen change costs microseconds instead of copying
 *                and DMAing the whole 8 MB 1080p frame every time.
 *   nvenc      : NVENC from system memory (fallback if CUDA is unavailable).
 *   vaapi      : Intel / AMD (VA-API), swscale BGRX->NV12 + surface upload.
 *   x264       : CPU fallback.
 *
 * All backends are configured for minimum latency:
 *   - no B-frames, no lookahead, zero reorder delay
 *   - "infinite" GOP, IDR only on request (new viewer / decoder resync)
 *   - quality-targeted VBR capped by a max bitrate (adjustable at runtime),
 *     or CBR / constant QP; preset, tuning, multipass and AQ are tunable
 *   - in-band SPS/PPS on every IDR (Annex B), suitable for WebCodecs
 *
 * NVENC converts RGB->YUV on the GPU and signals BT.601 limited range for
 * RGB input; VA-API/x264 use swscale with the same matrix for consistency.
 */
#define _GNU_SOURCE
#include "pve-h264d.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#ifdef HAVE_VA_VENDOR /* set by the Makefile when libva-dev is present */
#include <va/va.h>
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

/* --- minimal CUDA driver API (no toolkit needed; libcuda via dlopen) --- */
#define CUDA_VERSION 12000
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;
typedef int CUresult;
typedef struct {
    size_t srcXInBytes, srcY;
    int srcMemoryType;
    const void *srcHost;
    CUdeviceptr srcDevice;
    void *srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    int dstMemoryType;
    void *dstHost;
    CUdeviceptr dstDevice;
    void *dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
} CUDA_MEMCPY2D;
#define CU_MEMORYTYPE_HOST 1
#define CU_MEMORYTYPE_DEVICE 2
#include <libavutil/hwcontext_cuda.h>

static struct {
    bool tried, ok;
    CUresult (*memcpy2d)(const CUDA_MEMCPY2D *);
    CUresult (*push)(CUcontext);
    CUresult (*pop)(CUcontext *);
    CUresult (*host_register)(void *, size_t, unsigned);
    CUresult (*host_unregister)(void *);
} cu;

static bool cuda_load(void)
{
    if (cu.tried)
        return cu.ok;
    cu.tried = true;
    void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return false;
    cu.memcpy2d = dlsym(h, "cuMemcpy2D_v2");
    cu.push = dlsym(h, "cuCtxPushCurrent_v2");
    cu.pop = dlsym(h, "cuCtxPopCurrent_v2");
    cu.host_register = dlsym(h, "cuMemHostRegister_v2");
    cu.host_unregister = dlsym(h, "cuMemHostUnregister");
    cu.ok = cu.memcpy2d && cu.push && cu.pop && cu.host_register && cu.host_unregister;
    return cu.ok;
}

typedef enum { BK_NVENC, BK_NVENC_CUDA, BK_VAAPI, BK_X264 } Backend;

struct Encoder {
    Backend bk;
    AVCodecContext *ctx;
    AVFrame *src;  /* BGR0 shadow (system memory backends) */
    AVFrame *conv; /* sw YUV frame for x264 (yuv420p) / vaapi (nv12) */
    AVBufferRef *hwdev;
    struct SwsContext *sws;
    AVPacket *pkt;
    /* nvenc-cuda */
    AVFrame *cuframe;  /* persistent device surface */
    CUcontext cuctx;
    uint8_t *pinned;   /* page-locked BGRX shadow */
    size_t pinned_len;
    int pinned_stride;
    bool upload_all;
    int w, h; /* coded size (even) */
    int fps;
    int vbv_frames;
    bool cbr, cqp;
    int64_t pts;
    int64_t t_upload, t_send, t_n;
    char name[112];
};

static const char *rc_of(const EncoderParams *p)
{
    return p->rc && (!strcmp(p->rc, "cbr") || !strcmp(p->rc, "cqp")) ? p->rc : "vbr";
}
static const char *preset_of(const EncoderParams *p)
{
    const char *v = p->nvenc_preset;
    return v && v[0] == 'p' && v[1] >= '1' && v[1] <= '7' && !v[2] ? v : "p2";
}

/* human-readable summary of the tunables, e.g. "p4/ull, vbr cq24, aq8, qres" */
static void describe(char *out, size_t n, const EncoderParams *p, bool nvenc)
{
    const char *rc = rc_of(p);
    int l = 0;
    if (nvenc)
        l = snprintf(out, n, "%s/%s, ", preset_of(p), p->tune ? p->tune : "ull");
    if (!strcmp(rc, "vbr"))
        l += snprintf(out + l, n - l, "vbr %s%d", nvenc ? "cq" : "crf", p->quality);
    else if (!strcmp(rc, "cqp"))
        l += snprintf(out + l, n - l, "qp%d", p->quality);
    else
        l += snprintf(out + l, n - l, "cbr");
    if (nvenc && p->spatial_aq)
        l += snprintf(out + l, n - l, ", aq%d", p->aq_strength ? p->aq_strength : 8);
    if (nvenc && p->temporal_aq)
        l += snprintf(out + l, n - l, ", taq");
    if (nvenc && p->multipass && strcmp(p->multipass, "disabled"))
        snprintf(out + l, n - l, ", %s", p->multipass);
}

static void set_common(AVCodecContext *ctx, const EncoderParams *p, int w,
                       int h)
{
    ctx->width = w;
    ctx->height = h;
    ctx->time_base = (AVRational){1, p->fps};
    ctx->framerate = (AVRational){p->fps, 1};
    ctx->gop_size = INT_MAX; /* IDR only when we ask for one */
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER; /* in-band SPS/PPS */
    ctx->rc_max_rate = (int64_t)p->max_kbps * 1000;
    ctx->rc_buffer_size =
        (int)((int64_t)p->max_kbps * 1000 * p->vbv_frames / p->fps);
    ctx->colorspace = AVCOL_SPC_BT470BG;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_IEC61966_2_1; /* sRGB desktop content */
    ctx->color_range = AVCOL_RANGE_MPEG;
    if (!strcmp(rc_of(p), "cbr"))
        ctx->bit_rate = ctx->rc_max_rate;
}

static void nvenc_opts(AVCodecContext *ctx, const EncoderParams *p)
{
    const char *rc = rc_of(p);
    const char *tune = p->tune && (!strcmp(p->tune, "ll") || !strcmp(p->tune, "hq")) ? p->tune : "ull";
    const char *mp = p->multipass && (!strcmp(p->multipass, "qres") || !strcmp(p->multipass, "fullres"))
                         ? p->multipass : "disabled";
    av_opt_set(ctx->priv_data, "preset", preset_of(p), 0);
    av_opt_set(ctx->priv_data, "tune", tune, 0);
    if (!strcmp(rc, "cqp")) {
        av_opt_set(ctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(ctx->priv_data, "qp", p->quality, 0);
        ctx->rc_max_rate = ctx->rc_buffer_size = 0;
    } else if (!strcmp(rc, "cbr")) {
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
    } else {
        av_opt_set(ctx->priv_data, "rc", "vbr", 0);
        av_opt_set_double(ctx->priv_data, "cq", p->quality, 0);
    }
    av_opt_set(ctx->priv_data, "multipass", mp, 0);
    av_opt_set_int(ctx->priv_data, "spatial-aq", p->spatial_aq ? 1 : 0, 0);
    if (p->spatial_aq)
        av_opt_set_int(ctx->priv_data, "aq-strength",
                       p->aq_strength >= 1 && p->aq_strength <= 15 ? p->aq_strength : 8, 0);
    av_opt_set_int(ctx->priv_data, "temporal-aq", p->temporal_aq ? 1 : 0, 0);
    av_opt_set(ctx->priv_data, "profile", "high", 0);
    av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
    av_opt_set(ctx->priv_data, "delay", "0", 0);
    av_opt_set(ctx->priv_data, "rc-lookahead", "0", 0);
    av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
    av_opt_set(ctx->priv_data, "no-scenecut", "1", 0);
    av_opt_set(ctx->priv_data, "aud", "0", 0);
    av_opt_set_int(ctx->priv_data, "gpu", p->gpu, 0);
}

static int open_nvenc_cuda(Encoder *e, const EncoderParams *p, char *err,
                           size_t errlen)
{
    const AVCodec *c = avcodec_find_encoder_by_name("h264_nvenc");
    if (!c || !cuda_load()) {
        snprintf(err, errlen, "h264_nvenc/libcuda not available");
        return -1;
    }
    char dev[16];
    snprintf(dev, sizeof(dev), "%d", p->gpu);
    AVBufferRef *hwdev = NULL;
    int r = av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_CUDA, dev, NULL, 0);
    if (r < 0) {
        snprintf(err, errlen, "cuda device %s: %s", dev, av_err2str(r));
        return -1;
    }
    AVBufferRef *frames = av_hwframe_ctx_alloc(hwdev);
    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
    fc->format = AV_PIX_FMT_CUDA;
    fc->sw_format = AV_PIX_FMT_BGR0;
    fc->width = e->w;
    fc->height = e->h;
    fc->initial_pool_size = 1;
    if ((r = av_hwframe_ctx_init(frames)) < 0) {
        snprintf(err, errlen, "cuda frames: %s", av_err2str(r));
        goto fail;
    }
    AVFrame *cf = av_frame_alloc();
    if ((r = av_hwframe_get_buffer(frames, cf, 0)) < 0) {
        snprintf(err, errlen, "cuda surface: %s", av_err2str(r));
        av_frame_free(&cf);
        goto fail;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(c);
    set_common(ctx, p, e->w, e->h);
    ctx->pix_fmt = AV_PIX_FMT_CUDA;
    ctx->sw_pix_fmt = AV_PIX_FMT_BGR0;
    ctx->hw_frames_ctx = av_buffer_ref(frames);
    if (strcmp(rc_of(p), "cbr"))
        ctx->bit_rate = 0; /* pure quality-targeted VBR, capped by rc_max_rate */
    nvenc_opts(ctx, p);
    if ((r = avcodec_open2(ctx, c, NULL)) < 0) {
        snprintf(err, errlen, "h264_nvenc (cuda) open: %s", av_err2str(r));
        avcodec_free_context(&ctx);
        av_frame_free(&cf);
        goto fail;
    }

    /* page-locked shadow so dirty-rect uploads run at full DMA speed */
    e->pinned_stride = FFALIGN(e->w * 4, 64);
    e->pinned_len = FFALIGN((size_t)e->pinned_stride * e->h, 4096);
    if (posix_memalign((void **)&e->pinned, 4096, e->pinned_len)) {
        snprintf(err, errlen, "pinned alloc failed");
        avcodec_free_context(&ctx);
        av_frame_free(&cf);
        goto fail;
    }
    memset(e->pinned, 0, e->pinned_len);
    AVCUDADeviceContext *cud = ((AVHWDeviceContext *)hwdev->data)->hwctx;
    e->cuctx = cud->cuda_ctx;
    CUcontext dummy;
    cu.push(e->cuctx);
    if (cu.host_register(e->pinned, e->pinned_len, 0) != 0)
        LOG("encoder: cuMemHostRegister failed, uploads use pageable memory");
    cu.pop(&dummy);

    e->ctx = ctx;
    e->hwdev = hwdev;
    e->cuframe = cf;
    e->bk = BK_NVENC_CUDA;
    e->upload_all = true;
    av_buffer_unref(&frames);
    char d[64];
    describe(d, sizeof(d), p, true);
    snprintf(e->name, sizeof(e->name), "h264_nvenc (cuda, %s)", d);
    return 0;
fail:
    av_buffer_unref(&frames);
    av_buffer_unref(&hwdev);
    return -1;
}

static int open_nvenc(Encoder *e, const EncoderParams *p, char *err,
                      size_t errlen)
{
    const AVCodec *c = avcodec_find_encoder_by_name("h264_nvenc");
    if (!c) {
        snprintf(err, errlen, "h264_nvenc not compiled in");
        return -1;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    set_common(ctx, p, e->w, e->h);
    ctx->pix_fmt = AV_PIX_FMT_BGR0;
    if (strcmp(rc_of(p), "cbr"))
        ctx->bit_rate = 0; /* pure quality-targeted VBR, capped by rc_max_rate */
    nvenc_opts(ctx, p);
    int r = avcodec_open2(ctx, c, NULL);
    if (r < 0) {
        snprintf(err, errlen, "h264_nvenc open failed: %s", av_err2str(r));
        avcodec_free_context(&ctx);
        return -1;
    }
    e->ctx = ctx;
    e->bk = BK_NVENC;
    char d[64];
    describe(d, sizeof(d), p, true);
    snprintf(e->name, sizeof(e->name), "h264_nvenc (sysmem, %s)", d);
    return 0;
}

/* kernel driver behind a DRM node, e.g. "i915", "xe", "amdgpu", "nvidia" */
static void drm_driver(const char *dev, char *out, size_t outlen)
{
    char link[256], target[256];
    const char *node = strrchr(dev, '/');
    snprintf(link, sizeof(link), "/sys/class/drm/%s/device/driver", node ? node + 1 : dev);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) {
        snprintf(out, outlen, "?");
        return;
    }
    target[n] = 0;
    const char *b = strrchr(target, '/');
    b = b ? b + 1 : target;
    if (strlen(b) < outlen)
        memcpy(out, b, strlen(b) + 1);
    else
        snprintf(out, outlen, "?");
}

static void errcat(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void errcat(char *err, size_t errlen, const char *fmt, ...)
{
    size_t l = strlen(err);
    if (l + 4 >= errlen)
        return;
    if (l)
        l += (size_t)snprintf(err + l, errlen - l, "; ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + l, errlen - l, fmt, ap);
    va_end(ap);
}

static int try_vaapi_device(Encoder *e, const EncoderParams *p,
                            const char *dev, char *err, size_t errlen)
{
    const AVCodec *c = avcodec_find_encoder_by_name("h264_vaapi");
    if (!c) {
        errcat(err, errlen, "h264_vaapi not compiled in");
        return -1;
    }
    char drv[32];
    drm_driver(dev, drv, sizeof(drv));
    if (!strcmp(drv, "nvidia")) {
        errcat(err, errlen, "%s: nvidia (no VA-API encode)", dev);
        return -1;
    }
    AVBufferRef *hwdev = NULL;
    int r = av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VAAPI, dev, NULL, 0);
    if (r < 0) {
        const char *hint = "";
        if (!strcmp(drv, "i915") || !strcmp(drv, "xe")) {
            if (access("/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so", F_OK) &&
                access("/usr/lib/x86_64-linux-gnu/dri/i965_drv_video.so", F_OK))
                hint = " - no Intel VA driver installed: apt install "
                       "intel-media-va-driver-non-free (Debian 'non-free' component; "
                       "i965-va-driver for pre-Broadwell)";
            else
                hint = " - VA driver failed to initialise (run 'vainfo --display drm "
                       "--device <node>' for details)";
        }
        errcat(err, errlen, "%s (%s): cannot open VA-API: %s%s", dev, drv,
               av_err2str(r), hint);
        return -1;
    }
    const char *vendor = "";
#ifdef HAVE_VA_VENDOR
    vendor = vaQueryVendorString(((AVVAAPIDeviceContext *)
                                  ((AVHWDeviceContext *)hwdev->data)->hwctx)->display);
#endif

    /* Newer Intel iGPUs (Gen11/Tiger Lake and later, e.g. N100) only have the
     * low-power (VDENC) H.264 encoder, which FFmpeg never selects on its own.
     * Try the regular entrypoint first, then low-power; per entrypoint prefer
     * quality-targeted VBR, then capped VBR, then constant QP. */
    static const char *m_vbr[] = {"QVBR", "VBR", "CQP", NULL};
    static const char *m_cbr[] = {"CBR", "VBR", "CQP", NULL};
    static const char *m_cqp[] = {"CQP", NULL};
    const char *rc = rc_of(p);
    const char **modes = !strcmp(rc, "cbr") ? m_cbr : !strcmp(rc, "cqp") ? m_cqp : m_vbr;
    for (int lp = 0; lp <= 1; lp++) {
        for (size_t i = 0; modes[i]; i++) {
            AVCodecContext *ctx = avcodec_alloc_context3(c);
            set_common(ctx, p, e->w, e->h);
            ctx->pix_fmt = AV_PIX_FMT_VAAPI;
            ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
            if (!strcmp(modes[i], "CBR"))
                ctx->bit_rate = ctx->rc_max_rate;
            else if (strcmp(modes[i], "CQP"))
                ctx->bit_rate = (int64_t)p->max_kbps * 1000 * 6 / 10;
            else
                ctx->rc_max_rate = ctx->rc_buffer_size = 0;
            ctx->global_quality = p->quality;

            AVBufferRef *frames = av_hwframe_ctx_alloc(hwdev);
            AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
            fc->format = AV_PIX_FMT_VAAPI;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = e->w;
            fc->height = e->h;
            fc->initial_pool_size = 4;
            if ((r = av_hwframe_ctx_init(frames)) < 0) {
                errcat(err, errlen, "%s (%s): NV12 surfaces: %s", dev, drv, av_err2str(r));
                av_buffer_unref(&frames);
                avcodec_free_context(&ctx);
                av_buffer_unref(&hwdev);
                return -1;
            }
            ctx->hw_frames_ctx = frames;
            av_opt_set(ctx->priv_data, "rc_mode", modes[i], 0);
            av_opt_set(ctx->priv_data, "profile", "high", 0);
            av_opt_set(ctx->priv_data, "async_depth", "1", 0);
            av_opt_set(ctx->priv_data, "aud", "0", 0);
            av_opt_set_int(ctx->priv_data, "low_power", lp, 0);
            if (!strcmp(modes[i], "CQP"))
                av_opt_set_int(ctx, "qp", p->quality, AV_OPT_SEARCH_CHILDREN);
            r = avcodec_open2(ctx, c, NULL);
            if (r == 0) {
                e->ctx = ctx;
                e->hwdev = hwdev;
                e->bk = BK_VAAPI;
                e->cbr = !strcmp(modes[i], "CBR");
                e->cqp = !strcmp(modes[i], "CQP");
                snprintf(e->name, sizeof(e->name), "h264_vaapi (%s %s, %s%s)",
                         dev, drv, modes[i], lp ? ", low-power" : "");
                if (*vendor)
                    LOG("encoder: VA driver: %s", vendor);
                return 0;
            }
            DBG("vaapi %s %s%s: %s", dev, modes[i], lp ? " low-power" : "", av_err2str(r));
            avcodec_free_context(&ctx);
        }
        errcat(err, errlen, "%s (%s): no usable H.264 %s entrypoint%s%s", dev, drv,
               lp ? "low-power" : "regular", *vendor ? " - driver: " : "", vendor);
    }
    av_buffer_unref(&hwdev);
    return -1;
}

static int open_vaapi(Encoder *e, const EncoderParams *p, char *err,
                      size_t errlen)
{
    if (p->vaapi_device && *p->vaapi_device)
        return try_vaapi_device(e, p, p->vaapi_device, err, errlen);
    for (int i = 128; i < 136; i++) {
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/dri/renderD%d", i);
        if (access(dev, R_OK | W_OK) != 0)
            continue;
        if (try_vaapi_device(e, p, dev, err, errlen) == 0)
            return 0;
    }
    if (!*err)
        snprintf(err, errlen, "no /dev/dri/renderD* node (no Intel/AMD GPU, or its driver isn't loaded)");
    return -1;
}

static int open_x264(Encoder *e, const EncoderParams *p, char *err,
                     size_t errlen)
{
    const AVCodec *c = avcodec_find_encoder_by_name("libx264");
    if (!c) {
        snprintf(err, errlen, "libx264 not compiled in");
        return -1;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    set_common(ctx, p, e->w, e->h);
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->thread_count = 0;
    /* p1/p2 = ultrafast (the default, CPU encoding at 60 fps) .. p7 = medium */
    static const char *x264_presets[] = {"ultrafast", "ultrafast", "superfast",
                                         "veryfast", "faster", "fast", "medium"};
    const char *rc = rc_of(p);
    av_opt_set(ctx->priv_data, "preset", x264_presets[preset_of(p)[1] - '1'], 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    if (!strcmp(rc, "vbr"))
        av_opt_set_double(ctx->priv_data, "crf", p->quality, 0);
    else if (!strcmp(rc, "cqp")) {
        av_opt_set_int(ctx->priv_data, "qp", p->quality, 0);
        ctx->rc_max_rate = ctx->rc_buffer_size = 0;
    }
    av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
    av_opt_set(ctx->priv_data, "x264-params", "scenecut=0:keyint=infinite", 0);
    if (p->spatial_aq) {
        av_opt_set_int(ctx->priv_data, "aq-mode", 1, 0);
        av_opt_set_double(ctx->priv_data, "aq-strength",
                          (p->aq_strength >= 1 && p->aq_strength <= 15 ? p->aq_strength : 8) / 8.0, 0);
    }
    int r = avcodec_open2(ctx, c, NULL);
    if (r < 0) {
        snprintf(err, errlen, "libx264 open failed: %s", av_err2str(r));
        avcodec_free_context(&ctx);
        return -1;
    }
    e->ctx = ctx;
    e->bk = BK_X264;
    char d[64];
    describe(d, sizeof(d), p, false);
    snprintf(e->name, sizeof(e->name), "libx264 (cpu, %s, %s)",
             x264_presets[preset_of(p)[1] - '1'], d);
    return 0;
}

Encoder *encoder_open(const EncoderParams *p, char *err, size_t errlen)
{
    Encoder *e = calloc(1, sizeof(*e));
    e->w = (p->width + 1) & ~1;
    e->h = (p->height + 1) & ~1;
    e->fps = p->fps;
    e->vbv_frames = p->vbv_frames;
    e->cbr = !strcmp(rc_of(p), "cbr");
    e->cqp = !strcmp(rc_of(p), "cqp");
    err[0] = 0;

    const char *bk = p->backend && *p->backend ? p->backend : "auto";
    bool any = !strcmp(bk, "auto");
    int r = -1;
    char e0[256] = "", e1[256] = "", e2[256] = "", e3[256] = "";
    if (r < 0 && (any || !strcmp(bk, "nvenc")))
        r = open_nvenc_cuda(e, p, e0, sizeof(e0));
    if (r < 0 && (any || !strcmp(bk, "nvenc") || !strcmp(bk, "nvenc-sysmem")))
        r = open_nvenc(e, p, e1, sizeof(e1));
    if (r < 0 && (any || !strcmp(bk, "vaapi")))
        r = open_vaapi(e, p, e2, sizeof(e2));
    if (r < 0 && (any || !strcmp(bk, "x264")))
        r = open_x264(e, p, e3, sizeof(e3));
    if (r < 0) {
        err[0] = 0;
        const char *parts[] = {e0, e1, e2, e3};
        for (int i = 0; i < 4; i++)
            if (*parts[i])
                errcat(err, errlen, "%s", parts[i]);
        free(e);
        return NULL;
    }
    if (*e0 && e->bk != BK_NVENC_CUDA)
        LOG("encoder: nvenc-cuda unavailable: %s", e0);
    if (*e1 && e->bk != BK_NVENC && e->bk != BK_NVENC_CUDA)
        LOG("encoder: nvenc unavailable: %s", e1);
    if (*e2 && e->bk == BK_X264)
        LOG("encoder: vaapi unavailable: %s", e2);

    if (e->bk != BK_NVENC_CUDA) {
        e->src = av_frame_alloc();
        e->src->format = AV_PIX_FMT_BGR0;
        e->src->width = e->w;
        e->src->height = e->h;
        av_frame_get_buffer(e->src, 64);
        for (int y = 0; y < e->h; y++)
            memset(e->src->data[0] + (size_t)y * e->src->linesize[0], 0,
                   (size_t)e->w * 4);
    }

    if (e->bk == BK_VAAPI || e->bk == BK_X264) {
        enum AVPixelFormat cf =
            e->bk == BK_VAAPI ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        e->conv = av_frame_alloc();
        e->conv->format = cf;
        e->conv->width = e->w;
        e->conv->height = e->h;
        av_frame_get_buffer(e->conv, 64);
        e->sws = sws_getContext(e->w, e->h, AV_PIX_FMT_BGR0, e->w, e->h, cf,
                                SWS_POINT, NULL, NULL, NULL);
        /* BT.601 limited range, matching what NVENC signals for RGB input */
        const int *coefs = sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(e->sws, coefs, 1, coefs, 0, 0, 1 << 16,
                                 1 << 16);
        av_opt_set_int(e->sws, "threads", 0, 0);
    }
    e->pkt = av_packet_alloc();
    LOG("encoder: opened %s %dx%d @%dfps max %d kbps", e->name, e->w, e->h,
        p->fps, p->max_kbps);
    return e;
}

uint8_t *encoder_input(Encoder *e, int *stride)
{
    if (e->bk == BK_NVENC_CUDA) {
        *stride = e->pinned_stride;
        return e->pinned;
    }
    /* If libavcodec still holds a reference to the shadow buffer this makes
     * a private copy (contents preserved), so dirty-rect updates stay valid. */
    av_frame_make_writable(e->src);
    *stride = e->src->linesize[0];
    return e->src->data[0];
}

static int drain(Encoder *e, EncoderPacketCb cb, void *opaque)
{
    for (;;) {
        int r = avcodec_receive_packet(e->ctx, e->pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return 0;
        if (r < 0)
            return r;
        cb(e->pkt->data, e->pkt->size, (e->pkt->flags & AV_PKT_FLAG_KEY) != 0,
           opaque);
        av_packet_unref(e->pkt);
    }
}

static int cuda_upload(Encoder *e, const int *rect)
{
    int x = 0, y = 0, w = e->w, h = e->h;
    if (!e->upload_all) {
        if (!rect || rect[2] <= 0 || rect[3] <= 0)
            return 0; /* nothing changed (quality refinement frame) */
        x = rect[0]; y = rect[1]; w = rect[2]; h = rect[3];
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > e->w) w = e->w - x;
        if (y + h > e->h) h = e->h - y;
        if (w <= 0 || h <= 0)
            return 0;
    }
    CUDA_MEMCPY2D m = {0};
    m.srcMemoryType = CU_MEMORYTYPE_HOST;
    m.srcHost = e->pinned;
    m.srcPitch = (size_t)e->pinned_stride;
    m.srcXInBytes = (size_t)x * 4;
    m.srcY = (size_t)y;
    m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    m.dstDevice = (CUdeviceptr)(uintptr_t)e->cuframe->data[0];
    m.dstPitch = (size_t)e->cuframe->linesize[0];
    m.dstXInBytes = (size_t)x * 4;
    m.dstY = (size_t)y;
    m.WidthInBytes = (size_t)w * 4;
    m.Height = (size_t)h;
    CUcontext dummy;
    cu.push(e->cuctx);
    CUresult r = cu.memcpy2d(&m); /* synchronous: done before NVENC reads */
    cu.pop(&dummy);
    if (r != 0) {
        LOG("encoder: cuMemcpy2D failed (%d)", r);
        return AVERROR_EXTERNAL;
    }
    e->upload_all = false;
    return 0;
}

int encoder_encode(Encoder *e, bool force_idr, const int *rect,
                   EncoderPacketCb cb, void *opaque)
{
    AVFrame *in = e->src;
    AVFrame *hw = NULL;
    int r;
    int64_t t0 = now_us();

    if (e->bk == BK_NVENC_CUDA) {
        if ((r = cuda_upload(e, rect)) < 0)
            return r;
        in = e->cuframe;
    } else if (e->bk != BK_NVENC) {
        av_frame_make_writable(e->conv);
        sws_scale(e->sws, (const uint8_t *const *)e->src->data,
                  e->src->linesize, 0, e->h, e->conv->data, e->conv->linesize);
        in = e->conv;
        if (e->bk == BK_VAAPI) {
            hw = av_frame_alloc();
            if ((r = av_hwframe_get_buffer(e->ctx->hw_frames_ctx, hw, 0)) < 0 ||
                (r = av_hwframe_transfer_data(hw, e->conv, 0)) < 0) {
                av_frame_free(&hw);
                return r;
            }
            in = hw;
        }
    }
    int64_t t1 = now_us();

    in->pts = e->pts++;
    in->pict_type = force_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (force_idr)
        in->flags |= AV_FRAME_FLAG_KEY;
    else
        in->flags &= ~AV_FRAME_FLAG_KEY;

    r = avcodec_send_frame(e->ctx, in);
    if (hw)
        av_frame_free(&hw);
    if (r < 0)
        return r;
    r = drain(e, cb, opaque);

    e->t_upload += t1 - t0;
    e->t_send += now_us() - t1;
    if (++e->t_n == 300) {
        DBG("encoder timing: prepare/upload %.2f ms, encode %.2f ms (avg of 300)",
            e->t_upload / 300000.0, e->t_send / 300000.0);
        e->t_upload = e->t_send = e->t_n = 0;
    }
    return r;
}

void encoder_set_bitrate(Encoder *e, int max_kbps)
{
    /* NVENC (support_dyn_bitrate) and libx264 pick these up on the next
     * frame without an IDR. VA-API ignores runtime changes. */
    if (e->cqp)
        return; /* no rate control */
    e->ctx->rc_max_rate = (int64_t)max_kbps * 1000;
    e->ctx->rc_buffer_size =
        (int)((int64_t)max_kbps * 1000 * e->vbv_frames / e->fps);
    e->ctx->bit_rate = e->cbr ? e->ctx->rc_max_rate : 0;
}

const char *encoder_name(const Encoder *e) { return e->name; }

const char *encoder_kind(const Encoder *e)
{
    return e->bk == BK_VAAPI ? "vaapi" : e->bk == BK_X264 ? "x264" : "nvenc";
}

void encoder_coded_size(const Encoder *e, int *w, int *h)
{
    *w = e->w;
    *h = e->h;
}

void encoder_close(Encoder *e)
{
    if (!e)
        return;
    avcodec_free_context(&e->ctx);
    av_frame_free(&e->src);
    av_frame_free(&e->conv);
    av_frame_free(&e->cuframe);
    if (e->pinned) {
        if (e->cuctx) {
            CUcontext dummy;
            cu.push(e->cuctx);
            cu.host_unregister(e->pinned);
            cu.pop(&dummy);
        }
        free(e->pinned);
    }
    sws_freeContext(e->sws);
    av_packet_free(&e->pkt);
    av_buffer_unref(&e->hwdev);
    free(e);
}
