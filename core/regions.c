#include "regions.h"

#include <string.h>

#include "port.h"

static void skip_space(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r'))
        (*p)++;
}

static int parse_u16(const char **p, const char *end, uint16_t *out)
{
    unsigned long v = 0;
    int digits = 0;
    skip_space(p, end);
    while (*p < end && **p >= '0' && **p <= '9') {
        v = v * 10 + (unsigned long)(**p - '0');
        if (v > 65535)
            return -1;
        (*p)++;
        digits++;
    }
    skip_space(p, end);
    if (!digits)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

/* Does the line [p, end) have this key? Sets *val past its '='. */
static int line_key(const char *p, const char *end, const char *key, const char **val)
{
    size_t n = strlen(key);
    skip_space(&p, end);
    if ((size_t)(end - p) < n || memcmp(p, key, n) != 0)
        return 0;
    p += n;
    skip_space(&p, end);
    if (p >= end || *p != '=')
        return 0;
    *val = p + 1;
    return 1;
}

/* "x,y,w,h" up to end: a non-empty rectangle inside the screen; or "full"
 * (w == 0). */
static int parse_rect(const char *p, const char *end, VjoRect *out)
{
    VjoRect r;
    skip_space(&p, end);
    if (end - p >= 4 && !memcmp(p, "full", 4)) {
        const char *q = p + 4;
        skip_space(&q, end);
        if (q == end) {
            memset(out, 0, sizeof(*out));
            return 1;
        }
    }
    if (parse_u16(&p, end, &r.x) == 0 && p < end && *p++ == ',' &&
        parse_u16(&p, end, &r.y) == 0 && p < end && *p++ == ',' &&
        parse_u16(&p, end, &r.w) == 0 && p < end && *p++ == ',' &&
        parse_u16(&p, end, &r.h) == 0 && p == end && r.w > 0 && r.h > 0 &&
        (uint32_t)r.x + r.w <= 65536u && (uint32_t)r.y + r.h <= 65536u) {
        *out = r;
        return 1;
    }
    return 0;
}

static const char *line_end(const char *p, const char *end)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    return nl ? nl : end;
}

static const char *next_line(const char *e, const char *end)
{
    return e < end ? e + 1 : end;
}

int vjo_region_parse(const char *text, size_t len, const char *title_id, VjoRect *out)
{
    const char *p = text, *end = text + len, *val;
    int found = VJO_REGION_NONE;
    VjoRect r;
    while (p < end) {
        const char *e = line_end(p, end);
        if (title_id && *title_id && line_key(p, e, title_id, &val) && parse_rect(val, e, &r)) {
            *out = r;
            return VJO_REGION_GAME;
        }
        if (!found && line_key(p, e, VJO_REGION_FALLBACK, &val) && parse_rect(val, e, &r)) {
            *out = r;
            found = VJO_REGION_ALL;
        }
        p = next_line(e, end);
    }
    return found;
}

int vjo_region_format(char *dst, size_t cap, const char *key, const VjoRect *r)
{
    int n = r->w ? vjo_snprintf(dst, cap, "%s = %u,%u,%u,%u\n", key, r->x, r->y, r->w, r->h)
                 : vjo_snprintf(dst, cap, "%s = full\n", key);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

char *vjo_region_update(VjoArena *a, const char *text, size_t len, const char *key, const VjoRect *r)
{
    const char *p = text, *end = text + len, *val;
    char line[80];
    int n = vjo_region_format(line, sizeof(line), key, r);
    VjoBuf b;
    if (n < 0)
        return NULL;
    vjo_buf_init(&b, a);
    while (p < end) {
        const char *e = line_end(p, end);
        if (line_key(p, e, key, &val)) {
            vjo_buf_append(&b, line, (size_t)n); /* the first one only */
            n = 0;
        } else if (e > p) {
            vjo_buf_append(&b, p, (size_t)(e - p));
            vjo_buf_putc(&b, '\n');
        }
        p = next_line(e, end);
    }
    vjo_buf_append(&b, line, (size_t)n);
    return b.oom ? NULL : vjo_buf_cstr(&b);
}
