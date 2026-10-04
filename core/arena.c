#include "arena.h"

#include <stdarg.h>
#include "port.h"
#include <string.h>

#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

void vjo_arena_init(VjoArena *a, void *mem, size_t size)
{
    a->base = (uint8_t *)mem;
    a->size = size;
    a->used = 0;
    a->peak = 0;
}

void vjo_arena_reset(VjoArena *a)
{
    a->used = 0;
}

void *vjo_arena_alloc(VjoArena *a, size_t n)
{
    size_t off = ALIGN8(a->used);
    if (n > a->size || off > a->size - n)
        return NULL;
    a->used = off + n;
    if (a->used > a->peak)
        a->peak = a->used;
    return a->base + off;
}

void *vjo_arena_zalloc(VjoArena *a, size_t n)
{
    void *p = vjo_arena_alloc(a, n);
    if (p)
        memset(p, 0, n);
    return p;
}

char *vjo_arena_strndup(VjoArena *a, const char *s, size_t n)
{
    char *p = (char *)vjo_arena_alloc(a, n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int vjo_arena_extend_last(VjoArena *a, void *p, size_t old_n, size_t new_n)
{
    uint8_t *u = (uint8_t *)p;
    size_t off;
    if (u < a->base || u > a->base + a->size)
        return -1;
    off = (size_t)(u - a->base);
    if (off + old_n != a->used)
        return -1;
    if (new_n > a->size - off)
        return -1;
    a->used = off + new_n;
    if (a->used > a->peak)
        a->peak = a->used;
    return 0;
}

void vjo_buf_init(VjoBuf *b, VjoArena *a)
{
    b->arena = a;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

static int buf_reserve(VjoBuf *b, size_t extra)
{
    size_t want, need;
    uint8_t *p;
    if (b->oom)
        return -1;
    need = b->len + extra + 1;
    if (need <= b->cap)
        return 0;
    want = b->cap ? b->cap : 256;
    while (want < need)
        want *= 2;
    /* Grow in place while the buffer is the arena's last allocation. */
    if (b->data && vjo_arena_extend_last(b->arena, b->data, b->cap, want) == 0) {
        b->cap = want;
        return 0;
    }
    if (b->data && vjo_arena_extend_last(b->arena, b->data, b->cap, need) == 0) {
        b->cap = need;
        return 0;
    }
    /* Otherwise move to a fresh block (exact size if the doubled one does
     * not fit); the old block stays until the arena is released. */
    p = (uint8_t *)vjo_arena_alloc(b->arena, want);
    if (!p)
        p = (uint8_t *)vjo_arena_alloc(b->arena, want = need);
    if (!p) {
        b->oom = 1;
        return -1;
    }
    if (b->len)
        memcpy(p, b->data, b->len);
    b->data = p;
    b->cap = want;
    return 0;
}

int vjo_buf_append(VjoBuf *b, const void *p, size_t n)
{
    if (buf_reserve(b, n) < 0)
        return -1;
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

int vjo_buf_putc(VjoBuf *b, char c)
{
    return vjo_buf_append(b, &c, 1);
}

int vjo_buf_puts(VjoBuf *b, const char *s)
{
    return vjo_buf_append(b, s, strlen(s));
}

int vjo_buf_printf(VjoBuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vjo_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    return vjo_buf_append(b, tmp, (size_t)n);
}

char *vjo_buf_cstr(VjoBuf *b)
{
    if (buf_reserve(b, 0) < 0)
        return NULL;
    b->data[b->len] = '\0';
    return (char *)b->data;
}
