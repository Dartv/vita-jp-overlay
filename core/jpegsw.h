/* Software baseline JPEG encoder for the OCR capture. */
#ifndef VJO_JPEGSW_H
#define VJO_JPEGSW_H

#include <stdint.h>

#include "arena.h"

/* Fills rows [row, row + n) as R,G,B,A bytes, `stride` bytes per row, into
 * dst; returns the number of rows written. */
typedef int (*VjoRowsFn)(void *ud, uint32_t row, uint32_t n, uint8_t *dst);

/* Encodes w x h pixels (4:2:0, quality 1..100), appending to `out`. Scratch
 * memory (~stride*16 + 2 KB) comes from `a`. */
int vjo_jpeg_encode(VjoArena *a, uint32_t w, uint32_t h, uint32_t stride, VjoRowsFn rows, void *ud,
                    int quality, VjoBuf *out);

#endif
