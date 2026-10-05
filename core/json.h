/* Small JSON helpers over jsmn (tokens allocated from an arena). */
#ifndef VJO_JSON_H
#define VJO_JSON_H

#include <stddef.h>

#include "arena.h"

#define JSMN_HEADER
#include "../third_party/jsmn/jsmn.h"

typedef struct {
    const char *js;
    jsmntok_t *t;
    int n;
} VjoJson;

/* Returns 0 on success, -1 on parse error / OOM. */
int vjo_json_parse(VjoArena *a, const char *js, size_t len, VjoJson *out);
/* Index of the token following token i and all of its children. */
int vjo_json_skip(const VjoJson *j, int i);
/* Value token for `key` in object token obj, or -1. */
int vjo_json_obj_get(const VjoJson *j, int obj, const char *key);
/* k-th element token of array arr, or -1. */
int vjo_json_arr_get(const VjoJson *j, int arr, int k);
int vjo_json_is_null(const VjoJson *j, int i);
int vjo_json_is_type(const VjoJson *j, int i, jsmntype_t type);
/* A true/false token. Returns 0 on success. */
int vjo_json_bool(const VjoJson *j, int i, int *out);
/* Parses an integral number token. Returns 0 on success. */
int vjo_json_int(const VjoJson *j, int i, long *out);
/* Unescaped, NUL-terminated copy of a string token (or the raw text of a
 * primitive, like Java's toString()). */
char *vjo_json_str(VjoArena *a, const VjoJson *j, int i);
/* Appends the same text to b. Returns 0, or -1 on a bad index / OOM. */
int vjo_json_append_str(VjoBuf *b, const VjoJson *j, int i);
/* Appends s (n bytes) as a quoted JSON string. */
int vjo_json_write_stringn(VjoBuf *b, const char *s, size_t n);
int vjo_json_write_string(VjoBuf *b, const char *s);

#endif
