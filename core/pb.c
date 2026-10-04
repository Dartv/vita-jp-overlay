#include "pb.h"

#include <string.h>

size_t pb_varint_size(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

size_t pb_len_field_size(uint32_t field, size_t payload)
{
    return pb_varint_size((uint64_t)field << 3) + pb_varint_size(payload) + payload;
}

int pb_put_varint(VjoBuf *b, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do {
        uint8_t c = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v)
            c |= 0x80;
        tmp[n++] = c;
    } while (v);
    return vjo_buf_append(b, tmp, n);
}

int pb_put_tag(VjoBuf *b, uint32_t field, int wire)
{
    return pb_put_varint(b, ((uint64_t)field << 3) | (uint32_t)wire);
}

int pb_put_uint(VjoBuf *b, uint32_t field, uint64_t v)
{
    if (pb_put_tag(b, field, PB_VARINT) < 0)
        return -1;
    return pb_put_varint(b, v);
}

int pb_put_len_prefix(VjoBuf *b, uint32_t field, size_t n)
{
    if (pb_put_tag(b, field, PB_LEN) < 0)
        return -1;
    return pb_put_varint(b, n);
}

int pb_put_bytes(VjoBuf *b, uint32_t field, const void *p, size_t n)
{
    if (pb_put_len_prefix(b, field, n) < 0)
        return -1;
    return vjo_buf_append(b, p, n);
}

int pb_put_string(VjoBuf *b, uint32_t field, const char *s)
{
    return pb_put_bytes(b, field, s, strlen(s));
}

int pb_put_fixed32(VjoBuf *b, uint32_t field, uint32_t v)
{
    uint8_t t[4];
    if (pb_put_tag(b, field, PB_FIXED32) < 0)
        return -1;
    t[0] = (uint8_t)v;
    t[1] = (uint8_t)(v >> 8);
    t[2] = (uint8_t)(v >> 16);
    t[3] = (uint8_t)(v >> 24);
    return vjo_buf_append(b, t, 4);
}

void pb_reader_init(PbReader *r, const void *p, size_t n)
{
    r->p = (const uint8_t *)p;
    r->end = r->p + n;
    r->error = 0;
}

static int read_varint(PbReader *r, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end && shift < 64) {
        uint8_t c = *r->p++;
        v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *out = v;
            return 0;
        }
        shift += 7;
    }
    r->error = 1;
    return -1;
}

int pb_next(PbReader *r, PbField *f)
{
    uint64_t key, v;
    if (r->error)
        return -1;
    if (r->p >= r->end)
        return 0;
    if (read_varint(r, &key) < 0)
        return -1;
    memset(f, 0, sizeof(*f));
    f->field = (uint32_t)(key >> 3);
    f->wire = (int)(key & 7);
    switch (f->wire) {
    case PB_VARINT:
        if (read_varint(r, &f->varint) < 0)
            return -1;
        return 1;
    case PB_FIXED64:
        if (r->end - r->p < 8)
            break;
        v = 0;
        for (int i = 7; i >= 0; i--)
            v = (v << 8) | r->p[i];
        f->varint = v;
        r->p += 8;
        return 1;
    case PB_FIXED32:
        if (r->end - r->p < 4)
            break;
        f->varint = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
                    ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
        r->p += 4;
        return 1;
    case PB_LEN:
        if (read_varint(r, &v) < 0)
            return -1;
        if (v > (uint64_t)(r->end - r->p))
            break;
        f->data = r->p;
        f->len = (size_t)v;
        r->p += v;
        return 1;
    default:
        break;
    }
    r->error = 1;
    return -1;
}

void pb_sub_reader(PbReader *sub, const PbField *f)
{
    pb_reader_init(sub, f->data, f->wire == PB_LEN ? f->len : 0);
}

float pb_as_float(const PbField *f)
{
    union {
        uint32_t u;
        float f;
    } c;
    c.u = (uint32_t)f->varint;
    return c.f;
}
