/* config.ini parsing (ux0:data/VitaJPOverlay/config.ini). */
#ifndef VJO_CONFIG_H
#define VJO_CONFIG_H

#include <stddef.h>

#include "../include/vjo_api.h"

enum { VJO_OCR_AUTO = 0, VJO_OCR_ON_PRESS = 1 };
enum { VJO_DICT_JPDB = 0, VJO_DICT_JITEN = 1, VJO_DICT_COUNT };

#define VJO_CONFIG_MAX_WARNINGS 4

typedef struct {
    int dictionary;           /* VJO_DICT_* */
    char api_key[VJO_DICT_COUNT][128]; /* indexed by VJO_DICT_* */
    int non_japanese_filter;  /* VJO_FILTER_NONE | VJO_FILTER_LINES */
    int font_size_ja;         /* 8..40: header, headwords, readings */
    int font_size_en;         /* 8..40: meanings, rank, messages */
    int toggle_button;        /* enum VjoTrigger */
    int ocr_mode;             /* VJO_OCR_* */
    char log_host[64];        /* "" = UDP log off */
    int log_file;             /* 0/1 */
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

#endif
