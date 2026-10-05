/* Base64 (RFC 4648) for AnkiConnect media uploads. */
#ifndef VJO_BASE64_H
#define VJO_BASE64_H

#include <stddef.h>
#include <stdint.h>

/* Encoded length of n bytes (with padding). */
static inline size_t vjo_base64_len(size_t n) { return (n + 2) / 3 * 4; }

/* Writes vjo_base64_len(n) characters (no NUL) to out. A stream can be
 * encoded in chunks: every chunk but the last must be a multiple of 3 bytes,
 * so that only the last one is padded. */
void vjo_base64_encode(const uint8_t *in, size_t n, char *out);

#endif
