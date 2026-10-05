/* Anki: base64, note fields, AnkiConnect requests and replies (over a fake
 * network), the anki_* settings. */
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
        vjo_anki_picture_name(name, sizeof(name), 1791158400u, 7);
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
    TEST_ASSERT(vjo_anki_add_request(&A, &cfg, &n, "vitajp_1.jpg", &b) == 0);
    TEST_CHECK(!strcmp(b.prefix,
                       "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET ",\"fields\":{"
                       "\"Expression\":\"猫\",\"ExpressionReading\":\"ねこ\",\"ExpressionFurigana\":\"猫[ねこ]\","
                       "\"MainDefinition\":\"<ol><li>cat &quot;neko&quot;</li><li>a\\\\b &lt;&amp;&gt;\\nc</li></ol>\","
                       "\"Sentence\":\"<b>猫</b>が<br>好き\",\"FreqSort\":\"1500\"},"
                       "\"tags\":[\"vita\",\"jp-overlay\"]," OPTS ","
                       "\"picture\":[{\"filename\":\"vitajp_1.jpg\",\"fields\":[\"Picture\"],\"data\":\""));
    TEST_MSG("%s", b.prefix);
    TEST_CHECK(b.prefix_len == strlen(b.prefix) && !strcmp(b.suffix, "\"}]}}}") && b.suffix_len == 6);

    /* blank fields are skipped; no picture field: no picture; no rank: no frequency */
    cfg.anki_field[VJO_ANKI_READING][0] = '\0';
    cfg.anki_field[VJO_ANKI_FURIGANA][0] = '\0';
    cfg.anki_field[VJO_ANKI_DEFINITION][0] = '\0';
    cfg.anki_field[VJO_ANKI_SENTENCE][0] = '\0';
    cfg.anki_field[VJO_ANKI_PICTURE][0] = '\0';
    cfg.anki_tags[0] = '\0';
    TEST_ASSERT(vjo_anki_note_from_entry(&A, &l, 2, &n) == 0);
    TEST_ASSERT(vjo_anki_add_request(&A, &cfg, &n, "vitajp_1.jpg", &b) == 0);
    TEST_CHECK(!strcmp(b.prefix, "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{" TARGET
                                 ",\"fields\":{\"Expression\":\"好き\"},\"tags\":[]," OPTS "}}}"));
    TEST_MSG("%s", b.prefix);
    TEST_CHECK(b.suffix_len == 0);
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
    int port, timeout_us;
} FakeNet;

static int fake_connect(void *ud, const char *host, int port, int timeout_us, VjoConn *out)
{
    FakeNet *f = (FakeNet *)ud;
    int k = f->next;
    (void)host;
    if (k >= f->n_responses || !f->responses[k])
        return VJO_E_NET;
    f->next++;
    f->port = port;
    f->timeout_us = timeout_us;
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
    TEST_CHECK(!strcmp(f.sent[0], "POST / HTTP/1.1\r\nHost: 192.168.1.23\r\nContent-Type: application/json\r\n"
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
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, jpeg, sizeof(jpeg), "vitajp_1.jpg", &err) == VJO_OK);
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
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, NULL, 0, NULL, &err) == VJO_OK);
    TEST_CHECK(f.next == 3 && strstr(f.sent[2], "\"action\":\"addNote\""));
    TEST_CHECK(strstr(f.sent[1], "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"Default\"}}"));
    TEST_CHECK(!strstr(f.sent[2], "\"picture\"")); /* no JPEG: no picture */

    /* AnkiConnect refusals and their messages */
#define REFUSED(error, rc, text)                                                                               \
    do {                                                                                                       \
        fake_net(&f, &p, 1, ok("{\"result\": null, \"error\": \"" error "\"}"));                             \
        TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, NULL, 0, NULL, &err) == (rc));                   \
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
    TEST_CHECK(vjo_anki_add(&A, &p, "h", 8765, &cfg, &n, NULL, 0, NULL, &err) == VJO_E_STATUS);
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
                      "Anki_Field_Word = Word\n";
    vjo_config_defaults(&c);
    TEST_CHECK(!c.anki_host[0] && !strcmp(c.anki_deck, "Default") && !strcmp(c.anki_note_type, "Lapis") &&
               !strcmp(c.anki_tags, "vita-jp-overlay"));
    TEST_CHECK(!strcmp(c.anki_field[VJO_ANKI_WORD], "Expression") &&
               !strcmp(c.anki_field[VJO_ANKI_FREQUENCY], "FreqSort"));
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 0);
    TEST_CHECK(!strcmp(c.anki_host, "auto"));
    TEST_CHECK(!strcmp(c.anki_deck, "Mining #1 ; main"));
    TEST_CHECK(!strcmp(c.anki_note_type, "Lapis;v2"));
    TEST_CHECK(!strcmp(c.anki_tags, "a b"));
    TEST_CHECK(c.anki_field[VJO_ANKI_PICTURE][0] == 0 && !strcmp(c.anki_field[VJO_ANKI_WORD], "Word"));

    ini = "anki_host = 192.168.1.5:99999\nanki_deck =\nanki_host2 = x\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.n_warnings == 3 && !c.anki_host[0] && !strcmp(c.anki_deck, "Default"));
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
    {"anki_config", test_anki_config},
    {NULL, NULL},
};
