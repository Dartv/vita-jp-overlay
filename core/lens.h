/* Google Lens (lensfrontend-pa crupload) request builder and response parser.
 * Field numbers follow Chromium's lens_overlay_*.proto (BSD-3-Clause); the
 * .proto files are not vendored. */
#ifndef VJO_LENS_H
#define VJO_LENS_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

#define VJO_LENS_HOST "lensfrontend-pa.googleapis.com"
#define VJO_LENS_PATH "/v1/crupload"
#define VJO_LENS_API_KEY "AIzaSyDr2UxVnv_U85AbhhY8XSHSIavUW0DC-sY" /* Chromium's public key */
#define VJO_LENS_USER_AGENT                                                            \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like " \
    "Gecko) Chrome/134.0.0.0 Safari/537.36"

typedef struct {
    float cx, cy, w, h, rot; /* normalized to the image, as returned by Lens */
} VjoBox;

typedef struct {
    const char *text;
    const char *sep; /* text_separator, "" if absent */
    VjoBox box;
} VjoLensWord;

typedef struct {
    VjoLensWord *words;
    int n_words;
    VjoBox box;
} VjoLensLine;

typedef struct {
    VjoLensLine *lines;
    int n_lines;
    VjoBox box;
    int writing_direction; /* 0 LTR, 1 RTL, 2 top-to-bottom */
} VjoLensParagraph;

typedef struct {
    VjoLensParagraph *paragraphs;
    int n_paragraphs;
    int server_error; /* LensOverlayServerError.error_type, 0 if none */
} VjoLensResult;

/* Request body = prefix + jpeg bytes + suffix. Splitting it lets the caller
 * stream the JPEG straight into the connection. `rnd` supplies 24 random
 * bytes (8 for the request uuid, 16 for the analytics id). */
typedef struct {
    const uint8_t *prefix;
    size_t prefix_len;
    const uint8_t *suffix;
    size_t suffix_len;
    size_t body_len; /* prefix_len + jpeg_len + suffix_len */
} VjoLensRequest;

int vjo_lens_build_request(VjoArena *a, const uint8_t rnd[24], uint32_t width, uint32_t height,
                           size_t jpeg_len, VjoLensRequest *out);

/* Returns 0 on success, -1 on malformed protobuf / OOM. */
int vjo_lens_parse_response(VjoArena *a, const uint8_t *data, size_t len, VjoLensResult *out);

/* Words joined with their separators, one line per paragraph, then
 * trimmed (vjo_java_trim). */
char *vjo_lens_text(VjoArena *a, const VjoLensResult *r);

#endif
