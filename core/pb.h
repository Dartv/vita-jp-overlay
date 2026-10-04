/* Minimal protobuf wire-format writer and reader. */
#ifndef VJO_PB_H
#define VJO_PB_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

enum { PB_VARINT = 0, PB_FIXED64 = 1, PB_LEN = 2, PB_FIXED32 = 5 };

/* ---- writer: appends to a VjoBuf ---- */
int pb_put_varint(VjoBuf *b, uint64_t v);
int pb_put_tag(VjoBuf *b, uint32_t field, int wire);
int pb_put_uint(VjoBuf *b, uint32_t field, uint64_t v);
int pb_put_bytes(VjoBuf *b, uint32_t field, const void *p, size_t n);
int pb_put_string(VjoBuf *b, uint32_t field, const char *s);
int pb_put_fixed32(VjoBuf *b, uint32_t field, uint32_t v);
/* Writes only the tag + length prefix of a length-delimited field. */
int pb_put_len_prefix(VjoBuf *b, uint32_t field, size_t n);
size_t pb_varint_size(uint64_t v);
/* Size of a whole length-delimited field (tag + length + payload). */
size_t pb_len_field_size(uint32_t field, size_t payload);

/* ---- reader ---- */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int error;
} PbReader;

typedef struct {
    uint32_t field;
    int wire;
    uint64_t varint;          /* PB_VARINT, PB_FIXED32, PB_FIXED64 */
    const uint8_t *data;      /* PB_LEN */
    size_t len;               /* PB_LEN */
} PbField;

void pb_reader_init(PbReader *r, const void *p, size_t n);
/* Returns 1 when a field was read, 0 at end, -1 on malformed input. */
int pb_next(PbReader *r, PbField *f);
void pb_sub_reader(PbReader *sub, const PbField *f);
float pb_as_float(const PbField *f);

#endif
