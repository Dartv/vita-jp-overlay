#include "regions.h"

#include <string.h>

#include "port.h"

#define KEY "region"

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

int vjo_region_parse(const char *text, size_t len, VjoRect *out)
{
    const char *p = text, *end = text + len;
    VjoRect r;
    skip_space(&p, end);
    if ((size_t)(end - p) < sizeof(KEY) - 1 || memcmp(p, KEY, sizeof(KEY) - 1) != 0)
        return 0;
    p += sizeof(KEY) - 1;
    skip_space(&p, end);
    if (p >= end || *p++ != '=')
        return 0;
    if (parse_u16(&p, end, &r.x) == 0 && p < end && *p++ == ',' &&
        parse_u16(&p, end, &r.y) == 0 && p < end && *p++ == ',' &&
        parse_u16(&p, end, &r.w) == 0 && p < end && *p++ == ',' &&
        parse_u16(&p, end, &r.h) == 0 && (p == end || *p == '\n') && r.w > 0 && r.h > 0 &&
        (uint32_t)r.x + r.w <= 65536u && (uint32_t)r.y + r.h <= 65536u) {
        *out = r;
        return 1;
    }
    return 0;
}

int vjo_region_format(char *dst, size_t cap, const VjoRect *r)
{
    int n = vjo_snprintf(dst, cap, KEY " = %u,%u,%u,%u\n", r->x, r->y, r->w, r->h);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}
