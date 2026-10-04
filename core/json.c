/* The jsmn implementation is compiled here (static); json.h's include of
 * jsmn.h is then a no-op thanks to its include guard. */
#define JSMN_STATIC
#define JSMN_STRICT
#include "../third_party/jsmn/jsmn.h"

#include "json.h"

#include <string.h>

#include "utf.h"

int vjo_json_parse(VjoArena *a, const char *js, size_t len, VjoJson *out)
{
    jsmn_parser p;
    int n;
    jsmn_init(&p);
    n = jsmn_parse(&p, js, len, NULL, 0);
    if (n <= 0)
        return -1;
    out->t = (jsmntok_t *)vjo_arena_alloc(a, sizeof(jsmntok_t) * (size_t)n);
    if (!out->t)
        return -1;
    jsmn_init(&p);
    if (jsmn_parse(&p, js, len, out->t, (unsigned)n) != n)
        return -1;
    out->js = js;
    out->n = n;
    return 0;
}

int vjo_json_skip(const VjoJson *j, int i)
{
    int pending = 1;
    while (pending > 0 && i < j->n) {
        const jsmntok_t *t = &j->t[i];
        pending--;
        /* Objects count their keys, arrays their elements; a key string has
         * size 1 (its value). */
        pending += t->size;
        i++;
    }
    return i;
}

static int tok_eq(const VjoJson *j, int i, const char *s)
{
    const jsmntok_t *t = &j->t[i];
    size_t n = strlen(s);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == n &&
           memcmp(j->js + t->start, s, n) == 0;
}

int vjo_json_obj_get(const VjoJson *j, int obj, const char *key)
{
    int i, k;
    if (obj < 0 || obj >= j->n || j->t[obj].type != JSMN_OBJECT)
        return -1;
    i = obj + 1;
    for (k = 0; k < j->t[obj].size; k++) {
        /* jsmn: key token has size 1, value follows. */
        if (tok_eq(j, i, key))
            return i + 1;
        i = vjo_json_skip(j, i + 1);
    }
    return -1;
}

int vjo_json_arr_get(const VjoJson *j, int arr, int k)
{
    int i;
    if (arr < 0 || arr >= j->n || j->t[arr].type != JSMN_ARRAY || k < 0 || k >= j->t[arr].size)
        return -1;
    i = arr + 1;
    while (k-- > 0)
        i = vjo_json_skip(j, i);
    return i;
}

int vjo_json_is_type(const VjoJson *j, int i, jsmntype_t type)
{
    return i >= 0 && i < j->n && j->t[i].type == type;
}

int vjo_json_is_null(const VjoJson *j, int i)
{
    return i < 0 || i >= j->n ||
           (j->t[i].type == JSMN_PRIMITIVE && j->js[j->t[i].start] == 'n');
}

int vjo_json_int(const VjoJson *j, int i, long *out)
{
    const char *s;
    int n, neg = 0;
    long v = 0;
    if (!vjo_json_is_type(j, i, JSMN_PRIMITIVE))
        return -1;
    s = j->js + j->t[i].start;
    n = j->t[i].end - j->t[i].start;
    if (n > 0 && *s == '-') {
        neg = 1;
        s++;
        n--;
    }
    if (n <= 0 || *s < '0' || *s > '9')
        return -1;
    while (n > 0 && *s >= '0' && *s <= '9') {
        if (v > 99999999)
            return -1; /* over 9 digits: out of range even for a 32-bit long */
        v = v * 10 + (*s - '0');
        s++;
        n--;
    }
    /* Java's Number.intValue() truncates a fractional part. */
    *out = neg ? -v : v;
    return 0;
}

static int hex4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        char c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

/* JSON string unescape; out may alias s (the output never outruns the input). */
static size_t unescape(const char *s, size_t n, char *out)
{
    size_t k, o = 0;
    for (k = 0; k < n; k++) {
        char c = s[k];
        if (c != '\\' || k + 1 >= n) {
            out[o++] = c;
            continue;
        }
        c = s[++k];
        switch (c) {
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'u': {
            uint32_t cp, lo;
            if (k + 4 >= n || hex4(s + k + 1, &cp) < 0) {
                out[o++] = 'u';
                break;
            }
            k += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && k + 6 < n && s[k + 1] == '\\' &&
                s[k + 2] == 'u' && hex4(s + k + 3, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                k += 6;
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            o += (size_t)vjo_utf8_encode(cp, out + o);
            break;
        }
        default: out[o++] = c; break; /* \" \\ \/ */
        }
    }
    return o;
}

char *vjo_json_str(VjoArena *a, const VjoJson *j, int i)
{
    const jsmntok_t *t;
    size_t n, o;
    char *out;
    if (i < 0 || i >= j->n)
        return NULL;
    t = &j->t[i];
    n = (size_t)(t->end - t->start);
    out = (char *)vjo_arena_alloc(a, n + 1); /* unescaping never grows */
    if (!out)
        return NULL;
    memcpy(out, j->js + t->start, n);
    o = t->type == JSMN_STRING ? unescape(out, n, out) : n;
    out[o] = '\0';
    return out;
}

int vjo_json_append_str(VjoBuf *b, const VjoJson *j, int i)
{
    const jsmntok_t *t;
    size_t n, start = b->len;
    if (i < 0 || i >= j->n)
        return -1;
    t = &j->t[i];
    n = (size_t)(t->end - t->start);
    if (vjo_buf_append(b, j->js + t->start, n) < 0)
        return -1;
    if (t->type == JSMN_STRING)
        b->len = start + unescape((char *)b->data + start, n, (char *)b->data + start);
    return 0;
}

int vjo_json_write_string(VjoBuf *b, const char *s)
{
    static const char hexd[] = "0123456789abcdef";
    vjo_buf_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': vjo_buf_puts(b, "\\\""); break;
        case '\\': vjo_buf_puts(b, "\\\\"); break;
        case '\n': vjo_buf_puts(b, "\\n"); break;
        case '\r': vjo_buf_puts(b, "\\r"); break;
        case '\t': vjo_buf_puts(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char e[6] = {'\\', 'u', '0', '0', hexd[c >> 4], hexd[c & 15]};
                vjo_buf_append(b, e, 6);
            } else {
                vjo_buf_putc(b, (char)c);
            }
        }
    }
    vjo_buf_putc(b, '"');
    return b->oom ? -1 : 0;
}
