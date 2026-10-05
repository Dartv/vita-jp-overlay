/* Triggers: detecting the two button triggers on the pad, and hiding their
 * buttons from the game. The state machines behind input.c, kept free of
 * psp2kern so the host tests run them. */
#ifndef VJO_TRIGGERS_H
#define VJO_TRIGGERS_H

#include <stdint.h>

/* The two triggers (VjoKernelState.trigger). */
enum { TRIG_TOGGLE = 0, TRIG_SUBTITLE = 1, TRIG_COUNT };

static inline int trig_other(int k)
{
    return k == TRIG_TOGGLE ? TRIG_SUBTITLE : TRIG_TOGGLE;
}

/* ---- detection (the kernel worker's pad poll) ----
 * A trigger fires when its buttons are pressed. When its combo is part of
 * the other trigger's (select and select+r) it fires on release instead,
 * and only if the other combo was not completed meanwhile: pressing
 * select+r starts with select. */
typedef struct {
    int prev_pressed;
    int superset_seen; /* the other combo was completed during this press */
} TrigEdge;

/* One poll of a trigger with buttons `mask` (0 = none) next to the other
 * trigger's `other`. Returns 1 when it fires. */
int trig_edge(TrigEdge *e, uint32_t mask, uint32_t other, uint32_t buttons);

/* ---- hiding (the game's pad calls) ----
 * Single-button triggers are always hidden. A combo is hidden while fully
 * held; with a delay, part of it held alone is hidden for delay_us too, as
 * the rest may follow (pressing l+r, L lands a frame or two before R).
 * Once the whole combo was held, its buttons stay hidden until released.
 * A tap released before the delay ran out is replayed to the game, unless
 * a completed combo of the other trigger shared its buttons. */
typedef struct {
    uint32_t mask[TRIG_COUNT]; /* 0 = no buttons (rear double tap) */
    int single[TRIG_COUNT];    /* a single-button trigger */
    int64_t delay_us;          /* 0 = off */
} TrigConfig;

typedef struct {
    int64_t start_us;  /* first sample of this press, 0 = released */
    int combo;         /* the whole combo was held during this press */
    int claimed;       /* the other trigger's completed combo shared these buttons */
    uint32_t buttons;  /* combo buttons pressed during this press */
    uint32_t replay;
    int64_t replay_until_us;
} TrigHold;

/* Shortest replay: two frames at 60 fps, one at 30. */
#define TRIG_REPLAY_MIN_US 34000

/* One sample of the game's pad (positive logic): returns the buttons the
 * game sees. holds: TRIG_COUNT entries for this pad port, or NULL to skip
 * the delay (combos hidden only while fully held). */
uint32_t trig_filter(const TrigConfig *c, TrigHold *holds, uint32_t pos, int64_t now);

#endif
