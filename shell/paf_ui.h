/* Shared by the paf pages (overlay.cpp, strip.cpp): screen geometry and
 * paf::ui::Text helpers. paf layout: origin at the screen centre, y up. */
#ifndef VJO_PAF_UI_H
#define VJO_PAF_UI_H

#include <limits> /* before paf.h: its vector uses numeric_limits */
#include <paf.h>
#include <psp2/kernel/clib.h>

extern "C" {
#include "../core/styled.h"
#include "shell.h"
}

#define SCREEN_W 960
#define SCREEN_H 544
#define BOX_W 920.0f /* text width: the screen less the side margins */

/* paf::math::v2/v4 keep their floats private. */
static inline paf::math::v2 v2f(float x, float y)
{
    paf::math::v2 v;
    float f[2] = {x, y};
    sceClibMemcpy(&v, f, sizeof(f));
    return v;
}

static inline float v2_get(const paf::math::v2 &v, int k)
{
    float f[2];
    sceClibMemcpy(f, &v, sizeof(f));
    return f[k];
}

static inline paf::math::v4 rgba(uint32_t rgb)
{
    return paf::math::v4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
}

static inline void text_set(paf::ui::Text *w, const VjoStyled *st)
{
    if (!w)
        return;
    w->SetString(paf::wstring((const wchar_t *)st->text, st->len));
    for (int i = 0; i < st->n_spans; i++) {
        const VjoSpan *sp = &st->spans[i];
        /* Point takes a v2 (glyph width, height), as in GrapheneCt's NetStream */
        w->SetStyleAttribute(paf::graph::TextStyleAttribute_Point, sp->start, sp->len,
                             v2f((float)sp->px, (float)sp->px));
        w->SetStyleAttribute(paf::graph::TextStyleAttribute_Color, sp->start, sp->len, rgba(sp->rgb));
    }
}

/* Laid-out size of a Text's content; 0 if not laid out yet. */
static inline float text_bound(paf::ui::Text *w, int k)
{
    paf::math::v2 b;
    if (!w || w->GetBounds(b) < 0)
        return 0.0f;
    return v2_get(b, k);
}

static inline float text_height(paf::ui::Text *w)
{
    return text_bound(w, 1);
}

static inline void font_px(int *ja, int *en)
{
    vjo_view_lock();
    *ja = vjo_font_px(g_view.font_size_ja);
    *en = vjo_font_px(g_view.font_size_en);
    vjo_view_unlock();
}

/* strip.cpp: the subtitles strip. One frame: the page is open while want
 * (closed otherwise) and rendered on a new strip_version. */
void vjo_strip_frame(paf::Plugin *plugin, int want, unsigned version);
int vjo_strip_is_open(void);

#endif
