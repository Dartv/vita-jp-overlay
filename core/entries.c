#include "entries.h"

#include <limits.h>
#include <string.h>

#include "port.h"
#include "utf.h"

char *vjo_entry_format(VjoArena *a, const VjoVocab *v)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, v->spelling);
    if (strcmp(v->spelling, v->reading) != 0) {
        vjo_buf_puts(&b, " (");
        vjo_buf_puts(&b, v->reading);
        vjo_buf_puts(&b, ")");
    }
    vjo_buf_printf(&b, " %d", v->rank);
    for (int i = 0; i < v->n_meanings; i++) {
        vjo_buf_putc(&b, '\n');
        vjo_buf_puts(&b, v->meanings[i]);
    }
    return vjo_buf_cstr(&b);
}

/* Byte offset in `filtered` of UTF-16 index pos16 of the newline-stripped
 * text (the same removal rule as vjo_strip_newlines). */
static int u16_to_byte(const char *filtered, size_t len, int pos16)
{
    size_t i = 0;
    int u = 0;
    while (i < len) {
        size_t start = i;
        uint32_t cp;
        if (filtered[i] == '\n' || (filtered[i] == '\r' && i + 1 < len && filtered[i + 1] == '\n')) {
            i++;
            continue;
        }
        if (u >= pos16)
            return (int)start;
        cp = vjo_utf8_next(filtered, len, &i);
        u += vjo_utf16_units(cp);
    }
    return (int)len;
}

int vjo_entries_build(VjoArena *a, const char *filtered, const VjoDictResult *r, VjoEntryList *out)
{
    size_t flen = strlen(filtered);
    size_t lead = 0;
    char *header;
    int n = r->n_vocab;

    /* Header = Java trim() of the filtered text; remember the cut offset. */
    while (lead < flen && (uint8_t)filtered[lead] <= 0x20)
        lead++;
    header = vjo_arena_strndup(a, filtered + lead, flen - lead);
    if (!header)
        return -1;
    {
        size_t hl = strlen(header);
        while (hl && (uint8_t)header[hl - 1] <= 0x20)
            header[--hl] = '\0';
    }
    out->header = header;
    out->entries = NULL;
    out->n_entries = 0;

    if (!n)
        return 0;
    out->entries = (VjoEntry *)vjo_arena_zalloc(a, sizeof(VjoEntry) * (size_t)n);
    if (!out->entries)
        return -1;

    for (int k = 0; k < n; k++) {
        const VjoVocab *v = &r->vocab[k];
        VjoEntry *e = &out->entries[k];
        int len16 = 0;
        e->vocab = v;
        e->first_pos16 = -1;
        e->hl_start = e->hl_end = -1;
        for (int t = 0; t < r->n_tokens; t++) {
            if (r->tokens[t].vocab == k) {
                e->first_pos16 = r->tokens[t].pos16;
                len16 = r->tokens[t].len16;
                break;
            }
        }
        if (e->first_pos16 >= 0) {
            /* Positions come from the server: saturate the end (no signed
             * overflow); u16_to_byte maps positions past the text to its end. */
            int pos = e->first_pos16;
            int end = len16 <= 0 ? pos : len16 > INT_MAX - pos ? INT_MAX : pos + len16;
            int bs = u16_to_byte(filtered, flen, pos) - (int)lead;
            int be = u16_to_byte(filtered, flen, end) - (int)lead;
            int hl = (int)strlen(header);
            if (bs < 0)
                bs = 0;
            if (be > hl)
                be = hl;
            if (bs < be) {
                e->hl_start = bs;
                e->hl_end = be;
            }
        }
        e->text = vjo_entry_format(a, v);
        if (!e->text)
            return -1;
    }
    out->n_entries = n;

    /* Stable insertion sort into text order; entries without a token keep
     * the dictionary's order at the end. */
    for (int i = 1; i < n; i++) {
        VjoEntry tmp = out->entries[i];
        int key = tmp.first_pos16 < 0 ? 0x7fffffff : tmp.first_pos16;
        int j = i - 1;
        while (j >= 0) {
            int kj = out->entries[j].first_pos16 < 0 ? 0x7fffffff : out->entries[j].first_pos16;
            if (kj <= key)
                break;
            out->entries[j + 1] = out->entries[j];
            j--;
        }
        out->entries[j + 1] = tmp;
    }
    return 0;
}

char *vjo_entries_body(VjoArena *a, const VjoEntryList *l)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    for (int i = 0; i < l->n_entries; i++) {
        if (i)
            vjo_buf_puts(&b, "\n\n");
        vjo_buf_puts(&b, l->entries[i].text);
    }
    return vjo_buf_cstr(&b);
}
