/* Change detection: when the region shows a new screen, and when that
 * screen has settled. Kept free of psp2kern so the host tests run it.
 *
 * A frame's signature is a hash of each cell of a grid over the region
 * (cells about as wide as tall: 16x8 on a full screen, 32x4 on a text box),
 * so any change in a cell's sampled pixels is seen, and any cell that
 * differs from the screen's first frame makes a new screen, except:
 *
 * A cell that keeps cycling through a few states (SCENE_ANIM_REVISITS
 * changes in a row back to a recent state) for SCENE_ANIM_SPAN_US, with no
 * pause longer than SCENE_ANIM_GAP_US and no player input, is animated: a
 * blinking or bobbing "next" icon, an AUTO mark, a sparkle. A change within
 * SCENE_INPUT_US of input (a button, a stick, a touch the game read) is the
 * player's doing: a menu scrolled up and down, a choice toggled, a line
 * advanced. It never counts as animation, however few cells it touches.
 * Input is a press or release, a stick or a touch the game read (a held
 * button counts only for its first 1.5 s: see kernel/input.c). An input
 * edge (a press, a release, a touch) also ends every mask, which is a
 * change: a short menu masked while a held button scrolled it is seen
 * again at the release or the next tap; the icons are masked again once
 * they cycle. That costs a press that changes nothing, on a screen with
 * an icon, up to two changes (the mask ending, then the icon's own change
 * within SCENE_INPUT_US). Input is what the game gets: not the overlay's
 * trigger combos, nothing while the overlay is open. Without input
 * (an auto-advancing game, UI that reacts later than SCENE_INPUT_US, input
 * through unhooked touch calls), a menu cycled for SCENE_ANIM_SPAN_US is
 * masked like an icon, until SCENE_ANIM_GAP_US after it stops.
 * At most SCENE_ANIM_MAX cells are masked: an animated background masks
 * none. A cell masked within SCENE_ANIM_WARM_US is masked again as soon as
 * it cycles (the icon of the next line).
 *
 * A mask ends SCENE_ANIM_GAP_US after the cell stops, and that is a change:
 * whatever the mask hid is not taken as the screen after that.
 *
 * Settling waits while an icon cycles before it is masked: a screen with a
 * blinking icon settles when the icon first appears (a new state) and
 * again about SCENE_ANIM_SPAN_US later, once it is masked; with a recent
 * mask, once. A single icon-sized revert (A, B, A) without input also
 * waits up to SCENE_ANIM_GAP_US; a line replaced by a shorter one does
 * not. */
#ifndef VJO_SCENE_H
#define VJO_SCENE_H

#include <stdint.h>

#define SCENE_CELLS         128
#define SCENE_HISTORY       8       /* recent states per cell (an icon's frames) */
#define SCENE_ANIM_REVISITS 2       /* changes back to a recent state, in a row */
#define SCENE_ANIM_SPAN_US  2000000 /* cycling this long = animated */
#define SCENE_ANIM_GAP_US   1500000 /* a longer pause ends the run (a slow blink fits) */
#define SCENE_ANIM_WARM_US  5000000 /* masked this recently: masked again at once */
#define SCENE_ANIM_MAX      8       /* animated cells masked at most */
#define SCENE_INPUT_US      500000  /* changes this soon after input are the player's */
#define SCENE_ICON          2       /* an icon-sized change fits in 2x2 cells */
#define SCENE_STABLE_US     300000  /* no change this long = settled */

typedef struct {
    uint32_t w, h; /* the region size it was built for; 0 = none */
    uint32_t cols; /* grid columns (cell i is at column i % cols) */
    uint32_t hash[SCENE_CELLS];
} SceneSig;

/* Builds a signature from A8B8G8R8 rows of a w x h region (w, h >= 1),
 * fed in increasing order; only rows from scene_acc_next_row() are used.
 * The grid is recomputed only when w x h changes, so one zero-initialized
 * SceneAcc is kept per caller. Nothing here divides per row or pixel (the
 * Vita's CPU has no divide instruction). */
typedef struct {
    uint32_t w, h, cols, ystep, xstep;
    uint32_t x0[SCENE_CELLS + 1]; /* first pixel of each grid column */
    uint32_t y0[SCENE_CELLS + 1]; /* first row of each grid row */
    uint32_t next_y, cell_row;
    SceneSig *out;
} SceneAcc;

void scene_acc_begin(SceneAcc *a, SceneSig *out, uint32_t w, uint32_t h);
/* Adds row y; ignored unless y == scene_acc_next_row(). */
void scene_acc_row(SceneAcc *a, uint32_t y, const uint32_t *abgr);

static inline uint32_t scene_acc_next_row(const SceneAcc *a)
{
    return a->next_y; /* >= h: the signature is complete */
}

typedef struct {
    uint32_t last;                /* the state in the last signature */
    int seen;                     /* last is set */
    uint32_t hist[SCENE_HISTORY]; /* recent distinct states, a ring */
    int n, next;
    int revisits;                 /* changes back to a recent state, in a row */
    int64_t run_start_us;         /* first change of the current run of changes */
    int64_t moved_us;             /* last change */
    int64_t unmasked_us;          /* last mask ended */
    int masked;                   /* animated: ignored (as of the last signature) */
} SceneCell;

/* Scene numbers start at 1 and are never reused (0 = unknown), so a number
 * kept by the shell can't match a later screen, even after a reset. */
typedef struct {
    SceneSig ref;          /* the current screen (ref.w == 0: none yet) */
    SceneCell cell[SCENE_CELLS];
    uint32_t id;
    int stable;            /* the current screen has settled */
    int64_t changed_us;    /* last change */
    int64_t unsettled_us;  /* first change after the last settled screen */
    int masked;            /* cells masked as animated (log) */
    int icon_change;       /* the last change was icon-sized */
    int64_t edge_us;       /* the last input edge seen */
} SceneTracker;

/* Forget the screen (new game or region); keeps the numbering. */
void scene_reset(SceneTracker *t);

/* One signature from the display hook; input_us: the last player input,
 * edge_us: the last press, release or touch. Returns 1 when the screen
 * settles. */
int scene_update(SceneTracker *t, const SceneSig *sig, int64_t now, int64_t input_us, int64_t edge_us);

/* t->id if sig shows the current screen (masked cells aside), else 0. */
uint32_t scene_match(const SceneTracker *t, const SceneSig *sig);

/* How long the region has been changing without settling (0 = settled). */
int64_t scene_unsettled_us(const SceneTracker *t, int64_t now);

#endif
