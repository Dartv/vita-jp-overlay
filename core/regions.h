/* region.ini: OCR regions, one line per game ("PCSG00001 = x,y,w,h", or
 * "PCSG00001 = full" for the full screen) and "region = x,y,w,h" for games
 * without their own (0..65535, see VjoRect; from before regions were per
 * game, never written now). */
#ifndef VJO_REGIONS_H
#define VJO_REGIONS_H

#include <stddef.h>

#include "../include/vjo_api.h"
#include "arena.h"

#define VJO_REGION_FALLBACK "region" /* key of the line for all other games */

/* vjo_region_parse results */
enum { VJO_REGION_NONE = 0, VJO_REGION_GAME = 1, VJO_REGION_ALL = 2 };

/* Finds title_id's region in region.ini text, else the fallback's. Fills
 * *out and returns VJO_REGION_GAME or VJO_REGION_ALL for a valid region
 * (non-empty and inside the screen, or "full": w == 0), else
 * VJO_REGION_NONE. title_id may be NULL (fallback only). */
int vjo_region_parse(const char *text, size_t len, const char *title_id, VjoRect *out);

/* Writes the line "key = x,y,w,h\n" ("key = full\n" for w == 0) into dst.
 * Returns its length, or -1 if cap is too small. */
int vjo_region_format(char *dst, size_t cap, const char *key, const VjoRect *r);

/* region.ini text with key's line replaced by r's (added at the end if
 * there is none); other non-blank lines are kept. Returns the new
 * NUL-terminated text, NULL on OOM. */
char *vjo_region_update(VjoArena *a, const char *text, size_t len, const char *key, const VjoRect *r);

#endif
