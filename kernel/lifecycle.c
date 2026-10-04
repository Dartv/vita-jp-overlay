/* Foreground game tracking via the process event handler (as PSVshell and
 * reVita do); the state machine is in foreground.c. Buffer allocation is
 * deferred to the worker thread. */
#include <psp2kern/kernel/sysclib.h>
#include <taihen.h>

#include "vjo_kernel.h"

static tai_hook_ref_t hook_ref;
static SceUID hook_uid = -1;

/* The transitions are in foreground.c; these copy g's foreground state in
 * and out around them. Called with g.game_lock held. game_active is stored
 * before game_pid, so the lock-free readers (hooks) never see a new
 * foreground process still marked active. */
static VjoForeground fg_load(void)
{
    VjoForeground f;
    f.game_pid = g.game_pid;
    f.game_active = g.game_active;
    f.prev = g.prev_game_pid;
    f.shell_pid = g.shell_pid;
    return f;
}

static void fg_store(const VjoForeground *f, uint32_t iev)
{
    g.game_active = f->game_active;
    g.game_pid = f->game_pid;
    g.prev_game_pid = f->prev;
    if (iev)
        ksceKernelSetEventFlag(g.ievf, iev);
}

static int procevent_patched(int pid, int ev, int a3, int a4, int *a5, int a6)
{
    VjoForeground f;
    uint32_t iev;
    if (ksceKernelLockMutex(g.game_lock, 1, NULL) < 0)
        goto out;
    f = fg_load();
    iev = fg_process_event(&f, pid, ev);
    if ((iev & IEV_GAME_EXIT) && (iev & IEV_GAME_START))
        klog("pid 0x%X gone: back to game pid 0x%X", pid, f.game_pid);
    fg_store(&f, iev);
    ksceKernelUnlockMutex(g.game_lock, 1);
out:
    return TAI_CONTINUE(int, hook_ref, pid, ev, a3, a4, a5, a6);
}

/* vjoSetGameActive: the shell's verdict on the foreground process. */
int lifecycle_set_game_active(SceUID pid, int active)
{
    VjoForeground f;
    uint32_t iev;
    int ret = 0;
    ksceKernelLockMutex(g.game_lock, 1, NULL);
    f = fg_load();
    if (fg_set_game_active(&f, pid, active, &iev) < 0) {
        ret = VJO_ERR_NO_GAME;
    } else {
        if (active)
            klog("game pid 0x%X active", pid);
        else if (iev & IEV_GAME_START)
            klog("pid 0x%X is not a game: back to game pid 0x%X", pid, f.game_pid);
        fg_store(&f, iev);
    }
    ksceKernelUnlockMutex(g.game_lock, 1);
    return ret;
}

int lifecycle_hooks_install(void)
{
    hook_uid = taiHookFunctionImportForKernel(KERNEL_PID, &hook_ref, "SceProcessmgr", 0x887F19D0,
                                              0x414CC813, procevent_patched);
    if (hook_uid < 0)
        klog("procevent hook failed 0x%08X", hook_uid);
    return hook_uid < 0 ? hook_uid : 0;
}

void lifecycle_hooks_release(void)
{
    if (hook_uid >= 0)
        taiHookReleaseForKernel(hook_uid, hook_ref);
    hook_uid = -1;
}
