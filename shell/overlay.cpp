/* Full-screen ScePaf overlay: header (OCR text, selected word highlighted),
 * separator, the selected word's definition, and the touch region selector.
 * Runs entirely on the paf main thread via MainThreadCallList.
 *
 * Header and body are ui::Text widgets in a fixed-size box inside a
 * ui::ScrollView (as in GrapheneCt's NetStream, the text takes the box's
 * width and grows to fit). The view only clips: its content is the box, which
 * never changes size, so the view never re-centres or eases the text (it did
 * both when the Text was its direct child). Sizes and colors are set per
 * range with SetStyleAttribute (in pixels), and the Text reports where text
 * is (GetCharInfo/GetLineInfo), so nothing is estimated. Moving the
 * selection only recolors two ranges of the header. Each Text is placed
 * inside its box every frame from its laid-out size: paf positions are
 * centre-based and y-up, so the text's top sits at the box's top when
 * pos.y = (box height - text height) / 2, plus the scroll offset. ScrollView
 * DoSnap and Text GetNextDownHit are not used: pressing ▼ with them crashed
 * the console.
 *
 * Touch: SceTouch routes the front panel by region, and while a game is in
 * front SceShell's regions are inactive. The region selector activates
 * SceShell's default touch region while it is open (as SceShell's own menus
 * over a game must) and reads the touches through paf's input device. */
#include <psp2/kernel/modulemgr.h>
#include <limits>
#include <paf.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

extern "C" {
#include "../core/styled.h"
#include "shell.h"
}

using namespace paf;

/* The paf text info classes have virtual destructors, whose deleting
 * variants reference sized delete (no C++ runtime is linked; these objects
 * live on the stack). */
void operator delete(void *p, unsigned int)
{
    sce_paf_free(p);
}

#define SCREEN_W 960
#define SCREEN_H 544
/* Layout (paf coordinates: origin at the screen centre, y up). The header
 * fits its text, 1..HEADER_MAX_LINES lines; the separator and the body follow
 * it, and the body keeps its bottom (shell/rco/vitajpoverlay.xml holds the
 * initial sizes: header 128, body 320). */
#define BOX_W 920.0f
#define HEADER_TOP 260.0f
#define HEADER_MAX_LINES 3
#define HEADER_SEP_GAP 19.0f /* header bottom -> separator */
#define SEP_BODY_GAP 17.0f   /* separator -> body top */
#define BODY_BOTTOM -224.0f
#define HEADER_H_INITIAL 128.0f
#define BODY_H_INITIAL 320.0f
#define HEADER_UNITS 4096 /* UTF-16 units */
#define BODY_UNITS 4096
#define MAX_SPANS 96
#define STICK_DEADZONE 40
#define STICK_SCROLL_PX 0.12f /* per frame per unit of stick deflection */
#define LAYOUT_RETRY_FRAMES 6 /* layout may land a frame or two after SetString */
#define CLEAR_HOLD_US 1000000
#define MAX_RANGES 512 /* entries whose header highlight range is cached */

enum { MODE_OVERLAY = 0, MODE_REGION = 1 };

static Plugin *s_plugin;
static ui::Scene *s_page;
/* A Text in a fixed-size box inside a clipping ScrollView (see the top). */
struct Pane {
    ui::Widget *view, *box;
    ui::Text *text;
    float h;       /* box height */
    float scroll;  /* content y at the box top */
    float applied; /* last pos.y given to the Text; NOT_PLACED = place afresh */
};
#define NOT_PLACED (-1e9f)
static Pane s_header = {NULL, NULL, NULL, HEADER_H_INITIAL, 0.0f, NOT_PLACED};
static Pane s_body = {NULL, NULL, NULL, BODY_H_INITIAL, 0.0f, NOT_PLACED};
static ui::Widget *s_separator, *s_band_mid;
static ui::RichText *s_hint, *s_region_hint;
static ui::Widget *s_panel, *s_region_layer, *s_region_rect;

static unsigned s_seen_version;
static int s_selected;
/* Copied from the view under its lock, so nothing outside render() reads
 * the result memory (the control thread frees it when a game exits) and no
 * paf call runs while the lock is held. */
static int s_n_entries;
static uint32_t s_rng_start[MAX_RANGES], s_rng_len[MAX_RANGES]; /* len 0 = no highlight */

/* logged when the overlay closes */
static int64_t s_frame_max_us;
static int s_frames, s_moves, s_stick_max; /* largest stick deflection from centre */
static int s_mode;
static uint32_t s_prev_buttons;
static int s_prev_touch;

/* header state */
static uint32_t s_hl_start, s_hl_len; /* highlighted range now drawn */
static int s_follow_frames;           /* frames left to scroll the header to the word */

/* region selection (screen pixels) */
static int s_touch_region_on; /* SceShell's default front touch region activated by us */
static int s_drag, s_has_rect;
static int s_x0, s_y0, s_x1, s_y1;
static int64_t s_square_down_us;
static int s_square_clear_armed;

static uint8_t s_ui_mem[96 * 1024];
static VjoArena s_ui;

static void set_rich(ui::RichText *w, const char *s)
{
    if (w)
        w->SetCText(s, sce_paf_strlen(s));
}

/* math::v2/v4 keep their floats private. */
static math::v2 v2f(float x, float y)
{
    math::v2 v;
    float f[2] = {x, y};
    sceClibMemcpy(&v, f, sizeof(f));
    return v;
}

static float v2_get(const math::v2 &v, int k)
{
    float f[2];
    sceClibMemcpy(f, &v, sizeof(f));
    return f[k];
}

static math::v4 rgba(uint32_t rgb)
{
    return math::v4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
}

static void text_set(ui::Text *w, const VjoStyled *st)
{
    if (!w)
        return;
    w->SetString(paf::wstring((const wchar_t *)st->text, st->len));
    for (int i = 0; i < st->n_spans; i++) {
        const VjoSpan *sp = &st->spans[i];
        /* Point takes a v2 (glyph width, height), as in GrapheneCt's NetStream */
        w->SetStyleAttribute(graph::TextStyleAttribute_Point, sp->start, sp->len,
                             v2f((float)sp->px, (float)sp->px));
        w->SetStyleAttribute(graph::TextStyleAttribute_Color, sp->start, sp->len, rgba(sp->rgb));
    }
}

/* Laid-out size of a Text's content; 0 if not laid out yet. */
static float text_bound(ui::Text *w, int k)
{
    math::v2 b;
    if (!w || w->GetBounds(b) < 0)
        return 0.0f;
    return v2_get(b, k);
}

static float text_height(ui::Text *w)
{
    return text_bound(w, 1);
}

/* Places the pane's Text so that content y = scroll is at the box's top.
 * The widget takes the box's width (adjust 1) with the text left-aligned in
 * it, so x stays 0; its height is the laid-out height (adjust 2) and paf
 * positions are centre-based, so y comes from that height. Runs every frame
 * (cheap) and only calls SetPos on a change. */
static void pane_place(Pane *p)
{
    float h = text_height(p->text), y;
    if (!p->text || h <= 0.0f)
        return;
    y = (p->h - h) / 2.0f + p->scroll;
    if (y != p->applied) {
        p->applied = y;
        p->text->SetPos(0.0f, y, 0.0f);
    }
}

/* Scrolls to content y, kept within the laid-out text. */
static void pane_scroll_to(Pane *p, float y)
{
    float max = text_height(p->text) - p->h;
    if (y > max)
        y = max;
    if (y < 0.0f)
        y = 0.0f;
    p->scroll = y;
}

/* New text or a new box: scrolled to the top and placed afresh. */
static void pane_reset(Pane *p)
{
    p->scroll = 0.0f;
    p->applied = NOT_PLACED;
}

/* Sizes the view and its box (same size: the view only clips) with the
 * given top edge; the Text is placed afresh in the new box. */
static void pane_size(Pane *p, float h, float top)
{
    p->h = h;
    if (p->view) {
        p->view->SetSize(BOX_W, h, 0.0f);
        p->view->SetPos(0.0f, top - h / 2.0f, 0.0f);
    }
    if (p->box)
        p->box->SetSize(BOX_W, h, 0.0f);
    p->applied = NOT_PLACED;
}

static void font_px(int *ja, int *en)
{
    vjo_view_lock();
    *ja = vjo_font_px(g_view.font_size_ja);
    *en = vjo_font_px(g_view.font_size_en);
    vjo_view_unlock();
}

/* Recolors the highlighted word: the old range back to white, the new one
 * yellow. No relayout. */
static void header_highlight(void)
{
    if (!s_header.text)
        return;
    if (s_hl_len)
        s_header.text->SetStyleAttribute(graph::TextStyleAttribute_Color, s_hl_start, s_hl_len, rgba(VJO_RGB_TEXT));
    s_hl_len = 0;
    if (s_selected < s_n_entries && s_selected < MAX_RANGES && s_rng_len[s_selected]) {
        s_hl_start = s_rng_start[s_selected];
        s_hl_len = s_rng_len[s_selected];
        s_header.text->SetStyleAttribute(graph::TextStyleAttribute_Color, s_hl_start, s_hl_len, rgba(VJO_RGB_HIGHLIGHT));
    }
    s_follow_frames = LAYOUT_RETRY_FRAMES;
}

/* Scrolls the header just enough to show the highlighted word's whole line.
 * The line comes from the Text (GetCharInfo); the line height is the laid
 * out height over the line count (the line frame's fields turned out not to
 * be x, y, w, h: on the device line 0 was "0, 0, 0, 746" for a 746 px line).
 * Returns 1 when done. */
static int header_follow(void)
{
    graph::TextLayoutCharInfo ci;
    uint32_t lines = 0;
    float height, lh, top, scroll = s_header.scroll;
    ui::Text *t = s_header.text;
    if (!t || !s_hl_len)
        return 1;
    if (t->GetCharInfo(s_hl_start, ci) < 0 || t->GetLineCount(lines) < 0 || !lines)
        return 0; /* not laid out yet */
    height = text_height(t);
    if (height <= 0.0f)
        return 0;
    lh = height / (float)lines;
    top = (float)ci.line * lh;
    if (top + lh > scroll + s_header.h)
        scroll = top + lh - s_header.h;
    if (top < scroll)
        scroll = top;
    pane_scroll_to(&s_header, scroll);
    return 1;
}

/* new_content: a new result (header text rebuilt and scrolled to the top);
 * otherwise only the selection moved. The text is built from the view under
 * its lock; the paf calls happen after it is released. */
static void render(int new_content)
{
    VjoStyled hdr, body;
    int ja, en, have_hdr = 0, have_body = 0;
    const VjoEntryList *l;

    font_px(&ja, &en);
    vjo_view_lock();
    l = g_view.list;
    s_n_entries = l ? l->n_entries : 0;
    if (s_selected >= s_n_entries)
        s_selected = s_n_entries ? s_n_entries - 1 : 0;
    vjo_arena_reset(&s_ui);

    if (new_content) {
        if (vjo_styled_init(&hdr, &s_ui, HEADER_UNITS, 1) == 0) {
            vjo_styled_header(&hdr, l, ja);
            have_hdr = 1;
        }
        for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++)
            if (!vjo_entry_header_range(l, i, &s_rng_start[i], &s_rng_len[i]))
                s_rng_len[i] = 0;
    }

    /* body: status, the selected entry (or "no entries"), config warnings */
    if (vjo_styled_init(&body, &s_ui, BODY_UNITS, MAX_SPANS) == 0) {
        have_body = 1;
        if (g_view.status[0])
            vjo_styled_puts(&body, g_view.status, g_view.status_is_error ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        if (s_n_entries) {
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            vjo_styled_entry(&body, l, s_selected, ja, en);
        } else if (l && !g_view.status[0]) {
            vjo_styled_puts(&body, "No dictionary entries for this text.", VJO_RGB_DIM, en);
        }
        for (int i = 0; i < g_view.n_warnings; i++) {
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, "config.ini: ", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, g_view.warnings[i], VJO_RGB_ERROR, en);
        }
    }
    vjo_view_unlock();

    if (new_content) {
        if (have_hdr)
            text_set(s_header.text, &hdr);
        pane_reset(&s_header);
        s_hl_len = 0;
    }
    header_highlight();
    if (have_body)
        text_set(s_body.text, &body);
    pane_reset(&s_body);
}

static void layout_apply(float header_h)
{
    float sep = HEADER_TOP - header_h - HEADER_SEP_GAP;
    float body_top = sep - SEP_BODY_GAP;
    pane_size(&s_header, header_h, HEADER_TOP);
    pane_size(&s_body, body_top - BODY_BOTTOM, body_top);
    /* the mid band covers the gap between the boxes, centred on the separator */
    if (s_separator)
        s_separator->SetPos(0.0f, sep, 0.0f);
    if (s_band_mid)
        s_band_mid->SetPos(0.0f, sep, 0.0f);
    if (s_hl_len)
        s_follow_frames = LAYOUT_RETRY_FRAMES;
}

/* Fits the header to its text: 1..HEADER_MAX_LINES laid-out lines. Cheap
 * (two queries); applies only on a change. */
static void header_fit(void)
{
    uint32_t lines = 0;
    float h = text_height(s_header.text), want;
    if (!s_header.text || h <= 0.0f || s_header.text->GetLineCount(lines) < 0 || !lines)
        return;
    want = h / (float)lines * (float)(lines < HEADER_MAX_LINES ? lines : HEADER_MAX_LINES);
    if (want > HEADER_H_INITIAL)
        want = HEADER_H_INITIAL;
    want = (float)(int)(want + 0.5f);
    if (want != s_header.h) {
        layout_apply(want);
        pane_scroll_to(&s_body, s_body.scroll); /* keep the body scroll in range */
    }
}

static void show_region_rect(void)
{
    int x = s_x0 < s_x1 ? s_x0 : s_x1, y = s_y0 < s_y1 ? s_y0 : s_y1;
    int w = s_x0 < s_x1 ? s_x1 - s_x0 : s_x0 - s_x1, hgt = s_y0 < s_y1 ? s_y1 - s_y0 : s_y0 - s_y1;
    if (!s_region_rect)
        return;
    if (w < 2 || hgt < 2) {
        s_region_rect->Hide();
        return;
    }
    /* paf layout: origin at the screen centre, y up */
    s_region_rect->SetSize((float)w, (float)hgt, 0.0f);
    s_region_rect->SetPos((float)(x + w / 2 - SCREEN_W / 2), (float)(SCREEN_H / 2 - (y + hgt / 2)), 0.0f);
    s_region_rect->Show();
}

/* Front touches go to SceShell while on (and not to the game). */
static void touch_region(int on)
{
    int rc;
    if (on == s_touch_region_on)
        return;
    rc = inputdevice::touchscreen::ActivateDefaultRegion(inputdevice::touchscreen::PORT_FRONT, on != 0);
    s_touch_region_on = on;
    vjo_log("touch region %s: 0x%X", on ? "on" : "off", rc);
}

static void enter_region_mode(void)
{
    s_mode = MODE_REGION;
    touch_region(1);
    s_drag = 0;
    s_has_rect = 0;
    s_square_down_us = sceKernelGetProcessTimeWide();
    s_square_clear_armed = 1;
    if (s_panel)
        s_panel->Hide();
    if (s_region_layer)
        s_region_layer->Show();
    if (s_region_rect)
        s_region_rect->Hide();
    set_rich(s_region_hint, "<font color=\"#ffffff\">Drag on the screen to select the text area · "
                            "× confirm · ○ cancel · hold □ for full screen</font>");
}

static void leave_region_mode(void)
{
    touch_region(0);
    s_mode = MODE_OVERLAY;
    if (s_region_layer)
        s_region_layer->Hide();
    if (s_panel)
        s_panel->Show();
}

static void open_page(void)
{
    Plugin::PageOpenParam param;
    param.overwrite_draw_priority = 6; /* above the game, like PSVshellPlus' HUD */
    s_page = s_plugin->PageOpen("vjo_page_overlay", param);
    if (!s_page) {
        vjo_log("overlay page failed to open");
        return;
    }
    s_panel = s_page->FindChild("vjo_panel");
    s_header.text = (ui::Text *)s_page->FindChild("vjo_header");
    s_body.text = (ui::Text *)s_page->FindChild("vjo_body");
    s_header.view = s_page->FindChild("vjo_header_view");
    s_header.box = s_page->FindChild("vjo_header_box");
    s_body.view = s_page->FindChild("vjo_body_view");
    s_body.box = s_page->FindChild("vjo_body_box");
    s_separator = s_page->FindChild("vjo_separator");
    s_band_mid = s_page->FindChild("vjo_band_mid");
    s_header.h = HEADER_H_INITIAL; /* the page opens with the RCO layout */
    s_body.h = BODY_H_INITIAL;
    pane_reset(&s_header);
    pane_reset(&s_body);
    s_hint = (ui::RichText *)s_page->FindChild("vjo_hint");
    {
        /* The views get these empty scrollbars instead of their default
         * ones, which flashed whenever the text moved. */
        static const char *const sbars[] = {"vjo_header_sbar_v", "vjo_header_sbar_h", "vjo_body_sbar_v",
                                            "vjo_body_sbar_h"};
        for (unsigned i = 0; i < sizeof(sbars) / sizeof(sbars[0]); i++) {
            ui::Widget *w = s_page->FindChild(sbars[i]);
            if (w)
                w->Hide();
        }
    }
    s_region_layer = s_page->FindChild("vjo_region_layer");
    s_region_rect = s_page->FindChild("vjo_region_rect");
    s_region_hint = (ui::RichText *)s_page->FindChild("vjo_region_hint");
    if (s_region_layer)
        s_region_layer->Hide();
    /* Wrapped text with Japanese line-breaking rules. */
    {
        ui::Text *texts[2] = {s_header.text, s_body.text};
        for (int i = 0; i < 2; i++) {
            if (!texts[i])
                continue;
            texts[i]->SetLayoutAttribute(graph::TextLayoutAttribute_WordWrap, true);
            texts[i]->SetLayoutAttribute(graph::TextLayoutAttribute_Kinsoku, true);
        }
    }
    set_rich(s_hint, "<font color=\"#8a94a6\">◀ ▶ ▲ ▼ word · stick scroll · □ region · ○ close</font>");
    s_mode = MODE_OVERLAY;
    s_selected = 0;
    s_hl_len = 0;
    s_n_entries = 0;
    s_frames = s_moves = 0;
    s_frame_max_us = 0;
    s_stick_max = 0;
    s_prev_buttons = ~0u; /* ignore buttons already held when opening */
    s_prev_touch = 1;
}

static void close_page(void)
{
    Plugin::PageCloseParam param;
    if (s_mode == MODE_REGION)
        leave_region_mode();
    param.fade = false;
    s_plugin->PageClose("vjo_page_overlay", param);
    s_page = NULL;
    s_panel = s_region_layer = s_region_rect = NULL;
    vjo_log("overlay closed: %d frames, %d word moves, stick %d, slowest frame %d ms", s_frames, s_moves,
            s_stick_max, (int)(s_frame_max_us / 1000));
    s_header.view = s_header.box = s_body.view = s_body.box = NULL;
    s_header.text = s_body.text = NULL;
    s_separator = s_band_mid = NULL;
    s_hint = s_region_hint = NULL;
    s_n_entries = 0;
}

/* ▲/▼: the word on the nearest line above/below that has one, closest in x
 * to the selected word; from the header's own layout (GetCharInfo). */
static int entry_vertical(int up)
{
    graph::TextLayoutCharInfo ci;
    uint32_t cur_line;
    float cur_x;
    int best = s_selected;
    uint32_t best_line = 0;
    float best_dx = 0.0f;
    if (!s_header.text || !s_hl_len || s_header.text->GetCharInfo(s_hl_start, ci) < 0)
        return s_selected;
    cur_line = ci.line;
    cur_x = ci.x;
    for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++) {
        float dx;
        if (i == s_selected || !s_rng_len[i] || s_header.text->GetCharInfo(s_rng_start[i], ci) < 0)
            continue;
        if (up ? ci.line >= cur_line : ci.line <= cur_line)
            continue;
        dx = ci.x > cur_x ? ci.x - cur_x : cur_x - ci.x;
        /* nearest line first, then nearest x */
        if (best == s_selected || (up ? ci.line > best_line : ci.line < best_line) ||
            (ci.line == best_line && dx < best_dx)) {
            best = i;
            best_line = ci.line;
            best_dx = dx;
        }
    }
    return best;
}

static void input_overlay(const VjoInput *in, uint32_t pressed)
{
    int n = s_n_entries;
    int next = s_selected;
    int dy;
    if ((pressed & SCE_CTRL_RIGHT) && s_selected + 1 < n)
        next = s_selected + 1;
    if ((pressed & SCE_CTRL_LEFT) && s_selected > 0)
        next = s_selected - 1;
    if (pressed & SCE_CTRL_UP)
        next = entry_vertical(1);
    if (pressed & SCE_CTRL_DOWN)
        next = entry_vertical(0);
    if (next != s_selected) {
        s_selected = next;
        s_moves++;
        render(0);
    }
    /* either stick scrolls a long definition */
    dy = (int)in->ly - 128;
    if (dy > -STICK_DEADZONE && dy < STICK_DEADZONE)
        dy = (int)in->ry - 128;
    if ((dy < 0 ? -dy : dy) > s_stick_max)
        s_stick_max = dy < 0 ? -dy : dy;
    if (dy <= -STICK_DEADZONE || dy >= STICK_DEADZONE)
        pane_scroll_to(&s_body, s_body.scroll + (float)dy * STICK_SCROLL_PX);
    if (pressed & SCE_CTRL_CIRCLE)
        vjo_post_command(VJO_CMD_CLOSED, NULL);
    if (pressed & SCE_CTRL_SQUARE)
        enter_region_mode();
}

static void input_region(uint32_t held, uint32_t pressed)
{
    const inputdevice::touchscreen::Data *pd = NULL;
    int touching = 0, x = 0, y = 0;
    /* paf: screen pixels from the top left */
    inputdevice::touchscreen::GetData(inputdevice::touchscreen::PORT_FRONT, 0, &pd);
    if (pd && pd->state != inputdevice::touchscreen::Data::STATE_NONE &&
        pd->state != inputdevice::touchscreen::Data::STATE_RELEASE) {
        float fx = 0.0f, fy = 0.0f;
        int rc = inputdevice::touchscreen::GetPointFromTopleft(inputdevice::touchscreen::PORT_FRONT, 0, fx, fy);
        touching = 1;
        x = rc >= 0 ? (int)fx : pd->x;
        y = rc >= 0 ? (int)fy : pd->y;
    }
    if (touching) {
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x >= SCREEN_W)
            x = SCREEN_W - 1;
        if (y >= SCREEN_H)
            y = SCREEN_H - 1;
        if (!s_prev_touch || !s_drag) {
            s_drag = 1;
            s_x0 = s_x1 = x;
            s_y0 = s_y1 = y;
            vjo_log("region drag from %d,%d", x, y);
        } else {
            s_x1 = x;
            s_y1 = y;
        }
        s_has_rect = (s_x0 != s_x1) && (s_y0 != s_y1);
        show_region_rect();
    } else {
        if (s_drag)
            vjo_log("region drag to %d,%d", s_x1, s_y1);
        s_drag = 0;
    }
    s_prev_touch = touching;

    /* hold □ (from entering region mode) for 1 s -> full screen */
    if (!(held & SCE_CTRL_SQUARE))
        s_square_clear_armed = 0;
    if (pressed & SCE_CTRL_SQUARE) {
        s_square_clear_armed = 1;
        s_square_down_us = sceKernelGetProcessTimeWide();
    }
    if (s_square_clear_armed && sceKernelGetProcessTimeWide() - s_square_down_us >= CLEAR_HOLD_US) {
        s_square_clear_armed = 0;
        leave_region_mode();
        vjo_post_command(VJO_CMD_CLEAR_REGION, NULL);
        return;
    }
    if ((pressed & SCE_CTRL_CROSS) && s_has_rect) {
        VjoRect r;
        int x = s_x0 < s_x1 ? s_x0 : s_x1, y = s_y0 < s_y1 ? s_y0 : s_y1;
        int w = s_x0 < s_x1 ? s_x1 - s_x0 : s_x0 - s_x1, h = s_y0 < s_y1 ? s_y1 - s_y0 : s_y0 - s_y1;
        r.x = (uint16_t)(x * 65536 / SCREEN_W);
        r.y = (uint16_t)(y * 65536 / SCREEN_H);
        r.w = (uint16_t)((w * 65536 / SCREEN_W) > 65535 ? 65535 : w * 65536 / SCREEN_W);
        r.h = (uint16_t)((h * 65536 / SCREEN_H) > 65535 ? 65535 : h * 65536 / SCREEN_H);
        leave_region_mode();
        vjo_post_command(VJO_CMD_SET_REGION, &r);
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE)
        leave_region_mode();
}

static void frame_body(unsigned version);

static void frame(void *arg)
{
    int want_open;
    unsigned version;
    (void)arg;

    /* Runs on SceShell's paf main thread every frame: while closed, skip the
     * lock unless an open is published (a store under the lock; a stale read
     * only delays the open by a frame). */
    if (!s_page && !__atomic_load_n(&g_view.open, __ATOMIC_ACQUIRE))
        return;
    vjo_view_lock();
    want_open = g_view.open;
    version = g_view.version;
    vjo_view_unlock();

    if (want_open && !s_page) {
        open_page();
        s_seen_version = version - 1;
    } else if (!want_open && s_page) {
        close_page();
        return;
    }
    if (!s_page)
        return;
    {
        int64_t t0 = sceKernelGetProcessTimeWide();
        frame_body(version);
        t0 = sceKernelGetProcessTimeWide() - t0;
        if (t0 > s_frame_max_us)
            s_frame_max_us = t0;
        s_frames++;
    }
}

/* One frame with the overlay page open. */
static void frame_body(unsigned version)
{
    if (version != s_seen_version) {
        s_seen_version = version;
        render(1);
    }
    if (s_follow_frames > 0 && (header_follow() || --s_follow_frames == 0))
        s_follow_frames = 0;
    header_fit();
    pane_place(&s_header);
    pane_place(&s_body);

    {
        VjoInput in;
        uint32_t held, pressed;
        sceClibMemset(&in, 0, sizeof(in));
        in.ly = in.ry = 128;
        vjoPollInput(&in);
        held = in.buttons;
        pressed = held & ~s_prev_buttons;
        s_prev_buttons = held;
        if (s_mode == MODE_OVERLAY)
            input_overlay(&in, pressed);
        else
            input_region(held, pressed);
    }
}

extern "C" void vjo_overlay_init(void *plugin)
{
    s_plugin = (Plugin *)plugin;
    vjo_arena_init(&s_ui, s_ui_mem, sizeof(s_ui_mem));
    common::MainThreadCallList::Register(frame, NULL);
}
