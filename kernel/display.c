/* sceDisplaySetFrameBufInternal hook: remembers the game's frame geometry and
 * performs pending captures on the exact frame the game submits. A game that
 * submits no frames (VJO_GAME_STATIC_FB) is sampled from its pad calls
 * instead (display_static_fb_sample). */
#include <psp2kern/display.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "vjo_kernel.h"

static tai_hook_ref_t hook_ref;
static SceUID hook_uid = -1;
static uint32_t frame_n;

typedef struct {
    uintptr_t base;
    uint32_t pitch, fmt, w, h;
    SceUID pid;
    int64_t us; /* when submitted */
} GameFb;

/* The game's last submitted framebuffer, address and geometry together:
 * a seqlock (fb_seq odd while written), so neither side ever waits in a
 * hook. Two threads submitting at once: one write is skipped (fb_writing),
 * and last_fb may keep the older framebuffer until the next submit (games
 * submit from one thread). */
static GameFb last_fb;
static volatile uint32_t fb_seq;
static volatile int fb_writing;

static void fb_publish(const GameFb *fb)
{
    if (!__sync_bool_compare_and_swap(&fb_writing, 0, 1))
        return;
    fb_seq++;
    __sync_synchronize();
    last_fb = *fb;
    __sync_synchronize();
    fb_seq++;
    __sync_lock_release(&fb_writing);
}

/* 0: *out is a consistent copy. */
static int fb_read(GameFb *out)
{
    uint32_t seq = fb_seq;
    if (seq & 1)
        return -1;
    __sync_synchronize();
    *out = last_fb;
    __sync_synchronize();
    return fb_seq == seq ? 0 : -1;
}

/* A frame of the game, in its context: geometry, the change detection
 * signature, and a pending capture. One caller at a time (frame_busy, held
 * for a whole capture copy): the display hook and the static framebuffer
 * sampling may run at once on different threads. */
static volatile int frame_busy;

static void on_frame(const GameFb *fb)
{
    if (!__sync_bool_compare_and_swap(&frame_busy, 0, 1))
        return;
    g.fb_pitch = fb->pitch;
    g.fb_fmt = fb->fmt;
    g.fb_w = fb->w;
    g.fb_h = fb->h;

    if (g.game_active && g.capture_state == CAPTURE_IDLE && ++frame_n % VJO_CHECK_EVERY_FRAMES == 0) {
        uint32_t seq = g.hook_sig_seq;
        if (region_sig_hook(fb->base, fb->pitch, fb->fmt, fb->w, fb->h, &g.hook_sig[(seq + 1) & 1]) == 0) {
            __sync_synchronize(); /* the signature is written before the bump */
            g.hook_sig_seq = seq + 1;
        }
    }

    if (g.capture_state == CAPTURE_PENDING && capture_claim()) {
        int64_t t0 = ksceKernelGetSystemTimeWide();
        g.capture_result = capture_copy(fb->base, fb->pitch, fb->fmt, fb->w, fb->h);
        g.capture_state = CAPTURE_COPIED;
        ksceKernelSetEventFlag(g.ievf, IEV_CAPTURED);
        klog("capture %ux%u fmt %08X in %d us -> %d", g.crop_w, g.crop_h, fb->fmt,
             (int)(ksceKernelGetSystemTimeWide() - t0), g.capture_result);
    }
    __sync_lock_release(&frame_busy);
}

static int display_hook(int head, int index, const SceDisplayFrameBuf *p, int sync)
{
    GameFb fb;
    if (index != 0 || !p || !p->base || g.game_pid <= 0)
        goto out;
    if (head != ksceDisplayGetPrimaryHead())
        goto out;
    fb.pid = ksceKernelGetProcessId();
    if (fb.pid != g.game_pid)
        goto out;

    fb.base = (uintptr_t)p->base;
    fb.pitch = p->pitch;
    fb.fmt = p->pixelformat;
    fb.w = p->width;
    fb.h = p->height;
    fb.us = ksceKernelGetSystemTimeWide();
    fb_publish(&fb);
    on_frame(&fb);
out:
    return TAI_CONTINUE(int, hook_ref, head, index, p, sync);
}

/* A game that sets its framebuffer once and then draws into it (the PSP
 * emulator, under Adrenaline) submits no frames for the hook. Its pad calls
 * run in its context too: when no frame came for a while, the framebuffer
 * it set last, still on screen, is read from there, at most once per
 * STATIC_FB_POLL_US (a capture copy then stalls that pad call for a few
 * ms). Only for VJO_GAME_STATIC_FB games: others may free a framebuffer
 * they no longer submit (a loading pause), and reading it crashed the
 * console. Accepted: the emulator switching and freeing its framebuffer
 * during the few ms of a sampled copy. */
#define STATIC_FB_STALE_US 200000
#define STATIC_FB_POLL_US  16000

/* Unsynchronized between the game's threads (32 bits: no torn store): at
 * worst one extra sample, which frame_busy serializes. */
static uint32_t sampled_us;

void display_static_fb_sample(void)
{
    GameFb fb;
    int64_t now;
    if (g.game_active != VJO_GAME_STATIC_FB || fb_read(&fb) < 0 || !fb.base || fb.pid != g.game_pid ||
        ksceKernelGetProcessId() != fb.pid)
        return;
    now = ksceKernelGetSystemTimeWide();
    if (now - fb.us < STATIC_FB_STALE_US || (uint32_t)now - sampled_us < STATIC_FB_POLL_US)
        return;
    sampled_us = (uint32_t)now;
    on_frame(&fb);
}

int display_hook_install(void)
{
    /* SceDisplay, SceDisplayForDriver 0x9FED47AC, sceDisplaySetFrameBufInternal */
    hook_uid = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref, "SceDisplay", 0x9FED47AC,
                                              0x16466675, display_hook);
    if (hook_uid < 0)
        klog("display hook failed 0x%08X", hook_uid);
    return hook_uid < 0 ? hook_uid : 0;
}

void display_hook_release(void)
{
    if (hook_uid >= 0)
        taiHookReleaseForKernel(hook_uid, hook_ref);
    hook_uid = -1;
}
