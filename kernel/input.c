/* Input: trigger detection (buttons polled from the worker thread, the rear
 * double tap sampled in the game's pad calls), hiding the trigger from the
 * game, blocking all game input while the overlay is open, and the raw pad
 * for the shell (vjoPollInput). */
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "vjo_kernel.h"

/* Touch types/functions (SceTouchForDriver; not declared in psp2kern). */
typedef struct {
    uint8_t id;
    uint8_t force;
    int16_t x;
    int16_t y;
    uint8_t reserved[8];
    uint16_t info;
} KTouchReport;

typedef struct {
    uint64_t timeStamp;
    uint32_t status;
    uint32_t reportNum;
    KTouchReport report[8];
} KTouchData;

int ksceTouchPeek(uint32_t port, KTouchData *pData, uint32_t nBufs);
int ksceTouchSetSamplingState(uint32_t port, int state);

enum {
    H_PEEK_NEG, H_PEEK_NEG2, H_PEEK_POS, H_PEEK_POS2,
    H_READ_NEG, H_READ_NEG2, H_READ_POS, H_READ_POS2,
    H_PEEK_POS_EXT, H_READ_POS_EXT, H_PEEK_POS_EXT2, H_READ_POS_EXT2,
    H_TOUCH_PEEK, H_TOUCH_READ, H_TOUCH_PEEK_REGION, H_TOUCH_READ_REGION,
    H_COUNT
};

static tai_hook_ref_t refs[H_COUNT];
static SceUID hooks[H_COUNT];

static uint32_t trigger_mask(int trigger)
{
    switch (trigger) {
    case VJO_TRIGGER_SELECT: return SCE_CTRL_SELECT;
    case VJO_TRIGGER_START: return SCE_CTRL_START;
    case VJO_TRIGGER_L_R: return SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    case VJO_TRIGGER_SELECT_L: return SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER;
    case VJO_TRIGGER_SELECT_R: return SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER;
    default: return 0;
    }
}

static int is_single_button(int trigger)
{
    return trigger == VJO_TRIGGER_SELECT || trigger == VJO_TRIGGER_START;
}

/* ---- trigger events ---- */

static const uint32_t trig_events[TRIG_COUNT] = {
    [TRIG_TOGGLE] = VJO_EV_TRIGGER,
    [TRIG_SUBTITLE] = VJO_EV_SUBTITLE,
};

static void fire_trigger(int k, uint32_t mask)
{
    __sync_fetch_and_or(&g.suppress_mask, mask);
    ksceKernelSetEventFlag(g.evf, trig_events[k]);
}

/* The trigger set to the rear double tap, or -1. */
static int rear_trigger(void)
{
    for (int k = 0; k < TRIG_COUNT; k++)
        if (g.trigger[k] == VJO_TRIGGER_REAR_DOUBLE_TAP)
            return k;
    return -1;
}

/* Rear double tap. The back panel is read in the game's context, from its
 * pad calls (as reVita reads touch inside its ctrl hooks): on the device,
 * touch peeks from our worker thread (kernel process) never reported. */
#define TOUCH_BACK     1
#define TOUCH_INTERNAL 0x564A4F49u /* KTouchData.status of our own peek: the touch hooks pass it */
#define REAR_SAMPLE_US 15000      /* at most one read per frame */
#define TAP_MAX_US     250000
#define TAP_GAP_US     350000
#define TAP_MAX_MOVE   120

static volatile int rear_busy; /* pad calls can come from several game threads */
/* Thread inside rear_sample's marked peek, else 0. The marker alone is not
 * enough: a game's touch call gets a kernel buffer its syscall does not
 * initialise, which may hold a stale marker from our peek on that stack. */
static volatile SceUID rear_peek_thid;
static int64_t rear_sampled_us;
static int rear_down, rear_taps;
static int64_t rear_down_t, rear_up_t;
static int16_t rear_x, rear_y;

static void rear_double_tap(const KTouchData *t, int64_t now)
{
    int down = t->reportNum > 0;
    if (down && !rear_down) {
        rear_down_t = now;
        rear_x = t->report[0].x;
        rear_y = t->report[0].y;
        if (rear_taps && now - rear_up_t > TAP_GAP_US)
            rear_taps = 0;
    } else if (!down && rear_down) {
        if (rear_taps < 0) {
            rear_taps = 0; /* end of a drag */
        } else if (now - rear_down_t < TAP_MAX_US) {
            if (rear_taps == 1 && now - rear_up_t < TAP_GAP_US + TAP_MAX_US) {
                int k = rear_trigger();
                rear_taps = 0;
                if (k >= 0)
                    fire_trigger(k, 0);
            } else {
                rear_taps = 1;
                rear_up_t = now;
            }
        } else {
            rear_taps = 0;
        }
    } else if (down && rear_down) {
        int dx = t->report[0].x - rear_x, dy = t->report[0].y - rear_y;
        if (dx * dx + dy * dy > TAP_MAX_MOVE * TAP_MAX_MOVE)
            rear_taps = -1; /* a drag, not a tap: ignore until released */
    }
    rear_down = down;
}

/* Called from the ctrl hooks. Also while the overlay blocks input: the
 * marked peek bypasses the touch filter, so a double tap closes it. */
static void rear_sample(void)
{
    KTouchData t;
    int64_t now;
    int n;
    if (rear_trigger() < 0 || g.game_pid <= 0 || !g.game_active ||
        ksceKernelGetProcessId() != g.game_pid)
        return;
    if (!__sync_bool_compare_and_swap(&rear_busy, 0, 1))
        return;
    now = ksceKernelGetSystemTimeWide();
    if (now - rear_sampled_us >= REAR_SAMPLE_US) {
        rear_sampled_us = now;
        ksceTouchSetSamplingState(TOUCH_BACK, 1);
        memset(&t, 0, sizeof(t));
        t.status = TOUCH_INTERNAL;
        rear_peek_thid = ksceKernelGetThreadId();
        n = ksceTouchPeek(TOUCH_BACK, &t, 1);
        rear_peek_thid = 0;
        if (n > 0)
            rear_double_tap(&t, now);
    }
    __sync_lock_release(&rear_busy);
}

/* Combo delay state (triggers.h), per pad port: a game may also read empty
 * ports, which must not end a press. Pad calls can come from several game
 * threads: one at a time uses it, the others skip the delay. Reset when the
 * triggers change (g.trigger_gen). */
#define COMBO_DELAY_US 50000 /* covers the gap between a combo's two presses */
#define HOLD_PORTS 5         /* 0 = the Vita's pad, 1-4 = PS TV controllers */

static TrigHold hold[HOLD_PORTS][TRIG_COUNT];
static volatile int hold_busy;
static uint32_t hold_gen;

static void trig_config(TrigConfig *c)
{
    for (int k = 0; k < TRIG_COUNT; k++) {
        c->mask[k] = trigger_mask(g.trigger[k]);
        c->single[k] = is_single_button(g.trigger[k]);
    }
    c->delay_us = COMBO_DELAY_US;
}

/* Rewrites the game's pad data (user memory) in place. */
static void filter_ctrl(int port, SceCtrlData *pad_data, int n, int negative)
{
    TrigConfig c;
    TrigHold *holds = NULL;
    int64_t now;
    if (n <= 0 || g.game_pid <= 0 || !g.game_active || ksceKernelGetProcessId() != g.game_pid)
        return;
    if (n > 64)
        n = 64;
    /* One time for the call's samples: games read one at a time. */
    now = ksceKernelGetSystemTimeWide();
    trig_config(&c);
    if (c.delay_us && port >= 0 && port < HOLD_PORTS && __sync_bool_compare_and_swap(&hold_busy, 0, 1)) {
        if (hold_gen != g.trigger_gen) {
            memset(hold, 0, sizeof(hold));
            hold_gen = g.trigger_gen;
        }
        holds = hold[port];
    }
    for (int i = 0; i < n; i++) {
        struct {
            uint32_t buttons;
            uint8_t lx, ly, rx, ry;
        } d;
        uint32_t pos;
        uintptr_t u = (uintptr_t)&pad_data[i].buttons;
        if (ksceKernelMemcpyUserToKernel(&d, (const void *)u, sizeof(d)) < 0)
            break;
        pos = negative ? ~d.buttons : d.buttons;
        if (g.input_block) {
            pos = 0;
            d.lx = d.ly = d.rx = d.ry = 0x80;
        } else {
            pos = trig_filter(&c, holds, pos, now) & ~g.suppress_mask;
        }
        d.buttons = negative ? ~pos : pos;
        ksceKernelMemcpyKernelToUser((void *)u, &d, sizeof(d));
    }
    if (holds)
        __sync_lock_release(&hold_busy);
}

#define CTRL_HOOK(idx, name, negative)                                       \
    static int name##_patched(int port, SceCtrlData *pad_data, int count)    \
    {                                                                        \
        int ret = TAI_CONTINUE(int, refs[idx], port, pad_data, count);       \
        rear_sample();                                                       \
        if (ret > 0)                                                         \
            filter_ctrl(port, pad_data, ret, negative);                            \
        return ret;                                                          \
    }

CTRL_HOOK(H_PEEK_NEG, peek_neg, 1)
CTRL_HOOK(H_PEEK_NEG2, peek_neg2, 1)
CTRL_HOOK(H_PEEK_POS, peek_pos, 0)
CTRL_HOOK(H_PEEK_POS2, peek_pos2, 0)
CTRL_HOOK(H_READ_NEG, read_neg, 1)
CTRL_HOOK(H_READ_NEG2, read_neg2, 1)
CTRL_HOOK(H_READ_POS, read_pos, 0)
CTRL_HOOK(H_READ_POS2, read_pos2, 0)
CTRL_HOOK(H_PEEK_POS_EXT, peek_pos_ext, 0)
CTRL_HOOK(H_READ_POS_EXT, read_pos_ext, 0)
CTRL_HOOK(H_PEEK_POS_EXT2, peek_pos_ext2, 0)
CTRL_HOOK(H_READ_POS_EXT2, read_pos_ext2, 0)

/* Kernel touch exports receive kernel buffers (the user syscalls copy out
 * afterwards). Our own peek (rear_sample) is marked, made from the thread in
 * rear_peek_thid, and passes. */
static int filter_touch(KTouchData *p, int ret)
{
    if (ret <= 0 || ret > 64 || !g.input_block || g.game_pid <= 0 || !g.game_active)
        return ret;
    if (ksceKernelGetProcessId() != g.game_pid)
        return ret;
    p[0] = p[ret - 1];
    p[0].reportNum = 0;
    return 1;
}

static int touch_peek_patched(uint32_t port, KTouchData *p, uint32_t n)
{
    if (p && p->status == TOUCH_INTERNAL && rear_peek_thid == ksceKernelGetThreadId())
        return TAI_CONTINUE(int, refs[H_TOUCH_PEEK], port, p, n);
    return filter_touch(p, TAI_CONTINUE(int, refs[H_TOUCH_PEEK], port, p, n));
}

static int touch_read_patched(uint32_t port, KTouchData *p, uint32_t n)
{
    return filter_touch(p, TAI_CONTINUE(int, refs[H_TOUCH_READ], port, p, n));
}

static int touch_peek_region_patched(uint32_t port, KTouchData *p, uint32_t n, int region)
{
    return filter_touch(p, TAI_CONTINUE(int, refs[H_TOUCH_PEEK_REGION], port, p, n, region));
}

static int touch_read_region_patched(uint32_t port, KTouchData *p, uint32_t n, int region)
{
    return filter_touch(p, TAI_CONTINUE(int, refs[H_TOUCH_READ_REGION], port, p, n, region));
}

int input_hooks_install(void)
{
    static const struct {
        int idx;
        uint32_t nid;
        void *fn;
    } ctrl[] = {
        /* SceCtrl user exports (library 0xD197E3C7), as hooked by PSVshell */
        {H_PEEK_NEG, 0x104ED1A7, peek_neg_patched},
        {H_PEEK_NEG2, 0x81A89660, peek_neg2_patched},
        {H_PEEK_POS, 0xA9C3CED6, peek_pos_patched},
        {H_PEEK_POS2, 0x15F81E8C, peek_pos2_patched},
        {H_READ_NEG, 0x15F96FB0, read_neg_patched},
        {H_READ_NEG2, 0x27A0C5FB, read_neg2_patched},
        {H_READ_POS, 0x67E7AB83, read_pos_patched},
        {H_READ_POS2, 0xC4226A3E, read_pos2_patched},
        /* Ext/Ext2 variants (hooked by reVita too) */
        {H_PEEK_POS_EXT, 0xA59454D3, peek_pos_ext_patched},
        {H_READ_POS_EXT, 0xE2D99296, read_pos_ext_patched},
        {H_PEEK_POS_EXT2, 0x860BF292, peek_pos_ext2_patched},
        {H_READ_POS_EXT2, 0xA7178860, read_pos_ext2_patched},
    };
    static const struct {
        int idx;
        uint32_t nid;
        void *fn;
    } touch[] = {
        /* SceTouch kernel exports, as hooked by reVita */
        {H_TOUCH_PEEK, 0xBAD1960B, touch_peek_patched},
        {H_TOUCH_READ, 0x70C8AACE, touch_read_patched},
        {H_TOUCH_PEEK_REGION, 0x9B3F7207, touch_peek_region_patched},
        {H_TOUCH_READ_REGION, 0x9A91F624, touch_read_region_patched},
    };
    int failed = 0;

    for (int i = 0; i < H_COUNT; i++)
        hooks[i] = -1;
    for (unsigned i = 0; i < sizeof(ctrl) / sizeof(ctrl[0]); i++) {
        hooks[ctrl[i].idx] = taiHookFunctionExportForKernel(KERNEL_PID, &refs[ctrl[i].idx], "SceCtrl",
                                                            0xD197E3C7, ctrl[i].nid, ctrl[i].fn);
        if (hooks[ctrl[i].idx] < 0) {
            klog("ctrl hook %08X failed 0x%08X", ctrl[i].nid, hooks[ctrl[i].idx]);
            failed++;
        }
    }
    for (unsigned i = 0; i < sizeof(touch) / sizeof(touch[0]); i++) {
        hooks[touch[i].idx] = taiHookFunctionExportForKernel(
            KERNEL_PID, &refs[touch[i].idx], "SceTouch", TAI_ANY_LIBRARY, touch[i].nid, touch[i].fn);
        if (hooks[touch[i].idx] < 0) {
            klog("touch hook %08X failed 0x%08X", touch[i].nid, hooks[touch[i].idx]);
            failed++;
        }
    }
    return failed ? -1 : 0;
}

void input_hooks_release(void)
{
    for (int i = 0; i < H_COUNT; i++) {
        if (hooks[i] >= 0)
            taiHookReleaseForKernel(hooks[i], refs[i]);
        hooks[i] = -1;
    }
}

/* ---- pad polling (worker thread, ~60 Hz) ---- */

static TrigEdge edges[TRIG_COUNT];
static int analog_on;

void input_poll(void)
{
    SceCtrlData c;
    VjoInput in;

    /* The sampling mode is per process and starts digital, which reports
     * centred sticks; this thread's reads need analog (as reVita's setup
     * does for the game). */
    if (!analog_on) {
        int mode = -1;
        ksceCtrlGetSamplingMode(&mode);
        if (mode == SCE_CTRL_MODE_DIGITAL)
            klog("pad sampling: digital -> analog (0x%X)", ksceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG));
        analog_on = 1;
    }

    memset(&in, 0, sizeof(in));
    in.lx = in.ly = in.rx = in.ry = 128; /* centred unless the pad reports */
    if (ksceCtrlPeekBufferPositive(0, &c, 1) > 0 || ksceCtrlPeekBufferPositive(1, &c, 1) > 0) {
        g.raw_buttons = c.buttons;
        in.buttons = c.buttons;
        in.lx = c.lx;
        in.ly = c.ly;
        in.rx = c.rx;
        in.ry = c.ry;
    }
    /* Held-over buttons stay hidden from the game until released. */
    __sync_fetch_and_and(&g.suppress_mask, g.raw_buttons);

    for (int k = 0; k < TRIG_COUNT; k++) {
        uint32_t mask = trigger_mask(g.trigger[k]);
        if (trig_edge(&edges[k], mask, trigger_mask(g.trigger[trig_other(k)]), g.raw_buttons) &&
            g.game_pid > 0 && g.game_active)
            fire_trigger(k, mask);
    }

    g.last_input = in;
}
