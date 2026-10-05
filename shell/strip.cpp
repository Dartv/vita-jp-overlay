/* Subtitles strip: the recognized sentence at the top of the screen over
 * the running game, while the overlay is closed (a page of its own,
 * vjo_page_strip). It takes no input. It grows with its text; text that
 * would pass the screen's bottom is cut with "…". A status or an error
 * shows in the strip too, and the system's busy spinner in its corner while a new
 * sentence is being recognized. */
#include "paf_ui.h"

using namespace paf;

#define STRIP_PAD 8.0f       /* above and below the text */
#define STRIP_BOX_W 872.0f     /* the text; the right margin holds the spinner */
#define STRIP_BUSY_INSET 22.0f /* the spinner's centre from the strip's bottom right */
#define STRIP_ELLIPSIS 0x2026

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

/* Shows the first len units of s_strip_u in one size and color. */
static void strip_apply(uint32_t len)
{
    s_strip_text->SetString(paf::wstring((const wchar_t *)s_strip_u, len));
    s_strip_text->SetStyleAttribute(graph::TextStyleAttribute_Point, 0, len, v2f((float)s_strip_px, (float)s_strip_px));
    s_strip_text->SetStyleAttribute(graph::TextStyleAttribute_Color, 0, len, rgba(s_strip_rgb));
    s_strip_h = 0.0f; /* sized again once laid out (until then the old size stays) */
}

static void strip_render(void)
{
    VjoStyled st;
    int ja, en, kind, busy;
    font_px(&ja, &en);
    vjo_arena_init(&s_mem, s_mem_buf, sizeof(s_mem_buf));
    if (vjo_styled_init(&st, &s_mem, VJO_STRIP_MAX, 1) < 0)
        return;
    vjo_view_lock();
    kind = g_view.strip_kind;
    busy = g_view.strip_busy;
    s_strip_px = kind == VJO_STRIP_SENTENCE ? ja : en;
    s_strip_rgb = kind == VJO_STRIP_SENTENCE ? VJO_RGB_TEXT : kind == VJO_STRIP_ERROR ? VJO_RGB_ERROR : VJO_RGB_DIM;
    vjo_styled_puts(&st, g_view.strip_text, s_strip_rgb, s_strip_px);
    vjo_view_unlock();

    sceClibMemcpy(s_strip_u, st.text, st.len * sizeof(uint16_t));
    s_strip_len = st.len;
    s_strip_cut_lines = 0;
    if (s_strip_text)
        strip_apply(s_strip_len);
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

/* Sizes the strip to its laid-out text, anchored at the screen top. Text
 * that would pass the screen's bottom is cut with "…" after the last line
 * that fits; if the "…" wraps to one more line, a line earlier. */
static void strip_fit(void)
{
    uint32_t lines = 0;
    float h = text_height(s_strip_text), sh;
    if (!s_strip_text || h <= 0.0f || h == s_strip_h)
        return;
    if (h + 2.0f * STRIP_PAD > SCREEN_H && s_strip_text->GetLineCount(lines) >= 0 && lines > 1) {
        uint32_t fit = s_strip_cut_lines ? s_strip_cut_lines - 1
                                         : (uint32_t)((SCREEN_H - 2.0f * STRIP_PAD) / (h / (float)lines));
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
    sh = h + 2.0f * STRIP_PAD;
    if (sh > SCREEN_H)
        sh = SCREEN_H;
    if (s_strip_box)
        s_strip_box->SetSize(STRIP_BOX_W, h, 0.0f);
    if (s_strip) {
        s_strip->SetSize((float)SCREEN_W, sh, 0.0f);
        s_strip->SetPos(0.0f, SCREEN_H / 2.0f - sh / 2.0f, 0.0f);
        s_strip->Show();
    }
    if (s_strip_busy)
        s_strip_busy->SetPos(SCREEN_W / 2.0f - STRIP_BUSY_INSET, -sh / 2.0f + STRIP_BUSY_INSET, 0.0f);
}

static void strip_open(Plugin *plugin)
{
    Plugin::PageOpenParam param;
    param.overwrite_draw_priority = 6; /* above the game, as the overlay */
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

