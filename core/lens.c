#include "lens.h"

#include <string.h>

#include "pb.h"
#include "utf.h"

/* Enum values (Chromium lens_overlay_*.proto) */
#define PLATFORM_WEB 3
#define SURFACE_CHROMIUM 4
#define FILTER_AUTO 7

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* LensOverlayRequestContext { 3: request_id, 4: client_context } */
static int build_request_context(VjoBuf *b, VjoArena *a, const uint8_t rnd[24])
{
    VjoBuf rid, loc, flt, filters, cc;

    /* Sub-messages are serialized into their own buffers first. */
    vjo_buf_init(&rid, a);
    pb_put_uint(&rid, 1, rd_u64(rnd));          /* uuid */
    pb_put_bytes(&rid, 4, rnd + 8, 16);         /* analytics_id */
    pb_put_len_prefix(&rid, 6, 0);              /* routing_info {} */
    if (rid.oom)
        return -1;

    vjo_buf_init(&loc, a);
    pb_put_string(&loc, 1, "ja");               /* language */
    pb_put_string(&loc, 2, "Asia/Tokyo");       /* region */
    if (loc.oom)
        return -1;

    vjo_buf_init(&flt, a);
    pb_put_uint(&flt, 1, FILTER_AUTO);          /* AppliedFilter.filter_type */
    if (flt.oom)
        return -1;

    vjo_buf_init(&filters, a);
    pb_put_bytes(&filters, 1, flt.data, flt.len); /* AppliedFilters.filter */
    if (filters.oom)
        return -1;

    vjo_buf_init(&cc, a);
    pb_put_uint(&cc, 1, PLATFORM_WEB);
    pb_put_uint(&cc, 2, SURFACE_CHROMIUM);
    pb_put_bytes(&cc, 4, loc.data, loc.len);
    pb_put_bytes(&cc, 17, filters.data, filters.len);
    if (cc.oom)
        return -1;

    pb_put_bytes(b, 3, rid.data, rid.len);
    pb_put_bytes(b, 4, cc.data, cc.len);
    return b->oom ? -1 : 0;
}

int vjo_lens_build_request(VjoArena *a, const uint8_t rnd[24], uint32_t width, uint32_t height,
                           size_t jpeg_len, VjoLensRequest *out)
{
    VjoBuf ctx, meta, prefix, suffix;
    size_t payload_sz, imgdata_sz, objreq_sz;

    vjo_buf_init(&ctx, a);
    if (build_request_context(&ctx, a, rnd) < 0)
        return -1;

    vjo_buf_init(&meta, a);
    pb_put_uint(&meta, 1, width);
    pb_put_uint(&meta, 2, height);
    if (meta.oom)
        return -1;

    payload_sz = pb_len_field_size(1, jpeg_len);                     /* ImagePayload */
    imgdata_sz = pb_len_field_size(1, payload_sz) + pb_len_field_size(3, meta.len);
    objreq_sz = pb_len_field_size(1, ctx.len) + pb_len_field_size(3, imgdata_sz);

    vjo_buf_init(&prefix, a);
    pb_put_len_prefix(&prefix, 1, objreq_sz);      /* ServerRequest.objects_request */
    pb_put_bytes(&prefix, 1, ctx.data, ctx.len);   /* ObjectsRequest.request_context */
    pb_put_len_prefix(&prefix, 3, imgdata_sz);     /* ObjectsRequest.image_data */
    pb_put_len_prefix(&prefix, 1, payload_sz);     /* ImageData.payload */
    pb_put_len_prefix(&prefix, 1, jpeg_len);       /* ImagePayload.image_bytes */
    if (prefix.oom)
        return -1;
    out->prefix = prefix.data;
    out->prefix_len = prefix.len;

    vjo_buf_init(&suffix, a);
    pb_put_bytes(&suffix, 3, meta.data, meta.len); /* ImageData.image_metadata */
    if (suffix.oom)
        return -1;
    out->suffix = suffix.data;
    out->suffix_len = suffix.len;
    out->body_len = out->prefix_len + jpeg_len + out->suffix_len;
    return 0;
}

/* ---- response ---- */

static void parse_box(const PbField *geom, VjoBox *box)
{
    PbReader g, bb;
    PbField f, h;
    memset(box, 0, sizeof(*box));
    pb_sub_reader(&g, geom);
    while (pb_next(&g, &f) > 0) {
        if (f.field != 1 || f.wire != PB_LEN) /* Geometry.bounding_box */
            continue;
        pb_sub_reader(&bb, &f);
        while (pb_next(&bb, &h) > 0) {
            if (h.wire != PB_FIXED32)
                continue;
            switch (h.field) {
            case 1: box->cx = pb_as_float(&h); break;
            case 2: box->cy = pb_as_float(&h); break;
            case 3: box->w = pb_as_float(&h); break;
            case 4: box->h = pb_as_float(&h); break;
            case 5: box->rot = pb_as_float(&h); break;
            }
        }
    }
}

static int count_fields(const PbField *msg, uint32_t field)
{
    PbReader r;
    PbField f;
    int n = 0;
    pb_sub_reader(&r, msg);
    while (pb_next(&r, &f) > 0)
        if (f.field == field && f.wire == PB_LEN)
            n++;
    return r.error ? -1 : n;
}

static int parse_word(VjoArena *a, const PbField *msg, VjoLensWord *w)
{
    PbReader r;
    PbField f;
    w->text = "";
    w->sep = "";
    pb_sub_reader(&r, msg);
    while (pb_next(&r, &f) > 0) {
        if (f.wire != PB_LEN)
            continue;
        if (f.field == 2 || f.field == 3) {
            char *s = vjo_arena_strndup(a, (const char *)f.data, f.len);
            if (!s)
                return -1;
            if (f.field == 2)
                w->text = s;
            else
                w->sep = s;
        } else if (f.field == 4) {
            parse_box(&f, &w->box);
        }
    }
    return r.error ? -1 : 0;
}

static int parse_line(VjoArena *a, const PbField *msg, VjoLensLine *l)
{
    PbReader r;
    PbField f;
    int n = count_fields(msg, 1), i = 0;
    if (n < 0)
        return -1;
    l->n_words = n;
    l->words = n ? (VjoLensWord *)vjo_arena_zalloc(a, sizeof(VjoLensWord) * (size_t)n) : NULL;
    if (n && !l->words)
        return -1;
    pb_sub_reader(&r, msg);
    while (pb_next(&r, &f) > 0) {
        if (f.wire != PB_LEN)
            continue;
        if (f.field == 1) {
            if (parse_word(a, &f, &l->words[i++]) < 0)
                return -1;
        } else if (f.field == 2) {
            parse_box(&f, &l->box);
        }
    }
    return 0;
}

static int parse_paragraph(VjoArena *a, const PbField *msg, VjoLensParagraph *p)
{
    PbReader r;
    PbField f;
    int n = count_fields(msg, 2), i = 0;
    if (n < 0)
        return -1;
    p->n_lines = n;
    p->lines = n ? (VjoLensLine *)vjo_arena_zalloc(a, sizeof(VjoLensLine) * (size_t)n) : NULL;
    if (n && !p->lines)
        return -1;
    pb_sub_reader(&r, msg);
    while (pb_next(&r, &f) > 0) {
        if (f.field == 2 && f.wire == PB_LEN) {
            if (parse_line(a, &f, &p->lines[i++]) < 0)
                return -1;
        } else if (f.field == 3 && f.wire == PB_LEN) {
            parse_box(&f, &p->box);
        } else if (f.field == 4 && f.wire == PB_VARINT) {
            p->writing_direction = (int)f.varint;
        }
    }
    return 0;
}

/* Finds the first length-delimited `field` inside `msg`. */
static int find_sub(const PbField *msg, uint32_t field, PbField *out)
{
    PbReader r;
    PbField f;
    pb_sub_reader(&r, msg);
    while (pb_next(&r, &f) > 0) {
        if (f.field == field && f.wire == PB_LEN) {
            *out = f;
            return 1;
        }
    }
    return 0;
}

int vjo_lens_parse_response(VjoArena *a, const uint8_t *data, size_t len, VjoLensResult *out)
{
    PbField root, objects, text, layout, err, f;
    PbReader r;
    int n, i = 0;

    memset(out, 0, sizeof(*out));
    root.field = 0;
    root.wire = PB_LEN;
    root.data = data;
    root.len = len;

    {
        /* Validate the whole top level once. */
        PbReader v;
        pb_sub_reader(&v, &root);
        while (pb_next(&v, &f) > 0)
            ;
        if (v.error)
            return -1;
    }

    if (find_sub(&root, 1, &err)) { /* ServerResponse.error */
        PbReader er;
        pb_sub_reader(&er, &err);
        out->server_error = -1;
        while (pb_next(&er, &f) > 0)
            if (f.field == 1 && f.wire == PB_VARINT)
                out->server_error = (int)f.varint;
    }

    /* objects_response(2) -> text(3) -> text_layout(1) -> paragraphs(1) */
    if (!find_sub(&root, 2, &objects) || !find_sub(&objects, 3, &text) ||
        !find_sub(&text, 1, &layout))
        return 0;

    n = count_fields(&layout, 1);
    if (n < 0)
        return -1;
    out->n_paragraphs = n;
    out->paragraphs =
        n ? (VjoLensParagraph *)vjo_arena_zalloc(a, sizeof(VjoLensParagraph) * (size_t)n) : NULL;
    if (n && !out->paragraphs)
        return -1;
    pb_sub_reader(&r, &layout);
    while (pb_next(&r, &f) > 0) {
        if (f.field == 1 && f.wire == PB_LEN)
            if (parse_paragraph(a, &f, &out->paragraphs[i++]) < 0)
                return -1;
    }
    return 0;
}

char *vjo_lens_text(VjoArena *a, const VjoLensResult *r)
{
    VjoBuf b;
    char *s;
    vjo_buf_init(&b, a);
    for (int p = 0; p < r->n_paragraphs; p++) {
        const VjoLensParagraph *para = &r->paragraphs[p];
        for (int l = 0; l < para->n_lines; l++) {
            const VjoLensLine *line = &para->lines[l];
            for (int w = 0; w < line->n_words; w++) {
                vjo_buf_puts(&b, line->words[w].text);
                vjo_buf_puts(&b, line->words[w].sep);
            }
        }
        vjo_buf_putc(&b, '\n');
    }
    s = vjo_buf_cstr(&b);
    if (!s)
        return NULL;
    return vjo_java_trim(s);
}
