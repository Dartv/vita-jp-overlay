/* config.ini parsing (ux0:data/VitaJPOverlay/config.ini). */
#ifndef VJO_CONFIG_H
#define VJO_CONFIG_H

#include <stddef.h>

#include "../include/vjo_api.h"

enum { VJO_OCR_AUTO = 0, VJO_OCR_ON_PRESS = 1 };
enum { VJO_DICT_JPDB = 0, VJO_DICT_JITEN = 1, VJO_DICT_COUNT };

/* Data put on an Anki note, each into the field named by its
 * anki_field_* setting ("" = not added). */
enum {
    VJO_ANKI_WORD = 0,   /* spelling */
    VJO_ANKI_READING,    /* kana */
    VJO_ANKI_FURIGANA,   /* 言葉[ことば] */
    VJO_ANKI_DEFINITION, /* <ol><li>meaning</li>...</ol> */
    VJO_ANKI_SENTENCE,   /* the recognized text, the word in <b> */
    VJO_ANKI_PICTURE,    /* screenshot */
    VJO_ANKI_FREQUENCY,  /* frequency rank */
    VJO_ANKI_AUDIO,      /* the word's pronunciation, downloaded by Anki */
    VJO_ANKI_FIELD_COUNT
};

#define VJO_CONFIG_MAX_WARNINGS 4

typedef struct {
    int dictionary;           /* VJO_DICT_* */
    char api_key[VJO_DICT_COUNT][128]; /* indexed by VJO_DICT_* */
    int non_japanese_filter;  /* VJO_FILTER_NONE | VJO_FILTER_LINES */
    int font_size_ja;         /* 8..40: header, headwords, readings */
    int font_size_en;         /* 8..40: meanings, rank, messages */
    int toggle_button;        /* enum VjoTrigger */
    int subtitle_button;      /* enum VjoTrigger, != toggle_button */
    int ocr_mode;             /* VJO_OCR_* */
    char log_host[64];        /* "" = UDP log off */
    int log_file;             /* 0/1 */
    char anki_host[64];       /* see vjo_anki_endpoint */
    char anki_deck[128];
    char anki_note_type[64];
    char anki_tags[128];      /* separated by spaces */
    char anki_field[VJO_ANKI_FIELD_COUNT][64]; /* note field names, "" = skip */
    char anki_audio_url[256]; /* "" = no word audio; see vjo_anki_audio_endpoint */
    char warnings[VJO_CONFIG_MAX_WARNINGS][96];
    int n_warnings;
} VjoConfig;

void vjo_config_defaults(VjoConfig *c);
/* Parses INI text over the defaults; invalid values keep the default and
 * add a warning. */
void vjo_config_parse(VjoConfig *c, const char *text, size_t len);
/* Default file written on first run (with comments). */
const char *vjo_config_default_text(void);

const char *vjo_trigger_name(int trigger);

/* A dictionary's config-level names (its network side: client.h). */
typedef struct {
    const char *id;          /* config.ini dictionary / CLI --dict value: "jpdb" | "jiten" */
    const char *name;        /* "jpdb.io" | "jiten.moe" */
    const char *key_setting; /* config.ini key of its API key */
    const char *key_env;     /* host CLI environment variable of its API key */
} VjoDictInfo;

/* Info for a VJO_DICT_* value (jpdb for an out-of-range one). */
const VjoDictInfo *vjo_dict_info(int dictionary);
const char *vjo_dict_name(int dictionary); /* "jpdb.io" | "jiten.moe" */
/* VJO_DICT_* for an id ("jpdb" | "jiten", any case), or -1. */
int vjo_dict_find(const char *id);
/* API key of the selected dictionary ("" if unset). */
const char *vjo_config_api_key(const VjoConfig *c);

/* anki_host (or the saved anki_host.txt): "" = off, "auto" = search the
 * network, or host[:port]. For VJO_ANKI_MANUAL, host (cap bytes) and port
 * are set. Returns -1 for an invalid value. */
#define VJO_ANKI_PORT 8765
enum { VJO_ANKI_OFF = 0, VJO_ANKI_AUTO = 1, VJO_ANKI_MANUAL = 2 };
int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port);

/* anki_audio_url: a Yomitan custom audio source,
 * http(s)://host[:port][/path][?query] with {term} (and usually {reading})
 * in it. Sets host (VJO_HOST_MAX bytes), port, tls and the path template
 * (in url, from the '/' or '?' after the host; "" for none). Returns 0, or
 * -1 for an invalid or empty value. */
#define VJO_HOST_MAX 128
int vjo_anki_audio_endpoint(const char *url, char *host, int *port, int *tls, const char **path);

#endif
