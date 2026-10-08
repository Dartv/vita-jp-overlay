/* Kernel <-> SceShell interface of Vita JP Overlay. Shared by kernel/,
 * shell/ and core/ (for the plain data types). */
#ifndef VJO_API_H
#define VJO_API_H

#include <stdint.h>

#define VJO_API_VERSION 9

/* Normalized rectangle, 0..65535 on both axes. w == 0 means full screen. */
typedef struct {
    uint16_t x, y, w, h;
} VjoRect;

enum VjoTrigger {
    VJO_TRIGGER_SELECT = 0,
    VJO_TRIGGER_START = 1,
    VJO_TRIGGER_L_R = 2,
    VJO_TRIGGER_SELECT_L = 3,
    VJO_TRIGGER_SELECT_R = 4,
    VJO_TRIGGER_REAR_DOUBLE_TAP = 5,
    VJO_TRIGGER_COUNT
};

/* vjoWaitEvent bits */
#define VJO_EV_TRIGGER       0x01u /* toggle trigger pressed */
#define VJO_EV_REGION_STABLE 0x02u /* region changed, then unchanged (animation aside) for ~300 ms */
#define VJO_EV_GAME_START    0x04u
#define VJO_EV_GAME_EXIT     0x08u
#define VJO_EV_CAPTURE_DONE  0x10u /* raw rows ready (or failed, see VjoState.capture_result) */
#define VJO_EV_SUBTITLE      0x20u /* subtitle trigger pressed */
#define VJO_EV_REGION_QUIET  0x40u /* the region became quiet (VjoState.quiet) */
#define VJO_EV_ALL           0x7Fu

/* VjoState.alloc_status */
#define VJO_ALLOC_NONE 0  /* no game running */
#define VJO_ALLOC_OK   1
#define VJO_ALLOC_FAIL 2  /* not enough memory for capture buffers */

typedef struct {
    uint32_t size;          /* sizeof(VjoState), set by caller */
    int32_t game_pid;
    uint32_t fb_width, fb_height, fb_pitch, fb_pixelformat;
    uint32_t scene;         /* the screen in the region: a new number per change (animation aside), 0 = none yet */
    uint32_t stable;        /* 1 if the scene has settled */
    uint32_t alloc_status;
    int32_t capture_result; /* 0 ok (rows readable via vjoReadRaw), <0 error code */
    uint32_t width, height; /* captured region size in pixels */
    uint32_t capture_scene; /* scene of the captured pixels; 0 = unknown (or VJO_CAPTURE_FULL) */
    uint32_t raw_stride;    /* bytes per raw row (A8B8G8R8) */
    uint32_t done_seq;      /* sequence number of the last finished capture */
    uint32_t unsettled_ms;  /* the region has been changing this long without settling (0 = settled) */
    /* not settled, but only a small spot keeps changing (an icon not
     * masked yet): worth capturing */
    uint32_t quiet;
    uint32_t region_seq;    /* the last successful region capture (not VJO_CAPTURE_FULL); 0 = none */
    /* the scene if that capture shows it now (animated cells aside, and
     * the quiet spot for one taken since it started), else 0: a capture
     * taken before an icon was masked shows the screen once it is */
    uint32_t region_scene;
} VjoState;

typedef struct {
    uint32_t buttons;       /* SCE_CTRL_* of the real pad, unfiltered */
    uint8_t lx, ly, rx, ry; /* analog sticks, 128 = centre */
} VjoInput;

/* vjoRequestCapture flags */
#define VJO_CAPTURE_FULL 0x1u /* the whole frame instead of the region (Anki screenshot) */

/* Kernel capture error codes (VjoState.capture_result) */
#define VJO_ERR_NO_GAME      (-1)
#define VJO_ERR_NO_MEMORY    (-2)
#define VJO_ERR_BUSY         (-3)
#define VJO_ERR_TIMEOUT      (-4)
#define VJO_ERR_FORMAT       (-5)
#define VJO_ERR_ARG          (-6) /* bad argument */
#define VJO_ERR_COPY         (-7)
#define VJO_ERR_PERM         (-8) /* caller is not (or may not be) the registered shell */

/* vjoSetGameActive's mode for the foreground process. */
enum VjoGameMode {
    VJO_GAME_NONE = 0,      /* a system app */
    VJO_GAME = 1,
    VJO_GAME_STATIC_FB = 2, /* draws into a framebuffer it set once and submits no
                             * frames (the PSP emulator): sampled from its pad calls */
};

/* Syscalls exported by VitaJPOverlay_Kernel (library VitaJPOverlayForUser).
 * All return < 0 on error. */
#if !defined(VJO_HOST)
#include <psp2common/types.h>
int vjoGetVersion(void);                 /* VJO_API_VERSION, no side effects */
int vjoRegisterShell(void);              /* caller becomes the shell if it has VitaJPOverlay_Shell loaded */
int vjoWaitEvent(uint32_t mask, uint32_t *out, SceUInt32 timeout_us); /* 0 timeout = forever */
int vjoGetState(VjoState *out);          /* set out->size first */
int vjoSetRegion(const VjoRect *r);      /* NULL = full screen */
int vjoSetTriggers(int toggle, int subtitle); /* enum VjoTrigger each, not equal */
int vjoSetGameActive(int pid, int mode);  /* shell: enum VjoGameMode of the foreground process */
int vjoSetInputBlock(int on);            /* block all game input (not the shell) */
int vjoPollInput(VjoInput *out);         /* raw pad, unfiltered */
int vjoRequestCapture(uint32_t flags);   /* VJO_CAPTURE_*; returns seq > 0; raw rows, the shell encodes the JPEG */
int vjoReadRaw(uint32_t row, uint32_t n, void *dst);      /* returns rows copied */
#endif

#endif
