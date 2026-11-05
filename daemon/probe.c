/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d --probe : report which encoder backends work on this host and
 * why the others don't. Encodes a few 1080p frames with each backend.
 */
#define _GNU_SOURCE
#include "pve-h264d.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t g_bytes;
static void count_cb(const uint8_t *d, size_t n, bool key, void *o) { g_bytes += n; }

static void list_dri(void)
{
    DIR *dir = opendir("/dev/dri");
    printf("GPU render nodes:\n");
    if (!dir) {
        printf("  (none: /dev/dri missing - no GPU driver loaded)\n");
        return;
    }
    struct dirent *de;
    int n = 0;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "renderD", 7))
            continue;
        char link[PATH_MAX], target[256] = "?", vendor[64] = "?";
        int ll = snprintf(link, sizeof(link), "/sys/class/drm/%s/device/driver", de->d_name);
        if (ll < 0 || (size_t)ll >= sizeof(link))
            continue;
        ssize_t l = readlink(link, target, sizeof(target) - 1);
        if (l > 0)
            target[l] = 0;
        snprintf(link, sizeof(link), "/sys/class/drm/%s/device/vendor", de->d_name);
        FILE *f = fopen(link, "r");
        if (f) {
            if (!fgets(vendor, sizeof(vendor), f))
                vendor[0] = 0;
            fclose(f);
            vendor[strcspn(vendor, "\n")] = 0;
        }
        const char *b = strrchr(target, '/');
        printf("  /dev/dri/%s  driver=%s  pci-vendor=%s%s\n", de->d_name, b ? b + 1 : target,
               vendor, !strcmp(vendor, "0x8086") ? " (Intel)" : !strcmp(vendor, "0x10de") ? " (NVIDIA)" :
               !strcmp(vendor, "0x1002") ? " (AMD)" : "");
        n++;
    }
    closedir(dir);
    if (!n)
        printf("  (none)\n");
    const char *drvs[] = {"iHD_drv_video.so", "i965_drv_video.so", "radeonsi_drv_video.so", NULL};
    printf("VA-API drivers installed:");
    int any = 0;
    for (int i = 0; drvs[i]; i++) {
        char pth[128];
        snprintf(pth, sizeof(pth), "/usr/lib/x86_64-linux-gnu/dri/%s", drvs[i]);
        if (!access(pth, F_OK)) {
            printf(" %s", drvs[i]);
            any = 1;
        }
    }
    printf("%s\n", any ? "" : " none");
    if (getenv("LIBVA_DRIVER_NAME"))
        printf("LIBVA_DRIVER_NAME=%s\n", getenv("LIBVA_DRIVER_NAME"));
}

int probe_main(const EncoderParams *base)
{
    g_verbose = 1;
    list_dri();
    const char *bks[] = {"nvenc", "nvenc-sysmem", "vaapi", "x264"};
    char pick[160] = "";
    printf("\nEncoder backends (1920x1080, 10 frames):\n");
    for (size_t i = 0; i < sizeof(bks) / sizeof(*bks); i++) {
        EncoderParams p = *base;
        p.backend = bks[i];
        p.width = 1920;
        p.height = 1080;
        char err[2048];
        fflush(stdout);
        Encoder *e = encoder_open(&p, err, sizeof(err));
        if (!e) {
            printf("  %-13s FAIL  %s\n", bks[i], err);
            continue;
        }
        int stride, cw, ch;
        encoder_coded_size(e, &cw, &ch);
        g_bytes = 0;
        int64_t t0 = now_us();
        int rc = 0;
        for (int n = 0; n < 10 && rc >= 0; n++) {
            uint8_t *d = encoder_input(e, &stride);
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) {
                    uint8_t *px = d + (size_t)y * stride + (size_t)x * 4;
                    px[0] = (uint8_t)(x + n * 8); px[1] = (uint8_t)y; px[2] = (uint8_t)(x ^ y); px[3] = 0;
                }
            int rect[4] = {0, 0, cw, ch};
            rc = encoder_encode(e, n == 0, rect, count_cb, NULL);
        }
        double ms = (now_us() - t0) / 10000.0;
        if (rc < 0 || !g_bytes)
            printf("  %-13s FAIL  opened as %s but encoding failed (%d)\n", bks[i], encoder_name(e), rc);
        else {
            printf("  %-13s OK    %s  (%.1f ms/frame incl. test-pattern drawing)\n", bks[i],
                   encoder_name(e), ms);
            if (!*pick)
                snprintf(pick, sizeof(pick), "%s", encoder_name(e));
        }
        encoder_close(e);
    }
    printf("\nbackend=auto would use: %s\n", *pick ? pick : "nothing (no encoder works!)");
    printf("configured backend (" "/etc/pve-h264/h264d.conf" "): %s\n", base->backend);
    return *pick ? 0 : 1;
}
