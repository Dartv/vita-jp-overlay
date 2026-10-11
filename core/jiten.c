#include "jiten.h"

#include <string.h>

#include "json.h"
#include "utf.h"

char *vjo_jiten_build_request(VjoArena *a, const char *text)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"text\":[");
    vjo_json_write_string(&b, text);
    vjo_buf_puts(&b, "]}");
    return vjo_buf_cstr(&b);
}

char *vjo_jiten_ruby_to_kana(VjoArena *a, const char *ruby)
{
    size_t len = strlen(ruby), i = 0, o = 0;
    size_t seg_start = 0;   /* output offset after the last ']' */
    size_t kanji_start = 0; /* output offset where the current kanji run began */
    int in_kanji = 0;
    char *out = (char *)vjo_arena_alloc(a, len + 1);
    if (!out)
        return NULL;
    while (i < len) {
        size_t s = i;
        uint32_t cp = vjo_utf8_next(ruby, len, &i);
        if (cp == '[') {
            const char *close = memchr(ruby + i, ']', len - i);
            if (!close) {
                memcpy(out + o, ruby + s, len - s);
                o += len - s;
                break;
            }
            /* The bracket reads the kanji run before it; without one, the
             * whole segment since the previous bracket. */
            o = in_kanji ? kanji_start : seg_start;
            memcpy(out + o, ruby + i, (size_t)(close - (ruby + i)));
            o += (size_t)(close - (ruby + i));
            i = (size_t)(close - ruby) + 1;
            seg_start = o;
            in_kanji = 0;
            continue;
        }
        if (vjo_is_kanji(cp)) {
            if (!in_kanji)
                kanji_start = o;
            in_kanji = 1;
        } else {
            in_kanji = 0;
        }
        memcpy(out + o, ruby + s, i - s);
        o += i - s;
    }
    out[o] = '\0';
    return out;
}

static long obj_long(const VjoJson *j, int obj, const char *key, long dflt)
{
    long v;
    if (vjo_json_int(j, vjo_json_obj_get(j, obj, key), &v) == 0)
        return v;
    return dflt;
}

/* meaningsChunks: one array of glosses per sense -> "a; b" per sense. */
static int parse_meanings(VjoArena *a, const VjoJson *j, int chunks, VjoVocab *v)
{
    int n, i;
    v->meanings = NULL;
    v->n_meanings = 0;
    if (!vjo_json_is_type(j, chunks, JSMN_ARRAY) || j->t[chunks].size == 0)
        return 0;
    n = j->t[chunks].size;
    v->meanings = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)n);
    if (!v->meanings)
        return -1;
    i = chunks + 1;
    for (int k = 0; k < n; k++) {
        if (vjo_json_is_type(j, i, JSMN_ARRAY)) {
            int m = j->t[i].size;
            VjoBuf b;
            vjo_buf_init(&b, a);
            for (int g = 0; g < m; g++) {
                if (g)
                    vjo_buf_puts(&b, "; ");
                if (vjo_json_append_str(&b, j, vjo_json_arr_get(j, i, g)) < 0)
                    return -1;
            }
            if (b.oom || !(v->meanings[v->n_meanings] = vjo_buf_cstr(&b)))
                return -1;
            if (m)
                v->n_meanings++;
        }
        i = vjo_json_skip(j, i);
    }
    return 0;
}

/* pitchAccents: [0, 3], downstep positions. */
static void parse_pitch(const VjoJson *j, int arr, VjoVocab *v)
{
    v->n_pitch = 0;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY))
        return;
    for (int k = 0; k < j->t[arr].size && v->n_pitch < VJO_MAX_PITCH; k++) {
        long p;
        if (vjo_json_int(j, vjo_json_arr_get(j, arr, k), &p) == 0 && p >= 0 && p < 64)
            v->pitch[v->n_pitch++] = (int)p;
    }
}

/* meaningsPartOfSpeech: short codes ("v5r", "adj-i"), one per sense and
 * repeated; kept once each. */
static int parse_pos(VjoArena *a, const VjoJson *j, int arr, VjoVocab *v)
{
    int n;
    v->pos = NULL;
    v->n_pos = 0;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY) || (n = j->t[arr].size) == 0)
        return 0;
    v->pos = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)n);
    if (!v->pos)
        return -1;
    for (int k = 0; k < n; k++) {
        const char *s = vjo_json_str(a, j, vjo_json_arr_get(j, arr, k));
        int dup = 0;
        if (!s)
            return -1;
        for (int d = 0; d < v->n_pos && !dup; d++)
            dup = !strcmp(v->pos[d], s);
        if (!dup)
            v->pos[v->n_pos++] = s;
    }
    return 0;
}

static int parse_vocab(VjoArena *a, const VjoJson *j, int obj, VjoVocab *v)
{
    long rank;
    const char *ruby;
    if (!vjo_json_is_type(j, obj, JSMN_OBJECT))
        return -1;
    v->spelling = vjo_json_str(a, j, vjo_json_obj_get(j, obj, "spelling"));
    ruby = vjo_json_str(a, j, vjo_json_obj_get(j, obj, "reading"));
    if (!v->spelling || !ruby)
        return -1;
    v->reading = vjo_jiten_ruby_to_kana(a, ruby);
    if (!v->reading)
        return -1;
    v->ruby = ruby;
    rank = obj_long(j, obj, "frequencyRank", 0);
    v->rank = rank > 0 ? (int)rank : VJO_NO_RANK;
    parse_pitch(j, vjo_json_obj_get(j, obj, "pitchAccents"), v);
    if (parse_pos(a, j, vjo_json_obj_get(j, obj, "meaningsPartOfSpeech"), v) < 0)
        return -1;
    return parse_meanings(a, j, vjo_json_obj_get(j, obj, "meaningsChunks"), v);
}

int vjo_jiten_parse_response(VjoArena *a, const char *json, size_t len, VjoDictResult *out)
{
    VjoJson j;
    int voc, tok, n, nv = 0, nt = 0;
    long *ids;
    int *ris;

    memset(out, 0, sizeof(*out));
    if (vjo_json_parse(a, json, len, &j) < 0)
        return -1;
    voc = vjo_json_obj_get(&j, 0, "vocabulary");
    if (!vjo_json_is_type(&j, voc, JSMN_ARRAY))
        return -1;
    n = j.t[voc].size;
    ids = (long *)vjo_arena_alloc(a, sizeof(long) * (size_t)(n ? n : 1));
    ris = (int *)vjo_arena_alloc(a, sizeof(int) * (size_t)(n ? n : 1));
    out->vocab = (VjoVocab *)vjo_arena_zalloc(a, sizeof(VjoVocab) * (size_t)(n ? n : 1));
    if (!ids || !ris || !out->vocab)
        return -1;
    for (int k = 0, i = voc + 1; k < n; k++, i = vjo_json_skip(&j, i)) {
        long id = obj_long(&j, i, "wordId", -1);
        int ri = (int)obj_long(&j, i, "readingIndex", 0), dup = 0;
        for (int d = 0; d < nv && !dup; d++)
            dup = ids[d] == id && ris[d] == ri;
        if (dup)
            continue;
        if (parse_vocab(a, &j, i, &out->vocab[nv]) < 0)
            return -1;
        ids[nv] = id;
        ris[nv] = ri;
        nv++;
    }
    out->n_vocab = nv;

    /* tokens: one array per paragraph (we send one). */
    tok = vjo_json_obj_get(&j, 0, "tokens");
    if (!vjo_json_is_type(&j, tok, JSMN_ARRAY))
        return 0;
    for (int p = 0, i = tok + 1; p < j.t[tok].size; p++, i = vjo_json_skip(&j, i))
        if (vjo_json_is_type(&j, i, JSMN_ARRAY))
            nt += j.t[i].size;
    if (!nt)
        return 0;
    out->tokens = (VjoToken *)vjo_arena_zalloc(a, sizeof(VjoToken) * (size_t)nt);
    if (!out->tokens)
        return -1;
    for (int p = 0, i = tok + 1; p < j.t[tok].size; p++, i = vjo_json_skip(&j, i)) {
        if (!vjo_json_is_type(&j, i, JSMN_ARRAY))
            continue;
        for (int k = 0, t = i + 1; k < j.t[i].size; k++, t = vjo_json_skip(&j, t)) {
            long id = obj_long(&j, t, "wordId", -1);
            int ri = (int)obj_long(&j, t, "readingIndex", 0);
            VjoToken *o = &out->tokens[out->n_tokens++];
            o->vocab = -1;
            for (int d = 0; d < nv; d++) {
                if (ids[d] == id && ris[d] == ri) {
                    o->vocab = d;
                    break;
                }
            }
            o->pos16 = (int)obj_long(&j, t, "start", 0);
            o->len16 = (int)obj_long(&j, t, "length", 0);
        }
    }
    return 0;
}

const char *vjo_jiten_error_message(VjoArena *a, const char *json, size_t len)
{
    static const char *const keys[] = {"detail", "title", "message", "error"};
    VjoJson j;
    if (vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT))
        return NULL;
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        int t = vjo_json_obj_get(&j, 0, keys[k]);
        if (vjo_json_is_type(&j, t, JSMN_STRING))
            return vjo_json_str(a, &j, t);
    }
    return NULL;
}
