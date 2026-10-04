#include "render.h"

#define SEPARATOR "────────────────────────"

char *vjo_render_overlay(VjoArena *a, const VjoOverlayData *d)
{
    VjoBuf b;
    const char *body = vjo_entries_body(a, &d->list);
    const char *err = d->err.rc ? vjo_err_text(a, d->failed_stage, &d->err) : NULL;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, d->list.header ? d->list.header : "");
    vjo_buf_putc(&b, '\n');
    if (err) {
        vjo_buf_puts(&b, "[error] ");
        vjo_buf_puts(&b, err);
        vjo_buf_putc(&b, '\n');
    }
    if (body && *body) {
        vjo_buf_puts(&b, SEPARATOR "\n");
        vjo_buf_puts(&b, body);
        vjo_buf_putc(&b, '\n');
    }
    return vjo_buf_cstr(&b);
}

char *vjo_render_highlight(VjoArena *a, const VjoEntryList *l, int entry)
{
    VjoBuf b;
    const VjoEntry *e = &l->entries[entry];
    vjo_buf_init(&b, a);
    if (e->hl_start < 0) {
        vjo_buf_puts(&b, l->header);
    } else {
        vjo_buf_append(&b, l->header, (size_t)e->hl_start);
        vjo_buf_puts(&b, "【");
        vjo_buf_append(&b, l->header + e->hl_start, (size_t)(e->hl_end - e->hl_start));
        vjo_buf_puts(&b, "】");
        vjo_buf_puts(&b, l->header + e->hl_end);
    }
    return vjo_buf_cstr(&b);
}
