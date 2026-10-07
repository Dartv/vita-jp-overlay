/* Region capture and region signatures (scene.h), both inside the display
 * hook. The shell reads the raw rows and encodes the JPEG itself. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>

#include "vjo_kernel.h"

#define FMT_A8B8G8R8    0x00000000u
#define FMT_A2B10G10R10 0x60800000u
#define FMT_BGRA5551    0x50000000u /* 16 bpp, R in the low bits (udcd-uvc) */

static uint32_t fmt_bpp(uint32_t fmt)
{
    return fmt == FMT_BGRA5551 ? 2 : 4;
}

void capture_compute_crop(uint32_t fb_w, uint32_t fb_h, uint32_t *x, uint32_t *y, uint32_t *w,
                          uint32_t *h)
{
    VjoRect r = g.region;
    uint32_t cx, cy, cw, ch;
    if (fb_w > VJO_MAX_W)
        fb_w = VJO_MAX_W;
    if (fb_h > VJO_MAX_H)
        fb_h = VJO_MAX_H;
    if (r.w == 0 || r.h == 0) {
        *x = 0;
        *y = 0;
        *w = fb_w;
        *h = fb_h;
        return;
    }
    cx = (uint32_t)r.x * fb_w >> 16;
    cy = (uint32_t)r.y * fb_h >> 16;
    cw = ((uint32_t)r.w * fb_w + 65535) >> 16;
    ch = ((uint32_t)r.h * fb_h + 65535) >> 16;
    /* Lens needs some context around tiny selections. */
    if (cw < 32)
        cw = 32;
    if (ch < 32)
        ch = 32;
    if (cx + cw > fb_w)
        cx = cw > fb_w ? 0 : fb_w - cw;
    if (cy + ch > fb_h)
        cy = ch > fb_h ? 0 : fb_h - ch;
    if (cw > fb_w)
        cw = fb_w;
    if (ch > fb_h)
        ch = fb_h;
    cx &= ~1u;
    *x = cx;
    *y = cy;
    *w = cw;
    *h = ch;
}

/* Converts one row of n pixels (source format) to A8B8G8R8 at dst. */
static void convert_row(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t fmt)
{
    uint32_t *d = (uint32_t *)dst;
    if (fmt == FMT_A2B10G10R10) {
        const uint32_t *s = (const uint32_t *)src;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = s[i];
            uint32_t r = (v >> 2) & 0xFF, gg = (v >> 12) & 0xFF, b = (v >> 22) & 0xFF;
            d[i] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        }
    } else if (fmt == FMT_BGRA5551) {
        const uint16_t *s = (const uint16_t *)src;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = s[i];
            uint32_t r = (v & 0x1F) << 3, gg = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
            d[i] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        }
    }
}

/* Runs in the game's context (display hook): its framebuffer is user memory
 * of the current process. Same-process copies only, as PSVshell/reVita do
 * in this hook; reading another process' framebuffer from the worker
 * crashed the console. */
static int copy_from_game(void *dst, uintptr_t src, uint32_t len)
{
    return ksceKernelMemcpyUserToKernel(dst, (const void *)src, len);
}

/* Reads n pixels at src as A8B8G8R8 into dst; tmp holds a converted
 * format's source row. */
static int read_row(uint8_t *dst, uint8_t *tmp, uintptr_t src, uint32_t n, uint32_t fmt)
{
    if (fmt == FMT_A8B8G8R8)
        return copy_from_game(dst, src, n * 4);
    if (copy_from_game(tmp, src, n * fmt_bpp(fmt)) < 0)
        return -1;
    convert_row(dst, tmp, n, fmt);
    return 0;
}

static int fmt_supported(uint32_t fmt)
{
    return fmt == FMT_A8B8G8R8 || fmt == FMT_A2B10G10R10 || fmt == FMT_BGRA5551;
}

/* Copies the region (or, for a VJO_CAPTURE_FULL request, the whole frame)
 * of a game frame into g.raw (A8B8G8R8, raw_stride bytes per row), and the
 * region's signature into g.capture_sig. Game context only (display or pad
 * hook, display.c). */
int capture_copy(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    static SceneAcc acc;
    static uint8_t tmp[VJO_MAX_W * 4];
    uint32_t x, y, cw, ch, bpp, stride;
    int full = g.capture_full;

    if (!g.raw)
        return VJO_ERR_NO_MEMORY;
    if (!fmt_supported(fmt))
        return VJO_ERR_FORMAT;
    bpp = fmt_bpp(fmt);
    if (full) {
        x = y = 0;
        cw = w > VJO_MAX_W ? VJO_MAX_W : w;
        ch = h > VJO_MAX_H ? VJO_MAX_H : h;
    } else {
        capture_compute_crop(w, h, &x, &y, &cw, &ch);
        scene_acc_begin(&acc, &g.capture_sig, cw, ch);
    }
    g.crop_w = cw;
    g.crop_h = ch;
    stride = g.raw_stride = cw * 4;

    for (uint32_t row = 0; row < ch; row++) {
        uint8_t *dst = g.raw + row * stride;
        if (read_row(dst, tmp, base + ((y + row) * pitch + x) * bpp, cw, fmt) < 0)
            return VJO_ERR_COPY;
        if (!full)
            scene_acc_row(&acc, row, (const uint32_t *)dst);
    }
    return 0;
}

/* Game context (display or pad hook): signature of the region of the
 * frame, reading only the sampled rows. Same-process copies only, as
 * PSVshell/reVita do in the display hook; reading another process'
 * framebuffer from the worker crashed the console. */
int region_sig_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h, SceneSig *out)
{
    static SceneAcc acc;
    static uint8_t row[VJO_MAX_W * 4], tmp[VJO_MAX_W * 4];
    uint32_t x, y, cw, ch, bpp;

    if (!base)
        return VJO_ERR_COPY;
    if (!fmt_supported(fmt))
        return VJO_ERR_FORMAT;
    bpp = fmt_bpp(fmt);
    capture_compute_crop(w, h, &x, &y, &cw, &ch);
    if (!cw || !ch)
        return VJO_ERR_FORMAT;
    scene_acc_begin(&acc, out, cw, ch);
    for (uint32_t r = 0; r < ch; r = scene_acc_next_row(&acc)) {
        if (read_row(row, tmp, base + ((y + r) * pitch + x) * bpp, cw, fmt) < 0)
            return VJO_ERR_COPY;
        scene_acc_row(&acc, r, (const uint32_t *)row);
    }
    return 0;
}
