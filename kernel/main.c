/* Vita JP Overlay kernel module: module entry, worker thread and the
 * syscalls used by the SceShell plugin (see include/vjo_api.h). */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "vjo_kernel.h"

VjoKernelState g;

static SceUID worker_uid = -1;
static volatile int worker_run = 1;

#define POLL_US         16000   /* input poll period */
#define CHECK_EVERY     4       /* change detection every 4th poll (~64 ms) */
#define CAPTURE_WAIT_US 1500000 /* no new frame from the game within this -> timeout */

/* Called with the lock held. Signatures already published are skipped. One
 * the hook is still building for the old region can still arrive; it only
 * costs an extra change. */
static void reset_change_detection(void)
{
    scene_reset(&g.tracker);
    g.region_seq = 0; /* for another region */
    g.seen_sig_seq = g.hook_sig_seq;
}

static void change_detection(void)
{
    static SceneSig sig;
    uint32_t seq, id = 0;
    SceneEvent ev = SCENE_NONE;
    int animated = 0;
    if (g.game_pid <= 0 || !g.game_active)
        return;
    VJO_LOCK(); /* against reset_change_detection */
    seq = g.hook_sig_seq;
    if (seq != g.seen_sig_seq) {
        __sync_synchronize(); /* pairs with the hook's barrier */
        sig = g.hook_sig[seq & 1];
        __sync_synchronize();
        if (g.hook_sig_seq == seq) { /* else rewritten meanwhile: next poll */
            g.seen_sig_seq = seq;
            int64_t now = ksceKernelGetSystemTimeWide();
            /* the low 32 bits, widened: within the last 71 min */
            int64_t input_us = now - (uint32_t)((uint32_t)now - g.input_us32);
            int64_t edge_us = now - (uint32_t)((uint32_t)now - g.input_edge_us32);
            ev = scene_update(&g.tracker, &sig, now, input_us, edge_us);
            id = g.tracker.id;
            animated = g.tracker.masked;
        }
    }
    VJO_UNLOCK();
    if (ev == SCENE_QUIET) {
        klog("region quiet: scene %u", id);
        ksceKernelSetEventFlag(g.evf, VJO_EV_REGION_QUIET);
    } else if (ev == SCENE_SETTLED) {
        klog("region settled: scene %u (%d animated cells masked)", id, animated);
        ksceKernelSetEventFlag(g.evf, VJO_EV_REGION_STABLE);
    }
}

static void finish_capture(void)
{
    VJO_LOCK();
    if (g.alloc_status != VJO_ALLOC_OK || !g.raw) {
        /* The game exited (buffers freed) between the copy and now. */
        if (g.capture_result == 0)
            g.capture_result = VJO_ERR_NO_GAME;
    }
    g.raw_valid = g.capture_result == 0;
    g.capture_scene = g.raw_valid && !g.capture_full ? scene_match(&g.tracker, &g.capture_sig) : 0;
    if (g.raw_valid && !g.capture_full) {
        g.region_seq = g.capture_seq;
        g.region_us = g.capture_copy_us;
    }
    g.done_seq = g.capture_seq;
    g.capture_state = CAPTURE_IDLE;
    VJO_UNLOCK();
    ksceKernelSetEventFlag(g.evf, VJO_EV_CAPTURE_DONE);
}

/* Cancels a pending capture and waits for a copy in progress in the display
 * hook, so the buffers can be freed safely. Called with the lock held. */
static void capture_quiesce(void)
{
    int spins = 0;
    if (__sync_bool_compare_and_swap(&g.capture_state, CAPTURE_PENDING, CAPTURE_IDLE))
        return;
    while (g.capture_state == CAPTURE_COPYING && spins++ < 200)
        ksceKernelDelayThread(1000);
}

static void free_game_buffers(void)
{
    VJO_LOCK();
    capture_quiesce();
    if (g.capture_state != CAPTURE_COPYING) {
        g.capture_state = CAPTURE_IDLE;
        g.raw_valid = 0;
        buffers_free();
    } else {
        klog("buffers kept: capture still copying");
    }
    VJO_UNLOCK();
}

/* The kernel log goes straight to ux0:data/VitaJPOverlay/kernel.txt (flushed
 * every worker tick) so it survives a crash; the previous boot's file is kept
 * as kernel.prev.txt. Capped at 64 KB per boot. The file is first opened once
 * SceShell has registered: earlier in boot ux0: can still be another device
 * (SD2Vita remaps it), and the rotation would happen there. Lines logged
 * before that wait in the ring. */
#define KFILE "ux0:data/VitaJPOverlay/kernel.txt"
#define KFILE_PREV "ux0:data/VitaJPOverlay/kernel.prev.txt"
#define KFILE_CAP (64 * 1024)
static int kfile_size = -1;

static void kernel_file_flush(void)
{
    char buf[512];
    int n;
    SceUID fd;
    if (kfile_size > KFILE_CAP)
        return;
    if (kfile_size < 0) {
        if (g.shell_pid <= 0)
            return;
        ksceIoMkdir("ux0:data", 0777);
        ksceIoMkdir("ux0:data/VitaJPOverlay", 0777);
        ksceIoRemove(KFILE_PREV);
        ksceIoRename(KFILE, KFILE_PREV);
        fd = ksceIoOpen(KFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd < 0)
            return; /* ux0 not mounted yet: retry later */
        ksceIoClose(fd);
        kfile_size = 0;
    }
    n = klog_read(buf, sizeof(buf));
    if (n <= 0)
        return;
    fd = ksceIoOpen(KFILE, SCE_O_WRONLY | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    do {
        ksceIoWrite(fd, buf, n);
        kfile_size += n;
    } while ((n = klog_read(buf, sizeof(buf))) > 0);
    ksceIoClose(fd);
}

static int worker(SceSize args, void *argp)
{
    unsigned tick = 0;
    (void)args;
    (void)argp;
    while (worker_run) {
        unsigned int bits = 0;
        SceUInt timeout = POLL_US;
        ksceKernelWaitEventFlag(g.ievf, IEV_GAME_START | IEV_GAME_EXIT | IEV_CAPTURED | IEV_ACTIVATE | IEV_QUIT,
                                SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
        if (bits & IEV_QUIT)
            break;
        if (bits & IEV_CAPTURED)
            finish_capture();
        if (bits & IEV_GAME_EXIT) {
            free_game_buffers();
            klog("game exit");
            ksceKernelSetEventFlag(g.evf, VJO_EV_GAME_EXIT);
        }
        if ((bits & IEV_GAME_START) && g.game_pid > 0) {
            VJO_LOCK();
            reset_change_detection();
            VJO_UNLOCK();
            ksceKernelSetEventFlag(g.evf, VJO_EV_GAME_START);
        }
        /* Buffers only for processes the shell confirmed as games. */
        if ((bits & IEV_ACTIVATE) && g.game_pid > 0 && g.game_active) {
            VJO_LOCK();
            buffers_alloc();
            reset_change_detection();
            VJO_UNLOCK();
        }
        if ((g.game_pid <= 0 || !g.game_active) && g.mem_uid > 0 && g.capture_state != CAPTURE_COPIED)
            free_game_buffers();

        /* A capture request but no new frame from the game in time (paused
         * game): the capture fails with a timeout. The last frame is not read
         * from the worker: cross-process framebuffer reads were the suspected
         * cause of the first on-device crash. */
        if (g.capture_state == CAPTURE_PENDING &&
            ksceKernelGetSystemTimeWide() - g.capture_requested_us > CAPTURE_WAIT_US && capture_claim()) {
            g.capture_result = VJO_ERR_TIMEOUT;
            klog("capture timed out: no frame from the game");
            g.capture_state = CAPTURE_COPIED;
            finish_capture();
        }

        input_poll();
        if (++tick % CHECK_EVERY == 0)
            change_detection();
        kernel_file_flush();
    }
    return 0;
}

/* ---------------- syscalls ---------------- */

/* Only the registered shell process may use the syscalls that read the
 * screen or control input. */
static int caller_ok(void)
{
    return g.shell_pid > 0 && ksceKernelGetProcessId() == g.shell_pid;
}

int vjoGetVersion(void)
{
    return VJO_API_VERSION;
}

/* Module name of the SceShell plugin (shell/exports.yml). */
#define SHELL_MODULE "VitaJPOverlay_Shell"

/* Registers the caller as the shell if it has the shell plugin loaded.
 * SceShell can restart without a reboot (e.g. after a crash), so a new
 * registration that passes the check replaces the old one, and the old
 * shell's game state is reset. */
int vjoRegisterShell(void)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid;
    int ret = 0;
    ENTER_SYSCALL(state);
    pid = ksceKernelGetProcessId();
    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(pid, SHELL_MODULE, &info) < 0) {
        klog("shell registration refused: pid 0x%X has no " SHELL_MODULE, pid);
        ret = VJO_ERR_PERM;
    } else if (g.shell_pid != pid) {
        klog("shell registered: pid 0x%X (was 0x%X)", pid, g.shell_pid);
        g.shell_pid = pid;
        g.input_block = 0;
        if (g.game_pid > 0)
            ksceKernelSetEventFlag(g.evf, VJO_EV_GAME_START); /* re-classify the foreground app */
    }
    EXIT_SYSCALL(state);
    return ret;
}

int vjoWaitEvent(uint32_t mask, uint32_t *out, SceUInt32 timeout_us)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    unsigned int bits = 0;
    SceUInt to = timeout_us;
    int ret;
    ENTER_SYSCALL(state);
    ret = ksceKernelWaitEventFlag(g.evf, mask & VJO_EV_ALL, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                  &bits, timeout_us ? &to : NULL);
    if (out)
        ksceKernelMemcpyKernelToUser(out, &bits, sizeof(bits));
    EXIT_SYSCALL(state);
    return ret;
}

int vjoGetState(VjoState *out)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    VjoState s;
    uint32_t size = 0;
    ENTER_SYSCALL(state);
    memset(&s, 0, sizeof(s));
    ksceKernelMemcpyUserToKernel(&size, out, sizeof(size));
    if (size > sizeof(s))
        size = sizeof(s);
    s.size = sizeof(s);
    s.game_pid = g.game_pid;
    s.fb_width = g.fb_w;
    s.fb_height = g.fb_h;
    s.fb_pitch = g.fb_pitch;
    s.fb_pixelformat = g.fb_fmt;
    VJO_LOCK();
    s.scene = g.tracker.id;
    s.stable = g.tracker.stable;
    s.unsettled_ms = (uint32_t)(scene_unsettled_us(&g.tracker, ksceKernelGetSystemTimeWide()) / 1000);
    s.quiet = g.tracker.quiet && !g.tracker.stable;
    /* 0 while a region capture rewrites capture_sig */
    if (g.region_seq) {
        s.region_seq = g.region_seq;
        s.region_scene = scene_match_since(&g.tracker, &g.capture_sig, g.region_us);
    }
    VJO_UNLOCK();
    s.alloc_status = g.alloc_status;
    s.capture_result = g.capture_result;
    s.width = g.crop_w;
    s.height = g.crop_h;
    s.capture_scene = g.capture_scene;
    s.raw_stride = g.raw_stride;
    s.done_seq = g.done_seq;
    if (size >= sizeof(uint32_t))
        ksceKernelMemcpyKernelToUser(out, &s, size);
    EXIT_SYSCALL(state);
    return 0;
}

int vjoSetRegion(const VjoRect *r)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    VjoRect k = {0, 0, 0, 0};
    ENTER_SYSCALL(state);
    if (r)
        ksceKernelMemcpyUserToKernel(&k, r, sizeof(k));
    VJO_LOCK();
    g.region = k;
    reset_change_detection();
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return 0;
}

/* The shell enables the kernel for the current foreground process once it
 * has checked that it is a game (not a system app). */
int vjoSetGameActive(int pid, int mode)
{
    uint32_t state;
    int ret;
    if (!caller_ok())
        return VJO_ERR_PERM;
    if (mode < VJO_GAME_NONE || mode > VJO_GAME_STATIC_FB)
        return VJO_ERR_ARG;
    ENTER_SYSCALL(state);
    ret = lifecycle_set_game_active(pid, mode);
    EXIT_SYSCALL(state);
    return ret;
}

int vjoSetTriggers(int toggle, int subtitle)
{
    if (!caller_ok())
        return VJO_ERR_PERM;
    if (toggle < 0 || toggle >= VJO_TRIGGER_COUNT || subtitle < 0 || subtitle >= VJO_TRIGGER_COUNT ||
        toggle == subtitle)
        return VJO_ERR_ARG;
    if (g.trigger[TRIG_TOGGLE] != toggle || g.trigger[TRIG_SUBTITLE] != subtitle) {
        g.trigger[TRIG_TOGGLE] = toggle;
        g.trigger[TRIG_SUBTITLE] = subtitle;
        g.trigger_gen++;
    }
    return 0;
}

int vjoSetInputBlock(int on)
{
    if (!caller_ok())
        return VJO_ERR_PERM;
    if (!on && g.input_block) /* e.g. the ○ that closed the overlay */
        __sync_fetch_and_or(&g.suppress_mask, g.raw_buttons);
    g.input_block = on ? 1 : 0;
    return 0;
}

int vjoPollInput(VjoInput *out)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    VjoInput in;
    ENTER_SYSCALL(state);
    in = g.last_input;
    ksceKernelMemcpyKernelToUser(out, &in, sizeof(in));
    EXIT_SYSCALL(state);
    return 0;
}

/* Returns the capture sequence number (> 0); VjoState.done_seq reaches it
 * when this capture has finished. */
int vjoRequestCapture(uint32_t flags)
{
    uint32_t state;
    int ret;
    if (!caller_ok())
        return VJO_ERR_PERM;
    if (g.game_pid <= 0 || !g.game_active)
        return VJO_ERR_NO_GAME;
    if (g.alloc_status != VJO_ALLOC_OK)
        return VJO_ERR_NO_MEMORY;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    if (g.capture_state != CAPTURE_IDLE) {
        ret = VJO_ERR_BUSY;
    } else {
        g.capture_seq = (g.capture_seq + 1) & 0x7FFFFFFF;
        if (!g.capture_seq)
            g.capture_seq = 1;
        ret = (int)g.capture_seq;
        g.raw_valid = 0;
        g.capture_full = (flags & VJO_CAPTURE_FULL) != 0;
        if (!g.capture_full)
            g.region_seq = 0; /* capture_sig gets rewritten */
        g.capture_requested_us = ksceKernelGetSystemTimeWide();
        g.capture_state = CAPTURE_PENDING;
    }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return ret;
}

/* Rows [row, row + n) of the last capture, raw_stride bytes each. */
int vjoReadRaw(uint32_t row, uint32_t n, void *dst)
{
    uint32_t state;
    if (!caller_ok())
        return VJO_ERR_PERM;
    int ret;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    if (g.capture_state != CAPTURE_IDLE || !g.raw_valid || !g.raw || row >= g.crop_h) {
        ret = -1;
    } else {
        uint32_t stride = g.raw_stride;
        if (n > g.crop_h - row)
            n = g.crop_h - row;
        ret = ksceKernelMemcpyKernelToUser(dst, g.raw + row * stride, n * stride) < 0 ? -1 : (int)n;
    }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return ret;
}

/* ---------------- module ---------------- */

static void delete_sync_objects(void)
{
    if (g.game_lock > 0)
        ksceKernelDeleteMutex(g.game_lock);
    if (g.lock > 0)
        ksceKernelDeleteMutex(g.lock);
    if (g.ievf > 0)
        ksceKernelDeleteEventFlag(g.ievf);
    if (g.evf > 0)
        ksceKernelDeleteEventFlag(g.evf);
    g.game_lock = g.lock = g.ievf = g.evf = 0;
}

static void release_hooks(void)
{
    lifecycle_hooks_release();
    input_hooks_release();
    display_hook_release();
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    memset(&g, 0, sizeof(g));
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_L_R;
    g.trigger[TRIG_SUBTITLE] = VJO_TRIGGER_SELECT_R;
    g.input_us32 = g.input_edge_us32 = (uint32_t)(ksceKernelGetSystemTimeWide() - 3600000000LL); /* none yet */
    g.evf = ksceKernelCreateEventFlag("VjoEvents", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    g.ievf = ksceKernelCreateEventFlag("VjoInternal", 0, 0, NULL);
    g.lock = ksceKernelCreateMutex("VjoLock", 0, 0, NULL);
    g.game_lock = ksceKernelCreateMutex("VjoGameLock", 0, 0, NULL);
    if (g.evf < 0 || g.ievf < 0 || g.lock < 0 || g.game_lock < 0) {
        delete_sync_objects();
        return SCE_KERNEL_START_FAILED;
    }

    {
        int d = display_hook_install(), i = input_hooks_install(), l = lifecycle_hooks_install();
        klog("hooks: display %d, input %d, procevent %d (0 = ok)", d, i, l);
    }

    /* Without the worker nothing would ever be polled or finished. */
    worker_uid = ksceKernelCreateThread("VjoWorker", worker, 0x3C, 0x4000, 0, 0x10000, NULL);
    if (worker_uid < 0 || ksceKernelStartThread(worker_uid, 0, NULL) < 0) {
        if (worker_uid >= 0)
            ksceKernelDeleteThread(worker_uid);
        worker_uid = -1;
        release_hooks();
        delete_sync_objects();
        return SCE_KERNEL_START_FAILED;
    }
    klog("Vita JP Overlay kernel %d started", VJO_API_VERSION);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    worker_run = 0;
    if (worker_uid >= 0) {
        ksceKernelSetEventFlag(g.ievf, IEV_QUIT);
        ksceKernelWaitThreadEnd(worker_uid, NULL, NULL);
        ksceKernelDeleteThread(worker_uid);
    }
    release_hooks();
    buffers_free();
    delete_sync_objects();
    return SCE_KERNEL_STOP_SUCCESS;
}
