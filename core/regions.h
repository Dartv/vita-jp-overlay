/* region.ini: the OCR region, shared by all games. One line
 * "region = x,y,w,h" (0..65535, see VjoRect). */
#ifndef VJO_REGIONS_H
#define VJO_REGIONS_H

#include <stddef.h>

#include "../include/vjo_api.h"

/* Parses region.ini text. Returns 1 and fills *out for a valid region
 * (non-empty and inside the screen), else 0. */
int vjo_region_parse(const char *text, size_t len, VjoRect *out);

/* Writes r's region.ini line ("region = x,y,w,h\n") into dst. Returns its
 * length, or -1 if cap is too small. */
int vjo_region_format(char *dst, size_t cap, const VjoRect *r);

#endif
