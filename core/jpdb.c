#include "jpdb.h"

#include <string.h>

#include "json.h"

char *vjo_jpdb_build_request(VjoArena *a, const char *text)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"text\":");
    vjo_json_write_string(&b, text);
    vjo_buf_puts(&b, ",\"token_fields\":[\"vocabulary_index\",\"position\",\"length\",\"furigana\"]"
                     ",\"position_length_encoding\":\"utf16\""
                     ",\"vocabulary_fields\":[\"vid\",\"sid\",\"rid\",\"spelling\",\"reading\","
                     "\"frequency_rank\",\"meanings\",\"part_of_speech\",\"pitch_accent\"]}");
    return vjo_buf_cstr(&b);
}

static long get_long(const VjoJson *j, int arr, int k, long dflt)
{
    long v;
    if (vjo_json_int(j, vjo_json_arr_get(j, arr, k), &v) == 0)
        return v;
    return dflt;
}

/* The strings of array arr (not an array: none). */
static int str_array(VjoArena *a, const VjoJson *j, int arr, const char ***out, int *n_out)
{
    int n;
    *out = NULL;
    *n_out = 0;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY) || (n = j->t[arr].size) == 0)
        return 0;
    *out = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)n);
    if (!*out)
        return -1;
    for (int k = 0; k < n; k++)
        if (!((*out)[k] = vjo_json_str(a, j, vjo_json_arr_get(j, arr, k))))
            return -1;
    *n_out = n;
    return 0;
}

int vjo_jpdb_pitch_position(const char *pattern)
{
    for (int i = 0; pattern[i] && pattern[i + 1]; i++)
        if (pattern[i] == 'H' && pattern[i + 1] == 'L')
            return i + 1;
    return 0;
}

/* pitch_accent: ["LHLL", ...] -> downstep positions. */
static void parse_pitch(const VjoJson *j, int arr, VjoVocab *v)
{
    char pattern[32];
    v->n_pitch = 0;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY))
        return;
    for (int k = 0; k < j->t[arr].size && v->n_pitch < VJO_MAX_PITCH; k++) {
        int t = vjo_json_arr_get(j, arr, k);
        int len = vjo_json_is_type(j, t, JSMN_STRING) ? j->t[t].end - j->t[t].start : 0;
        if (len <= 0 || len >= (int)sizeof(pattern))
            continue;
        memcpy(pattern, j->js + j->t[t].start, (size_t)len);
        pattern[len] = '\0';
        v->pitch[v->n_pitch++] = vjo_jpdb_pitch_position(pattern);
    }
}

static int parse_vocab(VjoArena *a, const VjoJson *j, int arr, VjoVocab *v)
{
    int t, m;
    long rank;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY))
        return -1;
    /* 0-2: vid, sid, rid (requested, unused) */
    v->spelling = vjo_json_str(a, j, vjo_json_arr_get(j, arr, 3));
    v->reading = vjo_json_str(a, j, vjo_json_arr_get(j, arr, 4));
    if (!v->spelling || !v->reading)
        return -1;
    t = vjo_json_arr_get(j, arr, 5);
    /* Java: isNull(5) or non-Number -> 99999, else intValue() */
    if (vjo_json_is_null(j, t) || vjo_json_int(j, t, &rank) < 0)
        v->rank = VJO_NO_RANK;
    else
        v->rank = (int)rank;
    m = vjo_json_arr_get(j, arr, 6);
    v->meanings = NULL;
    v->n_meanings = 0;
    if (vjo_json_is_type(j, m, JSMN_ARRAY) && j->t[m].size > 0) {
        int n = j->t[m].size;
        v->meanings = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)n);
        if (!v->meanings)
            return -1;
        for (int k = 0; k < n; k++) {
            const char *s = vjo_json_str(a, j, vjo_json_arr_get(j, m, k));
            if (!s)
                return -1;
            v->meanings[k] = s;
        }
        v->n_meanings = n;
    }
    parse_pitch(j, vjo_json_arr_get(j, arr, 8), v);
    return str_array(a, j, vjo_json_arr_get(j, arr, 7), &v->pos, &v->n_pos);
}

/* A token's furigana, [["降","ふ"],"っていた"], as " 降[ふ]っていた"; NULL
 * when it has none. */
static const char *parse_furigana(VjoArena *a, const VjoJson *j, int arr, int *oom)
{
    VjoBuf b;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY) || j->t[arr].size == 0)
        return NULL;
    vjo_buf_init(&b, a);
    for (int k = 0, i = arr + 1; k < j->t[arr].size; k++, i = vjo_json_skip(j, i)) {
        if (vjo_json_is_type(j, i, JSMN_STRING)) {
            vjo_json_append_str(&b, j, i);
        } else if (vjo_json_is_type(j, i, JSMN_ARRAY) && j->t[i].size == 2) {
            vjo_buf_putc(&b, ' ');
            vjo_json_append_str(&b, j, vjo_json_arr_get(j, i, 0));
            vjo_buf_putc(&b, '[');
            vjo_json_append_str(&b, j, vjo_json_arr_get(j, i, 1));
            vjo_buf_putc(&b, ']');
        }
    }
    if (b.oom) {
        *oom = 1;
        return NULL;
    }
    return vjo_buf_cstr(&b);
}

int vjo_jpdb_parse_response(VjoArena *a, const char *json, size_t len, VjoDictResult *out)
{
    VjoJson j;
    int voc, tok, n, oom = 0;

    memset(out, 0, sizeof(*out));
    if (vjo_json_parse(a, json, len, &j) < 0)
        return -1;
    voc = vjo_json_obj_get(&j, 0, "vocabulary");
    if (!vjo_json_is_type(&j, voc, JSMN_ARRAY))
        return -1;
    n = j.t[voc].size;
    if (n > 0) {
        int i = voc + 1;
        out->vocab = (VjoVocab *)vjo_arena_zalloc(a, sizeof(VjoVocab) * (size_t)n);
        if (!out->vocab)
            return -1;
        for (int k = 0; k < n; k++) {
            if (parse_vocab(a, &j, i, &out->vocab[k]) < 0)
                return -1;
            i = vjo_json_skip(&j, i);
        }
    }
    out->n_vocab = n;

    tok = vjo_json_obj_get(&j, 0, "tokens");
    if (vjo_json_is_type(&j, tok, JSMN_ARRAY) && j.t[tok].size > 0) {
        int i = tok + 1, nt = j.t[tok].size, o = 0;
        out->tokens = (VjoToken *)vjo_arena_zalloc(a, sizeof(VjoToken) * (size_t)nt);
        if (!out->tokens)
            return -1;
        for (int k = 0; k < nt; k++) {
            if (vjo_json_is_type(&j, i, JSMN_ARRAY)) {
                long vi = get_long(&j, i, 0, -1);
                VjoToken *t = &out->tokens[o++];
                t->vocab = (vi >= 0 && vi < n) ? (int)vi : -1;
                t->pos16 = (int)get_long(&j, i, 1, 0);
                t->len16 = (int)get_long(&j, i, 2, 0);
                t->furigana = parse_furigana(a, &j, vjo_json_arr_get(&j, i, 3), &oom);
                if (oom)
                    return -1;
            }
            i = vjo_json_skip(&j, i);
        }
        out->n_tokens = o;
    }
    return 0;
}

const char *vjo_jpdb_error_message(VjoArena *a, const char *json, size_t len)
{
    VjoJson j;
    int t;
    if (vjo_json_parse(a, json, len, &j) < 0)
        return NULL;
    t = vjo_json_obj_get(&j, 0, "error_message");
    if (t < 0)
        t = vjo_json_obj_get(&j, 0, "error");
    if (!vjo_json_is_type(&j, t, JSMN_STRING))
        return NULL;
    return vjo_json_str(a, &j, t);
}
