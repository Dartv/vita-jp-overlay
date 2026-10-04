/* Host replay of recorded responses (vjo-cli --replay/--record and the
 * fixture tests): in-memory connection, fixture file names and a
 * VjoPlatform that serves a fixture dir. */
#ifndef VJO_REPLAY_H
#define VJO_REPLAY_H

#include "client.h"

/* Reads a whole file into the arena (NUL-terminated). */
char *vjo_read_file(VjoArena *a, const char *path, size_t *len);

/* In-memory VjoConn: recv serves `in` (at most `step` bytes per call when
 * step > 0) and then returns end_rc (0 = EOF); send appends to `out` (up to
 * out_cap bytes; discarded when out is NULL). */
typedef struct {
    const char *in;
    size_t pos, len, step;
    char *out;
    size_t out_cap, out_len;
    int end_rc;
} VjoMemConn;

void vjo_memconn_init(VjoMemConn *m, VjoConn *c);

/* Recorded response file in a fixture dir for a request host: "lens.pb",
 * "jpdb.json" or "jiten.json" (--record writes, replay reads). */
const char *vjo_fixture_file(const char *host);

/* Prepares cfg to replay dir: the dictionary whose response was recorded
 * (jiten.json, else jpdb.json, else cfg's) and placeholder API keys (they
 * are never sent), so the output does not depend on the environment. */
void vjo_replay_config(VjoConfig *cfg, const char *dir);

/* vjo_overlay_from_jpeg (empty JPEG) on a VjoPlatform whose connect()
 * serves dir's recording for the host as a plain HTTP 200 response, or
 * fails like an offline network when there is none. The responses are
 * loaded into `files`. Returns vjo_overlay_from_jpeg's result. */
int vjo_replay_overlay(VjoArena *a, VjoArena *files, const char *dir, const VjoConfig *cfg,
                       VjoOverlayData *out);

#endif
