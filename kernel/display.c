/* sceDisplaySetFrameBufInternal hook: remembers the game's frame geometry and
 * performs pending captures on the exact frame the game submits. */
#include <psp2kern/display.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "vjo_kernel.h"

static tai_hook_ref_t hook_ref;
static SceUID hook_uid = -1;
static uint32_t frame_n;

static int display_hook(int head, int index, const SceDisplayFrameBuf *p, int sync)
{
    SceUID pid;
    if (index != 0 || !p || !p->base || g.game_pid <= 0)
        goto out;
    if (head != ksceDisplayGetPrimaryHead())
        goto out;
    pid = ksceKernelGetProcessId();
    if (pid != g.game_pid)
        goto out;

    g.fb_pitch = p->pitch;
    g.fb_fmt = p->pixelformat;
    g.fb_w = p->width;
    g.fb_h = p->height;

    if (g.game_active && g.capture_state == CAPTURE_IDLE && ++frame_n % VJO_CHECK_EVERY_FRAMES == 0) {
        uint32_t cs = region_checksum_hook((uintptr_t)p->base, p->pitch, p->pixelformat, p->width, p->height);
        if (cs) {
            g.hook_checksum = cs;
            g.hook_checksum_seq++;
        }
    }

    if (g.capture_state == CAPTURE_PENDING && capture_claim()) {
        int64_t t0 = ksceKernelGetSystemTimeWide();
        g.capture_result = capture_copy((uintptr_t)p->base, p->pitch, p->pixelformat, p->width, p->height);
        g.capture_state = CAPTURE_COPIED;
        ksceKernelSetEventFlag(g.ievf, IEV_CAPTURED);
        klog("capture %ux%u fmt %08X in %d us -> %d", g.crop_w, g.crop_h, p->pixelformat,
             (int)(ksceKernelGetSystemTimeWide() - t0), g.capture_result);
    }
out:
    return TAI_CONTINUE(int, hook_ref, head, index, p, sync);
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
