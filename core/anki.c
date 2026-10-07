#include "anki.h"

#include <string.h>

#include "base64.h"
#include "json.h"
#include "port.h"
#include "utf.h"

/* A computer that is off: give up quickly (the default is ~20 s). */
#define CONNECT_TIMEOUT_US (3 * 1000 * 1000)

/* AnkiConnect's error messages (its createNote), matched by prefix. */
#define ERR_DUPLICATE "cannot create note because it is a duplicate"
#define ERR_EMPTY "cannot create note because it is empty"
#define ERR_NO_MODEL "model was not found: "
#define ERR_NO_DECK "deck was not found: "

static int starts_with(const char *s, const char *prefix)
{
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ---------------- field values ---------------- */

static void html_escape(VjoBuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': vjo_buf_puts(b, "&amp;"); break;
        case '<': vjo_buf_puts(b, "&lt;"); break;
        case '>': vjo_buf_puts(b, "&gt;"); break;
        case '"': vjo_buf_puts(b, "&quot;"); break;
        default: vjo_buf_putc(b, s[i]); break;
        }
    }
}

/* Escaped, with line breaks as <br> (CR dropped). */
static void html_text(VjoBuf *b, const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '\n' || s[i] == '\r') {
            html_escape(b, s + start, i - start);
            if (i < n && s[i] == '\n')
                vjo_buf_puts(b, "<br>");
            start = i + 1;
        }
    }
}

void vjo_anki_sentence(VjoBuf *b, const char *text, int hl_start, int hl_end)
{
    size_t n = strlen(text);
    if (hl_start < 0 || hl_end <= hl_start || (size_t)hl_end > n) {
        html_text(b, text, n);
        return;
    }
    html_text(b, text, (size_t)hl_start);
    vjo_buf_puts(b, "<b>");
    html_text(b, text + hl_start, (size_t)(hl_end - hl_start));
    vjo_buf_puts(b, "</b>");
    html_text(b, text + hl_end, n - (size_t)hl_end);
}

static int is_kanji(uint32_t cp)
{
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x3134F) || cp == 0x3005 /* 々 */;
}

static int has_kanji(const char *s)
{
    size_t n = strlen(s), i = 0;
    while (i < n)
        if (is_kanji(vjo_utf8_next(s, n, &i)))
            return 1;
    return 0;
}

void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading)
{
    html_escape(b, spelling, strlen(spelling));
    if (reading && *reading && strcmp(spelling, reading) != 0 && has_kanji(spelling)) {
        vjo_buf_putc(b, '[');
        html_escape(b, reading, strlen(reading));
        vjo_buf_putc(b, ']');
    }
}

void vjo_anki_definition(VjoBuf *b, const char *const *meanings, int n)
{
    if (n <= 0)
        return;
    vjo_buf_puts(b, "<ol>");
    for (int i = 0; i < n; i++) {
        vjo_buf_puts(b, "<li>");
        html_escape(b, meanings[i], strlen(meanings[i]));
        vjo_buf_puts(b, "</li>");
    }
    vjo_buf_puts(b, "</ol>");
}

static char *dup(VjoArena *a, const char *s)
{
    return vjo_arena_strndup(a, s ? s : "", s ? strlen(s) : 0);
}

int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out)
{
    const VjoEntry *e;
    const VjoVocab *v;
    char **m;
    if (!l || entry < 0 || entry >= l->n_entries)
        return -1;
    e = &l->entries[entry];
    v = e->vocab;
    out->spelling = dup(a, v->spelling);
    out->reading = dup(a, v->reading);
    out->sentence = dup(a, l->header);
    out->rank = v->rank;
    out->hl_start = e->hl_start;
    out->hl_end = e->hl_end;
    out->n_meanings = 0;
    m = (char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(v->n_meanings ? v->n_meanings : 1));
    if (!out->spelling || !out->reading || !out->sentence || !m)
        return -1;
    for (int i = 0; i < v->n_meanings; i++)
        if (!(m[i] = dup(a, v->meanings[i])))
            return -1;
    out->meanings = (const char **)m;
    out->n_meanings = v->n_meanings;
    return 0;
}

/* ---------------- requests ---------------- */

static size_t body_len(const VjoAnkiBody *b)
{
    return b->prefix_len + (b->picture_len ? vjo_base64_len(b->picture_len) : 0) + b->suffix_len;
}

#define VERSION_REQUEST "{\"action\":\"version\",\"version\":6}"

static char *create_deck_request(VjoArena *a, const char *deck)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":");
    vjo_json_write_string(&b, deck);
    vjo_buf_puts(&b, "}}");
    return b.oom ? NULL : vjo_buf_cstr(&b);
}

/* "deckName":..,"modelName":.. */
static void put_target(VjoBuf *b, const VjoConfig *cfg)
{
    vjo_buf_puts(b, "\"deckName\":");
    vjo_json_write_string(b, cfg->anki_deck);
    vjo_buf_puts(b, ",\"modelName\":");
    vjo_json_write_string(b, cfg->anki_note_type);
}

/* Duplicates are refused, checked in the target deck only. */
static void put_options(VjoBuf *b, const VjoConfig *cfg)
{
    vjo_buf_puts(b, ",\"options\":{\"allowDuplicate\":false,\"duplicateScope\":\"deck\","
                    "\"duplicateScopeOptions\":{\"deckName\":");
    vjo_json_write_string(b, cfg->anki_deck);
    vjo_buf_puts(b, ",\"checkChildren\":false,\"checkAllModels\":false}}");
}

char *vjo_anki_can_add_request(VjoArena *a, const VjoConfig *cfg, const VjoEntryList *l, int n)
{
    const char *word = cfg->anki_field[VJO_ANKI_WORD];
    VjoBuf b;
    if (!*word || !l)
        return NULL;
    if (n > l->n_entries)
        n = l->n_entries;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"action\":\"canAddNotesWithErrorDetail\",\"version\":6,\"params\":{\"notes\":[");
    for (int i = 0; i < n; i++) {
        if (i)
            vjo_buf_putc(&b, ',');
        vjo_buf_putc(&b, '{');
        put_target(&b, cfg);
        vjo_buf_puts(&b, ",\"fields\":{");
        vjo_json_write_string(&b, word);
        vjo_buf_putc(&b, ':');
        vjo_json_write_string(&b, l->entries[i].vocab->spelling);
        vjo_buf_putc(&b, '}');
        put_options(&b, cfg);
        vjo_buf_putc(&b, '}');
    }
    vjo_buf_puts(&b, "]}}");
    return b.oom ? NULL : vjo_buf_cstr(&b);
}

/* "tags":[...] from tags separated by spaces. */
static void put_tags(VjoBuf *b, const char *t)
{
    int k = 0;
    vjo_buf_puts(b, "\"tags\":[");
    for (;;) {
        size_t len;
        while (*t == ' ' || *t == '\t')
            t++;
        for (len = 0; t[len] && t[len] != ' ' && t[len] != '\t'; len++)
            ;
        if (!len)
            break;
        if (k++)
            vjo_buf_putc(b, ',');
        vjo_json_write_stringn(b, t, len);
        t += len;
    }
    vjo_buf_putc(b, ']');
}

static int is_media(int field)
{
    return field == VJO_ANKI_PICTURE || field == VJO_ANKI_AUDIO;
}

/* The HTML value of a VJO_ANKI_* field that is not media. */
static char *value(VjoArena *a, int field, const VjoAnkiNote *n)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (field) {
    case VJO_ANKI_WORD: html_escape(&b, n->spelling, strlen(n->spelling)); break;
    case VJO_ANKI_READING: html_escape(&b, n->reading, strlen(n->reading)); break;
    case VJO_ANKI_FURIGANA: vjo_anki_furigana(&b, n->spelling, n->reading); break;
    case VJO_ANKI_DEFINITION: vjo_anki_definition(&b, n->meanings, n->n_meanings); break;
    case VJO_ANKI_SENTENCE: vjo_anki_sentence(&b, n->sentence, n->hl_start, n->hl_end); break;
    case VJO_ANKI_FREQUENCY:
        if (n->rank != VJO_NO_RANK)
            vjo_buf_printf(&b, "%d", n->rank);
        break;
    default: break;
    }
    return vjo_buf_cstr(&b);
}

/* "audio"/"picture":[{"filename":..,"fields":[field], then the source */
static void put_media(VjoBuf *b, const char *kind, const char *name, const char *field)
{
    vjo_buf_printf(b, ",\"%s\":[{\"filename\":", kind);
    vjo_json_write_string(b, name);
    vjo_buf_puts(b, ",\"fields\":[");
    vjo_json_write_string(b, field);
    vjo_buf_puts(b, "],");
}

int vjo_anki_add_request(VjoArena *a, const VjoConfig *cfg, const VjoAnkiNote *n, const VjoAnkiMedia *m,
                         VjoAnkiBody *out)
{
    char *v[VJO_ANKI_FIELD_COUNT];
    const char *pic = cfg->anki_field[VJO_ANKI_PICTURE], *audio = cfg->anki_field[VJO_ANKI_AUDIO];
    int first = 1;
    VjoBuf b;
    memset(out, 0, sizeof(*out));
    /* Values first, so the request below grows in place. */
    for (int f = 0; f < VJO_ANKI_FIELD_COUNT; f++) {
        v[f] = NULL;
        if (!is_media(f) && cfg->anki_field[f][0] && !(v[f] = value(a, f, n)))
            return -1;
        if (v[f] && !*v[f])
            v[f] = NULL; /* nothing to add (no rank, no meanings) */
    }
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{");
    put_target(&b, cfg);
    vjo_buf_puts(&b, ",\"fields\":{");
    for (int f = 0; f < VJO_ANKI_FIELD_COUNT; f++) {
        if (!v[f])
            continue;
        if (!first)
            vjo_buf_putc(&b, ',');
        first = 0;
        vjo_json_write_string(&b, cfg->anki_field[f]);
        vjo_buf_putc(&b, ':');
        vjo_json_write_string(&b, v[f]);
    }
    vjo_buf_puts(&b, "},");
    put_tags(&b, cfg->anki_tags);
    put_options(&b, cfg);
    if (m->audio_url && m->audio_name && *audio) {
        put_media(&b, "audio", m->audio_name, audio);
        vjo_buf_puts(&b, "\"url\":");
        vjo_json_write_string(&b, m->audio_url);
        vjo_buf_puts(&b, "}]");
    }
    if (m->picture_len && m->picture_name && *pic) {
        put_media(&b, "picture", m->picture_name, pic);
        vjo_buf_puts(&b, "\"data\":\"");
        out->picture = m->picture;
        out->picture_len = m->picture_len;
        out->suffix = "\"}]}}}";
    } else {
        vjo_buf_puts(&b, "}}}");
        out->suffix = "";
    }
    if (b.oom)
        return -1;
    out->prefix = vjo_buf_cstr(&b);
    if (!out->prefix)
        return -1;
    out->prefix_len = b.len;
    out->suffix_len = strlen(out->suffix);
    return 0;
}

void vjo_anki_media_name(char *out, size_t cap, uint64_t unix_ms, const char *ext)
{
    vjo_snprintf(out, cap, "vitajp_%u%03u.%s", (unsigned)(unix_ms / 1000u), (unsigned)(unix_ms % 1000u), ext);
}

/* ---------------- word audio ---------------- */

#define AUDIO_CONNECT_TIMEOUT_US (3 * 1000 * 1000)
#define AUDIO_IO_TIMEOUT_US (5 * 1000 * 1000) /* a stalled source: not the default 20 s */
#define AUDIO_NAME_CAP 40

int vjo_anki_audio_enabled(const VjoConfig *cfg)
{
    return cfg->anki_audio_url[0] && cfg->anki_field[VJO_ANKI_AUDIO][0];
}

static void url_encode(VjoBuf *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            vjo_buf_putc(b, (char)c);
        } else {
            vjo_buf_putc(b, '%');
            vjo_buf_putc(b, hex[c >> 4]);
            vjo_buf_putc(b, hex[c & 15]);
        }
    }
}

char *vjo_anki_audio_path(VjoArena *a, const char *path_template, const char *term, const char *reading)
{
    const char *t = path_template;
    VjoBuf b;
    if (!reading || !*reading)
        reading = term;
    vjo_buf_init(&b, a);
    if (*t != '/')
        vjo_buf_putc(&b, '/');
    while (*t) {
        if (!strncmp(t, "{term}", 6)) {
            url_encode(&b, term);
            t += 6;
        } else if (!strncmp(t, "{reading}", 9)) {
            url_encode(&b, reading);
            t += 9;
        } else if (!strncmp(t, "{language}", 10)) {
            vjo_buf_puts(&b, "ja");
            t += 10;
        } else {
            vjo_buf_putc(&b, *t++);
        }
    }
    return b.oom ? NULL : vjo_buf_cstr(&b);
}

static int is_http_url(const char *s)
{
    return starts_with(s, "https://") || starts_with(s, "http://");
}

/* {"type": "audioSourceList", "audioSources": [{"name": ..., "url": ...}, ...]} */
int vjo_anki_audio_parse(VjoArena *a, const char *json, size_t len, const char **url)
{
    VjoJson j;
    int list;
    *url = NULL;
    if (vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT))
        return VJO_E_PARSE;
    list = vjo_json_obj_get(&j, 0, "audioSources");
    if (!vjo_json_is_type(&j, list, JSMN_ARRAY))
        return VJO_E_PARSE;
    for (int k = 0; k < j.t[list].size; k++) {
        int u = vjo_json_obj_get(&j, vjo_json_arr_get(&j, list, k), "url");
        const char *s;
        if (!vjo_json_is_type(&j, u, JSMN_STRING))
            continue;
        if (!(s = vjo_json_str(a, &j, u)))
            return VJO_E_OOM;
        if (is_http_url(s)) {
            *url = s;
            break;
        }
    }
    return VJO_OK;
}

const char *vjo_anki_audio_ext(const char *url)
{
    static const char *const known[] = {"mp3", "ogg", "opus", "oga", "m4a", "aac", "wav", "flac", "webm"};
    const char *path = strstr(url, "://"), *ext = NULL;
    char e[8];
    size_t n = 0;
    path = path ? path + 3 : url;
    while (*path && *path != '/') /* past the host */
        path++;
    for (; *path && *path != '?' && *path != '#'; path++) {
        if (*path == '/')
            ext = NULL;
        else if (*path == '.')
            ext = path + 1;
    }
    if (!ext)
        return "mp3";
    while (ext + n < path && n < sizeof(e) - 1) {
        char c = ext[n];
        e[n++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    e[n] = '\0';
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (ext + n == path && !strcmp(e, known[i]))
            return known[i];
    return "mp3";
}

/* ---------------- replies ---------------- */

/* {"result": ..., "error": ...}: VJO_OK with the result token, or
 * VJO_E_ANKI_DUPLICATE / VJO_E_ANKI with the error in *error. */
static int parse_reply(VjoArena *a, const char *json, size_t len, VjoJson *j, int *result, const char **error)
{
    int r, e;
    *error = NULL;
    if (vjo_json_parse(a, json, len, j) < 0 || !vjo_json_is_type(j, 0, JSMN_OBJECT))
        return VJO_E_PARSE;
    r = vjo_json_obj_get(j, 0, "result");
    e = vjo_json_obj_get(j, 0, "error");
    if (r < 0 || e < 0)
        return VJO_E_PARSE; /* not AnkiConnect */
    if (!vjo_json_is_null(j, e)) {
        if (!(*error = vjo_json_str(a, j, e)))
            return VJO_E_OOM;
        return starts_with(*error, ERR_DUPLICATE) ? VJO_E_ANKI_DUPLICATE : VJO_E_ANKI;
    }
    *result = r;
    return VJO_OK;
}

/* canAddNotesWithErrorDetail: [{"canAdd": bool, "error"?: string}, ...] */
static int parse_can_add(VjoArena *a, const char *json, size_t len, uint8_t *marks, int n, const char **error)
{
    VjoJson j;
    int r, rc = parse_reply(a, json, len, &j, &r, error), i, k;
    if (rc)
        return rc;
    if (!vjo_json_is_type(&j, r, JSMN_ARRAY) || j.t[r].size != n)
        return VJO_E_PARSE;
    for (k = 0, i = r + 1; k < n; k++, i = vjo_json_skip(&j, i)) {
        int can_add, e = vjo_json_obj_get(&j, i, "error");
        if (vjo_json_bool(&j, vjo_json_obj_get(&j, i, "canAdd"), &can_add) < 0)
            return VJO_E_PARSE;
        marks[k] = !can_add && vjo_json_is_type(&j, e, JSMN_STRING) &&
                   starts_with(vjo_json_str(a, &j, e), ERR_DUPLICATE);
    }
    return VJO_OK;
}

/* ---------------- calls ---------------- */

typedef struct {
    const VjoAnkiBody *b;
    char *chunk; /* base64 of up to CHUNK_IN bytes */
} BodyWriter;

#define CHUNK_IN 3072 /* a multiple of 3: only the last chunk is padded */

static int write_anki_body(void *ud, VjoConn *c)
{
    BodyWriter *w = (BodyWriter *)ud;
    const VjoAnkiBody *b = w->b;
    int rc = vjo_conn_send_all(c, b->prefix, b->prefix_len);
    for (size_t off = 0; rc == VJO_OK && off < b->picture_len; off += CHUNK_IN) {
        size_t n = b->picture_len - off < CHUNK_IN ? b->picture_len - off : CHUNK_IN;
        vjo_base64_encode(b->picture + off, n, w->chunk);
        rc = vjo_conn_send_all(c, w->chunk, vjo_base64_len(n));
    }
    if (rc == VJO_OK && b->suffix_len)
        rc = vjo_conn_send_all(c, b->suffix, b->suffix_len);
    return rc;
}

/* POST / to AnkiConnect; the reply is in resp. */
static int anki_post(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiBody *body,
                     VjoHttpResponse *resp, VjoErr *err)
{
    VjoHttpRequest req;
    BodyWriter w;
    memset(err, 0, sizeof(*err));
    w.b = body;
    w.chunk = NULL;
    if (body->picture_len && !(w.chunk = (char *)vjo_arena_alloc(a, vjo_base64_len(CHUNK_IN))))
        return err->rc = VJO_E_OOM;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.host = host;
    req.path = "/";
    req.content_type = "application/json";
    req.body_len = body_len(body);
    req.write_body = write_anki_body;
    req.ud = &w;
    req.connect_timeout_us = CONNECT_TIMEOUT_US;
    return vjo_http_request(a, p, port, 0, &req, VJO_ANKI_MAX_RESPONSE, resp, err);
}

static void text_body(VjoAnkiBody *b, const char *json)
{
    memset(b, 0, sizeof(*b));
    b->prefix = json;
    b->prefix_len = strlen(json);
}

/* One action; its result token in *result. */
static int call(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoAnkiBody *body, VjoJson *j,
                int *result, VjoErr *err)
{
    VjoHttpResponse resp;
    const char *error;
    if (anki_post(a, p, host, port, body, &resp, err))
        return err->rc;
    err->rc = parse_reply(a, resp.body, resp.body_len, j, result, &error);
    err->detail = error;
    return err->rc;
}

int vjo_anki_probe(VjoArena *a, const VjoPlatform *p, const char *host, int port, VjoErr *err)
{
    VjoAnkiBody b;
    VjoJson j;
    int r;
    long v;
    size_t mark = vjo_arena_mark(a);
    text_body(&b, VERSION_REQUEST);
    if (call(a, p, host, port, &b, &j, &r, err) == VJO_OK && (vjo_json_int(&j, r, &v) < 0 || v != 6))
        err->rc = VJO_E_PARSE;
    vjo_arena_release(a, mark);
    return err->rc;
}

int vjo_anki_check(VjoArena *a, const VjoPlatform *p, const char *host, int port, const char *request,
                   uint8_t *marks, int n, VjoErr *err)
{
    VjoAnkiBody b;
    VjoHttpResponse resp;
    const char *error;
    text_body(&b, request);
    if (anki_post(a, p, host, port, &b, &resp, err))
        return err->rc;
    err->rc = parse_can_add(a, resp.body, resp.body_len, marks, n, &error);
    err->detail = error;
    return err->rc;
}

int vjo_anki_add(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoConfig *cfg,
                 const VjoAnkiNote *n, const VjoAnkiMedia *m, VjoErr *err)
{
    VjoAnkiBody note, deck;
    VjoJson j;
    int r;
    char *create;
    memset(err, 0, sizeof(*err));
    if (vjo_anki_add_request(a, cfg, n, m, &note) < 0)
        return err->rc = VJO_E_OOM;
    if (call(a, p, host, port, &note, &j, &r, err) != VJO_E_ANKI || !starts_with(err->detail, ERR_NO_DECK))
        return err->rc;
    /* The first note of a new deck. */
    if (!(create = create_deck_request(a, cfg->anki_deck)))
        return err->rc = VJO_E_OOM;
    text_body(&deck, create);
    if (call(a, p, host, port, &deck, &j, &r, err))
        return err->rc;
    return call(a, p, host, port, &note, &j, &r, err);
}

static int audio_lookup(VjoArena *a, const VjoPlatform *p, const char *audio_url, const VjoAnkiNote *n,
                        const char **url, VjoErr *err)
{
    VjoHttpRequest req;
    VjoHttpResponse resp;
    const char *path;
    char host[VJO_HOST_MAX];
    int port, tls;
    if (vjo_anki_audio_endpoint(audio_url, host, &port, &tls, &path) < 0)
        return err->rc = VJO_E_PARSE;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.host = host;
    req.connect_timeout_us = AUDIO_CONNECT_TIMEOUT_US;
    req.io_timeout_us = AUDIO_IO_TIMEOUT_US;
    if (!(req.path = vjo_anki_audio_path(a, path, n->spelling, n->reading)))
        return err->rc = VJO_E_OOM;
    if (vjo_http_request(a, p, port, tls, &req, VJO_ANKI_AUDIO_MAX_RESPONSE, &resp, err))
        return err->rc;
    return err->rc = vjo_anki_audio_parse(a, resp.body, resp.body_len, url);
}

int vjo_anki_find_audio(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const VjoAnkiNote *n,
                        uint64_t unix_ms, VjoAnkiMedia *m, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a), len;
    const char *url = NULL;
    char *keep;
    memset(err, 0, sizeof(*err));
    m->audio_url = m->audio_name = NULL;
    if (audio_lookup(a, p, cfg->anki_audio_url, n, &url, err) || !url) {
        vjo_arena_release(a, mark);
        return err->rc;
    }
    /* Keep just the URL: the reply and its tokens go back to the arena.
     * The copy lands at or below the URL, so memmove. */
    len = strlen(url);
    vjo_arena_release(a, mark);
    if (!(keep = (char *)vjo_arena_alloc(a, len + 1 + AUDIO_NAME_CAP)))
        return err->rc = VJO_E_OOM;
    memmove(keep, url, len + 1);
    vjo_anki_media_name(keep + len + 1, AUDIO_NAME_CAP, unix_ms, vjo_anki_audio_ext(keep));
    m->audio_url = keep;
    m->audio_name = keep + len + 1;
    return VJO_OK;
}

const char *vjo_anki_audio_err_text(VjoArena *a, const VjoErr *err)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK: return "no recording";
    case VJO_E_NET: return "audio source offline";
    case VJO_E_OOM: return "not enough memory";
    case VJO_E_TOO_LARGE: return "reply too large";
    case VJO_E_PARSE: return "not an audio source list";
    case VJO_E_HTTP: return "bad HTTP reply, or the word is too long";
    case VJO_E_STATUS: vjo_buf_printf(&b, "HTTP %d", err->http_status); break;
    case VJO_E_TLS: vjo_buf_printf(&b, "TLS error %d", err->tls_error); break;
    default: vjo_buf_printf(&b, "unexpected reply %d", err->rc); break;
    }
    return vjo_buf_cstr(&b);
}

const char *vjo_anki_err_text(VjoArena *a, const VjoErr *err)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK:
        return "";
    case VJO_E_NET:
        return "Anki offline";
    case VJO_E_NOT_FOUND:
        return "Anki not found on the network";
    case VJO_E_OOM:
        return "Anki: not enough memory";
    case VJO_E_ANKI_DUPLICATE:
        return "Already in Anki";
    case VJO_E_STATUS:
        vjo_buf_printf(&b, "Anki refused the request (HTTP %d)", err->http_status);
        break;
    case VJO_E_ANKI:
        if (starts_with(err->detail, ERR_NO_MODEL)) {
            vjo_buf_puts(&b, "Note type \"");
            vjo_buf_puts(&b, err->detail + strlen(ERR_NO_MODEL));
            vjo_buf_puts(&b, "\" not found in Anki (anki_note_type)");
        } else if (starts_with(err->detail, ERR_EMPTY)) {
            vjo_buf_puts(&b, "Anki: the note type's first field is empty (check anki_field_word)");
        } else {
            vjo_buf_puts(&b, "Anki: ");
            vjo_buf_puts(&b, err->detail ? err->detail : "error");
        }
        break;
    default:
        vjo_buf_printf(&b, "Anki: unexpected response (%d). Is AnkiConnect running?", err->rc);
        break;
    }
    return vjo_buf_cstr(&b);
}

int vjo_anki_host_stale(int rc)
{
    return rc != VJO_OK && rc != VJO_E_ANKI && rc != VJO_E_ANKI_DUPLICATE && rc != VJO_E_OOM;
}

/* ---------------- network search ---------------- */

int vjo_anki_scan_candidates(uint32_t ip, uint32_t mask, uint32_t *out, int cap)
{
    uint32_t m = mask | 0xFFFFFF00u, net, bcast; /* at most a /24 */
    int n = 0;
    if (~m & (~m + 1)) /* not a contiguous mask */
        m = 0xFFFFFF00u;
    net = ip & m;
    bcast = net | ~m;
    for (uint32_t h = net + 1; h < bcast && n < cap; h++)
        if (h != ip)
            out[n++] = h;
    return n;
}
