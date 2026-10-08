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
#include "scene.h"
#include "textfilter.h"
#include "triggers.h"
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
    /* one entry per occurrence: を twice */
    TEST_CHECK(l.n_entries == 4);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "𠮟る。【お茶】を飲み、茶を"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 1), "𠮟る。お茶【を】飲み、茶を"));
    TEST_CHECK(!strcmp(l.entries[2].text, "飲む (のむ) 299\nto drink; to gulp"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 3), "𠮟る。お茶を飲み、【茶】を"));
    TEST_CHECK(l.entries[3].vocab == l.entries[1].vocab && l.entries[3].text == l.entries[1].text);

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

    /* a word used twice: an entry for each, in text order; a word without
     * a token comes last */
    jp = "{\"tokens\":[[0,0,1,null],[1,1,1,null],[0,2,1,null]],\"vocabulary\":["
         "[10,20,30,\"猫\",\"ねこ\",1500,[\"cat\"]],"
         "[1,2,3,\"と\",\"と\",20,[\"and\"]],"
         "[11,21,31,\"犬\",\"いぬ\",900,[\"dog\"]]]}";
    TEST_ASSERT(vjo_jpdb_parse_response(&A, jp, strlen(jp), &r) == 0);
    TEST_ASSERT(vjo_entries_build(&A, "猫と猫", &r, &l) == 0);
    TEST_CHECK(l.n_entries == 4);
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 0), "【猫】と猫"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 1), "猫【と】猫"));
    TEST_CHECK(!strcmp(vjo_render_highlight(&A, &l, 2), "猫と【猫】"));
    TEST_CHECK(l.entries[2].vocab == l.entries[0].vocab);
    TEST_CHECK(!strcmp(l.entries[3].text, "犬 (いぬ) 900\ndog") && l.entries[3].hl_start == -1);
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
    TEST_CHECK(fg_set_game_active(f, GAME, VJO_GAME, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(f, GAME, VJO_GAME, 0));
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
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME, &iev) < 0 && iev == 0); /* not the foreground */
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME, 0));

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
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) == 0 && iev == 0);
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
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) < 0 && iev == 0);
    TEST_CHECK(fg_is(&f, APP2, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP2, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));

    /* SceShell restarts: the old one exits, the new one starts before it
     * registers (so it is a newcomer over the game), registers, and
     * classifies itself as not a game. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, SHELL, PROCEV_EXIT) == 0);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME, 0));
    TEST_CHECK(fg_process_event(&f, SHELL2, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, SHELL2, 0, GAME));
    f.shell_pid = SHELL2; /* vjoRegisterShell */
    TEST_CHECK(fg_set_game_active(&f, SHELL2, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_process_event(&f, SHELL2, PROCEV_RESUME) == 0);
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME, 0));

    /* A static framebuffer game keeps its mode until a system app opens over
     * it: it comes back inactive, to be re-classified by the shell. */
    fg_game_running(&f);
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME_STATIC_FB, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_is(&f, GAME, VJO_GAME_STATIC_FB, 0));
    TEST_CHECK(fg_process_event(&f, APP, PROCEV_STARTUP) == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, APP, 0, GAME));
    TEST_CHECK(fg_set_game_active(&f, APP, VJO_GAME_NONE, &iev) == 0 && iev == IEV_GAME_START);
    TEST_CHECK(fg_is(&f, GAME, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, GAME, VJO_GAME_STATIC_FB, &iev) == 0 && iev == IEV_ACTIVATE);
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_EXIT) == IEV_GAME_EXIT);
    TEST_CHECK(fg_is(&f, 0, 0, 0));

    /* The game exits with nothing behind it; pid 0 is never the foreground. */
    fg_game_running(&f);
    TEST_CHECK(fg_process_event(&f, GAME, PROCEV_EXIT) == IEV_GAME_EXIT);
    TEST_CHECK(fg_is(&f, 0, 0, 0));
    TEST_CHECK(fg_set_game_active(&f, 0, VJO_GAME, &iev) < 0);
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
                      "subtitle_button = select\n"
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
    TEST_CHECK(c.toggle_button == VJO_TRIGGER_SELECT_R && c.subtitle_button == VJO_TRIGGER_SELECT);
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
                   d.font_size_ja == 18 && d.font_size_en == 14 &&
                   d.toggle_button == VJO_TRIGGER_L_R && d.subtitle_button == VJO_TRIGGER_SELECT_R);
    }

    /* the same button for both: the toggle keeps it */
    {
        const char *same = "toggle_button = select+r\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, same, strlen(same));
        TEST_CHECK(c.toggle_button == VJO_TRIGGER_SELECT_R && c.subtitle_button == VJO_TRIGGER_SELECT_L &&
                   c.n_warnings == 1);
        same = "toggle_button = start\nsubtitle_button = start\n";
        vjo_config_defaults(&c);
        vjo_config_parse(&c, same, strlen(same));
        TEST_CHECK(c.toggle_button == VJO_TRIGGER_START && c.subtitle_button == VJO_TRIGGER_SELECT_R &&
                   c.n_warnings == 1);
    }


    /* settings renamed/removed since earlier releases, as in tools/migrate_config.py */
    {
        const char *old = "api_key = k1\nfont_size = 20\nhw_jpeg = on\nfrequency_filter = 3\ncombo_delay_ms = 0\n";
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
#define P(s, t, g) vjo_region_parse(s, strlen(s), t, g)
    VjoRect r = {100, 200, 30000, 40000}, g;
    char line[64], *t;
    int n = vjo_region_format(line, sizeof(line), VJO_REGION_FALLBACK, &r);
    TEST_CHECK(n > 0 && !strcmp(line, "region = 100,200,30000,40000\n"));
    TEST_CHECK(vjo_region_parse(line, (size_t)n, NULL, &g) == VJO_REGION_ALL && !memcmp(&g, &r, sizeof(r)));
    TEST_CHECK(vjo_region_format(line, 10, VJO_REGION_FALLBACK, &r) == -1);
    TEST_CHECK(P(" region=1, 2 ,3,4\r\n", NULL, &g) == VJO_REGION_ALL && g.x == 1 && g.h == 4);
    TEST_CHECK(P("region = 1,2,0,3\n", NULL, &g) == 0);        /* zero size */
    TEST_CHECK(P("region = 60000,2,6000,3\n", NULL, &g) == 0); /* off screen */
    TEST_CHECK(P("region = 1,2,3\n", NULL, &g) == 0);
    TEST_CHECK(P("region = 1,2,3,4x\n", NULL, &g) == 0);
    TEST_CHECK(P("PCSG00123 = 1,2,3,4\n", NULL, &g) == 0);
    TEST_CHECK(P("", "PCSG00123", &g) == 0);

    /* per game: the game's line wins, wherever it is; else the fallback */
    {
        const char *ini = "region = 1,1,1,1\nPCSG00123 = 2,2,2,2\nPCSG0012 = 3,3,3,3\n";
        TEST_CHECK(P(ini, "PCSG00123", &g) == VJO_REGION_GAME && g.x == 2);
        TEST_CHECK(P(ini, "PCSG0012", &g) == VJO_REGION_GAME && g.x == 3);
        TEST_CHECK(P(ini, "PCSB00001", &g) == VJO_REGION_ALL && g.x == 1);
        TEST_CHECK(P("PCSG00123 = 2,2,0,2\nregion = 1,1,1,1\n", "PCSG00123", &g) == VJO_REGION_ALL);
        TEST_CHECK(P("region = 1,1,1,1\nPCSG00123 =  full \r\n", "PCSG00123", &g) == VJO_REGION_GAME && g.w == 0);
        TEST_CHECK(P("PCSG00123 = fullx\n", "PCSG00123", &g) == 0);
        TEST_CHECK(P("region = 1,1,1,1", "PCSG00123", &g) == VJO_REGION_ALL); /* no final newline */
    }

    /* update: replace, add, remove; other lines kept */
    setup();
    {
        VjoRect q = {5, 6, 7, 8};
        const char *ini = "region = 1,1,1,1\n; note\nPCSG00123 = 2,2,2,2";
        t = vjo_region_update(&A, ini, strlen(ini), "PCSG00123", &q);
        TEST_CHECK(t && !strcmp(t, "region = 1,1,1,1\n; note\nPCSG00123 = 5,6,7,8\n"));
        t = vjo_region_update(&A, ini, strlen(ini), "PCSB00001", &q);
        TEST_CHECK(t && !strcmp(t, "region = 1,1,1,1\n; note\nPCSG00123 = 2,2,2,2\nPCSB00001 = 5,6,7,8\n"));
        q.w = 0; /* full screen */
        t = vjo_region_update(&A, ini, strlen(ini), "PCSG00123", &q);
        TEST_CHECK(t && !strcmp(t, "region = 1,1,1,1\n; note\nPCSG00123 = full\n"));
        q.w = 7;
        t = vjo_region_update(&A, "", 0, "PCSG00123", &q);
        TEST_CHECK(t && !strcmp(t, "PCSG00123 = 5,6,7,8\n"));
    }
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
    TEST_CHECK(strstr(m.out, "Connection: close\r\n") != NULL);
    /* a kept-alive request */
    m.out_len = 0;
    req.keep_alive = 1;
    TEST_CHECK(vjo_http_send(&c, &req) == 0);
    m.out[m.out_len] = 0;
    TEST_CHECK(strstr(m.out, "Connection:") == NULL && strstr(m.out, "A: b\r\n\r\nhello") != NULL);

    m.in = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    m.len = strlen(m.in);
    m.step = 3;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0);
    TEST_CHECK(resp.status == 200 && !strcmp(resp.body, "Wikipedia") && resp.keep_alive);
    /* trailers are read up to the end of the reply */
    m.in = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\nX-T: 1\r\n\r\n";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && !strcmp(resp.body, "ok") && resp.keep_alive);
    TEST_CHECK(m.pos == m.len);
    /* the server closes after this reply */
    m.in = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && !strcmp(resp.body, "ok") && !resp.keep_alive);
    /* more than the reply arrived: the connection is out of step */
    m.in = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP";
    m.len = strlen(m.in);
    m.pos = 0;
    m.step = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && !strcmp(resp.body, "ok") && !resp.keep_alive);
    m.step = 3;

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
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && !strcmp(resp.body, "until close") && !resp.keep_alive);
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
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP && resp.status == 0);
    m.in = "HTTP/1.1 -20 OK\r\nContent-Length: 0\r\n\r\n";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP);
    /* no body, whatever the headers say */
    m.in = "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n";
    m.len = strlen(m.in);
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == 0 && resp.body_len == 0 && resp.keep_alive);
    /* no reply at all (a kept connection the server dropped) */
    m.in = "";
    m.len = 0;
    m.pos = 0;
    TEST_CHECK(vjo_http_recv(&A, &c, 100, &resp) == VJO_E_HTTP && resp.status == 0);
}

/* ---------- kept connections (VjoNetPool) over a fake network ---------- */

/* Connection k serves responses[k] (several replies in turn when kept;
 * NULL = refused) one byte per read, so the reader never reads ahead. */
typedef struct {
    const char *responses[4];
    VjoMemConn conns[4];
    char hosts[4][32];
    int n_connects, n_disconnects;
    int acquire_rc; /* what acquire() reports */
    uint64_t now;
} PoolNet;

static int pool_connect(void *ud, const char *host, int port, int timeout_us, int io_timeout_us, VjoConn *out)
{
    PoolNet *f = (PoolNet *)ud;
    int k = f->n_connects;
    (void)port;
    (void)timeout_us;
    (void)io_timeout_us;
    if (k >= 4 || !f->responses[k])
        return VJO_E_NET;
    f->n_connects++;
    snprintf(f->hosts[k], sizeof(f->hosts[k]), "%s", host);
    memset(&f->conns[k], 0, sizeof(f->conns[k]));
    f->conns[k].in = f->responses[k];
    f->conns[k].len = strlen(f->responses[k]);
    f->conns[k].step = 1;
    vjo_memconn_init(&f->conns[k], out);
    return VJO_OK;
}

static void pool_disconnect(void *ud, VjoConn *c)
{
    (void)c;
    ((PoolNet *)ud)->n_disconnects++;
}

static int pool_acquire(void *ud, VjoConn *c)
{
    (void)c;
    return ((PoolNet *)ud)->acquire_rc;
}

static uint64_t pool_now(void *ud)
{
    return ((PoolNet *)ud)->now;
}

static void pool_net(PoolNet *f, VjoPlatform *p, VjoNetPool *pool, int slots)
{
    static uint8_t mem[4][32 * 1024];
    TEST_ASSERT(vjo_net_pool_size(slots) <= sizeof(mem));
    memset(f, 0, sizeof(*f));
    f->now = 1000000;
    memset(p, 0, sizeof(*p));
    p->ud = f;
    p->connect = pool_connect;
    p->disconnect = pool_disconnect;
    p->acquire = pool_acquire;
    p->now_us = pool_now;
    p->plain_http = 1;
    vjo_net_pool_init(pool, mem, vjo_net_pool_size(slots), 10000000);
    p->pool = pool;
}

#define REPLY1(c) "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n" c

static int pool_get(const VjoPlatform *p, const char *host, const char *expect)
{
    VjoHttpRequest req;
    VjoHttpResponse resp;
    VjoErr err;
    int rc;
    memset(&req, 0, sizeof(req));
    memset(&err, 0, sizeof(err));
    req.method = "GET";
    req.host = host;
    req.path = "/";
    rc = vjo_http_request(&A, p, 443, 1, &req, 100, &resp, &err);
    if (rc == VJO_OK && expect && strcmp(resp.body, expect))
        return -1;
    return rc;
}

static void test_net_pool(void)
{
    PoolNet f;
    VjoPlatform p;
    VjoNetPool pool;
    setup();

    /* two requests on one connection */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab" "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK && pool_get(&p, "a.example", "cd") == VJO_OK);
    TEST_CHECK(f.n_connects == 1 && f.n_disconnects == 0);
    vjo_net_pool_close(&p);
    TEST_CHECK(f.n_disconnects == 1);

    /* no reply on the kept connection (the server dropped it): once more
     * on a new one */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab";
    f.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK && pool_get(&p, "a.example", "cd") == VJO_OK);
    TEST_CHECK(f.n_connects == 2 && f.n_disconnects == 1);

    /* a reply cut short is an error, not retried */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab" "HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\ncd";
    f.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nef";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK && pool_get(&p, "a.example", NULL) != VJO_OK);
    TEST_CHECK(f.n_connects == 1 && f.n_disconnects == 1);

    /* the server closed it while idle (acquire says so): a new one */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab";
    f.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK);
    f.acquire_rc = VJO_E_NET;
    TEST_CHECK(pool_get(&p, "a.example", "cd") == VJO_OK && f.n_connects == 2 && f.n_disconnects == 1);

    /* cancelled: no request, no new connection, the kept one stays */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab";
    f.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK);
    f.acquire_rc = VJO_E_CANCELLED;
    TEST_CHECK(pool_get(&p, "a.example", NULL) == VJO_E_CANCELLED && f.n_connects == 1 && f.n_disconnects == 0);

    /* idle too long: closed, a new one */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab" "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nxx";
    f.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK);
    f.now += 10000001;
    TEST_CHECK(pool_get(&p, "a.example", "cd") == VJO_OK && f.n_connects == 2 && f.n_disconnects == 1);

    /* "Connection: close": not kept */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nab";
    f.responses[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncd";
    TEST_CHECK(pool_get(&p, "a.example", "ab") == VJO_OK && f.n_disconnects == 1);
    TEST_CHECK(pool_get(&p, "a.example", "cd") == VJO_OK && f.n_connects == 2);

    /* a third host takes the least recently used slot */
    pool_net(&f, &p, &pool, 2);
    f.responses[0] = REPLY1("a") REPLY1("A");
    f.responses[1] = REPLY1("b");
    f.responses[2] = REPLY1("c");
    TEST_CHECK(pool_get(&p, "a.example", "a") == VJO_OK);
    f.now += 1000;
    TEST_CHECK(pool_get(&p, "b.example", "b") == VJO_OK);
    f.now += 1000;
    TEST_CHECK(pool_get(&p, "a.example", "A") == VJO_OK); /* a is now the more recent */
    f.now += 1000;
    TEST_CHECK(pool_get(&p, "c.example", "c") == VJO_OK);
    TEST_CHECK(f.n_connects == 3 && f.n_disconnects == 1);
    vjo_net_pool_close(&p);
    TEST_CHECK(f.n_disconnects == 3);
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
        /* the subtitle (after the OCR phase) is the full overlay's header */
        TEST_CHECK(od.sentence && !strcmp(od.sentence, od.list.header));
        got = vjo_render_overlay(&A, &od);
        TEST_CHECK(!strcmp(got, expected));
        TEST_MSG("fixture %s differs:\n--- got ---\n%s\n--- expected ---\n%s", e->d_name, got, expected);
        n++;
    }
    closedir(d);
    TEST_CHECK_(n > 0, "%d fixtures", n);
}

/* kernel/triggers.c (pad masks as SCE_CTRL_*) */
#define B_SELECT 0x1u
#define B_L 0x100u
#define B_R 0x200u

static void test_trigger_edges(void)
{
    TrigEdge sel = {0, 0}, selr = {0, 0}, lr = {0, 0};
    const uint32_t s = B_SELECT, sr = B_SELECT | B_R;
    /* no overlap: fires on press, once */
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, B_L) == 0);
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, B_L | B_R) == 1);
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, B_L | B_R) == 0);
    TEST_CHECK(trig_edge(&lr, B_L | B_R, sr, 0) == 0);
    /* select alone: the subset fires on release */
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, 0) == 1);
    /* select, then R: only select+r fires (on press) */
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0 && trig_edge(&selr, sr, s, s) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, sr) == 0 && trig_edge(&selr, sr, s, sr) == 1);
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0 && trig_edge(&selr, sr, s, s) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, 0) == 0 && trig_edge(&selr, sr, s, 0) == 0);
    /* R, then select */
    TEST_CHECK(trig_edge(&sel, s, sr, B_R) == 0 && trig_edge(&selr, sr, s, B_R) == 0);
    TEST_CHECK(trig_edge(&sel, s, sr, sr) == 0 && trig_edge(&selr, sr, s, sr) == 1);
    TEST_CHECK(trig_edge(&sel, s, sr, 0) == 0 && trig_edge(&selr, sr, s, 0) == 0);
    /* the next select tap fires again */
    TEST_CHECK(trig_edge(&sel, s, sr, s) == 0 && trig_edge(&sel, s, sr, 0) == 1);
    /* no buttons (rear double tap) never fires */
    TEST_CHECK(trig_edge(&lr, 0, sr, 0xFFFFu) == 0);
}

static void test_trigger_filter(void)
{
    TrigConfig c = {{B_L | B_R, B_SELECT | B_R}, {0, 0}, 0};
    TrigHold h[TRIG_COUNT];
    int64_t t = 1000000;

    /* no delay: hidden only while the whole combo is held */
    TEST_CHECK(trig_filter(&c, NULL, B_L, t) == B_L);
    TEST_CHECK(trig_filter(&c, NULL, B_L | B_R | 0x4000u, t) == 0x4000u);
    c.single[TRIG_SUBTITLE] = 1;
    c.mask[TRIG_SUBTITLE] = B_SELECT;
    TEST_CHECK(trig_filter(&c, NULL, B_SELECT | 0x4000u, t) == 0x4000u);

    /* delay 50 ms: l+r pressed L first never shows L */
    c.mask[TRIG_SUBTITLE] = B_SELECT | B_R;
    c.single[TRIG_SUBTITLE] = 0;
    c.delay_us = 50000;
    memset(h, 0, sizeof(h));
    TEST_CHECK(trig_filter(&c, h, B_L, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 16000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L | B_R, t + 32000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 200000) == 0); /* held over: still hidden */
    TEST_CHECK(trig_filter(&c, h, 0, t + 216000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 232000) == 0);  /* no replay */

    /* L held alone: reaches the game after the delay */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_L, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 49000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_L, t + 50000) == B_L);
    TEST_CHECK(trig_filter(&c, h, 0, t + 66000) == 0);

    /* a short L tap is replayed after its release, for at least two frames */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_L, t) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 16000) == B_L);
    TEST_CHECK(trig_filter(&c, h, 0, t + 60000) == B_L);
    TEST_CHECK(trig_filter(&c, h, 0, t + 16000 + 50000) == 0);

    /* select+r (the other trigger) shares R: l+r does not replay it */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_SELECT, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_SELECT | B_R, t + 16000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 32000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 48000) == 0);
    /* nor when R is let go first */
    t += 1000000;
    TEST_CHECK(trig_filter(&c, h, B_R, t) == 0);
    TEST_CHECK(trig_filter(&c, h, B_SELECT | B_R, t + 16000) == 0);
    TEST_CHECK(trig_filter(&c, h, B_SELECT, t + 32000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 48000) == 0);
    TEST_CHECK(trig_filter(&c, h, 0, t + 64000) == 0);
}

/* ---- change detection (kernel/scene.c) ---- */

#define FRAME_MAX_W 960
#define FRAME_MAX_H 544
static uint32_t frame[FRAME_MAX_W * FRAME_MAX_H];
static uint32_t frame_w, frame_h;

#define DARK  0xFF202020u
#define WHITE 0xFFFFFFFFu
#define GLYPH 24
#define TICK  133000 /* a signature every 8 frames at 60 fps */
#define NO_INPUT 0   /* no player input (long before the tests' clock) */

static void frame_new(uint32_t w, uint32_t h)
{
    frame_w = w;
    frame_h = h;
    for (uint32_t i = 0; i < w * h; i++)
        frame[i] = DARK;
}

static void frame_rect(uint32_t x, uint32_t y, uint32_t rw, uint32_t rh, uint32_t color)
{
    for (uint32_t r = y; r < y + rh; r++)
        for (uint32_t c = x; c < x + rw; c++)
            frame[r * frame_w + c] = color;
}

/* A made-up glyph: about a third of its pixels inked, by seed. Glyphs of
 * different seeds have the same ink density, so a line swap keeps each
 * cell's brightness and only the pixels differ. */
static void frame_glyph(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t v = seed * 2654435761u + 1;
    for (uint32_t r = 0; r < GLYPH; r++)
        for (uint32_t c = 0; c < GLYPH; c++) {
            v = v * 1103515245u + 12345u;
            frame[(y + r) * frame_w + x + c] = (v >> 16) % 3 == 0 ? WHITE : DARK;
        }
}

static void frame_text(uint32_t x, uint32_t y, uint32_t seed, int n)
{
    for (int i = 0; i < n; i++)
        frame_glyph(x + (uint32_t)i * GLYPH, y, seed + (uint32_t)i);
}

static SceneSig frame_sig(void)
{
    static SceneAcc acc;
    SceneSig sig;
    scene_acc_begin(&acc, &sig, frame_w, frame_h);
    for (uint32_t y = 0; y < frame_h; y++)
        scene_acc_row(&acc, y, &frame[y * frame_w]); /* unsampled rows are skipped */
    return sig;
}

/* Feeds sig until the screen settles; returns the ticks it took (0: never
 * within max). */
static int settle(SceneTracker *t, const SceneSig *sig, int64_t *now, int max)
{
    for (int i = 1; i <= max; i++)
        if (scene_update(t, sig, *now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED)
            return i;
    return 0;
}

static void test_scene_grid(void)
{
    SceneAcc acc;
    SceneSig sig;
    memset(&acc, 0, sizeof(acc));
    scene_acc_begin(&acc, &sig, 960, 544);
    TEST_CHECK(acc.cols == 16 && acc.ystep == 16 && acc.xstep == 4);
    scene_acc_begin(&acc, &sig, 900, 120); /* a text box: 32x4 */
    TEST_CHECK(acc.cols == 32 && acc.y0[4] == 120);
    scene_acc_begin(&acc, &sig, 32, 32);
    TEST_CHECK(acc.ystep == 1 && acc.xstep == 1);
    /* every grid row gets a sampled row */
    for (uint32_t h = 32; h <= 544; h += 7) {
        scene_acc_begin(&acc, &sig, 960, h);
        for (uint32_t r = 0; acc.y0[r] < h; r++)
            TEST_CHECK(acc.y0[r + 1] - acc.y0[r] >= acc.ystep);
    }
}

static void test_scene_changes(void)
{
    SceneTracker t;
    SceneSig a, b, sig;
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    /* the first frame is a new screen; it settles once, after the window */
    frame_new(960, 544);
    frame_text(40, 420, 100, 12);
    a = frame_sig();
    TEST_CHECK(scene_update(&t, &a, now, NO_INPUT, NO_INPUT) == 0 && t.id == 1 && !t.stable);
    TEST_CHECK(settle(&t, &a, &now, 10) == 3);
    TEST_CHECK(t.stable && scene_unsettled_us(&t, now) == 0);
    TEST_CHECK(scene_update(&t, &a, now += TICK, NO_INPUT, NO_INPUT) == 0);

    /* another line of the same length and ink: a new screen, and a capture
     * of the previous line (taken just before) does not match it */
    frame_new(960, 544);
    frame_text(40, 420, 200, 12);
    b = frame_sig();
    TEST_CHECK(scene_update(&t, &b, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == 2 && !t.stable);
    TEST_CHECK(scene_match(&t, &a) == 0 && scene_match(&t, &b) == 2);

    /* a two-glyph reply replaced by another one, a line per 0.4 s */
    frame_new(960, 544);
    frame_text(40, 420, 300, 2);
    sig = frame_sig();
    scene_update(&t, &sig, now += 400000, NO_INPUT, NO_INPUT);
    id = t.id;
    frame_new(960, 544);
    frame_text(40, 420, 400, 2);
    sig = frame_sig();
    TEST_CHECK(scene_update(&t, &sig, now += 400000, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);

    /* a different grid (resolution or region): a new screen */
    frame_new(480, 272);
    sig = frame_sig();
    TEST_CHECK(scene_match(&t, &sig) == 0);
    TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 2);

    /* a reset keeps the numbering: an old number never matches again */
    id = t.id;
    scene_reset(&t);
    TEST_CHECK(scene_match(&t, &sig) == 0);
    TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
}

/* A text box (900x120: 32x4 cells of about 28x30 px). */
#define ARROW_X 846 /* inside one cell */
#define ARROW_Y 92

static SceneSig box_with_line(uint32_t seed, uint32_t arrow)
{
    frame_new(900, 120);
    frame_text(20, 10, seed, 20);
    if (arrow)
        frame_rect(ARROW_X, ARROW_Y, 20, 20, arrow);
    return frame_sig();
}

/* Feeds an icon cycling through n states (one per `every` signatures)
 * for `ticks` signatures; returns how often the screen settled. */
static int run_icon(SceneTracker *t, const SceneSig *states, int n, int every, int ticks, int64_t *now)
{
    int settled = 0;
    for (int i = 0; i < ticks; i++)
        settled += scene_update(t, &states[i / every % n], *now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED;
    return settled;
}

/* The "next" arrow blinks (about 0.5 s on, 0.5 s off). Its first
 * appearance is a new state, which settles like any change; once it goes
 * back to a recent state, settling waits until it has blinked for
 * SCENE_ANIM_SPAN_US and is masked, then the screen settles for good; a
 * capture with it on or off is the screen. */
static void test_scene_blinking(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &st[1], &now, 10) == 3);
    TEST_CHECK(run_icon(&t, st, 2, 4, 24, &now) == 2); /* 3.2 s */
    id = t.id;
    TEST_CHECK(t.stable && t.masked == 1);
    TEST_CHECK(run_icon(&t, st, 2, 4, 40, &now) == 0 && t.id == id);
    TEST_CHECK(scene_match(&t, &st[0]) == id && scene_match(&t, &st[1]) == id);

    /* a slow blink (1 s on, 1 s off) is masked too */
    memset(&t, 0, sizeof(t));
    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    TEST_CHECK(run_icon(&t, st, 2, 8, 40, &now) == 2 && t.masked == 1);
}

/* An icon with many states (bobbing, color cycle) changes every signature. */
static void test_scene_animated_icon(void)
{
    static const uint32_t colors[6] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u,
                                       0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFF00u};
    SceneTracker t;
    SceneSig st[6];
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 6; k++)
        st[k] = box_with_line(100, colors[k]);
    TEST_CHECK(run_icon(&t, st, 6, 1, 24, &now) == 1); /* revisits from the 7th state */
    id = t.id;
    TEST_CHECK(run_icon(&t, st, 6, 1, 40, &now) == 0 && t.id == id && t.masked == 1);

    /* the next line under the still blinking icon: a change */
    {
        SceneSig next[6];
        for (int k = 0; k < 6; k++) {
            next[k] = box_with_line(200, colors[k]);
        }
        TEST_CHECK(scene_update(&t, &next[0], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
    }
}

/* The icon stops in another state than the screen's first frame: once its
 * mask ends, that is a change (whatever the mask hid is seen). */
static void test_scene_mask_expires(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    int64_t now = 1000000;
    uint32_t id;
    int ticks = 0;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[0], now, NO_INPUT, NO_INPUT);
    run_icon(&t, st, 2, 4, 30, &now); /* ends on st[1] */
    id = t.id;
    TEST_CHECK(t.masked == 1 && scene_match(&t, &st[1]) == id);
    while (t.id == id && ticks++ < 30)
        scene_update(&t, &st[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id == id + 1 && t.masked == 0);
    TEST_CHECK((int64_t)ticks * TICK <= SCENE_ANIM_GAP_US + 2 * TICK);
}

/* An animated background: too many cells to mask, so nothing is masked
 * (it would hide the text) and it never settles. */
static void test_scene_animated_background(void)
{
    SceneTracker t;
    SceneSig st[3];
    int64_t now = 1000000;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 3; k++) {
        frame_new(960, 544);
        frame_rect(0, 0, 960, 200, 0xFF000000u + (uint32_t)k * 40);
        frame_text(40, 420, 100, 12);
        st[k] = frame_sig();
    }
    TEST_CHECK(run_icon(&t, st, 3, 1, 60, &now) == 0);
    TEST_CHECK(t.masked == 0 && scene_unsettled_us(&t, now) >= 59 * TICK);
}

/* Going back to the previous screen is a change, however small: a menu
 * cursor moved to item B and back to A must not keep B's number. It settles
 * once it is clear that this is no animation. */
static void test_scene_revert(void)
{
    SceneTracker t;
    SceneSig st[2];
    int64_t now = 1000000;
    uint32_t id;
    int ticks;
    memset(&t, 0, sizeof(t));

    for (uint32_t k = 0; k < 2; k++) {
        frame_new(960, 544);
        frame_text(40, 420, 100 + k * 100, 2);
        st[k] = frame_sig();
    }
    scene_update(&t, &st[0], now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &st[0], &now, 10) == 3);
    scene_update(&t, &st[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &st[1], &now, 10) == 3);
    id = t.id;
    TEST_CHECK(scene_update(&t, &st[0], now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
    TEST_CHECK(scene_match(&t, &st[0]) == id + 1 && scene_match(&t, &st[1]) == 0);
    /* an icon-sized revert waits until it is clear it is no blink */
    ticks = settle(&t, &st[0], &now, 20);
    TEST_CHECK(ticks > 3 && (int64_t)ticks * TICK <= SCENE_ANIM_GAP_US + 2 * TICK);

    /* moved back and forth for seconds: that is masked like an icon, but
     * once it stops, the mask ends and that is a change */
    run_icon(&t, st, 2, 3, 30, &now); /* ends on st[1] */
    id = t.id;
    TEST_CHECK(t.masked == 2); /* the two glyphs' cells */
    ticks = 0;
    while (t.id == id && ticks++ < 30)
        scene_update(&t, &st[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id == id + 1 && scene_match(&t, &st[1]) == t.id && scene_match(&t, &st[0]) == 0);
}

/* A menu scrolled up and down for seconds, a press per item: changes the
 * player causes are never animation, so stopping on an item is a change at
 * once and a capture taken while scrolling does not match it. Item texts
 * of n glyphs (1 glyph: one cell, like an icon). */
static void menu_scroll(uint32_t glyphs)
{
    SceneTracker t;
    SceneSig item[4];
    int64_t now = 1000000;
    uint32_t id;
    memset(&t, 0, sizeof(t));

    for (uint32_t k = 0; k < 4; k++) {
        frame_new(960, 544);
        frame_text(40, 420, 100 + k * 50, (int)glyphs);
        item[k] = frame_sig();
    }
    scene_update(&t, &item[0], now, NO_INPUT, NO_INPUT);
    for (int step = 0; step < 12; step++) { /* 0.4 s per item, 4.8 s */
        int k = step % 6 < 3 ? step % 6 : 6 - step % 6; /* 0 1 2 3 2 1 0 ... */
        int64_t pressed = now + TICK / 2;
        for (int i = 0; i < 3; i++) {
            scene_update(&t, &item[k], now += TICK, pressed, pressed);
            TEST_CHECK(t.masked == 0);
        }
    }
    id = t.id; /* scrolling ended on item 1: stop on item 3 */
    TEST_CHECK(scene_match(&t, &item[1]) == id);
    now += TICK;
    TEST_CHECK(scene_update(&t, &item[3], now, now - TICK / 2, now - TICK / 2) == 0 && t.id == id + 1);
    TEST_CHECK(scene_match(&t, &item[3]) == t.id && scene_match(&t, &item[1]) == 0);

    /* wiggled 3, 2, 3, 2 right after: each step is a change */
    for (int i = 0; i < 4; i++) {
        id = t.id;
        now += 3 * TICK;
        TEST_CHECK(scene_update(&t, &item[i % 2 ? 3 : 2], now, now - TICK / 2, now - TICK / 2) == 0);
        TEST_CHECK(t.id == id + 1 && t.masked == 0);
    }
}

static void test_scene_menu_scroll(void)
{
    menu_scroll(12); /* descriptions */
    menu_scroll(1);  /* one-glyph names: 剣, 槍, 斧, 弓 */
}

/* The documented gap: a menu cycled with no input recorded (it advances by
 * itself, or reacts later than SCENE_INPUT_US) is masked like an icon, and
 * the stop is seen once the mask ends. */
static void test_scene_menu_without_input(void)
{
    SceneTracker t;
    SceneSig item[2];
    int64_t now = 1000000;
    uint32_t id;
    int ticks = 0;
    memset(&t, 0, sizeof(t));

    for (uint32_t k = 0; k < 2; k++) {
        frame_new(960, 544);
        frame_text(10, 420, 100 + k * 50, 1); /* inside one cell */
        item[k] = frame_sig();
    }
    scene_update(&t, &item[0], now, NO_INPUT, NO_INPUT);
    run_icon(&t, item, 2, 3, 30, &now); /* ends on item 1 */
    id = t.id;
    TEST_CHECK(t.masked == 1 && scene_match(&t, &item[0]) == id);
    while (t.id == id && ticks++ < 30)
        scene_update(&t, &item[1], now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id == id + 1 && (int64_t)ticks * TICK <= SCENE_ANIM_GAP_US + 2 * TICK);
    TEST_CHECK(scene_match(&t, &item[0]) == 0);
}

/* A held button (no edge after the press) keeps a masked icon masked; a
 * press or release ends the masks (a change), and the icon is masked again
 * once it cycles. */
static void test_scene_input_and_masks(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    int64_t now = 1000000, edge;
    uint32_t id;
    int settled = 0;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    run_icon(&t, st, 2, 4, 24, &now);
    id = t.id;
    TEST_CHECK(t.masked == 1 && t.stable);
    edge = now + TICK / 2; /* pressed, then held for 5 s */
    for (int i = 0; i < 40; i++) {
        now += TICK;
        settled += scene_update(&t, &st[i / 4 % 2], now, now - edge <= 1500000 ? now : edge + 1500000, edge) == SCENE_SETTLED;
        if (i == 0)
            TEST_CHECK(t.id == id + 1 && t.masked == 0); /* the press */
    }
    /* the blinks in the held-input window are the player's: a few settles
     * until it is masked again */
    id = t.id;
    TEST_CHECK(t.masked == 1 && t.stable && settled <= 5);
    settled = 0;
    for (int i = 0; i < 24; i++)
        settled += scene_update(&t, &st[i / 4 % 2], now += TICK, edge + 1500000, edge) == SCENE_SETTLED;
    TEST_CHECK(t.id == id && settled == 0);
}

/* A short wrapping menu scrolled by a held button for 5 s (auto-repeat
 * past the held-input window: masked like an icon), then tapped back: each
 * tap is seen, and so is the release without taps. */
static void test_scene_held_scroll(void)
{
    SceneTracker t;
    SceneSig item[4];
    int64_t now = 1000000, edge;
    int k = 0;
    memset(&t, 0, sizeof(t));

    for (uint32_t n = 0; n < 4; n++) {
        frame_new(960, 544);
        frame_text(10, 420, 100 + n * 50, 1);
        item[n] = frame_sig();
    }
    scene_update(&t, &item[0], now, NO_INPUT, NO_INPUT);
    edge = now + TICK / 2;
    for (int i = 0; i < 38; i++) { /* an item per 2 signatures */
        if (i % 2 == 0)
            k = (k + 1) % 4;
        now += TICK;
        scene_update(&t, &item[k], now, now - edge <= 1500000 ? now : edge + 1500000, edge);
    }
    TEST_CHECK(t.masked == 1); /* the documented held-scroll residual */
    for (int tap = 0; tap < 2; tap++) { /* released, then tapped back twice */
        uint32_t id = t.id;
        edge = now + TICK / 2;
        k = (k + 3) % 4;
        now += 2 * TICK;
        scene_update(&t, &item[k], now, edge, edge);
        TEST_CHECK(t.id > id && t.masked == 0);
        TEST_CHECK(scene_match(&t, &item[k]) == t.id && scene_match(&t, &item[(k + 1) % 4]) == 0);
    }
}

/* Two icons blinking at different rates (the "next" arrow and an AUTO
 * mark): both are masked and the screen settles for good. */
static void test_scene_two_icons(void)
{
    SceneTracker t;
    SceneSig st[4];
    int64_t now = 1000000;
    int settled = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 4; k++) {
        frame_new(900, 120);
        frame_text(20, 10, 100, 20);
        if (k & 1)
            frame_rect(ARROW_X, ARROW_Y, 20, 20, WHITE);
        if (k & 2)
            frame_rect(2, 92, 20, 20, WHITE); /* inside the first cell */
        st[k] = frame_sig();
    }
    for (int i = 0; i < 40; i++) /* the arrow every 4 signatures, AUTO every 6 */
        settled += scene_update(&t, &st[(i / 4 % 2) | (i / 6 % 2) << 1], now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED;
    TEST_CHECK(t.masked == 2 && t.stable && settled <= 3);
    settled = 0;
    for (int i = 40; i < 120; i++)
        settled += scene_update(&t, &st[(i / 4 % 2) | (i / 6 % 2) << 1], now += TICK, NO_INPUT, NO_INPUT) == SCENE_SETTLED;
    TEST_CHECK(settled == 0 && t.masked == 2);
}

/* A line replaced by a shorter one: its tail goes back to empty cells
 * (states seen before), which is no animation: it settles as usual. */
static void test_scene_shorter_line(void)
{
    SceneTracker t;
    SceneSig line_long, line_short;
    int64_t now = 1000000;
    memset(&t, 0, sizeof(t));

    frame_new(960, 544);
    line_long = frame_sig();
    scene_update(&t, &line_long, now, NO_INPUT, NO_INPUT);
    frame_text(40, 420, 100, 12);
    line_long = frame_sig();
    scene_update(&t, &line_long, now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &line_long, &now, 10) == 3);
    frame_new(960, 544);
    frame_text(40, 420, 200, 3);
    line_short = frame_sig();
    scene_update(&t, &line_short, now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &line_short, &now, 10) == 3);
}

/* The next line's arrow, soon after the last one's mask ended (it was
 * hidden while the line was typed): masked as soon as it cycles. */
static void test_scene_warm_mask(void)
{
    SceneTracker t;
    SceneSig st[2] = {box_with_line(100, WHITE), box_with_line(100, 0)};
    SceneSig next[2] = {box_with_line(200, WHITE), box_with_line(200, 0)};
    int64_t now = 1000000;
    int settled = 0, ticks = 0;
    memset(&t, 0, sizeof(t));

    scene_update(&t, &st[1], now, NO_INPUT, NO_INPUT);
    run_icon(&t, st, 2, 4, 24, &now);
    TEST_CHECK(t.masked == 1);
    while (t.masked && ticks++ < 30) /* the next line, typed for 2 s */
        scene_update(&t, &next[1], now += TICK, NO_INPUT, NO_INPUT);
    run_icon(&t, &next[1], 1, 1, 13, &now);
    settled = run_icon(&t, next, 2, 4, 14, &now); /* on, off, on, off */
    TEST_CHECK(t.masked == 1 && t.stable);
    TEST_CHECK(settled <= 1);
    TEST_CHECK(run_icon(&t, next, 2, 4, 16, &now) == 0);
}

/* Short lines (two glyphs) faded in and out over 4 signatures each, shown
 * 0.8 s: every line is a new screen that settles, and a capture of one
 * never matches another. (Once their cells are masked as cycling, a line
 * is seen when it has held SCENE_STABLE_US.) */
static void test_scene_fades(void)
{
    static const uint32_t levels[4] = {0xFF404040u, 0xFF808080u, 0xFFC0C0C0u, WHITE};
    SceneTracker t;
    SceneSig sig, prev;
    int64_t now = 1000000;
    memset(&t, 0, sizeof(t));

    frame_new(960, 544);
    sig = prev = frame_sig();
    scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    for (uint32_t line = 0; line < 8; line++) {
        for (int step = 0; step < 4; step++) { /* fade in */
            frame_new(960, 544);
            frame_text(40, 420, 100 + line * 10, 2);
            for (uint32_t p = 0; p < 960 * 544; p++)
                if (frame[p] == WHITE)
                    frame[p] = levels[step];
            sig = frame_sig();
            scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT);
        }
        /* masked as cycling from the third line: held SCENE_STABLE_US, then settled */
        TEST_CHECK(settle(&t, &sig, &now, 8) == (line < 2 ? 3 : 6));
        TEST_CHECK(scene_match(&t, &sig) == t.id && scene_match(&t, &prev) == 0);
        prev = sig;
        for (int step = 3; step >= 0; step--) { /* fade out */
            frame_new(960, 544);
            frame_text(40, 420, 100 + line * 10, 2);
            for (uint32_t p = 0; p < 960 * 544; p++)
                if (frame[p] == WHITE)
                    frame[p] = step ? levels[step - 1] : DARK;
            sig = frame_sig();
            scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT);
        }
    }
}

/* Typewriter text: each glyph is a change, a pause shorter than the window
 * does not settle it, and it settles a window after the last glyph. */
static void test_scene_typewriter(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 1000000, started;
    memset(&t, 0, sizeof(t));

    frame_new(900, 120);
    sig = frame_sig();
    scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    TEST_CHECK(settle(&t, &sig, &now, 10) == 3);
    started = now + TICK;
    for (int i = 0; i < 16; i++) {
        uint32_t id = t.id;
        frame_glyph(20 + (uint32_t)i * GLYPH, 10, 100 + (uint32_t)i);
        sig = frame_sig();
        TEST_CHECK(scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == 0 && t.id == id + 1);
        if (i == 7) /* a pause after a comma: 2 ticks < SCENE_STABLE_US */
            TEST_CHECK(settle(&t, &sig, &now, 2) == 0);
    }
    TEST_CHECK(scene_unsettled_us(&t, now) == now - started);
    TEST_CHECK(settle(&t, &sig, &now, 10) == 3);
}

/* A VN text box (a PSP game's, 458x64) whose "next" mark spins after the
 * last glyph: 8 frames (widths of a turn, the back shaded), 4 game frames
 * each, sampled every 9, in other cells on every line. The first line
 * settles once the mark is masked (SCENE_ANIM_SPAN_US); later lines
 * sooner, a mask being recent; none changes again until the next press,
 * so a result taken when it settles stays current. */
static void spinning_mark(uint32_t x, uint32_t y, uint32_t f)
{
    static const uint32_t width[8] = {14, 12, 8, 5, 2, 5, 8, 12};
    uint32_t ph = f / 4 % 8, w = width[ph];
    frame_rect(x + (15 - w) / 2, y, w, 15, 0xFFC060FFu);
    if (ph > 4)
        frame_rect(x + (15 - w) / 2, y + 3, w / 2, 4, 0xFF9040C0u);
    else
        frame_rect(x + 7, y + 3, w / 2, 4, 0xFFE0A0FFu);
}

/* The mark read while the game draws the next frame: its rows from `split`
 * down are still the previous frame's. */
static void torn_mark(uint32_t x, uint32_t y, uint32_t f, uint32_t split)
{
    static uint32_t top[15][16];
    spinning_mark(x, y, f);
    for (uint32_t r = 0; r < split; r++)
        for (uint32_t c = 0; c < 16; c++)
            top[r][c] = frame[(y + r) * frame_w + x + c];
    frame_rect(x, y, 16, 15, DARK);
    spinning_mark(x, y, f - 4);
    for (uint32_t r = 0; r < split; r++)
        for (uint32_t c = 0; c < 16; c++)
            frame[(y + r) * frame_w + x + c] = top[r][c];
}

static void test_scene_spinning_mark(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000; /* not warm at the start */
    uint32_t f = 0;
    memset(&t, 0, sizeof(t));

    for (uint32_t line = 0; line < 8; line++) {
        int64_t press = now += TICK;
        uint32_t n = 6 + line * 5 % 12, settled = 0, id = 0;
        for (uint32_t i = 1; i <= n; i++, now += TICK) { /* typed, a glyph per signature */
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)i);
            sig = frame_sig();
            scene_update(&t, &sig, now, press, press);
        }
        for (uint32_t k = 1; k <= 45; k++, now += TICK, f += 9) { /* read for 6 s */
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)n);
            spinning_mark(6 + n * GLYPH, 8 + line % 2 * 30, f);
            sig = frame_sig();
            if (scene_update(&t, &sig, now, press, press) == SCENE_SETTLED) {
                TEST_CHECK(!settled);
                settled = k;
                id = t.id;
            }
        }
        TEST_CHECK(settled && settled * TICK < (line ? SCENE_ANIM_SPAN_US : SCENE_ANIM_SPAN_US + 1000000));
        TEST_CHECK(line || settled * TICK >= SCENE_ANIM_SPAN_US);
        TEST_MSG("line %u settled after %u signatures", line, settled);
        TEST_CHECK(t.id == id && t.stable && t.masked >= 1);
    }
}

/* The spinning mark on a game whose frame is read while it draws (the PSP
 * emulator): a third of the reads mix two frames, at any row, so the mark
 * keeps showing states its cells have not seen. Once masked, it stays
 * masked: the line settles once and keeps its scene. */
static void test_scene_torn_mark(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    uint32_t f = 0, v = 1;
    memset(&t, 0, sizeof(t));

    for (uint32_t line = 0; line < 6; line++) {
        int64_t press = now += TICK;
        uint32_t n = 8 + line * 3, settles = 0, id = 0;
        for (uint32_t k = 1; k <= 60; k++, now += TICK, f += 8) { /* 8 s */
            frame_new(458, 64);
            frame_text(4, 4, 100 * line, (int)n);
            v = v * 1103515245u + 12345u;
            if (v >> 16 & 1 && (v >> 17) % 3 == 0)
                torn_mark(6 + n * GLYPH, 8, f, 1 + (v >> 20) % 14);
            else
                spinning_mark(6 + n * GLYPH, 8, f);
            sig = frame_sig();
            if (scene_update(&t, &sig, now, press, press) == SCENE_SETTLED) {
                settles++;
                id = t.id;
            }
        }
        TEST_CHECK(settles == 1 && t.id == id && t.masked >= 1);
        TEST_MSG("line %u: settled %u times, masked %d", line, settles, t.masked);
    }
}

/* The spinning mark, read whole or torn: the screen is quiet (worth
 * capturing) well before it settles, never while a line is typed. A
 * capture taken once quiet shows the screen from then on, through the
 * mask and the settle; one of the line still being typed never does. */
static void test_scene_quiet_mark(void)
{
    SceneTracker t;
    SceneSig sig, cap, part;
    int64_t now = 100000000, cap_us = 0, part_us = 0;
    uint32_t f = 0, v = 1;
    memset(&t, 0, sizeof(t));

    for (uint32_t line = 0; line < 8; line++) {
        int64_t press = now += TICK;
        uint32_t n = 6 + line * 5 % 12, quiet = 0, settled = 0, typing_quiet = 0, lost = 0;
        int torn = line >= 4;
        for (uint32_t i = 1; i <= n; i++, now += TICK) {
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)i);
            sig = frame_sig();
            typing_quiet += scene_update(&t, &sig, now, press, press) == SCENE_QUIET;
            if (i == 1 && line) /* the last line's capture */
                TEST_CHECK(scene_match_since(&t, &cap, cap_us) == 0);
            if (i == n - 1) {
                part = sig;
                part_us = now;
            }
        }
        for (uint32_t k = 1; k <= 45; k++, now += TICK, f += 9) {
            int r;
            frame_new(458, 64);
            frame_text(4, 4 + line % 2 * 30, 100 * line, (int)n);
            v = v * 1103515245u + 12345u;
            if (torn && v >> 16 & 1 && (v >> 17) % 3 == 0)
                torn_mark(6 + n * GLYPH, 8 + line % 2 * 30, f, 1 + (v >> 20) % 14);
            else
                spinning_mark(6 + n * GLYPH, 8 + line % 2 * 30, f);
            sig = frame_sig();
            r = scene_update(&t, &sig, now, press, press);
            if (r == 2 && !quiet) {
                quiet = k;
                cap = sig;
                cap_us = now;
            }
            if (r == 1 && !settled)
                settled = k;
            if (quiet && k > quiet)
                lost += scene_match_since(&t, &cap, cap_us) != t.id;
            TEST_CHECK(scene_match_since(&t, &part, part_us) == 0);
        }
        TEST_CHECK(!typing_quiet && quiet && settled && quiet < settled);
        TEST_MSG("line %u: quiet after %u signatures, settled after %u, quiet while typed %u", line, quiet,
                 settled, typing_quiet);
        TEST_CHECK(quiet <= 8); /* a torn read can delay it */
        TEST_CHECK(!lost);
        TEST_MSG("line %u: the capture did not show the screen %u times", line, lost);
    }
}

/* Typing never makes the screen quiet, however slow (a glyph every 3
 * signatures: within one spot for 400 ms, but only ever new states). */
static void test_scene_quiet_typing(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    int quiet = 0;
    memset(&t, 0, sizeof(t));
    for (int i = 1; i <= 16 * 3; i++) {
        frame_new(458, 64);
        frame_text(4, 4, 7, (i + 2) / 3);
        sig = frame_sig();
        quiet += scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT) == SCENE_QUIET;
    }
    TEST_CHECK(!quiet);
}

/* A "next" mark blinking at a fixed spot, the line typed up to it (not
 * the player's doing after SCENE_INPUT_US): the glyphs next to the masked
 * mark are changes, the screen settles with the whole line only. */
static void test_scene_text_next_to_mark(void)
{
    SceneTracker t;
    SceneSig sig, settled_sig;
    int64_t now = 100000000, press;
    int settled_glyphs = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 45; k++, now += TICK) {
        frame_new(960, 544);
        frame_text(40, 420, 100, 30);
        if (k / 4 % 2)
            frame_rect(910, 490, 20, 20, WHITE);
        sig = frame_sig();
        scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    }
    TEST_CHECK(t.masked >= 1);
    press = now;
    for (int i = 2; i <= 2 * 36 + 20; i++, now += TICK) { /* 36 glyphs, 2 signatures each */
        int g = i / 2 < 36 ? i / 2 : 36;
        frame_new(960, 544);
        frame_text(40, 470, 300, g);
        if (i / 4 % 2)
            frame_rect(910, 490, 20, 20, WHITE);
        sig = frame_sig();
        if (scene_update(&t, &sig, now, press, press) == SCENE_SETTLED) {
            settled_glyphs = g;
            settled_sig = sig;
        }
    }
    TEST_CHECK(settled_glyphs == 36);
    TEST_MSG("settled with %d glyphs", settled_glyphs);
    TEST_CHECK(scene_match(&t, &settled_sig) == t.id);
}

/* An icon two cells wide, blinking, then replaced by a glyph (no input):
 * the glyph is new content once it has held SCENE_STABLE_US. */
static void test_scene_content_in_icon(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    uint32_t id;
    int ticks = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 40; k++, now += TICK) {
        frame_new(900, 120);
        frame_text(20, 10, 100, 20);
        if (k / 4 % 2)
            frame_rect(830, 92, 30, 20, WHITE);
        sig = frame_sig();
        scene_update(&t, &sig, now, NO_INPUT, NO_INPUT);
    }
    TEST_CHECK(t.masked >= 1 && t.stable);
    id = t.id;
    frame_new(900, 120);
    frame_text(20, 10, 100, 20);
    frame_glyph(830, 92, 999);
    sig = frame_sig();
    while (t.id == id && ticks++ < 20)
        scene_update(&t, &sig, now += TICK, NO_INPUT, NO_INPUT);
    TEST_CHECK(t.id != id && ticks * TICK <= SCENE_STABLE_US + 2 * TICK);
    TEST_MSG("seen after %d signatures", ticks);
    TEST_CHECK(settle(&t, &sig, &now, 10) && scene_match(&t, &sig) == t.id);
}

/* An icon of 12 frames in one cell, looping (a sparkle): masked once it
 * has looped, as an icon of a few frames; then the screen stays settled. */
static void test_scene_many_frames(void)
{
    SceneTracker t;
    SceneSig sig;
    int64_t now = 100000000;
    uint32_t id = 0;
    int settled = 0;
    memset(&t, 0, sizeof(t));

    for (int k = 0; k < 60; k++, now += TICK) {
        frame_new(960, 544);
        frame_text(40, 420, 100, 12);
        frame_glyph(900, 480, 500 + (uint32_t)(k % 12));
        sig = frame_sig();
        if (scene_update(&t, &sig, now, NO_INPUT, NO_INPUT) == SCENE_SETTLED) {
            settled++;
            id = t.id;
        }
    }
    TEST_CHECK(settled == 1 && t.id == id && t.stable && t.masked >= 1);
    TEST_MSG("settled %d times", settled);
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
    {"trigger_edges", test_trigger_edges},
    {"trigger_filter", test_trigger_filter},
    {"scene_grid", test_scene_grid},
    {"scene_changes", test_scene_changes},
    {"scene_blinking", test_scene_blinking},
    {"scene_animated_icon", test_scene_animated_icon},
    {"scene_mask_expires", test_scene_mask_expires},
    {"scene_animated_background", test_scene_animated_background},
    {"scene_revert", test_scene_revert},
    {"scene_menu_scroll", test_scene_menu_scroll},
    {"scene_menu_without_input", test_scene_menu_without_input},
    {"scene_input_and_masks", test_scene_input_and_masks},
    {"scene_held_scroll", test_scene_held_scroll},
    {"scene_two_icons", test_scene_two_icons},
    {"scene_shorter_line", test_scene_shorter_line},
    {"scene_warm_mask", test_scene_warm_mask},
    {"scene_fades", test_scene_fades},
    {"scene_typewriter", test_scene_typewriter},
    {"scene_spinning_mark", test_scene_spinning_mark},
    {"scene_quiet_mark", test_scene_quiet_mark},
    {"scene_quiet_typing", test_scene_quiet_typing},
    {"scene_torn_mark", test_scene_torn_mark},
    {"scene_text_next_to_mark", test_scene_text_next_to_mark},
    {"scene_content_in_icon", test_scene_content_in_icon},
    {"scene_many_frames", test_scene_many_frames},
    {"config", test_config},
    {"regions", test_regions},
    {"http", test_http},
    {"net_pool", test_net_pool},
    {"styled", test_styled},
    {"jpegsw", test_jpegsw},
    {"fixtures", test_fixtures},
    {NULL, NULL},
};
