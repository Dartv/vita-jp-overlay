/* Overlay rendering as plain text (CLI output and the fixture goldens). */
#ifndef VJO_RENDER_H
#define VJO_RENDER_H

#include "client.h"

/* Overlay as plain text:
 * header, separator, entries separated by blank lines. */
char *vjo_render_overlay(VjoArena *a, const VjoOverlayData *d);

/* Header with the selected entry's highlight shown as 【...】. */
char *vjo_render_highlight(VjoArena *a, const VjoEntryList *l, int entry);

#endif
