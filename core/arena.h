/* Bump allocator. The core never calls malloc: every buffer comes from a
 * caller-provided arena that is reset once per request. */
#ifndef VJO_ARENA_H
#define VJO_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
    size_t peak;
} VjoArena;

void vjo_arena_init(VjoArena *a, void *mem, size_t size);
void vjo_arena_reset(VjoArena *a);

/* 8-byte aligned; returns NULL when the arena is exhausted. */
void *vjo_arena_alloc(VjoArena *a, size_t n);
void *vjo_arena_zalloc(VjoArena *a, size_t n);
char *vjo_arena_strndup(VjoArena *a, const char *s, size_t n);

/* Grow the most recent allocation in place (used by append buffers). */
int vjo_arena_extend_last(VjoArena *a, void *p, size_t old_n, size_t new_n);

static inline size_t vjo_arena_mark(const VjoArena *a) { return a->used; }
static inline void vjo_arena_release(VjoArena *a, size_t mark) { a->used = mark; }
static inline size_t vjo_arena_free(const VjoArena *a) { return a->size - a->used; }

/* Growable byte buffer in an arena. It grows in place while it is the last
 * allocation and moves to a fresh block otherwise, so `data` may change on
 * every append. */
typedef struct {
    VjoArena *arena;
    uint8_t *data;
    size_t len;
    size_t cap;
    int oom;
} VjoBuf;

void vjo_buf_init(VjoBuf *b, VjoArena *a);
int vjo_buf_append(VjoBuf *b, const void *p, size_t n);
int vjo_buf_putc(VjoBuf *b, char c);
int vjo_buf_puts(VjoBuf *b, const char *s);
int vjo_buf_printf(VjoBuf *b, const char *fmt, ...);
/* NUL-terminates and returns the data (still owned by the arena). */
char *vjo_buf_cstr(VjoBuf *b);

#endif
