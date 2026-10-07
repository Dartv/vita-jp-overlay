/* Region capture (inside the display hook) and change detection (worker
 * thread). The shell reads the raw rows and encodes the JPEG itself. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>

#include "vjo_kernel.h"

#define FMT_A8B8G8R8    0x00000000u
#define FMT_A2B10G10R10 0x60800000u
#define FMT_BGRA5551    0x50000000u /* 16 bpp, R in the low bits (udcd-uvc) */

static uint8_t row_tmp[VJO_MAX_W * 4];

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

/* Converts one row in row_tmp (source format) to A8B8G8R8 at dst. */
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

/* FNV-1a over every 4th pixel of a converted row. */
static uint32_t hash_row(uint32_t h, const uint8_t *row, uint32_t n)
{
    const uint32_t *p = (const uint32_t *)row;
    for (uint32_t i = 0; i < n; i += 4) {
        h ^= p[i] & 0x00FFFFFFu; /* ignore alpha */
        h *= 16777619u;
    }
    return h;
}

/* Runs in the game's context (display hook): its framebuffer is user memory
 * of the current process. */
static int copy_from_game(void *dst, uintptr_t src, uint32_t len)
{
    return ksceKernelMemcpyUserToKernel(dst, (const void *)src, len);
}

/* Copies the region (or, for a VJO_CAPTURE_FULL request, the whole frame)
 * of a game frame into g.raw (A8B8G8R8, raw_stride bytes per row). Game
 * context only (display or pad hook, display.c). */
int capture_copy(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t x, y, cw, ch, bpp, stride, hash = 2166136261u;

    if (!g.raw)
        return VJO_ERR_NO_MEMORY;
    if (fmt != FMT_A8B8G8R8 && fmt != FMT_A2B10G10R10 && fmt != FMT_BGRA5551)
        return VJO_ERR_FORMAT;
    bpp = fmt_bpp(fmt);
    if (g.capture_full) {
        x = y = 0;
        cw = w > VJO_MAX_W ? VJO_MAX_W : w;
        ch = h > VJO_MAX_H ? VJO_MAX_H : h;
    } else {
        capture_compute_crop(w, h, &x, &y, &cw, &ch);
    }
    g.crop_w = cw;
    g.crop_h = ch;
    stride = g.raw_stride = cw * 4;

    for (uint32_t row = 0; row < ch; row++) {
        uint8_t *dst = g.raw + row * stride;
        uintptr_t src = base + ((y + row) * pitch + x) * bpp;
        int rc;
        if (bpp == 4 && fmt == FMT_A8B8G8R8) {
            rc = copy_from_game(dst, src, cw * 4);
        } else {
            rc = copy_from_game(row_tmp, src, cw * bpp);
            if (rc >= 0)
                convert_row(dst, row_tmp, cw, fmt);
        }
        if (rc < 0)
            return VJO_ERR_COPY;
        if (row % VJO_CHECK_ROW_STEP == 0)
            hash = hash_row(hash, dst, cw);
    }
    g.capture_checksum = hash;
    return 0;
}

/* Game context (display or pad hook): checksum of the region of the frame.
 * Same-process copies only, as PSVshell/reVita do in the display hook;
 * reading another process' framebuffer from the worker crashed the console. */
uint32_t region_checksum_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t x, y, cw, ch, bpp, hash = 2166136261u;
    static uint8_t row[VJO_MAX_W * 4], src_row[VJO_MAX_W * 4];

    if (!base)
        return 0;
    if (fmt != FMT_A8B8G8R8 && fmt != FMT_A2B10G10R10 && fmt != FMT_BGRA5551)
        return 0;
    bpp = fmt_bpp(fmt);
    capture_compute_crop(w, h, &x, &y, &cw, &ch);
    for (uint32_t r = 0; r < ch; r += VJO_CHECK_ROW_STEP) {
        uintptr_t src = base + ((y + r) * pitch + x) * bpp;
        if (fmt == FMT_A8B8G8R8) {
            if (copy_from_game(row, src, cw * 4) < 0)
                return 0;
        } else {
            if (copy_from_game(src_row, src, cw * bpp) < 0)
                return 0;
            convert_row(row, src_row, cw, fmt);
        }
        hash = hash_row(hash, row, cw);
    }
    return hash ? hash : 1;
}
