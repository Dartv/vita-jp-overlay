/* Foreground game tracking: the state transitions behind the process event
 * hook and vjoSetGameActive (lifecycle.c), kept free of psp2kern so the host
 * tests run them. lifecycle.c holds g.game_lock around each call, copies
 * the state in and out of g and does the side effects (log, event flag). */
#ifndef VJO_FOREGROUND_H
#define VJO_FOREGROUND_H

#include <stdint.h>

/* Internal event bits (g.ievf), consumed by the kernel worker thread. */
#define IEV_GAME_START  0x01u
#define IEV_GAME_EXIT   0x02u
#define IEV_CAPTURED    0x04u /* raw region copied in the display hook */
#define IEV_QUIT        0x08u
#define IEV_ACTIVATE    0x10u /* shell marked game_pid as a game */

/* SceProcessmgr process events (the hooked handler's ev argument). */
#define PROCEV_STARTUP 1
#define PROCEV_EXIT    3
#define PROCEV_SUSPEND 4
#define PROCEV_RESUME  5

/* Title IDs come from SceSysrootForKernel, whose NIDs differ between 3.60
 * and 3.63+, so the kernel only tracks the foreground process. The shell
 * reads its title ID in user space and enables us for games only
 * (vjoSetGameActive); system apps stay inactive.
 *
 * A process that starts over a confirmed game (a system or background app)
 * takes the foreground, but the game is kept in prev: if the newcomer turns
 * out not to be a game, or exits, the game comes back (the shell
 * re-classifies it on GAME_START), so the trigger, input and capture do not
 * stay dead until the game is suspended and resumed. */
typedef struct {
    int game_pid;    /* foreground process, 0 = none */
    int game_active; /* the shell confirmed game_pid is a game */
    int prev;        /* confirmed game behind game_pid, still running; 0 = none */
    int shell_pid;   /* registered SceShell (never becomes the foreground) */
} VjoForeground;

/* Process event ev (PROCEV_*) for pid. Returns the IEV_* bits to signal;
 * IEV_GAME_EXIT | IEV_GAME_START means the foreground went back to prev. */
uint32_t fg_process_event(VjoForeground *f, int pid, int ev);

/* The shell's verdict on pid. Returns -1 if pid is not the foreground
 * process (state unchanged), else 0 with the IEV_* bits to signal in *iev
 * (IEV_GAME_START: not a game, back to prev). */
int fg_set_game_active(VjoForeground *f, int pid, int active, uint32_t *iev);

#endif
