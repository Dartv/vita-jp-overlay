/* Anki: base64, note fields, AnkiConnect requests and replies (over a fake
 * network), word audio, the anki_* settings. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "anki.h"
#include "base64.h"
#include "config.h"
#include "jpdb.h"
#include "replay.h"

static uint8_t g_mem[4u << 20];
static VjoArena A;

static void setup(void)
{
    /* Garbage-fill like a reused arena on the Vita. */
    memset(g_mem, 0xA5, sizeof(g_mem));
    vjo_arena_init(&A, g_mem, sizeof(g_mem));
}

static void test_base64(void)
{
    static const char *const vec[][2] = {{"", ""},        {"f", "Zg=="},        {"fo", "Zm8="},
                                         {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
                                         {"foobar", "Zm9vYmFy"}};
    uint8_t data[1000];
    char one[1400], chunked[1400];
    size_t o = 0;
    for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        char out[16] = {0};
        size_t n = strlen(vec[i][0]);
        TEST_CHECK(vjo_base64_len(n) == strlen(vec[i][1]));
        vjo_base64_encode((const uint8_t *)vec[i][0], n, out);
        TEST_CHECK(!strcmp(out, vec[i][1]));
        TEST_MSG("'%s' -> '%s'", vec[i][0], out);
    }
    /* chunks of a multiple of 3 bytes, then the rest: same as one shot */
    for (int i = 0; i < 1000; i++)
        data[i] = (uint8_t)(i * 37 + 11);
    vjo_base64_encode(data, sizeof(data), one);
    for (size_t off = 0; off < sizeof(data); off += 99) {
        size_t n = sizeof(data) - off < 99 ? sizeof(data) - off : 99;
        vjo_base64_encode(data + off, n, chunked + o);
        o += vjo_base64_len(n);
    }
    TEST_CHECK(o == vjo_base64_len(sizeof(data)) && !memcmp(one, chunked, o));
}

static void test_anki_fields(void)
{
    VjoBuf b;
    char *s;
#define BUF(expr) (setup(), vjo_buf_init(&b, &A), (expr), s = vjo_buf_cstr(&b))
    /* the word in bold, escaped, line breaks as <br>, CR dropped */
    BUF(vjo_anki_sentence(&b, "猫が\r\n好き<3&\"", 0, 3));
    TEST_CHECK(!strcmp(s, "<b>猫</b>が<br>好き&lt;3&amp;&quot;"));
    BUF(vjo_anki_sentence(&b, "猫が好き", 6, 12));
    TEST_CHECK(!strcmp(s, "猫が<b>好き</b>"));
    BUF(vjo_anki_sentence(&b, "猫が\n好き", 3, 7)); /* across a line break */
    TEST_CHECK(!strcmp(s, "猫<b>が<br></b>好き"));
    BUF(vjo_anki_sentence(&b, "𠮷野家", 0, 4)); /* 4-byte code point */
    TEST_CHECK(!strcmp(s, "<b>𠮷</b>野家"));
    BUF(vjo_anki_sentence(&b, "猫", -1, -1));
    TEST_CHECK(!strcmp(s, "猫"));
    BUF(vjo_anki_sentence(&b, "猫", 0, 99)); /* out of range: no bold */
    TEST_CHECK(!strcmp(s, "猫"));

    BUF(vjo_anki_furigana(&b, "言葉", "ことば"));
    TEST_CHECK(!strcmp(s, "言葉[ことば]"));
    BUF(vjo_anki_furigana(&b, "食べる", "たべる"));
    TEST_CHECK(!strcmp(s, "食べる[たべる]"));
    BUF(vjo_anki_furigana(&b, "ある", "ある"));
    TEST_CHECK(!strcmp(s, "ある"));
    BUF(vjo_anki_furigana(&b, "テレビ", "てれび")); /* no kanji */
    TEST_CHECK(!strcmp(s, "テレビ"));
    BUF(vjo_anki_furigana(&b, "時々", "ときどき"));
    TEST_CHECK(!strcmp(s, "時々[ときどき]"));

    {
        const char *m[] = {"to eat", "to live on <sth>"};
        BUF(vjo_anki_definition(&b, m, 2));
        TEST_CHECK(!strcmp(s, "<ol><li>to eat</li><li>to live on &lt;sth&gt;</li></ol>"));
        BUF(vjo_anki_definition(&b, m, 0));
        TEST_CHECK(!strcmp(s, ""));
    }
#undef BUF

    {
        char name[40];
        vjo_anki_media_name(name, sizeof(name), 1791158400007ull, "jpg");
        TEST_CHECK(!strcmp(name, "vitajp_1791158400007.jpg"));
    }
    {
        uint32_t c[300];
        int n = vjo_anki_scan_candidates(0xC0A8011Eu, 0xFFFFFF00u, c, 300); /* 192.168.1.30/24 */
        TEST_CHECK(n == 253 && c[0] == 0xC0A80101u && c[252] == 0xC0A801FEu);
        for (int i = 0; i < n; i++)
            TEST_CHECK(c[i] != 0xC0A8011Eu);
        n = vjo_anki_scan_candidates(0x0A000105u, 0xFFFF0000u, c, 300); /* a /16: its /24 only */
        TEST_CHECK(n == 253 && c[0] == 0x0A000101u);
        n = vjo_anki_scan_candidates(0xC0A80182u, 0xFFFFFF80u, c, 300); /* a /25 */
        TEST_CHECK(n == 125 && c[0] == 0xC0A80181u && c[124] == 0xC0A801FEu);
        TEST_CHECK(vjo_anki_scan_candidates(0xC0A8011Eu, 0xFFFFFF00u, c, 10) == 10);
    }
}

/* A dictionary result for "猫が\n好き" with tricky glosses. */
static void anki_list(VjoEntryList *l)
{
    VjoDictResult r;
    const char *jp = "{\"tokens\":[[1,0,1,null],[0,1,1,null],[2,2,2,null]],\"vocabulary\":["
                     "[1,2,3,\"が\",\"が\",50,[\"indicates subject\"]],"
                     "[10,20,30,\"猫\",\"ねこ\",1500,[\"cat \\\"neko\\\"\",\"a\\\\b <&>\\nc\"]],"
                     "[11,21,31,\"好き\",\"すき\",null,[\"liked\"]]]}";
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    TEST_ASSERT(vjo_entries_build(&A, "猫が\n好き", &r, l) == 0);
}

#define OPTS                                                                                                  \
    "\"options\":{\"allowDuplicate\":false,\"duplicateScope\":\"deck\",\"duplicateScopeOptions\":"            \
    "{\"deckName\":\"Japanese::Mining \\\"1\\\"\",\"checkChildren\":false,\"checkAllModels\":false}}"
#define TARGET "\"deckName\":\"Japanese::Mining \\\"1\\\"\",\"modelName\":\"Lapis\""

static void test_anki_requests(void)
{
    VjoConfig cfg;
    VjoEntryList l;
    VjoAnkiNote n;
    VjoAnkiMedia m = {(const uint8_t *)"jpeg", 4, "vitajp_1.jpg", NULL, NULL};
    VjoAnkiBody b;
    char *s;
    setup();
    vjo_config_defaults(&cfg);
    snprintf(cfg.anki_deck, sizeof(cfg.anki_deck), "Japanese::Mining \"1\"");
    snprintf(cfg.anki_tags, sizeof(cfg.anki_tags), " vita  jp-overlay ");
    anki_list(&l);

    s = vjo_anki_can_add_request(&A, &cfg, &l, 99);
    TEST_CHECK(s && !strcmp(s, "{\"action\":\"canAddNotesWithErrorDetail\",\"version\":6,\"params\":{\"notes\":["
                               "{" TARGET ",\"fields\":{\"Expression\":\"猫\"}," OPTS "},"
                               "{" TARGET ",\"fields\":{\"Expression\":\"が\"}," OPTS "},"
                               "{" TARGET ",\"fields\":{\"Expression\":\"好き\"}," OPTS "}]}}"));
    TEST_MSG("%s", s);
    s = vjo_anki_can_add_request(&A, &cfg, &l, 1); /* the first n only */
    TEST_CHECK(s && strstr(s, "猫") && !strstr(s, "が"));

    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
    TEST_CHECK(vjo_anki_note_from_entry(&A, &l, 3, &n) < 0);
    TEST_ASSERT(vjo_anki_add_request(&A, &cfg, &n, &m, &b) == 0);
    TEST_CHECK(!strcmp(b.prefix,
                       "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET ",\"fields\":{"
                       "\"Expression\":\"猫\",\"ExpressionReading\":\"ねこ\",\"ExpressionFurigana\":\"猫[ねこ]\","
                       "\"MainDefinition\":\"<ol><li>cat &quot;neko&quot;</li><li>a\\\\b &lt;&amp;&gt;\\nc</li></ol>\","
                       "\"Sentence\":\"<b>猫</b>が<br>好き\",\"FreqSort\":\"1500\"},"
                       "\"tags\":[\"vita\",\"jp-overlay\"]," OPTS ","
                       "\"picture\":[{\"filename\":\"vitajp_1.jpg\",\"fields\":[\"Picture\"],\"data\":\""));
    TEST_MSG("%s", b.prefix);
    TEST_CHECK(b.prefix_len == strlen(b.prefix) && !strcmp(b.suffix, "\"}]}}}") && b.suffix_len == 6);
    TEST_CHECK(b.picture == m.picture && b.picture_len == 4);

    /* the word's audio, for Anki to download, before the picture */
    m.audio_url = "https://example.com/a \"b\".opus";
    m.audio_name = "vitajp_1.opus";
    TEST_ASSERT(vjo_anki_add_request(&A, &cfg, &n, &m, &b) == 0);
    TEST_CHECK(strstr(b.prefix, "\"FreqSort\":\"1500\"},") && !strstr(b.prefix, "ExpressionAudio\":\""));
    TEST_CHECK(strstr(b.prefix, OPTS ",\"audio\":[{\"filename\":\"vitajp_1.opus\",\"fields\":[\"ExpressionAudio\"],"
                                "\"url\":\"https://example.com/a \\\"b\\\".opus\"}],"
                                "\"picture\":[{\"filename\":\"vitajp_1.jpg\"") != NULL);
    TEST_MSG("%s", b.prefix);
    cfg.anki_field[VJO_ANKI_AUDIO][0] = '\0'; /* no audio field: no audio */
    TEST_ASSERT(vjo_anki_add_request(&A, &cfg, &n, &m, &b) == 0);
    TEST_CHECK(!strstr(b.prefix, "\"audio\""));

    /* blank fields are skipped; no picture field: no picture; no rank: no frequency */
    cfg.anki_field[VJO_ANKI_READING][0] = '\0';
    cfg.anki_field[VJO_ANKI_FURIGANA][0] = '\0';
    cfg.anki_field[VJO_ANKI_DEFINITION][0] = '\0';
    cfg.anki_field[VJO_ANKI_SENTENCE][0] = '\0';
    cfg.anki_field[VJO_ANKI_PICTURE][0] = '\0';
    cfg.anki_tags[0] = '\0';
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 2, &n) == 0);
    TEST_ASSERT(vjo_anki_add_request(&A, &cfg, &n, &m, &b) == 0);
    TEST_CHECK(!strcmp(b.prefix, "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET
                                 ",\"fields\":{\"Expression\":\"好き\"},\"tags\":[]," OPTS "}}}"));
    TEST_MSG("%s", b.prefix);
    TEST_CHECK(b.suffix_len == 0 && b.picture_len == 0);
    cfg.anki_field[VJO_ANKI_WORD][0] = '\0';
    TEST_CHECK(vjo_anki_can_add_request(&A, &cfg, &l, 3) == NULL);
}

/* ---------- AnkiConnect over a fake network ---------- */

/* A VjoPlatform that serves canned HTTP responses in turn (NULL = nothing
 * listening) and records what was sent. */
typedef struct {
    const char *responses[4];
    int n_responses, next;
    VjoMemConn conn;
    char sent[4][8192];
    int port, timeout_us, io_timeout_us;
} FakeNet;

static int fake_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    FakeNet *f = (FakeNet *)ud;
    int k = f->next;
    (void)host;
    if (k >= f->n_responses || !f->responses[k])
        return VJO_E_NET;
    f->next++;
    f->port = port;
    f->timeout_us = timeout_us;
    f->io_timeout_us = io_timeout_us;
    memset(&f->conn, 0, sizeof(f->conn));
    f->conn.in = f->responses[k];
    f->conn.len = strlen(f->responses[k]);
    f->conn.out = f->sent[k];
    f->conn.out_cap = sizeof(f->sent[k]) - 1;
    vjo_memconn_init(&f->conn, out);
    return VJO_OK;
}

static void fake_disconnect(void *ud, VjoConn *c)
{
    FakeNet *f = (FakeNet *)ud;
    (void)c;
    f->sent[f->next - 1][f->conn.out_len] = '\0';
}

static void fake_net(FakeNet *f, VjoPlatform *p, int n, ...)
{
    va_list ap;
    memset(f, 0, sizeof(*f));
    memset(p, 0, sizeof(*p));
    p->ud = f;
    p->connect = fake_connect;
    p->disconnect = fake_disconnect;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        f->responses[i] = va_arg(ap, const char *);
    va_end(ap);
    f->n_responses = n;
}

/* A 200 response with a JSON body (one of 4 static buffers). */
static const char *ok(const char *json)
{
    static char bufs[4][1024];
    static int k;
    char *b = bufs[k++ % 4];
    snprintf(b, sizeof(bufs[0]), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
             (unsigned long)strlen(json), json);
    return b;
}

#define DUPLICATE "cannot create note because it is a duplicate"

static void test_anki_probe(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoErr err;
    setup();
    /* plain HTTP to the port, Content-Length, a connect timeout */
    fake_net(&f, &p, 1, ok("{\"result\": 6, \"error\": null}"));
    TEST_CHECK(vjo_anki_probe(&A, &p, "192.168.1.23", 8765, &err) == VJO_OK);
    TEST_CHECK(f.port == 8765 && f.timeout_us > 0);
    TEST_CHECK(!strcmp(f.sent[0], "POST / HTTP/1.1\r\nHost: 192.168.1.23:8765\r\nContent-Type: application/json\r\n"
                                  "Content-Length: 32\r\nConnection: close\r\n\r\n"
                                  "{\"action\":\"version\",\"version\":6}"));
    TEST_MSG("%s", f.sent[0]);
    /* something else on the port; then nothing listening */
    fake_net(&f, &p, 3, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", ok("{\"result\": 6}"),
             ok("[6]"));
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_PARSE); /* no "error" */
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_probe(&A, &p, "h", 8765, &err) == VJO_E_NET);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki offline"));
}

static void test_anki_check(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoConfig cfg;
    VjoEntryList l;
    VjoErr err;
    uint8_t marks[4];
    const char *req;
    setup();
    vjo_config_defaults(&cfg);
    anki_list(&l);
    req = vjo_anki_can_add_request(&A, &cfg, &l, 3);
    TEST_ASSERT(req != NULL);
    /* only a duplicate is marked: a missing deck (even one named after
     * duplicates) or note type is not */
    fake_net(&f, &p, 1,
             ok("{\"result\": [{\"canAdd\": false, \"error\": \"" DUPLICATE "\"}, {\"canAdd\": true}, "
                "{\"canAdd\": false, \"error\": \"deck was not found: Duplicates\"}], \"error\": null}"));
    memset(marks, 7, sizeof(marks));
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_OK);
    TEST_CHECK(marks[0] == 1 && marks[1] == 0 && marks[2] == 0 && marks[3] == 7);
    TEST_CHECK(strstr(f.sent[0], "\"Expression\":\"が\"") != NULL);
    /* malformed: the wrong count, a non-boolean canAdd, an old AnkiConnect */
    fake_net(&f, &p, 3, ok("{\"result\": [{\"canAdd\": true}], \"error\": null}"),
             ok("{\"result\": [{\"canAdd\": \"false\"}, {\"canAdd\": true}, {\"canAdd\": true}], \"error\": null}"),
             ok("{\"result\": null, \"error\": \"unsupported action\"}"));
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_E_PARSE);
    TEST_CHECK(vjo_anki_check(&A, &p, "h", 8765, req, marks, 3, &err) == VJO_E_ANKI);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki: unsupported action"));
}

static void test_anki_add(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoConfig cfg;
    VjoEntryList l;
    VjoAnkiNote n;
    VjoErr err;
    VjoAnkiMedia m = {0}, none = {0};
    uint8_t jpeg[100];
    char b64[200] = {0};
    const char *body;

    setup();
    vjo_config_defaults(&cfg);
    anki_list(&l);
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
    for (int i = 0; i < 100; i++)
        jpeg[i] = (uint8_t)(255 - i);
    vjo_base64_encode(jpeg, sizeof(jpeg), b64);

    /* one addNote, the picture streamed as base64 with an exact length */
    fake_net(&f, &p, 1, ok("{\"result\": 1700000000001, \"error\": null}"));
    m.picture = jpeg;
    m.picture_len = sizeof(jpeg);
    m.picture_name = "vitajp_1.jpg";
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, &m, &err) == VJO_OK);
    TEST_CHECK(f.next == 1);
    body = strstr(f.sent[0], "\r\n\r\n");
    TEST_ASSERT(body != NULL);
    body += 4;
    {
        char cl[64];
        const char *data = strstr(body, "\"data\":\"");
        snprintf(cl, sizeof(cl), "Content-Length: %lu\r\n", (unsigned long)strlen(body));
        TEST_CHECK(strstr(f.sent[0], cl) != NULL);
        TEST_MSG("%s", f.sent[0]);
        TEST_ASSERT(data != NULL);
        data += 8;
        TEST_CHECK(!strncmp(data, b64, strlen(b64)) && !strcmp(data + strlen(b64), "\"}]}}}"));
        TEST_CHECK(!strncmp(body, "{\"action\":\"addNote\"", 19));
    }

    /* a new deck: created, then the note again */
    fake_net(&f, &p, 3, ok("{\"result\": null, \"error\": \"deck was not found: Default\"}"),
             ok("{\"result\": 1, \"error\": null}"), ok("{\"result\": 2, \"error\": null}"));
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, &none, &err) == VJO_OK);
    TEST_CHECK(f.next == 3 && strstr(f.sent[2], "\"action\":\"addNote\""));
    TEST_CHECK(strstr(f.sent[1], "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"Default\"}}"));
    TEST_CHECK(!strstr(f.sent[2], "\"picture\"")); /* no JPEG: no picture */

    /* AnkiConnect refusals and their messages */
#define REFUSED(error, rc, text)                                                                               \
    do {                                                                                                       \
        fake_net(&f, &p, 1, ok("{\"result\": null, \"error\": \"" error "\"}"));                             \
        TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, &none, &err) == (rc));                   \
        TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), text));                                                \
        TEST_MSG("%s -> %s", error, vjo_anki_err_text(&A, &err));                                              \
    } while (0)
    REFUSED(DUPLICATE, VJO_E_ANKI_DUPLICATE, "Already in Anki");
    REFUSED("model was not found: Lapis", VJO_E_ANKI, "Note type \"Lapis\" not found in Anki (anki_note_type)");
    /* a note type named after duplicates is not one */
    REFUSED("model was not found: duplicate-lapis", VJO_E_ANKI,
            "Note type \"duplicate-lapis\" not found in Anki (anki_note_type)");
    REFUSED("cannot create note because it is empty", VJO_E_ANKI,
            "Anki: the note type's first field is empty (check anki_field_word)");
    REFUSED("collection is not available", VJO_E_ANKI, "Anki: collection is not available");
#undef REFUSED

    fake_net(&f, &p, 1, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, &none, &err) == VJO_E_STATUS);
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki refused the request (HTTP 403)"));
    err.rc = VJO_E_NOT_FOUND;
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), "Anki not found on the network"));
    err.rc = VJO_OK;
    TEST_CHECK(!strcmp(vjo_anki_err_text(&A, &err), ""));

    /* a saved host is searched for again unless AnkiConnect itself answered */
    TEST_CHECK(vjo_anki_host_stale(VJO_E_NET) && vjo_anki_host_stale(VJO_E_PARSE) &&
               vjo_anki_host_stale(VJO_E_HTTP) && vjo_anki_host_stale(VJO_E_STATUS));
    TEST_CHECK(!vjo_anki_host_stale(VJO_OK) && !vjo_anki_host_stale(VJO_E_ANKI) &&
               !vjo_anki_host_stale(VJO_E_ANKI_DUPLICATE) && !vjo_anki_host_stale(VJO_E_OOM));
}

/* ---------- word audio ---------- */

#define AUDIO_LIST                                                                                            \
    "{\"type\":\"audioSourceList\",\"audioSources\":["                                                           \
    "{\"name\":\"NHK16 ネコ＼ [1]\",\"url\":\"https://raw.example.com/x/audio/1.opus\"},"                         \
    "{\"name\":\"TTS 1\",\"url\":\"https://audio.example.workers.dev/tts/a+b/c=\"}]}"

static void test_anki_audio_parts(void)
{
    const char *url;
    VjoConfig cfg;
    setup();

    vjo_config_defaults(&cfg);
    TEST_CHECK(!vjo_anki_audio_enabled(&cfg)); /* no URL by default */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "https://h/?t={term}");
    TEST_CHECK(vjo_anki_audio_enabled(&cfg));
    cfg.anki_field[VJO_ANKI_AUDIO][0] = '\0';
    TEST_CHECK(!vjo_anki_audio_enabled(&cfg));

    /* the template with {term} and {reading} percent-encoded (no reading: the term) */
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "/audio/list?term={term}&reading={reading}&apiKey=k", "猫", "ねこ"),
                       "/audio/list?term=%E7%8C%AB&reading=%E3%81%AD%E3%81%93&apiKey=k"));
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "?t={term}&r={reading}&l={language}&x={other}", "a&b c", ""),
                       "/?t=a%26b%20c&r=a%26b%20c&l=ja&x={other}"));
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "?t={term}&r={reading}", "a", NULL), "/?t=a&r=a"));
    TEST_CHECK(!strcmp(vjo_anki_audio_path(&A, "", "a", "b"), "/"));

    /* the first http(s) URL; entries without one are skipped */
    TEST_CHECK(vjo_anki_audio_parse(&A, AUDIO_LIST, strlen(AUDIO_LIST), &url) == VJO_OK && url &&
               !strcmp(url, "https://raw.example.com/x/audio/1.opus"));
#define PARSE(json, rc) (vjo_anki_audio_parse(&A, json, strlen(json), &url) == (rc))
    TEST_CHECK(PARSE("{\"audioSources\":[{\"name\":\"x\"},\"y\",3,{\"url\":\"data:audio/mp3;base64,AA\"},"
                     "{\"url\":7},{\"url\":\"http://h/2.ogg\"}]}",
                     VJO_OK) &&
               url && !strcmp(url, "http://h/2.ogg"));
    TEST_CHECK(PARSE("{\"type\":\"audioSourceList\",\"audioSources\":[]}", VJO_OK) && !url);
    TEST_CHECK(PARSE("{\"audioSources\":[{\"url\":\"file:///etc/passwd\"}]}", VJO_OK) && !url);
    TEST_CHECK(PARSE("{\"error\":\"x\"}", VJO_E_PARSE) && !url);
    TEST_CHECK(PARSE("{\"audioSources\":{}}", VJO_E_PARSE) && PARSE("[]", VJO_E_PARSE) &&
               PARSE("<html>", VJO_E_PARSE));
#undef PARSE

    /* a known audio extension of the path, else mp3 (text-to-speech) */
#define EXT(u, e)                                                                                             \
    do {                                                                                                      \
        TEST_CHECK(!strcmp(vjo_anki_audio_ext(u), e));                                                        \
        TEST_MSG("%s -> %s", u, vjo_anki_audio_ext(u));                                                       \
    } while (0)
    EXT("https://h/x/audio/1.opus", "opus");
    EXT("https://h/a.b/1.OGG?x=y.wav#z.m4a", "ogg");
    EXT("https://audio.example.workers.dev/tts/a+b/c=", "mp3");
    EXT("https://audio.example.com", "mp3");     /* the host's dot */
    EXT("https://audio.example.com?f=a.opus", "mp3"); /* the query's */
    EXT("https://h/tts.php?t=x", "mp3");
    EXT("https://h/x.opus/1", "mp3");
    EXT("https://h/1.opus2", "mp3");
    EXT("h/1.wav", "wav");
#undef EXT
}

/* A response bigger than ok()'s buffers. */
static const char *big_response(size_t body_len)
{
    static char buf[64 * 1024];
    int n = snprintf(buf, sizeof(buf), "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)body_len);
    TEST_ASSERT(n > 0 && (size_t)n + body_len < sizeof(buf));
    memset(buf + n, ' ', body_len);
    buf[n + body_len] = '\0';
    return buf;
}

static void test_anki_find_audio(void)
{
    FakeNet f;
    VjoPlatform p;
    VjoConfig cfg;
    VjoEntryList l;
    VjoAnkiNote n;
    VjoAnkiMedia m;
    VjoErr err;
    size_t mark;
    char long_term[200 * 3 + 1];
    const uint64_t now = 1791158400007ull;
    setup();
    vjo_config_defaults(&cfg);
    anki_list(&l);
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 0, &n) == 0);
#define FIND() vjo_anki_find_audio(&A, &p, &cfg, &n, now, &m, &err)
#define WORKER "https://my-audio.example.workers.dev/audio/list?term={term}&reading={reading}&apiKey=k"
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), WORKER);

    /* a GET to the URL's host (HTTPS: 443, short timeouts); only the URL
     * and its file name stay in the arena */
    fake_net(&f, &p, 1, ok(AUDIO_LIST));
    p.plain_http = 1;
    mark = vjo_arena_mark(&A);
    TEST_CHECK(FIND() == VJO_OK && m.audio_url && !strcmp(m.audio_url, "https://raw.example.com/x/audio/1.opus") &&
               !strcmp(m.audio_name, "vitajp_1791158400007.opus"));
    TEST_CHECK(vjo_arena_mark(&A) - mark < 128);
    TEST_CHECK(f.port == 443 && f.timeout_us > 0 && f.io_timeout_us > 0);
    TEST_CHECK(!strcmp(f.sent[0], "GET /audio/list?term=%E7%8C%AB&reading=%E3%81%AD%E3%81%93&apiKey=k HTTP/1.1\r\n"
                                  "Host: my-audio.example.workers.dev\r\nConnection: close\r\n\r\n"));
    TEST_MSG("%s", f.sent[0]);

    /* plain HTTP on another port: the port in Host; a chunked reply */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "http://192.168.1.5:5050?term={term}");
    fake_net(&f, &p, 1,
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
             "10\r\n{\"audioSources\":\r\n1b\r\n[{\"url\":\"http://h/a.mp3\"}]}\r\n0\r\n\r\n");
    TEST_CHECK(FIND() == VJO_OK && m.audio_url && !strcmp(m.audio_url, "http://h/a.mp3") && f.port == 5050);
    TEST_CHECK(!strcmp(f.sent[0], "GET /?term=%E7%8C%AB HTTP/1.1\r\nHost: 192.168.1.5:5050\r\n"
                                  "Connection: close\r\n\r\n"));
    TEST_MSG("%s", f.sent[0]);

    /* no recording; then errors, with nothing left on the note or in the arena */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), WORKER);
    fake_net(&f, &p, 4, ok("{\"audioSources\":[]}"), "HTTP/1.1 401 No\r\nContent-Length: 0\r\n\r\n",
             ok("<html>"), NULL);
    p.plain_http = 1;
    mark = vjo_arena_mark(&A);
    TEST_CHECK(FIND() == VJO_OK && !m.audio_url && !m.audio_name);
    TEST_CHECK(!strcmp(vjo_anki_audio_err_text(&A, &err), "no recording"));
    TEST_CHECK(FIND() == VJO_E_STATUS && !m.audio_url && !strcmp(vjo_anki_audio_err_text(&A, &err), "HTTP 401"));
    TEST_CHECK(FIND() == VJO_E_PARSE && !m.audio_url &&
               !strcmp(vjo_anki_audio_err_text(&A, &err), "not an audio source list"));
    TEST_CHECK(FIND() == VJO_E_NET && !m.audio_url &&
               !strcmp(vjo_anki_audio_err_text(&A, &err), "audio source offline"));
    vjo_arena_release(&A, mark); /* (the error texts) */

    /* a reply over the limit; a term too long for the request line */
    fake_net(&f, &p, 1, big_response(VJO_ANKI_AUDIO_MAX_RESPONSE + 1));
    p.plain_http = 1;
    TEST_CHECK(FIND() == VJO_E_TOO_LARGE && !m.audio_url &&
               !strcmp(vjo_anki_audio_err_text(&A, &err), "reply too large"));
    for (int i = 0; i < 200; i++)
        memcpy(long_term + i * 3, "猫", 3);
    long_term[200 * 3] = '\0';
    n.spelling = long_term;
    fake_net(&f, &p, 1, ok(AUDIO_LIST));
    p.plain_http = 1;
    TEST_CHECK(FIND() == VJO_E_HTTP && !m.audio_url);

    /* an invalid URL (validated at load, so only by hand) */
    snprintf(cfg.anki_audio_url, sizeof(cfg.anki_audio_url), "ftp://x/{term}");
    TEST_CHECK(FIND() == VJO_E_PARSE && !m.audio_url);
#undef WORKER
#undef FIND
}

/* ---------- settings ---------- */

static void test_anki_config(void)
{
    VjoConfig c;
    char host[64];
    int port = 0;
    /* defaults, ';' and '#' kept in names, blank field = skip, inline
     * comments still allowed after anki_host */
    const char *ini = "anki_host = auto ; LAN\n"
                      "anki_deck = Mining #1 ; main\n"
                      "anki_note_type = Lapis;v2\n"
                      "anki_tags = a b\n"
                      "anki_field_picture =\n"
                      "anki_field_audio =\n"
                      "anki_audio_url = https://w.dev/audio/list?term={term}&reading={reading}&apiKey=a;b\n"
                      "Anki_Field_Word = Word\n";
    vjo_config_defaults(&c);
    TEST_CHECK(!c.anki_host[0] && !strcmp(c.anki_deck, "Default") && !strcmp(c.anki_note_type, "Lapis") &&
               !strcmp(c.anki_tags, "vita-jp-overlay"));
    TEST_CHECK(!strcmp(c.anki_field[VJO_ANKI_WORD], "Expression") &&
               !strcmp(c.anki_field[VJO_ANKI_FREQUENCY], "FreqSort") &&
               !strcmp(c.anki_field[VJO_ANKI_AUDIO], "ExpressionAudio") && !c.anki_audio_url[0]);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 0);
    TEST_CHECK(!strcmp(c.anki_host, "auto"));
    TEST_CHECK(!strcmp(c.anki_deck, "Mining #1 ; main"));
    TEST_CHECK(!strcmp(c.anki_note_type, "Lapis;v2"));
    TEST_CHECK(!strcmp(c.anki_tags, "a b"));
    TEST_CHECK(c.anki_field[VJO_ANKI_PICTURE][0] == 0 && c.anki_field[VJO_ANKI_AUDIO][0] == 0 &&
               !strcmp(c.anki_field[VJO_ANKI_WORD], "Word"));
    TEST_CHECK(!strcmp(c.anki_audio_url, "https://w.dev/audio/list?term={term}&reading={reading}&apiKey=a;b"));
    TEST_MSG("%s", c.anki_audio_url);

    ini = "anki_host = 192.168.1.5:99999\nanki_deck =\nanki_host2 = x\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 3 && !c.anki_host[0] && !strcmp(c.anki_deck, "Default"));
    TEST_CHECK(!strcmp(c.warnings[0], "anki_host: invalid value '192.168.1.5:99999' (empty, auto, or IP[:port])"));
    TEST_MSG("%s", c.warnings[0]);
    /* anki_audio_url: one warning for each invalid value, and no audio */
    {
        static const char *const bad[] = {
            "ftp://x/{term}", "https://", "https:///?t={term}", "https://h:0/{term}", "https://h/{term} b",
            "https://h/{term}#x", "https://h/?t=%s", "https://h/{term}\x7f", "https://h/{term}\xe7\x8c\xab",
            "https://h_h/{term}", "http//h/{term}",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            char line[256];
            snprintf(line, sizeof(line), "anki_audio_url = %s\n", bad[i]);
            vjo_config_defaults(&c);
            vjo_config_parse(&c, line, strlen(line));
            TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0]);
            TEST_MSG("%s", bad[i]);
        }
        TEST_CHECK(strstr(c.warnings[0], "anki_audio_url: invalid value") == c.warnings[0]);
        TEST_MSG("%s", c.warnings[0]);
    }
    /* a host of up to 127 bytes, a value of up to 255 */
    {
        char line[600], fill[300];
        memset(fill, 'h', sizeof(fill));
        fill[127] = '\0';
        snprintf(line, sizeof(line), "anki_audio_url = https://%s/?t={term}\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 0 && strlen(c.anki_audio_url) == strlen("https:///?t={term}") + 127);
        fill[127] = 'h';
        fill[128] = '\0';
        snprintf(line, sizeof(line), "anki_audio_url = https://%s/?t={term}\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0]);

        memset(fill, 'k', sizeof(fill));
        fill[255 - strlen("https://h/?t={term}&k=")] = '\0';
        snprintf(line, sizeof(line), "anki_audio_url = https://h/?t={term}&k=%s\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 0 && strlen(c.anki_audio_url) == 255);
        /* longer: a warning, not a URL cut short (and so a wrong API key) */
        snprintf(line, sizeof(line), "anki_audio_url = https://h/?t={term}&k=%sk\n", fill);
        vjo_config_defaults(&c);
        vjo_config_parse(&c, line, strlen(line));
        TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0] && strstr(c.warnings[0], "value too long"));
        /* a long inline comment after a short value: only the comment is cut */
        {
            static char comment[2000];
            memset(comment, 'x', sizeof(comment));
            memcpy(comment, "anki_host = auto ; ", 19);
            comment[sizeof(comment) - 2] = '\n';
            comment[sizeof(comment) - 1] = '\0';
            vjo_config_defaults(&c);
            vjo_config_parse(&c, comment, strlen(comment));
            TEST_CHECK(c.n_warnings == 0 && !strcmp(c.anki_host, "auto"));
        }
        /* a line longer than the line buffer: the same */
        {
            static char longline[2000];
            memset(longline, 'k', sizeof(longline));
            memcpy(longline, "anki_audio_url = https://h/?t={term}&k=", 39);
            longline[sizeof(longline) - 2] = '\n';
            longline[sizeof(longline) - 1] = '\0';
            vjo_config_defaults(&c);
            vjo_config_parse(&c, longline, strlen(longline));
            TEST_CHECK(c.n_warnings == 1 && !c.anki_audio_url[0] && strstr(c.warnings[0], "value too long"));
        }
    }
    ini = "anki_host = 192.168.1.5:8766\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 0 && !strcmp(c.anki_host, "192.168.1.5:8766"));

    TEST_CHECK(vjo_anki_endpoint("", host, sizeof(host), &port) == VJO_ANKI_OFF);
    TEST_CHECK(vjo_anki_endpoint("Auto", host, sizeof(host), &port) == VJO_ANKI_AUTO);
    TEST_CHECK(vjo_anki_endpoint("192.168.1.23", host, sizeof(host), &port) == VJO_ANKI_MANUAL &&
               !strcmp(host, "192.168.1.23") && port == 8765);
    TEST_CHECK(vjo_anki_endpoint("my-pc.local:9000", host, sizeof(host), &port) == VJO_ANKI_MANUAL &&
               !strcmp(host, "my-pc.local") && port == 9000);
    TEST_CHECK(vjo_anki_endpoint("1.2.3.4:", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("1.2.3.4:70000", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("1.2.3.4:0", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("http://x", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint(":8765", host, sizeof(host), &port) < 0);
    TEST_CHECK(vjo_anki_endpoint("a b", host, sizeof(host), &port) < 0);
}

TEST_LIST = {
    {"base64", test_base64},
    {"anki_fields", test_anki_fields},
    {"anki_requests", test_anki_requests},
    {"anki_probe", test_anki_probe},
    {"anki_check", test_anki_check},
    {"anki_add", test_anki_add},
    {"anki_audio_parts", test_anki_audio_parts},
    {"anki_find_audio", test_anki_find_audio},
    {"anki_config", test_anki_config},
    {NULL, NULL},
};
