#include "styled.h"

#include <string.h>

#include "port.h"
#include "utf.h"

int vjo_font_px(int font_size)
{
    return (font_size * 3 + 1) / 2;
}

int vjo_styled_init(VjoStyled *s, VjoArena *a, uint32_t cap_units, int cap_spans)
{
    memset(s, 0, sizeof(*s));
    s->text = (uint16_t *)vjo_arena_alloc(a, sizeof(uint16_t) * (cap_units + 1));
    s->spans = (VjoSpan *)vjo_arena_alloc(a, sizeof(VjoSpan) * (size_t)cap_spans);
    if (!s->text || !s->spans)
        return -1;
    s->cap = cap_units;
    s->cap_spans = cap_spans;
    s->text[0] = 0;
    return 0;
}

void vjo_styled_add(VjoStyled *s, const char *utf8, size_t n, uint32_t rgb, int px)
{
    size_t i = 0;
    uint32_t start = s->len;
    while (i < n) {
        uint32_t cp = vjo_utf8_next(utf8, n, &i);
        int units = vjo_utf16_units(cp);
        if (cp == '\r')
            continue;
        if (s->len + (uint32_t)units > s->cap) {
            s->truncated = 1;
            break;
        }
        if (units == 2) {
            cp -= 0x10000;
            s->text[s->len++] = (uint16_t)(0xD800 | (cp >> 10));
            s->text[s->len++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            s->text[s->len++] = (uint16_t)cp;
        }
    }
    s->text[s->len] = 0;
    if (s->len == start)
        return;
    if (s->n_spans >= s->cap_spans) {
        s->truncated = 1;
        return;
    }
    s->spans[s->n_spans].start = start;
    s->spans[s->n_spans].len = s->len - start;
    s->spans[s->n_spans].rgb = rgb;
    s->spans[s->n_spans].px = px;
    s->n_spans++;
}

void vjo_styled_puts(VjoStyled *s, const char *utf8, uint32_t rgb, int px)
{
    vjo_styled_add(s, utf8, strlen(utf8), rgb, px);
}

uint32_t vjo_utf16_index(const char *utf8, size_t off)
{
    size_t len = strlen(utf8), i = 0;
    uint32_t u = 0;
    if (off > len)
        off = len;
    while (i < off) {
        uint32_t cp = vjo_utf8_next(utf8, len, &i);
        if (cp != '\r')
            u += (uint32_t)vjo_utf16_units(cp);
    }
    return u;
}

int vjo_strip_rubies(VjoArena *a, const char *text, const VjoEntryList *words, VjoStripRuby *out, int cap)
{
    size_t len = strlen(text), i = 0, mark = vjo_arena_mark(a);
    VjoRuby *r = (VjoRuby *)vjo_arena_alloc(a, sizeof(VjoRuby) * (size_t)cap);
    uint32_t u = 0;
    int nr, n = 0;
    if (!r || !words->header || strncmp(words->header, text, len)) {
        vjo_arena_release(a, mark);
        return 0;
    }
    nr = vjo_entries_rubies(words, r, cap);
    /* One pass over the text: the readings are in text order. */
    for (int k = 0; k < nr && r[k].end <= len; k++) {
        if (r[k].reading_len >= VJO_STRIP_READING_MAX || r[k].end - r[k].start > 0xFFFF)
            continue;
        while (i < r[k].start) {
            uint32_t cp = vjo_utf8_next(text, len, &i);
            if (cp != '\r')
                u += (uint32_t)vjo_utf16_units(cp);
        }
        out[n].start = (uint16_t)u;
        while (i < r[k].end) {
            uint32_t cp = vjo_utf8_next(text, len, &i);
            if (cp != '\r')
                u += (uint32_t)vjo_utf16_units(cp);
        }
        if (u > 0xFFFF)
            break;
        out[n].end = (uint16_t)u;
        memcpy(out[n].reading, r[k].reading, r[k].reading_len);
        out[n].reading[r[k].reading_len] = '\0';
        n++;
    }
    vjo_arena_release(a, mark);
    return n;
}

void vjo_styled_header(VjoStyled *s, const VjoEntryList *l, int ja_px)
{
    if (l && l->header)
        vjo_styled_puts(s, l->header, VJO_RGB_TEXT, ja_px);
}

int vjo_entry_header_range(const VjoEntryList *l, int entry, uint32_t *start, uint32_t *len)
{
    const VjoEntry *e;
    if (!l || entry < 0 || entry >= l->n_entries)
        return 0;
    e = &l->entries[entry];
    if (e->hl_start < 0)
        return 0;
    *start = vjo_utf16_index(l->header, (size_t)e->hl_start);
    *len = vjo_utf16_index(l->header, (size_t)e->hl_end) - *start;
    return *len > 0;
}

void vjo_styled_entry(VjoStyled *s, const VjoEntryList *l, int selected, int ja_px, int en_px, int in_anki)
{
    const VjoVocab *v;
    char rank[16];
    if (!l || selected < 0 || selected >= l->n_entries)
        return;
    v = l->entries[selected].vocab;
    vjo_styled_puts(s, v->spelling, VJO_RGB_HIGHLIGHT, ja_px);
    if (strcmp(v->spelling, v->reading) != 0) {
        vjo_styled_puts(s, " (", VJO_RGB_READING, ja_px);
        vjo_styled_puts(s, v->reading, VJO_RGB_READING, ja_px);
        vjo_styled_puts(s, ")", VJO_RGB_READING, ja_px);
    }
    vjo_snprintf(rank, sizeof(rank), " %d", v->rank);
    vjo_styled_puts(s, rank, VJO_RGB_RANK, en_px);
    if (in_anki)
        vjo_styled_puts(s, " " VJO_ANKI_MARK, VJO_RGB_OK, en_px);
    for (int i = 0; i < v->n_meanings; i++) {
        vjo_styled_puts(s, "\n", VJO_RGB_DIM, en_px);
        vjo_styled_puts(s, v->meanings[i], VJO_RGB_DIM, en_px);
    }
}
