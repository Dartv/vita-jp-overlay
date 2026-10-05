#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "client.h"
#include "config.h"
#include "entries.h"
#include "foreground.h"
#include "http.h"
#include "jiten.h"
#include "jpdb.h"
#include "json.h"
#include "jpegsw.h"
#include "lens.h"
#include "styled.h"
#include "pb.h"
#include "regions.h"
#include "render.h"
#include "replay.h"
#include "textfilter.h"
#include "utf.h"

static uint8_t g_mem[4u << 20];
static VjoArena A;

static void setup(void)
{
    /* Garbage-fill like a reused arena on the Vita, so code that relies on
     * zeroed memory fails here too. */
    memset(g_mem, 0xA5, sizeof(g_mem));
    vjo_arena_init(&A, g_mem, sizeof(g_mem));
}

/* ---------- pb ---------- */

static void test_pb_roundtrip(void)
{
    VjoBuf b;
    PbReader r;
    PbField f;
    setup();
    vjo_buf_init(&b, &A);
    pb_put_uint(&b, 1, 300);
    pb_put_string(&b, 2, "abc");
    pb_put_fixed32(&b, 3, 0x3f800000u); /* 1.0f */
    pb_put_uint(&b, 1000, UINT64_MAX);
    TEST_CHECK(!b.oom);
    pb_reader_init(&r, b.data, b.len);
    TEST_CHECK(pb_next(&r, &f) == 1 && f.field == 1 && f.varint == 300);
    TEST_CHECK(pb_next(&r, &f) == 1 && f.field == 2 && f.len == 3 && !memcmp(f.data, "abc", 3));
    TEST_CHECK(pb_next(&r, &f) == 1 && f.field == 3 && pb_as_float(&f) == 1.0f);
    TEST_CHECK(pb_next(&r, &f) == 1 && f.field == 1000 && f.varint == UINT64_MAX);
    TEST_CHECK(pb_next(&r, &f) == 0);
    TEST_CHECK(pb_len_field_size(1, 200) == 1 + 2 + 200);

    pb_reader_init(&r, "\x0a\x05zz", 4); /* truncated */
    TEST_CHECK(pb_next(&r, &f) == -1);
}

static void test_buf_interleaved(void)
{
    /* Two buffers growing in turn, with a plain allocation in between: the
     * one that is no longer last moves instead of overwriting the other. */
    VjoBuf x, y;
    char *p;
    setup();
    vjo_buf_init(&x, &A);
    vjo_buf_init(&y, &A);
    for (int i = 0; i < 300; i++) {
        vjo_buf_putc(&x, (char)('a' + i % 26));
        vjo_buf_putc(&y, (char)('A' + i % 26));
        if (i == 150)
            TEST_CHECK(vjo_arena_alloc(&A, 10) != NULL);
    }
    TEST_ASSERT(!x.oom && !y.oom && x.len == 300 && y.len == 300);
    for (int i = 0; i < 300; i++)
        if (x.data[i] != 'a' + i % 26 || y.data[i] != 'A' + i % 26)
            TEST_CHECK_(0, "byte %d", i);
    p = vjo_buf_cstr(&x);
    TEST_CHECK(p && strlen(p) == 300);
}

/* ---------- lens ---------- */

static int find_field(const uint8_t *p, size_t n, uint32_t field, PbField *out)
{
    PbReader r;
    PbField f;
    pb_reader_init(&r, p, n);
    while (pb_next(&r, &f) > 0)
        if (f.field == field) {
            *out = f;
            return 1;
        }
    return 0;
}

static void test_lens_request_layout(void)
{
    uint8_t rnd[24], jpeg[1000], *full;
    VjoLensRequest lr;
    PbField objreq, ctx, img, payload, bytes, meta, f, cc, loc;
    setup();
    for (int i = 0; i < 24; i++)
        rnd[i] = (uint8_t)i;
    for (int i = 0; i < 1000; i++)
        jpeg[i] = (uint8_t)(i * 7);
    TEST_ASSERT(vjo_lens_build_request(&A, rnd, 960, 544, sizeof(jpeg), &lr) == 0);
    full = vjo_arena_alloc(&A, lr.body_len);
    memcpy(full, lr.prefix, lr.prefix_len);
    memcpy(full + lr.prefix_len, jpeg, sizeof(jpeg));
    memcpy(full + lr.prefix_len + sizeof(jpeg), lr.suffix, lr.suffix_len);

    TEST_ASSERT(find_field(full, lr.body_len, 1, &objreq));
    TEST_CHECK(objreq.data + objreq.len == full + lr.body_len);
    TEST_ASSERT(find_field(objreq.data, objreq.len, 1, &ctx));
    TEST_ASSERT(find_field(objreq.data, objreq.len, 3, &img));
    TEST_ASSERT(find_field(img.data, img.len, 1, &payload));
    TEST_ASSERT(find_field(payload.data, payload.len, 1, &bytes));
    TEST_CHECK(bytes.len == sizeof(jpeg) && !memcmp(bytes.data, jpeg, sizeof(jpeg)));
    TEST_ASSERT(find_field(img.data, img.len, 3, &meta));
    TEST_CHECK(find_field(meta.data, meta.len, 1, &f) && f.varint == 960);
    TEST_CHECK(find_field(meta.data, meta.len, 2, &f) && f.varint == 544);

    TEST_ASSERT(find_field(ctx.data, ctx.len, 4, &cc));
    TEST_CHECK(find_field(cc.data, cc.len, 1, &f) && f.varint == 3);  /* PLATFORM_WEB */
    TEST_CHECK(find_field(cc.data, cc.len, 2, &f) && f.varint == 4);  /* SURFACE_CHROMIUM */
    TEST_ASSERT(find_field(cc.data, cc.len, 4, &loc));
    TEST_CHECK(find_field(loc.data, loc.len, 1, &f) && f.len == 2 && !memcmp(f.data, "ja", 2));
    TEST_CHECK(find_field(loc.data, loc.len, 2, &f) && f.len == 10);
    TEST_CHECK(find_field(cc.data, cc.len, 17, &f));                  /* client_filters */
    TEST_ASSERT(find_field(ctx.data, ctx.len, 3, &f));                /* request_id */
    {
        PbField uuid, aid, ri;
        TEST_CHECK(find_field(f.data, f.len, 1, &uuid) && uuid.varint == 0x0706050403020100ull);
        TEST_CHECK(find_field(f.data, f.len, 4, &aid) && aid.len == 16 && aid.data[0] == 8);
        TEST_CHECK(find_field(f.data, f.len, 6, &ri) && ri.len == 0);
    }
}

static void put_word(VjoBuf *line, const char *text, const char *sep)
{
    VjoBuf w;
    VjoArena *a = line->arena;
    (void)a;
    /* words are built in a scratch arena region to avoid VjoBuf nesting */
    static uint8_t wmem[512];
    VjoArena wa;
    vjo_arena_init(&wa, wmem, sizeof(wmem));
    vjo_buf_init(&w, &wa);
    pb_put_string(&w, 2, text);
    if (sep)
        pb_put_string(&w, 3, sep);
    pb_put_bytes(line, 1, w.data, w.len);
}

static void test_lens_response_parse(void)
{
    /* Two paragraphs: "こんにちは 世界" and "Hello" */
    static uint8_t m1[1024], m2[1024], m3[1024], m4[1024];
    VjoArena a1, a2, a3, a4;
    VjoBuf line1, para1, line2, para2, layout, text, objs, resp;
    VjoLensResult r;
    char *t;

    vjo_arena_init(&a1, m1, sizeof(m1));
    vjo_arena_init(&a2, m2, sizeof(m2));
    vjo_arena_init(&a3, m3, sizeof(m3));
    vjo_arena_init(&a4, m4, sizeof(m4));

    vjo_buf_init(&line1, &a1);
    put_word(&line1, "こんにちは", " ");
    put_word(&line1, "世界", NULL);
    vjo_buf_init(&para1, &a2);
    pb_put_bytes(&para1, 2, line1.data, line1.len);
    pb_put_uint(&para1, 4, 2);

    vjo_buf_init(&line2, &a3);
    put_word(&line2, "Hello", NULL);
    vjo_buf_init(&para2, &a4);
    pb_put_bytes(&para2, 2, line2.data, line2.len);

    setup();
    vjo_buf_init(&layout, &A);
    pb_put_bytes(&layout, 1, para1.data, para1.len);
    pb_put_bytes(&layout, 1, para2.data, para2.len);
    {
        static uint8_t m5[4096];
        VjoArena a5;
        vjo_arena_init(&a5, m5, sizeof(m5));
        vjo_buf_init(&text, &a5);
        pb_put_bytes(&text, 1, layout.data, layout.len);
        {
            static uint8_t m6[4096];
            VjoArena a6;
            vjo_arena_init(&a6, m6, sizeof(m6));
            vjo_buf_init(&objs, &a6);
            pb_put_bytes(&objs, 3, text.data, text.len);
            {
                static uint8_t m7[4096];
                VjoArena a7;
                vjo_arena_init(&a7, m7, sizeof(m7));
                vjo_buf_init(&resp, &a7);
                pb_put_bytes(&resp, 2, objs.data, objs.len);
            }
        }
    }
    TEST_ASSERT(vjo_lens_parse_response(&A, resp.data, resp.len, &r) == 0);
    TEST_CHECK(r.n_paragraphs == 2);
    TEST_CHECK(r.paragraphs[0].writing_direction == 2);
    TEST_CHECK(r.paragraphs[0].n_lines == 1 && r.paragraphs[0].lines[0].n_words == 2);
    t = vjo_lens_text(&A, &r);
    TEST_CHECK(strcmp(t, "こんにちは 世界\nHello") == 0);
    TEST_MSG("got '%s'", t);

    TEST_CHECK(vjo_lens_parse_response(&A, (const uint8_t *)"\x12\x09", 2, &r) == -1);
    TEST_CHECK(vjo_lens_parse_response(&A, (const uint8_t *)"", 0, &r) == 0 && r.n_paragraphs == 0);
}

/* ---------- text ---------- */

static void test_filter_lines(void)
{
    setup();
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "Score 100\r\nこんにちは\nHP 20/20\n漢字abc\n", 1),
                       "こんにちは\n漢字abc"));
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "abc\ndef", 1), ""));
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "abc\nかな", 0), "abc\nかな"));
    /* Full-width punctuation alone (U+3000-303F) is not Japanese */
    TEST_CHECK(!strcmp(vjo_filter_lines(&A, "「」\nア", 1), "ア"));
    TEST_CHECK(!strcmp(vjo_strip_newlines(&A, "a\r\nb\nc\rd\r\r\n"), "abc\rd\r"));
}

static void test_utf(void)
{
    char s[] = "  \t x y \n ";
    size_t i = 0;
    const char *e = "😀a";
    TEST_CHECK(!strcmp(vjo_java_trim(s), "x y"));
    TEST_CHECK(vjo_utf8_next(e, strlen(e), &i) == 0x1F600 && i == 4);
    TEST_CHECK(vjo_utf8_next("\xe3\x81", 2, &(size_t){0}) == 0xFFFD);
}

/* ---------- json / jpdb ---------- */

static void test_jpdb_request(void)
{
    char *j;
    setup();
    j = vjo_jpdb_build_request(&A, "猫が\"好き\"\\\n");
    TEST_CHECK(strstr(j, "\"text\":\"猫が\\\"好き\\\"\\\\\\n\"") != NULL);
    TEST_CHECK(strstr(j, "\"position_length_encoding\":\"utf16\"") != NULL);
    TEST_CHECK(strstr(j, "\"token_fields\":[\"vocabulary_index\",\"position\",\"length\",\"furigana\"]") != NULL);
    TEST_CHECK(strstr(j, "\"vocabulary_fields\":[\"vid\",\"sid\",\"rid\",\"spelling\",\"reading\",\"frequency_rank\",\"meanings\"]") != NULL);
}

static const char *JPDB_SAMPLE =
    "{\"tokens\":[[1,0,1,null],[0,1,1,null],[2,2,2,[[\"好\",\"す\"],\"き\"]],[1,5,1,null]],"
    "\"vocabulary\":["
    "[1,2,3,\"が\",\"が\",50,[\"indicates subject\"]],"
    "[10,20,30,\"猫\",\"ねこ\",1500,[\"cat\",\"\\u732b \\ud83d\\ude00\"]],"
    "[11,21,31,\"好き\",\"すき\",null,[\"liked\"]]"
    "]}";

static void test_jpdb_parse(void)
{
    VjoDictResult r;
    setup();
    TEST_ASSERT(vjo_jpdb_parse_response(&A, JPDB_SAMPLE, strlen(JPDB_SAMPLE), &r) == 0);
    TEST_CHECK(r.n_vocab == 3 && r.n_tokens == 4);
    TEST_CHECK(r.vocab[0].rank == 50);
    TEST_CHECK(r.vocab[2].rank == VJO_NO_RANK);
    TEST_CHECK(!strcmp(r.vocab[1].spelling, "猫") && !strcmp(r.vocab[1].reading, "ねこ"));
    TEST_CHECK(r.vocab[1].n_meanings == 2 && !strcmp(r.vocab[1].meanings[1], "猫 😀"));
    TEST_CHECK(r.tokens[2].vocab == 2 && r.tokens[2].pos16 == 2 && r.tokens[2].len16 == 2);
    {
        const char *eb = "{\"error\":\"bad_key\",\"error_message\":\"Invalid API key\"}";
        const char *em = vjo_jpdb_error_message(&A, eb, strlen(eb));
        TEST_CHECK(em && !strcmp(em, "Invalid API key"));
    }
    {
        VjoJson j;
        VjoBuf b;
        const char *js = "[\"a\\u732b\\n\",12]";
        TEST_ASSERT(vjo_json_parse(&A, js, strlen(js), &j) == 0);
        vjo_buf_init(&b, &A);
        TEST_CHECK(vjo_json_append_str(&b, &j, 1) == 0 && vjo_json_append_str(&b, &j, 2) == 0);
        TEST_CHECK(vjo_json_append_str(&b, &j, 9) == -1);
        TEST_CHECK(!strcmp(vjo_buf_cstr(&b), "a猫\n12"));
    }
    {
        VjoJson j;
        long v;
        const char *js = "[999999999,1000000000,-12,99999999999999999999999,3.7]";
        TEST_ASSERT(vjo_json_parse(&A, js, strlen(js), &j) == 0);
        TEST_CHECK(vjo_json_int(&j, 1, &v) == 0 && v == 999999999);
        TEST_CHECK(vjo_json_int(&j, 2, &v) == -1);
        TEST_CHECK(vjo_json_int(&j, 3, &v) == 0 && v == -12);
        TEST_CHECK(vjo_json_int(&j, 4, &v) == -1);
        TEST_CHECK(vjo_json_int(&j, 5, &v) == 0 && v == 3);
    }
    TEST_CHECK(vjo_jpdb_parse_response(&A, "{\"x\":1}", 7, &r) == -1);
    TEST_CHECK(vjo_jpdb_parse_response(&A, "{", 1, &r) == -1);
}

/* ---------- jiten.moe ---------- */

static void test_jiten_ruby(void)
{
    setup();
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "天[てん]気[き]"), "てんき"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "お茶[ちゃ]"), "おちゃ"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "取[と]り扱[あつか]い説[せつ]明[めい]書[しょ]"),
                       "とりあつかいせつめいしょ"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "今日[きょう]"), "きょう"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "人々[ひとびと]"), "ひとびと"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "ドア"), "ドア"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "ＡＢ[えーびー]だ"), "えーびーだ"));
    TEST_CHECK(!strcmp(vjo_jiten_ruby_to_kana(&A, "壊[こわ"), "壊[こわ"));
}

static void test_jiten_request(void)
{
    char *j;
    setup();
    j = vjo_jiten_build_request(&A, "猫が\"好き\"");
    TEST_CHECK(!strcmp(j, "{\"text\":[\"猫が\\\"好き\\\"\"]}"));
}

static void test_jiten_parse(void)
{
    /* Shape of a real /api/reader/parse response (trimmed): "𠮟" is a
     * surrogate pair, so お茶 starts at UTF-16 index 4; を repeats. */
    const char *js =
        "{\"tokens\":[[{\"wordId\":1002430,\"readingIndex\":0,\"start\":4,\"end\":6,\"length\":2,\"conjugations\":[]},"
        "{\"wordId\":2029010,\"readingIndex\":0,\"start\":6,\"end\":7,\"length\":1,\"conjugations\":[]},"
        "{\"wordId\":1169870,\"readingIndex\":0,\"start\":7,\"end\":9,\"length\":2,\"conjugations\":[\"past\"]},"
        "{\"wordId\":2029010,\"readingIndex\":0,\"start\":10,\"end\":11,\"length\":1,\"conjugations\":[]},"
        "{\"wordId\":4242,\"readingIndex\":3,\"start\":11,\"end\":12,\"length\":1,\"conjugations\":[]}]],"
        "\"vocabulary\":["
        "{\"wordId\":1002430,\"readingIndex\":0,\"spelling\":\"お茶\",\"reading\":\"お茶[ちゃ]\",\"frequencyRank\":1199,"
        "\"partsOfSpeech\":[\"noun\"],\"meaningsChunks\":[[\"tea (usu. green)\"],[\"tea ceremony\",\"chanoyu\"]],"
        "\"meaningsPartOfSpeech\":[\"n\"],\"knownState\":[0],\"pitchAccents\":[0],\"studyDeckIds\":[]},"
        "{\"wordId\":2029010,\"readingIndex\":0,\"spelling\":\"を\",\"reading\":\"を\",\"frequencyRank\":4,"
        "\"meaningsChunks\":[[\"indicates direct object of action\"]],\"knownState\":[0]},"
        "{\"wordId\":1169870,\"readingIndex\":0,\"spelling\":\"飲む\",\"reading\":\"飲[の]む\",\"frequencyRank\":299,"
        "\"meaningsChunks\":[[\"to drink\",\"to gulp\"],[]],\"knownState\":[1]},"
        "{\"wordId\":2029010,\"readingIndex\":0,\"spelling\":\"を\",\"reading\":\"を\",\"frequencyRank\":4,"
        "\"meaningsChunks\":[[\"indicates direct object of action\"]],\"knownState\":[0]}]}";
    VjoDictResult r;
    VjoEntryList l;
    setup();
    TEST_ASSERT(vjo_jiten_parse_response(&A, js, strlen(js), &r) == 0);
    TEST_CHECK(r.n_vocab == 3); /* を deduplicated */
    TEST_CHECK(!strcmp(r.vocab[0].reading, "おちゃ") && r.vocab[0].rank == 1199);
    TEST_CHECK(r.vocab[0].n_meanings == 2 && !strcmp(r.vocab[0].meanings[1], "tea ceremony; chanoyu"));
    TEST_CHECK(r.vocab[2].n_meanings == 1 && !strcmp(r.vocab[2].meanings[0], "to drink; to gulp"));
    TEST_CHECK(r.n_tokens == 5);
    TEST_CHECK(r.tokens[1].vocab == 1 && r.tokens[3].vocab == 1);
    TEST_CHECK(r.tokens[4].vocab == -1);
    TEST_CHECK(r.tokens[0].pos16 == 4 && r.tokens[0].len16 == 2);

    TEST_ASSERT(vjo_entries_build(&A, "𠮟る。お茶を飲み、茶を", &r, &l) == 0);
    TEST_CHECK(l.n_entries == 3);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "𠮟る。【お茶】を飲み、茶を"));
    TEST_CHECK(!strcmp(l.entries[2].text, "飲む (のむ) 299\nto drink; to gulp"));

    TEST_CHECK(vjo_jiten_parse_response(&A, "{\"tokens\":[]}", 13, &r) == -1);
    TEST_CHECK(!strcmp(vjo_jiten_error_message(&A, "{\"title\":\"Unauthorized\",\"status\":401}", 37),
                       "Unauthorized"));
    TEST_CHECK(vjo_jiten_error_message(&A, "Too many requests", 17) == NULL);
}

static void test_entries(void)
{
    VjoDictResult r;
    VjoEntryList l;
    const char *jp = "{\"tokens\":[[1,0,1,null],[0,1,1,null],[2,2,2,null]],\"vocabulary\":["
                     "[1,2,3,\"が\",\"が\",50,[\"indicates subject\"]],"
                     "[10,20,30,\"猫\",\"ねこ\",1500,[\"cat\",\"feline\"]],"
                     "[11,21,31,\"好き\",\"好き\",null,[\"liked\"]]]}";
    setup();
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    /* jpdb text "猫が好き" came from filtered "猫が\n好き" */
    TEST_ASSERT(vjo_entries_build(&A, "猫が\n好き", &r, &l) == 0);
    TEST_CHECK(!strcmp(l.header, "猫が\n好き"));
    /* every word gets an entry, in text order */
    TEST_CHECK(l.n_entries == 3);
    TEST_CHECK(!strcmp(l.entries[0].text, "猫 (ねこ) 1500\ncat\nfeline"));
    TEST_CHECK(!strcmp(l.entries[1].text, "が 50\nindicates subject"));
    TEST_CHECK(!strcmp(l.entries[2].text, "好き 99999\nliked"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "【猫】が\n好き"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 1), "猫【が\n】好き"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 2), "猫が\n【好き】"));
    TEST_CHECK(!strcmp(vjo_entries_body(&A, &l),
                       "猫 (ねこ) 1500\ncat\nfeline\n\nが 50\nindicates subject\n\n好き 99999\nliked"));
}

static void test_entries_surrogates_and_trim(void)
{
    VjoDictResult r;
    VjoEntryList l;
    /* "😀猫" : 猫 is at UTF-16 index 2 */
    const char *jp = "{\"tokens\":[[0,2,1,null]],\"vocabulary\":[[1,1,1,\"猫\",\"ねこ\",900,[]]]}";
    setup();
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    /* index 2 is the emoji's high surrogate: the highlight covers whole
     * code points (jpdb indexes the untrimmed text, 猫 is at index 4) */
    TEST_CHECK(l.entries[0].hl_start == 0 && l.entries[0].hl_end == 4);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "【😀】猫"));
    jp = "{\"tokens\":[[0,4,1,null]],\"vocabulary\":[[1,1,1,\"猫\",\"ねこ\",900,[]]]}";
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    TEST_CHECK(!strcmp(l.header, "😀猫"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "😀【猫】"));
    TEST_CHECK(!strcmp(l.entries[0].text, "猫 (ねこ) 900"));

    /* server positions past the text or huge lengths are clamped */
    jp = "{\"tokens\":[[0,4,999999999,null],[1,999999999,5,null]],\"vocabulary\":"
         "[[1,1,1,\"猫\",\"ねこ\",900,[]],[2,2,2,\"犬\",\"いぬ\",800,[]]]}";
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "😀【猫】"));
    TEST_CHECK(l.entries[1].hl_start == -1);
    /* pos + len would overflow int (beyond what the JSON parsers accept):
     * the end saturates */
    jp = "{\"tokens\":[[0,4,1,null]],\"vocabulary\":[[1,1,1,\"猫\",\"ねこ\",900,[]]]}";
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    r.tokens[0].len16 = 0x7fffffff;
    TEST_ASSERT(vjo_entries_build(&A, "  😀猫 ", &r, &l) == 0);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "😀【猫】"));
}

/* ---------- kernel foreground tracking ---------- */

enum { SHELL = 0x100, GAME = 0x200, APP = 0x300, APP2 = 0x400, SHELL2 = 0x500 };

static int fg_is(const VjoForeground *f, int game_pid, int game_active, int prev)
{
    return f->game_pid == game_pid && f->game_active == game_active && f->prev == prev;
}

/* GAME in the foreground and confirmed by the shell. */
static void fg_game_running(VjoForeground *f)
{
    uint32_t iev;
    memset(f, 0, sizeof(*f));
    f->shell_pid = SHELL;
    TEST_CHECK(fg_process_event(f, SHELL, PROCEV_RESUME) == 0); /* the shell never is the game */
    TEST_CHECK(fg_process_event(f, GAME, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(f, GAME, 1, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(f, GAME, 1, 0));
    TEST_CHECK(fg_process_event(f, GAME, PROCEV_RESUME) == 0); /* already the foreground */
}

static void test_foreground(void)
{
    VjoForeground f;
    uint32_t iev;

    /* A non-game starts over the game; the shell says it is not a game, so
     * the game comes back (inactive until the shell re-classifies it). */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, GAME, 1, &iev) < 0 && iev == 0); /* not the foreground */
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, 0, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, GAME, 1, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, 1, 0));

    /* The newcomer exits before it is classified: back to the game. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == (IEV_GAME_EXIT | IEV_GAME_START));
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* The game suspends while behind the newcomer: it is forgotten, the
     * newcomer's non-game verdict changes nothing, and the game's resume
     * makes it the foreground again. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_SUSPEND) == 0);
    TEST_CHECK(fg_is(&f, APP, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, APP, 0, &iev) == 0 && iev == 0);
    TEST_CHECK(fg_is(&f, APP, 0, 0));
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_RESUME) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* A second newcomer arrives before the first is classified: the game
     * stays behind both, the first's late verdict is refused, and the
     * second's brings the game back. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_process_event(&f, APP2, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, APP2, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, 0, &iev) < 0 && iev == 0);
    TEST_CHECK(fg_is(&f, APP2, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP2, 0, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* SceShell restarts: the old one exits, the new one starts before it
     * registers (so it is a newcomer over the game), registers, and
     * classifies itself as not a game. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, SHELL, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, 1, 0));
    TEST_CHECK(fg_process_event(&f, SHELL2, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, SHELL2, 0, GAME));
    f.shell_pid = SHELL2; /* vjoRegisterShell */
    TEST_CHECK(fg_set_game_active(&f, SHELL2, 0, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, SHELL2, PROCEV_RESUME) == 0);
    TEST_CHECK(fg_set_game_active(&f, GAME, 1, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, 1, 0));

    /* The game exits with nothing behind it; pid 0 is never the foreground. */
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_EXIT) == IEV_GAME_EXIT);
    TEST_CHECK(fg_is(&f, 0, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, 0, 1, &iev) < 0);
}

/* ---------- config / regions ---------- */

static void test_config(void)
{
    VjoConfig c;
    const char *ini = "\xEF\xBB\xBF; comment\n"
                      "dictionary = JPDB\n"
                      "jpdb_api_key = abc123 ; my key\n"
                      "jiten_api_key = ak_xyz\n"
                      "frequency_filter=500\n"
                      "non_japanese_filter = none\r\n"
                      "font_size_ja = 99\n"
                      "font_size_en = 10\n"
                      "toggle_button = Select+R\n"
                      "ocr_mode = on_press\n"
                      "log_host = 192.168.1.5\n"
                      "log_file = on\n"
                      "bogus = 1\n";
    vjo_config_defaults(&c);
    TEST_CHECK(c.dictionary == VJO_DICT_JITEN && c.font_size_ja == 18 && c.font_size_en == 14 &&
               c.toggle_button == VJO_TRIGGER_L_R);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.dictionary == VJO_DICT_JPDB);
    TEST_CHECK(!strcmp(c.api_key[VJO_DICT_JPDB], "abc123") && !strcmp(c.api_key[VJO_DICT_JITEN], "ak_xyz"));
    TEST_CHECK(!strcmp(vjo_config_api_key(&c), "abc123"));
    c.dictionary = VJO_DICT_JITEN;
    TEST_CHECK(!strcmp(vjo_config_api_key(&c), "ak_xyz") && !strcmp(vjo_dict_name(c.dictionary), "jiten.moe"));
    c.dictionary = VJO_DICT_JPDB;
    TEST_CHECK(c.non_japanese_filter == VJO_FILTER_NONE);
    TEST_CHECK(c.font_size_ja == 18); /* invalid -> default */
    TEST_CHECK(c.font_size_en == 10);
    TEST_CHECK(c.toggle_button == VJO_TRIGGER_SELECT_R);
    TEST_CHECK(c.ocr_mode == VJO_OCR_ON_PRESS);
    TEST_CHECK(!strcmp(c.log_host, "192.168.1.5"));
    TEST_CHECK(c.log_file == 1);
    /* font_size_ja and bogus; the removed frequency_filter is silent */
    TEST_CHECK(c.n_warnings == 2);
    TEST_MSG("warnings: %d", c.n_warnings);

    /* the default file parses cleanly to the defaults */
    {
        VjoConfig d;
        const char *t = vjo_config_default_text();
        vjo_config_defaults(&d);
        vjo_config_parse(&d, t, strlen(t));
        TEST_CHECK(d.n_warnings == 0);
        TEST_CHECK(d.dictionary == VJO_DICT_JITEN && d.ocr_mode == VJO_OCR_AUTO && d.api_key[VJO_DICT_JPDB][0] == 0 &&
                   d.font_size_ja == 18 && d.font_size_en == 14);
    }

    /* settings renamed/removed since earlier releases, as in tools/migrate_config.py */
    {
        const char *old = "api_key = k1\nfont_size = 20\nhw_jpeg = on\nfrequency_filter = 3\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, old, strlen(old));
        TEST_CHECK(!strcmp(c.api_key[VJO_DICT_JPDB], "k1"));
        TEST_CHECK(c.n_warnings == 0 && c.font_size_ja == 18);
    }

    /* docs/config.example.ini is the file written on first run */
    {
        size_t len;
        const char *t = vjo_config_default_text();
        char *doc;
        setup();
        doc = vjo_read_file(&A, VJO_FIXTURES "/../../docs/config.example.ini", &len);
        TEST_ASSERT(doc != NULL);
        TEST_CHECK(len == strlen(t) && !memcmp(doc, t, len));
        TEST_MSG("docs/config.example.ini differs from vjo_config_default_text()");
    }
}

static void test_regions(void)
{
#define P(s, g) vjo_region_parse(s, strlen(s), g)
    VjoRect r = {100, 200, 30000, 40000}, g;
    char line[64];
    int n = vjo_region_format(line, sizeof(line), &r);
    TEST_CHECK(n > 0 && !strcmp(line, "region = 100,200,30000,40000\n"));
    TEST_CHECK(vjo_region_parse(line, (size_t)n, &g) == 1 && !memcmp(&g, &r, sizeof(r)));
    TEST_CHECK(vjo_region_format(line, 10, &r) == -1);
    TEST_CHECK(P(" region=1, 2 ,3,4\r\n", &g) == 1 && g.x == 1 && g.h == 4);
    TEST_CHECK(P("region = 1,2,0,3\n", &g) == 0);        /* zero size */
    TEST_CHECK(P("region = 60000,2,6000,3\n", &g) == 0); /* off screen */
    TEST_CHECK(P("region = 1,2,3\n", &g) == 0);
    TEST_CHECK(P("region = 1,2,3,4x\n", &g) == 0);
    TEST_CHECK(P("PCSG00123 = 1,2,3,4\n", &g) == 0);
    TEST_CHECK(P("", &g) == 0);
#undef P
}

/* ---------- http over a memory connection ---------- */

static int body_cb(void *ud, VjoConn *c)
{
    return vjo_conn_send_all(c, "hello", 5);
}

static void test_http(void)
{
    VjoMemConn m = {0};
    char sent[4096];
    VjoConn c;
    VjoHttpRequest req = {"POST", "example.com", "/x", "text/plain", "A: b\r\n", 5, body_cb, NULL};
    VjoHttpResponse resp;
    setup();
    m.out = sent;
    m.out_cap = sizeof(sent) - 1;
    vjo_memconn_init(&m, &c);
    TEST_CHECK(vjo_http_send(&c, &req) == 0);
    m.out[m.out_len] = 0;
    TEST_CHECK(strstr(m.out, "POST /x HTTP/1.1\r\nHost: example.com\r\n") == m.out);
    TEST_CHECK(strstr(m.out, "Content-Length: 5\r\n") != NULL);
    TEST_CHECK(strstr(m.out, "A: b\r\n\r\nhello") != NULL);

    m.in = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    m.len = strlen(m.in);
    m.step = 3;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0);
    TEST_CHECK(resp.status == 200 && !strcmp(resp.body, "Wikipedia"));

    m.in = "HTTP/1.1 403 Forbidden\r\ncontent-length: 2\r\nContent-Encoding: gzip\r\n\r\nno";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0);
    TEST_CHECK(resp.status == 403 && resp.gzip && !strcmp(resp.body, "no"));

    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 1, &resp) == VJO_E_TOO_LARGE);

    m.in = "HTTP/1.0 200 OK\r\n\r\nuntil close";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && !strcmp(resp.body, "until close"));
    /* closed without close_notify: a read-until-close body may be cut */
    m.pos = 0;
    m.end_rc = VJO_E_HTTP;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP);
    /* ... but a Content-Length body is complete */
    m.in = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && !strcmp(resp.body, "ok"));
    m.end_rc = 0;

    /* overflowing Content-Length (UB on a 32-bit long before) */
    m.in = "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999999999999\r\n\r\nx";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_TOO_LARGE);
    m.in = "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551617\r\n\r\nx";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_TOO_LARGE);
    m.in = "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\nx";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP);

    /* status must be three digits */
    m.in = "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP);
    m.in = "HTTP/1.1 -20 OK\r\nContent-Length: 0\r\n\r\n";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP);
}

/* ---------- styled text (ui::Text) ---------- */

static void test_styled(void)
{
    VjoDictResult r;
    VjoEntryList l;
    VjoStyled st;
    uint32_t a, n;
    const char *jp = "{\"tokens\":[[0,0,1,null],[1,2,2,null]],\"vocabulary\":["
                     "[1,1,1,\"猫\",\"ねこ\",1500,[\"cat <pet>\",\"A&B\"]],"
                     "[2,2,2,\"好き\",\"すき\",900,[\"liked\"]]]}";
    setup();
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    TEST_ASSERT(vjo_entries_build(&A, "猫が\r\n好き", &r, &l) == 0);

    /* header: one span, \r dropped; highlight ranges in UTF-16 units */
    TEST_ASSERT(vjo_styled_init(&st, &A, 64, 4) == 0);
    vjo_styled_header(&st, &l, 21);
    TEST_CHECK(st.len == 5 && st.text[0] == 0x732B && st.text[2] == '\n' && st.n_spans == 1);
    TEST_CHECK(vjo_entry_header_range(&l, 0, &a, &n) && a == 0 && n == 1);
    TEST_CHECK(vjo_entry_header_range(&l, 1, &a, &n) && a == 3 && n == 2);
    TEST_CHECK(!vjo_entry_header_range(&l, 2, &a, &n));

    /* body: headword+reading at ja px, rank+meanings at en px */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 0);
    TEST_CHECK(st.n_spans == 9); /* 猫 ( ねこ ) rank \n meaning \n meaning */
    TEST_CHECK(st.spans[0].len == 1 && st.spans[0].px == 21 && st.spans[0].rgb == VJO_RGB_HIGHLIGHT);
    TEST_CHECK(st.spans[2].rgb == VJO_RGB_READING && st.spans[2].px == 21);
    TEST_CHECK(st.spans[4].px == 18 && st.spans[4].rgb == VJO_RGB_RANK); /* " 1500" */
    TEST_CHECK(st.spans[6].px == 18 && st.spans[6].len == 9);           /* "cat <pet>" unescaped */
    /* in Anki: a green √ after the rank */
    TEST_ASSERT(vjo_styled_init(&st, &A, 256, 16) == 0);
    vjo_styled_entry(&st, &l, 0, 21, 18, 1);
    TEST_CHECK(st.n_spans == 10 && st.spans[5].rgb == VJO_RGB_OK && st.spans[5].len == 2 &&
               st.text[st.spans[5].start + 1] == 0x221A);

    /* surrogate pairs and truncation */
    TEST_ASSERT(vjo_styled_init(&st, &A, 3, 4) == 0);
    vjo_styled_puts(&st, "😀ab", 0, 10);
    TEST_CHECK(st.len == 3 && st.text[0] == 0xD83D && st.text[1] == 0xDE00 && st.truncated);
    TEST_CHECK(vjo_utf16_index("😀猫", 4) == 2 && vjo_utf16_index("a\r\nb", 3) == 2);
    TEST_CHECK(vjo_font_px(15) == 23 && vjo_font_px(8) == 12);
}

/* ---------- software JPEG ---------- */

static int grad_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    uint32_t w = *(uint32_t *)ud;
    for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = dst + (y * w + x) * 4;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)(row + y);
            p[2] = (uint8_t)((x + row + y) & 0x80 ? 255 : 0);
            p[3] = 255;
        }
    return (int)n;
}

/* Pseudo-random pixels: dense entropy data, so 0xFF bytes occur. */
static int noise_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    uint32_t w = *(uint32_t *)ud;
    for (uint32_t i = 0; i < n * w * 4; i++) {
        uint32_t x = (row * w * 4 + i) * 2654435761u;
        dst[i] = (uint8_t)(x >> 24);
    }
    return (int)n;
}

static void test_jpegsw(void)
{
    VjoBuf out;
    uint32_t w = 37;
    setup();
    vjo_buf_init(&out, &A);
    TEST_ASSERT(vjo_jpeg_encode(&A, w, 21, w * 4, grad_rows, &w, 90, &out) == 0);
    TEST_CHECK(out.len > 600);
    TEST_CHECK(out.data[0] == 0xFF && out.data[1] == 0xD8);
    TEST_CHECK(out.data[out.len - 2] == 0xFF && out.data[out.len - 1] == 0xD9);
    /* no unstuffed 0xFF inside the entropy-coded data: past the SOS header
     * every FF is followed by 00 (stuffing), RSTn or EOI */
    w = 64;
    vjo_buf_init(&out, &A);
    TEST_ASSERT(vjo_jpeg_encode(&A, w, 64, w * 4, noise_rows, &w, 95, &out) == 0);
    {
        size_t i = 2, ecs = 0;
        int stuffed = 0;
        while (!ecs && i + 4 <= out.len && out.data[i] == 0xFF) {
            size_t seg = ((size_t)out.data[i + 2] << 8) | out.data[i + 3];
            if (out.data[i + 1] == 0xDA)
                ecs = i + 2 + seg;
            i += 2 + seg;
        }
        TEST_ASSERT(ecs > 0 && ecs < out.len);
        for (i = ecs; i + 1 < out.len; i++) {
            uint8_t m = out.data[i + 1];
            if (out.data[i] != 0xFF)
                continue;
            if (m == 0x00)
                stuffed++;
            else if (!(m >= 0xD0 && m <= 0xD7) && !(m == 0xD9 && i + 2 == out.len))
                TEST_CHECK_(0, "unstuffed FF %02X at %lu", m, (unsigned long)i);
        }
        TEST_CHECK_(stuffed > 0, "%d stuffed FF bytes", stuffed);
    }
    vjo_buf_init(&out, &A);
    TEST_CHECK(vjo_jpeg_encode(&A, 0, 21, 0, grad_rows, &w, 90, &out) == -1);
}

/* ---------- recorded fixtures (host/fixtures/<name>/{lens.pb,jpdb.json|jiten.json,expected.txt}) ---------- */

static int fixture_has(const char *dir, const char *name)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static void test_fixtures(void)
{
    static uint8_t fmem[1u << 20];
    VjoArena files;
    DIR *d = opendir(VJO_FIXTURES);
    struct dirent *e;
    int n = 0;
    TEST_ASSERT_(d != NULL, "fixtures dir %s", VJO_FIXTURES);
    while ((e = readdir(d))) {
        char dir[1024], path[1100];
        VjoConfig cfg;
        VjoOverlayData od;
        size_t len;
        char *expected, *got;
        if (e->d_name[0] == '.')
            continue;
        snprintf(dir, sizeof(dir), "%s/%s", VJO_FIXTURES, e->d_name);
        snprintf(path, sizeof(path), "%s/expected.txt", dir);
        setup();
        expected = vjo_read_file(&A, path, &len);
        if (!expected)
            continue;
        /* same config as tools/record_fixtures.sh's vjo-cli --replay */
        vjo_config_defaults(&cfg);
        vjo_replay_config(&cfg, dir);
        vjo_arena_init(&files, fmem, sizeof(fmem));
        TEST_CASE(e->d_name);
        /* without one the replay fails the lookup like an offline network,
         * and the golden would carry that error */
        TEST_CHECK_(fixture_has(dir, "jiten.json") || fixture_has(dir, "jpdb.json"),
                    "%s has no dictionary recording (jiten.json or jpdb.json)", e->d_name);
        vjo_replay_overlay(&A, &files, dir, &cfg, &od);
        TEST_CHECK(od.failed_stage != VJO_STAGE_OCR);
        got = vjo_render_overlay(&A, &od);
        TEST_CHECK(!strcmp(got, expected));
        TEST_MSG("fixture %s differs:\n--- got ---\n%s\n--- expected ---\n%s", e->d_name, got, expected);
        n++;
    }
    closedir(d);
    TEST_CHECK_(n > 0, "%d fixtures", n);
}

TEST_LIST = {
    {"pb_roundtrip", test_pb_roundtrip},
    {"buf_interleaved", test_buf_interleaved},
    {"lens_request_layout", test_lens_request_layout},
    {"lens_response_parse", test_lens_response_parse},
    {"filter_lines", test_filter_lines},
    {"utf", test_utf},
    {"jpdb_request", test_jpdb_request},
    {"jpdb_parse", test_jpdb_parse},
    {"jiten_ruby", test_jiten_ruby},
    {"jiten_request", test_jiten_request},
    {"jiten_parse", test_jiten_parse},
    {"entries", test_entries},
    {"entries_surrogates_and_trim", test_entries_surrogates_and_trim},
    {"foreground", test_foreground},
    {"config", test_config},
    {"regions", test_regions},
    {"http", test_http},
    {"styled", test_styled},
    {"jpegsw", test_jpegsw},
    {"fixtures", test_fixtures},
    {NULL, NULL},
};
