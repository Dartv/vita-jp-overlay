#include "client.h"

#include <stddef.h>
#include <string.h>

#include "jiten.h"
#include "jpdb.h"
#include "port.h"
#include "textfilter.h"
#include "utf.h"

typedef struct {
    const VjoLensRequest *lr;
    const VjoJpegSource *src;
    uint8_t *chunk;
    uint32_t chunk_len;
} LensBody;

static int write_lens_body(void *ud, VjoConn *c)
{
    LensBody *b = (LensBody *)ud;
    uint32_t off = 0;
    int rc = vjo_conn_send_all(c, b->lr->prefix, b->lr->prefix_len);
    while (rc == VJO_OK && off < b->src->size) {
        uint32_t n = b->src->size - off;
        if (n > b->chunk_len)
            n = b->chunk_len;
        if (b->src->read(b->src->ud, off, b->chunk, n) < 0)
            return VJO_E_SOURCE;
        rc = vjo_conn_send_all(c, b->chunk, n);
        off += n;
    }
    if (rc == VJO_OK)
        rc = vjo_conn_send_all(c, b->lr->suffix, b->lr->suffix_len);
    return rc;
}

int vjo_lens_ocr(VjoArena *a, const VjoPlatform *p, const VjoJpegSource *src,
                 VjoLensResult *res, const char **text, VjoErr *err)
{
    uint8_t rnd[24];
    VjoLensRequest lr;
    LensBody body;
    VjoHttpRequest req;
    VjoHttpResponse resp;
    char *t;

    memset(err, 0, sizeof(*err));
    p->random(p->ud, rnd, sizeof(rnd));
    if (vjo_lens_build_request(a, rnd, src->width, src->height, src->size, &lr) < 0)
        return err->rc = VJO_E_OOM;
    body.lr = &lr;
    body.src = src;
    body.chunk_len = 4096;
    body.chunk = (uint8_t *)vjo_arena_alloc(a, body.chunk_len);
    if (!body.chunk)
        return err->rc = VJO_E_OOM;

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.host = VJO_LENS_HOST;
    req.path = VJO_LENS_PATH;
    req.content_type = "application/x-protobuf";
    req.extra_headers = "X-Goog-Api-Key: " VJO_LENS_API_KEY "\r\n"
                        "User-Agent: " VJO_LENS_USER_AGENT "\r\n";
    req.body_len = lr.body_len;
    req.write_body = write_lens_body;
    req.ud = &body;

    if (vjo_http_request(a, p, 443, 1, &req, VJO_LENS_MAX_RESPONSE, &resp, err))
        return err->rc;
    if (resp.gzip) {
        err->detail = "gzip response";
        return err->rc = VJO_E_PARSE;
    }
    if (vjo_lens_parse_response(a, (const uint8_t *)resp.body, resp.body_len, res) < 0)
        return err->rc = VJO_E_PARSE;
    t = vjo_lens_text(a, res);
    if (!t)
        return err->rc = VJO_E_OOM;
    *text = t;
    return VJO_OK;
}

typedef struct {
    const char *json;
    size_t len;
} JsonBody;

static int write_json_body(void *ud, VjoConn *c)
{
    JsonBody *b = (JsonBody *)ud;
    return vjo_conn_send_all(c, b->json, b->len);
}

static const VjoDictBackend backends[VJO_DICT_COUNT] = {
    [VJO_DICT_JPDB] = {VJO_JPDB_HOST, VJO_JPDB_PATH, "Authorization: Bearer %s\r\n",
                       vjo_jpdb_build_request, vjo_jpdb_parse_response, vjo_jpdb_error_message},
    [VJO_DICT_JITEN] = {VJO_JITEN_HOST, VJO_JITEN_PATH, "X-Api-Key: %s\r\n",
                        vjo_jiten_build_request, vjo_jiten_parse_response, vjo_jiten_error_message},
};

const VjoDictBackend *vjo_dict_backend(int dictionary)
{
    return &backends[dictionary >= 0 && dictionary < VJO_DICT_COUNT ? dictionary : VJO_DICT_JPDB];
}

int vjo_dict_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *text,
                    VjoDictResult *res, VjoErr *err)
{
    const VjoDictBackend *be = vjo_dict_backend(cfg->dictionary);
    const char *key = vjo_config_api_key(cfg);
    VjoHttpRequest req;
    VjoHttpResponse resp;
    JsonBody body;
    char auth[192];

    memset(err, 0, sizeof(*err));
    memset(res, 0, sizeof(*res));
    err->dict = cfg->dictionary;
    if (!*key)
        return err->rc = VJO_E_NO_KEY;
    body.json = be->build_request(a, text);
    if (!body.json)
        return err->rc = VJO_E_OOM;
    body.len = strlen(body.json);
    vjo_snprintf(auth, sizeof(auth), be->auth_header, key);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.host = be->host;
    req.path = be->path;
    req.content_type = "application/json; charset=utf-8";
    req.extra_headers = auth;
    req.body_len = body.len;
    req.write_body = write_json_body;
    req.ud = &body;

    if (vjo_http_request(a, p, 443, 1, &req, VJO_DICT_MAX_RESPONSE, &resp, err)) {
        if (err->rc == VJO_E_STATUS && resp.body)
            err->detail = be->error_message(a, resp.body, resp.body_len);
        return err->rc;
    }
    if (be->parse_response(a, resp.body, resp.body_len, res) < 0)
        return err->rc = VJO_E_PARSE;
    return VJO_OK;
}

static int is_blank(const char *s)
{
    for (; *s; s++)
        if ((uint8_t)*s > 0x20)
            return 0;
    return 1;
}

/* Sets ocr_text, filtered and sentence (part of the OCR stage). */
static int filter_text(VjoArena *a, const VjoConfig *cfg, const char *ocr_text, VjoOverlayData *out)
{
    char *copy;
    out->ocr_text = ocr_text;
    out->filtered = vjo_filter_lines(a, ocr_text, cfg->non_japanese_filter);
    copy = out->filtered ? vjo_arena_strndup(a, out->filtered, strlen(out->filtered)) : NULL;
    if (!copy) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc = VJO_E_OOM;
    }
    out->sentence = vjo_java_trim(copy); /* as vjo_entries_build trims the header */
    return VJO_OK;
}

int vjo_overlay_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, VjoOverlayData *out)
{
    VjoDictResult jr;
    char *stripped;

    if (!out->filtered) { /* no successful vjo_overlay_ocr */
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = out->err.rc ? out->err.rc : VJO_E_PARSE;
    }
    memset(&jr, 0, sizeof(jr));
    stripped = vjo_strip_newlines(a, out->filtered);
    if (!stripped) {
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = VJO_E_OOM;
    }
    /* Blank text: no lookup, nothing to show. */
    if (!is_blank(stripped)) {
        if (vjo_dict_lookup(a, p, cfg, stripped, &jr, &out->err)) {
            out->failed_stage = VJO_STAGE_DICT;
            memset(&jr, 0, sizeof(jr));
        }
    }
    if (vjo_entries_build(a, out->filtered, &jr, &out->list) < 0) {
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = VJO_E_OOM;
    }
    return out->err.rc;
}

int vjo_overlay_from_text(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const char *ocr_text, VjoOverlayData *out)
{
    if (filter_text(a, cfg, ocr_text, out))
        return out->err.rc;
    return vjo_overlay_lookup(a, p, cfg, out);
}

int vjo_overlay_ocr(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                    const VjoJpegSource *src, VjoOverlayData *out)
{
    VjoLensResult lr;
    const char *text = NULL;
    memset(out, 0, sizeof(*out));
    out->list.header = "";
    if (vjo_lens_ocr(a, p, src, &lr, &text, &out->err)) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc;
    }
    return filter_text(a, cfg, text, out);
}

int vjo_overlay_from_jpeg(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const VjoJpegSource *src, VjoOverlayData *out)
{
    if (vjo_overlay_ocr(a, p, cfg, src, out))
        return out->err.rc;
    return vjo_overlay_lookup(a, p, cfg, out);
}

void vjo_overlay_warm(const VjoPlatform *p, int dictionary, int timeout_us)
{
    if (vjo_net_warm(p, VJO_LENS_HOST, 443, timeout_us) == VJO_OK && dictionary >= 0)
        vjo_net_warm(p, vjo_dict_backend(dictionary)->host, 443, timeout_us);
}

const char *vjo_err_text(VjoArena *a, int stage, const VjoErr *err)
{
    const VjoDictInfo *di = vjo_dict_info(err->dict);
    const char *dict = di->name, *key = di->key_setting;
    char what[48];
    VjoBuf b;
    if (stage == VJO_STAGE_OCR)
        vjo_snprintf(what, sizeof(what), "Text recognition (Google Lens)");
    else
        vjo_snprintf(what, sizeof(what), "%s lookup", dict);
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK:
        return "";
    case VJO_E_NO_KEY:
        vjo_buf_printf(&b, "%s API key is not set. Add %s to ux0:data/VitaJPOverlay/config.ini", dict, key);
        break;
    case VJO_E_NET:
        vjo_buf_printf(&b, "%s failed: no network connection", what);
        break;
    case VJO_E_TLS:
        vjo_buf_printf(&b, "%s failed: secure connection error (TLS %d). Check the date/time.",
                       what, err->tls_error);
        break;
    case VJO_E_OOM:
        vjo_buf_printf(&b, "%s failed: not enough memory", what);
        break;
    case VJO_E_TOO_LARGE:
        vjo_buf_printf(&b, "%s failed: response too large", what);
        break;
    case VJO_E_STATUS:
        if (stage == VJO_STAGE_DICT && (err->http_status == 401 || err->http_status == 403))
            vjo_buf_printf(&b, "%s rejected the API key (HTTP %d). Check %s in config.ini", dict,
                           err->http_status, key);
        else if (stage == VJO_STAGE_DICT && err->http_status == 429)
            vjo_buf_printf(&b, "%s rate limit reached (HTTP 429). Try again shortly.", dict);
        else
            vjo_buf_printf(&b, "%s failed: HTTP %d", what, err->http_status);
        if (err->detail) {
            vjo_buf_puts(&b, " - ");
            vjo_buf_puts(&b, err->detail);
        }
        break;
    default:
        vjo_buf_printf(&b, "%s failed: unexpected response (%d)", what, err->rc);
        if (err->detail) {
            vjo_buf_puts(&b, " - ");
            vjo_buf_puts(&b, err->detail);
        }
        break;
    }
    return vjo_buf_cstr(&b);
}
