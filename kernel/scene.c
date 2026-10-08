/* Change detection (scene.h). Signatures are built in the display hook;
 * the tracker runs in the kernel worker thread. */
#include "scene.h"

#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

/* At least 4 samples along each side of a cell: every 16th row and 4th
 * pixel of a full screen (as many as the old whole-region checksum), every
 * row of a thin strip. */
#define SAMPLES_PER_CELL_SIDE 4
#define MAX_YSTEP 16
#define MAX_XSTEP 4

static uint32_t step_for(uint32_t len, uint32_t cells, uint32_t max)
{
    uint32_t s = len / (cells * SAMPLES_PER_CELL_SIDE);
    return s < 1 ? 1 : s > max ? max : s;
}

/* The grid of up to SCENE_CELLS cells closest to square cells: minimizes
 * |cell width - cell height| = |w * rows - h * cols| / (cols * rows),
 * compared by cross-multiplying. */
static void pick_grid(uint32_t w, uint32_t h, uint32_t *cols, uint32_t *rows)
{
    uint64_t best_num = 0, best_den = 0;
    *cols = *rows = 1;
    for (uint32_t r = 1; r <= SCENE_CELLS && r <= h; r++) {
        uint32_t c = SCENE_CELLS / r;
        uint64_t a = (uint64_t)w * r, b = (uint64_t)h * c;
        uint64_t num = a > b ? a - b : b - a, den = (uint64_t)c * r;
        if (c > w)
            continue;
        if (!best_den || num * best_den < best_num * den) {
            best_num = num;
            best_den = den;
            *cols = c;
            *rows = r;
        }
    }
}

void scene_acc_begin(SceneAcc *a, SceneSig *out, uint32_t w, uint32_t h)
{
    if (a->w != w || a->h != h || !a->cols) {
        uint32_t rows;
        pick_grid(w, h, &a->cols, &rows);
        for (uint32_t c = 0; c <= a->cols; c++)
            a->x0[c] = c * w / a->cols;
        for (uint32_t r = 0; r <= rows; r++)
            a->y0[r] = r * h / rows;
        a->ystep = step_for(h, rows, MAX_YSTEP);
        a->xstep = step_for(w, a->cols, MAX_XSTEP);
        a->w = w;
        a->h = h;
    }
    a->next_y = 0;
    a->cell_row = 0;
    a->out = out;
    out->w = w;
    out->h = h;
    out->cols = a->cols;
    for (int i = 0; i < SCENE_CELLS; i++)
        out->hash[i] = FNV_OFFSET;
}

void scene_acc_row(SceneAcc *a, uint32_t y, const uint32_t *abgr)
{
    uint32_t *hash;
    if (y != a->next_y || y >= a->h)
        return;
    a->next_y += a->ystep;
    while (y >= a->y0[a->cell_row + 1])
        a->cell_row++;
    hash = &a->out->hash[a->cell_row * a->cols];
    for (uint32_t c = 0; c < a->cols; c++) {
        uint32_t h = hash[c];
        for (uint32_t x = a->x0[c]; x < a->x0[c + 1]; x += a->xstep)
            h = (h ^ (abgr[x] & 0x00FFFFFFu)) * FNV_PRIME; /* alpha ignored */
        hash[c] = h;
    }
}

static int same_grid(const SceneSig *a, const SceneSig *b)
{
    return a->w && a->w == b->w && a->h == b->h;
}

static int in_history(const SceneCell *c, uint32_t hash)
{
    for (int k = 0; k < c->n; k++)
        if (c->hist[k] == hash)
            return 1;
    return 0;
}

/* Records the cell's state. A change extends its run of changes (or starts
 * a new one after a pause longer than SCENE_ANIM_GAP_US) and counts the
 * revisits in a row: changes back to a state among the last few. A change
 * the player caused (by_input) starts a new run, so it never makes a cell
 * animated; a masked icon keeps cycling through held input (its mask ends
 * at an input edge: unmask_on_edge). */
static void track(SceneCell *c, uint32_t hash, int64_t now, int by_input)
{
    int revisit;
    if (!c->seen) {
        c->seen = 1;
        c->last = c->hist[0] = hash;
        c->n = 1;
        c->next = 1 % SCENE_HISTORY;
        c->revisits = 0;
        c->run_start_us = c->moved_us = now - SCENE_ANIM_GAP_US - 1; /* no run */
        return;
    }
    if (hash == c->last)
        return;
    by_input &= !c->masked;
    if (by_input || now - c->moved_us > SCENE_ANIM_GAP_US) {
        c->run_start_us = now;
        c->revisits = 0;
    }
    revisit = in_history(c, hash);
    c->revisits = revisit && !by_input ? c->revisits + 1 : 0;
    if (!revisit) {
        c->hist[c->next] = hash;
        c->next = (c->next + 1) % SCENE_HISTORY;
        if (c->n < SCENE_HISTORY)
            c->n++;
    }
    c->moved_us = now;
    c->last = hash;
}

static int running(const SceneCell *c, int64_t now)
{
    return c->seen && now - c->moved_us <= SCENE_ANIM_GAP_US;
}

static int cycling(const SceneCell *c, int64_t now)
{
    return running(c, now) && c->revisits >= SCENE_ANIM_REVISITS;
}

/* Cycling for SCENE_ANIM_SPAN_US, or for SCENE_WARM_REVISITS revisits in
 * a row while warm: the "next" icon of the next line, wherever it ends.
 * More revisits than SCENE_ANIM_REVISITS: a cell of text being typed can
 * sample like its empty state twice in a row. A masked cell stays animated
 * while it cycles; a state not among its last SCENE_HISTORY is new
 * content, which ends the mask. */
static int animated(const SceneTracker *t, const SceneCell *c, int64_t now)
{
    int warm = t->masked_us && now - t->masked_us <= SCENE_ANIM_WARM_US;
    return cycling(c, now) && (c->moved_us - c->run_start_us >= SCENE_ANIM_SPAN_US ||
                               (warm && c->revisits >= SCENE_WARM_REVISITS));
}

void scene_reset(SceneTracker *t)
{
    t->ref.w = 0;
    t->stable = 0;
    t->masked_us = 0; /* not warm */
    for (int i = 0; i < SCENE_CELLS; i++) {
        t->cell[i].seen = 0;
        t->cell[i].masked = 0;
    }
}

/* Do the cells with keep[i] set fit in SCENE_ICON x SCENE_ICON cells?
 * A row of text does not. */
static int icon_sized(const uint8_t *keep, uint32_t cols)
{
    uint32_t c0 = cols, c1 = 0, r0 = SCENE_CELLS, r1 = 0, c = 0, r = 0;
    int n = 0;
    for (int i = 0; i < SCENE_CELLS; i++) {
        if (keep[i]) {
            n++;
            c0 = c < c0 ? c : c0;
            c1 = c > c1 ? c : c1;
            r0 = r < r0 ? r : r0;
            r1 = r > r1 ? r : r1;
        }
        if (++c == cols) { /* no division: the worker shares the file's rule */
            c = 0;
            r++;
        }
    }
    return n && c1 - c0 < SCENE_ICON && r1 - r0 < SCENE_ICON;
}

/* Is a cell next to the cell at row r, column c (8 neighbours) an icon in
 * mask? */
static int next_to_icon(const uint8_t *mask, uint32_t r, uint32_t c, uint32_t rows, uint32_t cols)
{
    for (uint32_t y = r ? r - 1 : 0; y <= r + 1 && y < rows; y++)
        for (uint32_t x = c ? c - 1 : 0; x <= c + 1 && x < cols; x++)
            if (mask[y * cols + x] == SCENE_MASK_ICON)
                return 1;
    return 0;
}

/* Masks the animated cells, unless there are more than SCENE_ANIM_MAX (an
 * animated background: masking it would hide the text over it). A cell
 * next to an animated one that went back to a recent state is its edge
 * (reached by the icon's largest frames only, too rarely to count as
 * cycling; an icon cell paused on a recent state): masked while the icon
 * is, until it shows something new. At most SCENE_ANIM_MAX edges, besides.
 * Returns 1 when a mask ended: the cell stopped or showed something new,
 * and whatever it hid is a change. */
static int mask_animated(SceneTracker *t, int64_t now, uint32_t cols)
{
    uint8_t mask[SCENE_CELLS];
    uint32_t rows = 0, r = 0, c = 0;
    int n = 0, edges = 0, ended = 0;
    for (uint32_t k = 0; k + cols <= SCENE_CELLS; k += cols) /* no division */
        rows++;
    for (int i = 0; i < SCENE_CELLS; i++) {
        mask[i] = animated(t, &t->cell[i], now) ? SCENE_MASK_ICON : 0;
        n += mask[i] != 0;
    }
    if (n > SCENE_ANIM_MAX)
        n = 0;
    for (int i = 0; i < SCENE_CELLS && n && r < rows; i++) {
        const SceneCell *cell = &t->cell[i];
        /* revisits > 0: the last change was a recent state, not the player's */
        if (!mask[i] && cell->revisits > 0 && (cell->masked || cell->moved_us == now) &&
            next_to_icon(mask, r, c, rows, cols)) {
            mask[i] = SCENE_MASK_EDGE;
            edges++;
        }
        if (++c == cols) {
            c = 0;
            r++;
        }
    }
    for (int i = 0; i < SCENE_CELLS; i++) {
        SceneCell *cell = &t->cell[i];
        int m = !n || (mask[i] == SCENE_MASK_EDGE && edges > SCENE_ANIM_MAX) ? 0 : mask[i];
        ended |= cell->masked && !m;
        cell->masked = m;
    }
    t->masked = n;
    if (n)
        t->masked_us = now;
    return ended;
}

/* Settling waits while icons may be starting to cycle (not masked yet):
 * cells cycling, or, when the last change was icon-sized, cells that went
 * back to a recent state once (a blink's first "off"). A line replaced by
 * a shorter one also goes back to empty cells, but that change is a row. */
static int waiting_for_animation(const SceneTracker *t, int64_t now)
{
    int n = 0;
    for (int i = 0; i < SCENE_CELLS; i++) {
        const SceneCell *c = &t->cell[i];
        n += running(c, now) && !c->masked &&
             (c->revisits >= SCENE_ANIM_REVISITS || (t->icon_change && c->revisits > 0));
    }
    return n > 0 && n <= SCENE_ANIM_MAX;
}

/* A new input edge (a press, a release, a touch) ends every mask: what
 * the player did may have changed what an icon-like cell shows (a short
 * menu scrolled by a held button, then tapped back). The icons are masked
 * again once they cycle (warm). Returns 1 when a mask ended. */
static int unmask_on_edge(SceneTracker *t, int64_t now, int64_t edge_us)
{
    int ended = 0;
    if (edge_us == t->edge_us)
        return 0;
    t->edge_us = edge_us;
    for (int i = 0; i < SCENE_CELLS; i++) {
        SceneCell *c = &t->cell[i];
        if (c->masked) {
            c->masked = 0;
            c->revisits = 0;
            c->run_start_us = now;
            ended = 1;
        }
    }
    return ended;
}

int scene_update(SceneTracker *t, const SceneSig *sig, int64_t now, int64_t input_us, int64_t edge_us)
{
    int changed = !same_grid(&t->ref, sig), by_input = now - input_us <= SCENE_INPUT_US;
    if (changed)
        scene_reset(t);
    for (int i = 0; i < SCENE_CELLS; i++)
        track(&t->cell[i], sig->hash[i], now, by_input);
    changed |= unmask_on_edge(t, now, edge_us);
    changed |= mask_animated(t, now, sig->cols);
    for (int i = 0; i < SCENE_CELLS && !changed; i++)
        changed = sig->hash[i] != t->ref.hash[i] && !t->cell[i].masked;
    if (changed) {
        uint8_t diff[SCENE_CELLS];
        for (int i = 0; i < SCENE_CELLS; i++)
            diff[i] = (uint8_t)(t->ref.w && sig->hash[i] != t->ref.hash[i]);
        t->icon_change = icon_sized(diff, sig->cols);
        if (t->stable || !t->ref.w)
            t->unsettled_us = now;
        t->ref = *sig;
        if (++t->id == 0) /* 0 = unknown */
            t->id = 1;
        t->stable = 0;
        t->changed_us = now;
        return 0;
    }
    if (t->stable || now - t->changed_us < SCENE_STABLE_US)
        return 0;
    if (waiting_for_animation(t, now))
        return 0;
    t->stable = 1;
    return 1;
}

uint32_t scene_match(const SceneTracker *t, const SceneSig *sig)
{
    if (!same_grid(&t->ref, sig))
        return 0;
    for (int i = 0; i < SCENE_CELLS; i++)
        if (sig->hash[i] != t->ref.hash[i] && !t->cell[i].masked)
            return 0;
    return t->id;
}

int64_t scene_unsettled_us(const SceneTracker *t, int64_t now)
{
    return t->stable || !t->ref.w ? 0 : now - t->unsettled_us;
}
