#include "textfilter.h"

#include <string.h>

#include "utf.h"

static int line_has_japanese(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n)
        if (vjo_is_japanese_char(vjo_utf8_next(s, n, &i)))
            return 1;
    return 0;
}

char *vjo_filter_lines(VjoArena *a, const char *text, int mode)
{
    VjoBuf b;
    const char *p = text;
    int first = 1;

    if (mode != VJO_FILTER_LINES)
        return vjo_arena_strndup(a, text, strlen(text));

    vjo_buf_init(&b, a);
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        size_t content = n;
        if (nl && content && p[content - 1] == '\r')
            content--; /* the \r of \r\n belongs to the separator */
        if (line_has_japanese(p, content)) {
            if (!first)
                vjo_buf_putc(&b, '\n');
            vjo_buf_append(&b, p, content);
            first = 0;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return vjo_buf_cstr(&b);
}

char *vjo_strip_newlines(VjoArena *a, const char *text)
{
    size_t n = strlen(text), o = 0;
    char *out = (char *)vjo_arena_alloc(a, n + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r' && i + 1 < n && text[i + 1] == '\n')
            continue;
        if (text[i] == '\n')
            continue;
        out[o++] = text[i];
    }
    out[o] = '\0';
    return out;
}
