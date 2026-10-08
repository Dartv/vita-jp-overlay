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

/* Appends an entry for v at token tok (NULL: none), with its highlight:
 * the token's byte range in the header (out->header, `lead` bytes into
 * `filtered`). */
static void add_entry(VjoEntryList *out, const VjoVocab *v, const char *text, const VjoToken *tok,
                      const char *filtered, size_t flen, size_t lead)
{
    VjoEntry *e = &out->entries[out->n_entries++];
    e->vocab = v;
    e->text = text;
    e->pos16 = -1;
    e->hl_start = e->hl_end = -1;
    if (tok && tok->pos16 >= 0) {
        /* Positions come from the server: saturate the end (no signed
         * overflow); u16_to_byte maps positions past the text to its end. */
        int pos = tok->pos16, len16 = tok->len16;
        int end = len16 <= 0 ? pos : len16 > INT_MAX - pos ? INT_MAX : pos + len16;
        int bs = u16_to_byte(filtered, flen, pos) - (int)lead;
        int be = u16_to_byte(filtered, flen, end) - (int)lead;
        int hl = (int)strlen(out->header);
        e->pos16 = pos;
        if (bs < 0)
            bs = 0;
        if (be > hl)
            be = hl;
        if (bs < be) {
            e->hl_start = bs;
            e->hl_end = be;
        }
    }
}

int vjo_entries_build(VjoArena *a, const char *filtered, const VjoDictResult *r, VjoEntryList *out)
{
    size_t flen = strlen(filtered);
    size_t lead;
    char *copy, *header;
    const char **texts;
    int n = r->n_vocab;

    /* Header = Java trim() of the filtered text; remember the cut offset. */
    copy = vjo_arena_strndup(a, filtered, flen);
    if (!copy)
        return -1;
    header = vjo_java_trim(copy);
    lead = (size_t)(header - copy);
    out->header = header;
    out->entries = NULL;
    out->n_entries = 0;

    if (!n)
        return 0;
    /* One entry per token with a word (a word used twice gets two), plus
     * one per word without a token: at most n_tokens + n entries. */
    texts = (const char **)vjo_arena_zalloc(a, sizeof(*texts) * (size_t)n);
    out->entries = (VjoEntry *)vjo_arena_zalloc(a, sizeof(VjoEntry) * ((size_t)r->n_tokens + (size_t)n));
    if (!texts || !out->entries)
        return -1;
    for (int k = 0; k < n; k++)
        if (!(texts[k] = vjo_entry_format(a, &r->vocab[k])))
            return -1;

    for (int t = 0; t < r->n_tokens; t++) {
        int k = r->tokens[t].vocab;
        if (k < 0 || k >= n)
            continue;
        add_entry(out, &r->vocab[k], texts[k], &r->tokens[t], filtered, flen, lead);
    }
    for (int k = 0; k < n; k++) {
        int has_token = 0;
        for (int i = 0; i < out->n_entries && !has_token; i++)
            has_token = out->entries[i].vocab == &r->vocab[k];
        if (!has_token)
            add_entry(out, &r->vocab[k], texts[k], NULL, filtered, flen, lead);
    }

    /* Stable insertion sort into text order; entries without a token keep
     * the dictionary's order at the end. */
    for (int i = 1; i < out->n_entries; i++) {
        VjoEntry tmp = out->entries[i];
        int key = tmp.pos16 < 0 ? 0x7fffffff : tmp.pos16;
        int j = i - 1;
        while (j >= 0) {
            int kj = out->entries[j].pos16 < 0 ? 0x7fffffff : out->entries[j].pos16;
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
