/* Dictionary lookup result shared by the jpdb.io and jiten.moe backends. */
#ifndef VJO_DICT_H
#define VJO_DICT_H

#define VJO_NO_RANK 99999 /* missing frequency_rank */

typedef struct {
    const char *spelling;
    const char *reading;  /* kana */
    int rank;             /* VJO_NO_RANK if unknown */
    const char **meanings;
    int n_meanings;
} VjoVocab;

typedef struct {
    int vocab;          /* index into vocabulary, -1 if none */
    int pos16, len16;   /* UTF-16 units into the request text */
} VjoToken;

typedef struct {
    VjoVocab *vocab;
    int n_vocab;
    VjoToken *tokens;
    int n_tokens;
} VjoDictResult;

#endif
