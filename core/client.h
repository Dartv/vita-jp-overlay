/* End-to-end requests: Lens OCR and dictionary lookup over HTTPS, plus the
 * overlay pipeline that ties them together (same code on host and Vita). */
#ifndef VJO_CLIENT_H
#define VJO_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "config.h"
#include "entries.h"
#include "dict.h"
#include "lens.h"
#include "net.h"


typedef struct {
    void *ud;
    /* Copies len bytes at offset off; returns 0 or < 0 on failure. */
    int (*read)(void *ud, uint32_t off, void *dst, uint32_t len);
    uint32_t size;
    uint32_t width, height;
} VjoJpegSource;


/* Stages for VjoOverlayData.failed_stage */
enum { VJO_STAGE_NONE = 0, VJO_STAGE_OCR = 1, VJO_STAGE_DICT = 2 }; /* OCR, then dictionary */

typedef struct {
    const char *ocr_text;   /* Lens text (vjo_lens_text) */
    const char *filtered;   /* after non_japanese_filter */
    const char *sentence;   /* filtered, trimmed: the header's text (and the subtitle) */
    VjoEntryList list;      /* header + entries */
    int failed_stage;
    VjoErr err;
} VjoOverlayData;

#define VJO_LENS_MAX_RESPONSE (256u * 1024u)
#define VJO_DICT_MAX_RESPONSE (192u * 1024u)

int vjo_lens_ocr(VjoArena *a, const VjoPlatform *p, const VjoJpegSource *src,
                 VjoLensResult *res, const char **text, VjoErr *err);

/* A dictionary service's network side; vjo_dict_backend(cfg->dictionary) is
 * the selected one (names and settings: VjoDictInfo in config.h). */
typedef struct {
    const char *host, *path;
    const char *auth_header; /* "Name: ...%s...\r\n", %s = API key */
    char *(*build_request)(VjoArena *a, const char *text);
    int (*parse_response)(VjoArena *a, const char *json, size_t len, VjoDictResult *out);
    const char *(*error_message)(VjoArena *a, const char *json, size_t len);
} VjoDictBackend;

/* Backend for a VJO_DICT_* value (jpdb for an out-of-range one). */
const VjoDictBackend *vjo_dict_backend(int dictionary);

/* Looks up text with the dictionary selected in cfg. */
int vjo_dict_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *text,
                    VjoDictResult *res, VjoErr *err);

/* A recognition in two phases (the sentence is ready before the lookup):
 * vjo_overlay_ocr: JPEG -> Lens -> filter, sets ocr_text, filtered and
 * sentence; vjo_overlay_lookup: dictionary -> entries (list), after a
 * successful vjo_overlay_ocr. Both return VJO_OK or the error, also in
 * out->err with out->failed_stage. */
int vjo_overlay_ocr(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                    const VjoJpegSource *src, VjoOverlayData *out);
int vjo_overlay_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, VjoOverlayData *out);

/* OCR text -> filter -> vjo_overlay_lookup. */
int vjo_overlay_from_text(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const char *ocr_text, VjoOverlayData *out);

/* vjo_overlay_ocr, then vjo_overlay_lookup. */
int vjo_overlay_from_jpeg(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const VjoJpegSource *src, VjoOverlayData *out);

/* Short user-facing description of an error. */
const char *vjo_err_text(VjoArena *a, int stage, const VjoErr *err);

#endif
