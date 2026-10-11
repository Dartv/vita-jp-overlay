/* Subtitles strip: the recognized sentence at the top of the screen over
 * the running game, while the overlay is closed (a page of its own,
 * vjo_page_strip). It takes no input. It grows with its text; text that
 * would pass the screen's bottom is cut with "…". A status or an error
 * shows in the strip too, and the system's busy spinner in its corner while a new
 * sentence is being recognized.
 *
 * A sentence keeps room above each line for furigana, so the readings
 * appear in place once its words are looked up: half-size texts (from the
 * template vjo_strip_ruby_tpl, made as needed) centred over their kanji by
 * the sentence's own layout (GetCharInfo). The room is the Text's LinePitch,
 * which on the device adds that much between lines (0 by default). */
#include "paf_ui.h"

extern "C" {
#include "../core/utf.h"
}

using namespace paf;

#define STRIP_PAD 8.0f       /* above and below the text */
#define STRIP_BOX_W 872.0f     /* the text; the right margin holds the spinner */
#define STRIP_BUSY_INSET 22.0f /* the spinner's centre from the strip's bottom right */
#define STRIP_ELLIPSIS 0x2026
#define RUBY_GAP 2.0f          /* between a reading and its kanji */
#define RUBY_RGB 0xFFFFFFu     /* as the sentence */

static ui::Scene *s_strip_page;
static ui::Widget *s_strip, *s_strip_box;
static ui::Text *s_strip_text;
static ui::BusyIndicator *s_strip_busy;
static unsigned s_strip_seen;
static float s_strip_h;   /* text height the strip is sized for, 0 = none yet */
static uint32_t s_strip_cut_lines; /* lines kept by the last cut, 0 = not cut */
static uint16_t s_strip_u[VJO_STRIP_MAX + 1]; /* UTF-16 shown, kept for cutting */
static uint32_t s_strip_len, s_strip_rgb;
static int s_strip_px;
static uint8_t s_mem_buf[(VJO_STRIP_MAX + 16) * sizeof(uint16_t) + 256]; /* the styled text */
static VjoArena s_mem;
static Plugin *s_plugin;
/* Furigana: the readings (UTF-16 ranges of s_strip_u) and their texts. */
static VjoStripRuby s_ruby[VJO_STRIP_RUBY_MAX];
static int s_n_ruby;
static int s_ruby_px;      /* 0: no room for readings */
static int s_ruby_placed;  /* placed for the current layout */
static ui::Text *s_ruby_w[VJO_STRIP_RUBY_MAX];
static int s_n_ruby_w;     /* made from the template */
static unsigned s_text_seq, s_ruby_seq; /* VjoView's, shown */
static int s_fresh;        /* the page just opened: nothing shown yet */

/* Room above each line for its readings. */
static float ruby_room(void)
{
    return s_ruby_px ? (float)s_ruby_px + RUBY_GAP : 0.0f;
}

/* Shows the first len units of s_strip_u in one size and color. */
static void strip_apply(uint32_t len)
{
    s_strip_text->SetString(paf::wstring((const wchar_t *)s_strip_u, len));
    s_strip_text->SetStyleAttribute(graph::TextStyleAttribute_Point, 0, len, v2f((float)s_strip_px, (float)s_strip_px));
    s_strip_text->SetStyleAttribute(graph::TextStyleAttribute_Color, 0, len, rgba(s_strip_rgb));
    s_strip_text->SetLayoutAttribute(graph::TextLayoutAttribute_LinePitch, ruby_room());
    s_strip_h = 0.0f; /* sized again once laid out (until then the old size stays) */
    s_ruby_placed = 0;
}

static void ruby_hide(void)
{
    for (int i = 0; i < s_n_ruby_w; i++)
        s_ruby_w[i]->Hide();
}

/* Reading text i, made from the template when first needed; NULL if it
 * can't be. */
static ui::Text *ruby_widget(int i)
{
    if (i >= VJO_STRIP_RUBY_MAX || !s_strip_box)
        return NULL;
    while (s_n_ruby_w <= i) {
        Plugin::TemplateOpenParam param;
        int n = s_strip_box->GetChildrenNum();
        if (s_plugin->TemplateOpen(s_strip_box, "vjo_strip_ruby_tpl", param) < 0 || s_strip_box->GetChildrenNum() != n + 1)
            return NULL;
        s_ruby_w[s_n_ruby_w++] = (ui::Text *)s_strip_box->GetChild(n);
    }
    return s_ruby_w[i];
}

static void ruby_set(ui::Text *w, const char *utf8)
{
    uint16_t u[VJO_STRIP_READING_MAX];
    size_t len = sceClibStrnlen(utf8, VJO_STRIP_READING_MAX), i = 0;
    uint32_t n = 0;
    while (i < len && n < sizeof(u) / sizeof(u[0])) {
        uint32_t cp = vjo_utf8_next(utf8, len, &i);
        if (cp < 0x10000) /* kana */
            u[n++] = (uint16_t)cp;
    }
    w->SetString(paf::wstring((const wchar_t *)u, n));
    w->SetStyleAttribute(graph::TextStyleAttribute_Point, 0, n, v2f((float)s_ruby_px, (float)s_ruby_px));
    w->SetStyleAttribute(graph::TextStyleAttribute_Color, 0, n, rgba(RUBY_RGB));
}

/* Puts each reading over its kanji, by the sentence's layout: its line
 * (lines are h / lines apart with their room, the first's above the text)
 * and the x of its first and last kanji on that line. A run that wraps
 * gets it over its first part; readings past a cut are left out. Returns
 * 0 while the text is not laid out. */
static int ruby_place(float h)
{
    graph::TextLayoutCharInfo ci;
    uint32_t lines = 0, shown = s_strip_cut_lines ? s_strip_len - 1 : s_strip_len; /* less the "…" */
    float pitch, room = ruby_room();
    int used = 0;
    if (!s_n_ruby || !s_ruby_px) {
        ruby_hide();
        return 1;
    }
    if (s_strip_text->GetLineCount(lines) < 0 || !lines)
        return 0;
    pitch = (h + room) / (float)lines;
    for (int k = 0; k < s_n_ruby; k++) {
        uint32_t line, last;
        float x0, x1;
        ui::Text *w;
        if (s_ruby[k].end > shown)
            break;
        if (s_strip_text->GetCharInfo(s_ruby[k].start, ci) < 0)
            return 0;
        line = ci.line;
        x0 = x1 = ci.x;
        for (last = s_ruby[k].start + 1; last < s_ruby[k].end; last++) {
            if (s_strip_u[last] == '\n' || (s_strip_u[last] & 0xFC00) == 0xDC00)
                continue;
            if (s_strip_text->GetCharInfo(last, ci) < 0)
                return 0;
            if (ci.line != line)
                break;
            x1 = ci.x;
        }
        if (!(w = ruby_widget(used)))
            break;
        used++;
        ruby_set(w, s_ruby[k].reading);
        w->SetPos(-STRIP_BOX_W / 2.0f + (x0 + x1 + (float)s_strip_px) / 2.0f,
                  h / 2.0f - (float)line * pitch + RUBY_GAP + (float)s_ruby_px / 2.0f, 0.0f);
        w->Show();
    }
    for (int i = used; i < s_n_ruby_w; i++)
        s_ruby_w[i]->Hide();
    return 1;
}

/* A new strip_version: the text is laid out again only when it changed
 * (its seq, or how it is drawn), the readings placed again when they did. */
static void strip_render(void)
{
    VjoStyled st;
    int ja, en, kind, busy, px, ruby_px, text_changed;
    uint32_t rgb;
    font_px(&ja, &en);
    vjo_arena_init(&s_mem, s_mem_buf, sizeof(s_mem_buf));
    if (vjo_styled_init(&st, &s_mem, VJO_STRIP_MAX, 1) < 0)
        return;
    vjo_view_lock();
    kind = g_view.strip_kind;
    busy = g_view.strip_busy;
    px = kind == VJO_STRIP_SENTENCE ? ja : en;
    rgb = kind == VJO_STRIP_SENTENCE ? VJO_RGB_TEXT : kind == VJO_STRIP_ERROR ? VJO_RGB_ERROR : VJO_RGB_DIM;
    /* the room stays while it waits for them */
    ruby_px = kind == VJO_STRIP_SENTENCE && g_view.strip_ruby_room ? ja / 2 : 0;
    text_changed = g_view.strip_text_seq != s_text_seq || px != s_strip_px || rgb != s_strip_rgb ||
                   ruby_px != s_ruby_px || s_fresh;
    if (text_changed)
        vjo_styled_puts(&st, g_view.strip_text, rgb, px);
    if (text_changed || g_view.strip_ruby_seq != s_ruby_seq) {
        s_n_ruby = g_view.strip_n_ruby;
        sceClibMemcpy(s_ruby, g_view.strip_ruby, sizeof(s_ruby[0]) * (size_t)s_n_ruby);
        s_ruby_placed = 0;
    }
    s_fresh = 0;
    s_text_seq = g_view.strip_text_seq;
    s_ruby_seq = g_view.strip_ruby_seq;
    vjo_view_unlock();

    if (text_changed) {
        s_strip_px = px;
        s_strip_rgb = rgb;
        s_ruby_px = ruby_px;
        sceClibMemcpy(s_strip_u, st.text, st.len * sizeof(uint16_t));
        s_strip_len = st.len;
        s_strip_cut_lines = 0;
        ruby_hide();
        if (s_strip_text)
            strip_apply(s_strip_len);
    }
    if (s_strip_busy) {
        if (busy && kind == VJO_STRIP_SENTENCE) {
            s_strip_busy->Show();
            s_strip_busy->Start();
        } else {
            s_strip_busy->Stop();
            s_strip_busy->Hide();
        }
    }
}

/* First UTF-16 unit on line `line` (by the Text's layout), else len. */
static uint32_t strip_line_start(uint32_t line)
{
    graph::TextLayoutCharInfo ci;
    uint32_t lo = 0, hi = s_strip_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s_strip_text->GetCharInfo(mid, ci) < 0)
            return s_strip_len;
        if (ci.line >= line)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* Sizes the strip to its laid-out text (and the readings' room above it),
 * anchored at the screen top. Text that would pass the screen's bottom is
 * cut with "…" after the last line that fits; if the "…" wraps to one more
 * line, a line earlier. Then the readings go over it. */
static void strip_fit(void)
{
    uint32_t lines = 0;
    float h = text_height(s_strip_text), sh, room = ruby_room(), avail = SCREEN_H - 2.0f * STRIP_PAD - room;
    if (!s_strip_text || h <= 0.0f)
        return;
    if (h == s_strip_h) {
        if (!s_ruby_placed)
            s_ruby_placed = ruby_place(h);
        return;
    }
    if (h > avail && s_strip_text->GetLineCount(lines) >= 0 && lines > 1) {
        uint32_t fit = s_strip_cut_lines ? s_strip_cut_lines - 1 : (uint32_t)((avail + room) / ((h + room) / (float)lines));
        uint32_t cut = fit ? strip_line_start(fit) : 0;
        if (cut > 1 && (s_strip_u[cut - 2] & 0xFC00) == 0xD800)
            cut--; /* the ellipsis replaces a whole surrogate pair */
        if (cut > 0 && cut < s_strip_len) {
            s_strip_u[cut - 1] = STRIP_ELLIPSIS;
            s_strip_len = cut;
            s_strip_cut_lines = fit;
            strip_apply(cut);
            return;
        }
    }
    s_strip_h = h;
    sh = h + 2.0f * STRIP_PAD + room;
    if (sh > SCREEN_H)
        sh = SCREEN_H;
    if (s_strip_box) {
        s_strip_box->SetSize(STRIP_BOX_W, h, 0.0f);
        s_strip_box->SetPos(0.0f, -room / 2.0f, 0.0f); /* the room above the first line */
    }
    if (s_strip) {
        s_strip->SetSize((float)SCREEN_W, sh, 0.0f);
        s_strip->SetPos(0.0f, SCREEN_H / 2.0f - sh / 2.0f, 0.0f);
        s_strip->Show();
    }
    if (s_strip_busy)
        s_strip_busy->SetPos(SCREEN_W / 2.0f - STRIP_BUSY_INSET, -sh / 2.0f + STRIP_BUSY_INSET, 0.0f);
    s_ruby_placed = ruby_place(h);
}

static void strip_open(Plugin *plugin)
{
    Plugin::PageOpenParam param;
    param.overwrite_draw_priority = 6; /* above the game, as the overlay */
    s_plugin = plugin;
    s_fresh = 1;
    s_strip_page = plugin->PageOpen("vjo_page_strip", param);
    if (!s_strip_page) {
        vjo_log("strip page failed to open");
        return;
    }
    s_strip = s_strip_page->FindChild("vjo_strip");
    s_strip_box = s_strip_page->FindChild("vjo_strip_box");
    s_strip_text = (ui::Text *)s_strip_page->FindChild("vjo_strip_text");
    s_strip_busy = (ui::BusyIndicator *)s_strip_page->FindChild("vjo_strip_busy");
    if (s_strip_text) {
        s_strip_text->SetLayoutAttribute(graph::TextLayoutAttribute_WordWrap, true);
        s_strip_text->SetLayoutAttribute(graph::TextLayoutAttribute_Kinsoku, true);
    }
    if (s_strip)
        s_strip->Hide(); /* shown once the text is laid out */
    s_strip_h = 0.0f;
}

static void strip_close(Plugin *plugin)
{
    Plugin::PageCloseParam param;
    param.fade = false;
    plugin->PageClose("vjo_page_strip", param);
    s_strip_page = NULL;
    s_strip = s_strip_box = NULL;
    s_strip_text = NULL;
    s_strip_busy = NULL;
    s_strip_h = 0.0f;
    s_n_ruby_w = 0; /* closed with the page */
}

int vjo_strip_is_open(void)
{
    return s_strip_page != NULL;
}

void vjo_strip_frame(Plugin *plugin, int want, unsigned version)
{
    if (want && !s_strip_page) {
        strip_open(plugin);
        s_strip_seen = version - 1;
    } else if (!want && s_strip_page) {
        strip_close(plugin);
    }
    if (!s_strip_page)
        return;
    if (version != s_strip_seen) {
        s_strip_seen = version;
        strip_render();
    }
    strip_fit();
}

