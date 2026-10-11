/* Dictionary lookup result shared by the jpdb.io and jiten.moe backends. */
#ifndef VJO_DICT_H
#define VJO_DICT_H

#define VJO_NO_RANK 99999 /* missing frequency_rank */
#define VJO_MAX_PITCH 4   /* pitch accents kept per word */

typedef struct {
    const char *spelling;
    const char *reading;  /* kana */
    int rank;             /* VJO_NO_RANK if unknown */
    const char **meanings;
    int n_meanings;
    const char *ruby;     /* the dictionary form in ruby markup, "雨[あめ]が降[ふ]る"; NULL if none */
    const char **pos;     /* part-of-speech codes: "n", "v5r", "adj-i", ... */
    int n_pos;
    int pitch[VJO_MAX_PITCH]; /* downstep mora, 0 = heiban */
    int n_pitch;
} VjoVocab;

typedef struct {
    int vocab;          /* index into vocabulary, -1 if none */
    int pos16, len16;   /* UTF-16 units into the request text */
    const char *furigana; /* the text as written in ruby markup, "降[ふ]っていた"; NULL if none */
} VjoToken;

typedef struct {
    VjoVocab *vocab;
    int n_vocab;
    VjoToken *tokens;
    int n_tokens;
    const char *source; /* frequency source name: "jpdb" | "Jiten" */
} VjoDictResult;

#endif
