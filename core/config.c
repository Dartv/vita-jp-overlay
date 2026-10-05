#include "config.h"

#include <stddef.h>
#include <string.h>

#include "port.h"
#include "textfilter.h"
#include "utf.h"

static const char *const trigger_names[VJO_TRIGGER_COUNT] = {
    "select", "start", "l+r", "select+l", "select+r", "rear_double_tap",
};

const char *vjo_trigger_name(int trigger)
{
    if (trigger < 0 || trigger >= VJO_TRIGGER_COUNT)
        return "?";
    return trigger_names[trigger];
}

static const VjoDictInfo dicts[VJO_DICT_COUNT] = {
    [VJO_DICT_JPDB] = {"jpdb", "jpdb.io", "jpdb_api_key", "VJO_JPDB_KEY"},
    [VJO_DICT_JITEN] = {"jiten", "jiten.moe", "jiten_api_key", "VJO_JITEN_KEY"},
};

const VjoDictInfo *vjo_dict_info(int dictionary)
{
    return &dicts[dictionary >= 0 && dictionary < VJO_DICT_COUNT ? dictionary : VJO_DICT_JPDB];
}

const char *vjo_dict_name(int dictionary)
{
    return vjo_dict_info(dictionary)->name;
}

const char *vjo_config_api_key(const VjoConfig *c)
{
    return c->api_key[c->dictionary];
}

static const struct {
    const char *key, *def; /* defaults: the Lapis note type */
} anki_fields[VJO_ANKI_FIELD_COUNT] = {
    [VJO_ANKI_WORD] = {"anki_field_word", "Expression"},
    [VJO_ANKI_READING] = {"anki_field_reading", "ExpressionReading"},
    [VJO_ANKI_FURIGANA] = {"anki_field_furigana", "ExpressionFurigana"},
    [VJO_ANKI_DEFINITION] = {"anki_field_definition", "MainDefinition"},
    [VJO_ANKI_SENTENCE] = {"anki_field_sentence", "Sentence"},
    [VJO_ANKI_PICTURE] = {"anki_field_picture", "Picture"},
    [VJO_ANKI_FREQUENCY] = {"anki_field_frequency", "FreqSort"},
};

void vjo_config_defaults(VjoConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->dictionary = VJO_DICT_JITEN;
    c->non_japanese_filter = VJO_FILTER_LINES;
    c->font_size_ja = 18;
    c->font_size_en = 14;
    c->toggle_button = VJO_TRIGGER_L_R;
    c->ocr_mode = VJO_OCR_AUTO;
    vjo_snprintf(c->anki_deck, sizeof(c->anki_deck), "Default");
    vjo_snprintf(c->anki_note_type, sizeof(c->anki_note_type), "Lapis");
    vjo_snprintf(c->anki_tags, sizeof(c->anki_tags), "vita-jp-overlay");
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++)
        vjo_snprintf(c->anki_field[i], sizeof(c->anki_field[i]), "%s", anki_fields[i].def);
}

const char *vjo_config_default_text(void)
{
    return "; Vita JP Overlay settings. Changes apply the next time the overlay opens.\n"
           "\n"
           "; Dictionary used for word lookups: jpdb | jiten\n"
           "dictionary = jiten\n"
           "\n"
           "; jpdb.io API key (needed for dictionary = jpdb): jpdb.io -> Settings -> API key\n"
           "jpdb_api_key =\n"
           "\n"
           "; jiten.moe API key (needed for dictionary = jiten): jiten.moe -> Settings -> API key\n"
           "jiten_api_key =\n"
           "\n"
           "; Drop OCR lines without Japanese characters: none | lines\n"
           "non_japanese_filter = lines\n"
           "\n"
           "; Text size, 8-40: Japanese (sentence, words, readings) and English (meanings, messages)\n"
           "font_size_ja = 18\n"
           "font_size_en = 14\n"
           "\n"
           "; Opens/closes the overlay: a button combination, or a double tap on the rear\n"
           "; touchpad (buttons are hidden from the game; rear taps are not):\n"
           "; l+r | select | start | select+l | select+r | rear_double_tap\n"
           "toggle_button = l+r\n"
           "\n"
           "; auto: recognize text in the background when the region changes (instant overlay)\n"
           "; on_press: recognize only when the overlay is opened\n"
           "ocr_mode = auto\n"
           "\n"
           "; Debugging: IP of a computer running tools/udp_log_listener.py (empty = off)\n"
           "log_host =\n"
           "\n"
           "; Debugging: write ux0:data/VitaJPOverlay/log.txt (max 256 KB + one rotated file): on | off\n"
           "log_file = off\n"
           "\n"
           "; ---- Anki (optional, see README) ----\n"
           "; × in the overlay adds the selected word to Anki through AnkiConnect on a computer.\n"
           "; The anki_ values may contain ; and #, so don't put comments after them.\n"
           "\n"
           "; The computer running Anki: empty = off, auto = search the local network,\n"
           "; or its IP address (optionally with :port, default 8765)\n"
           "anki_host =\n"
           "\n"
           "; Deck (created if missing), note type, and tags separated by spaces\n"
           "anki_deck = Default\n"
           "anki_note_type = Lapis\n"
           "anki_tags = vita-jp-overlay\n"
           "\n"
           "; The note type's field for each piece of data (empty = not added). Anki checks\n"
           "; duplicates on the note type's first field, so map the word to that one.\n"
           "anki_field_word = Expression\n"
           "anki_field_reading = ExpressionReading\n"
           "anki_field_furigana = ExpressionFurigana\n"
           "anki_field_definition = MainDefinition\n"
           "anki_field_sentence = Sentence\n"
           "anki_field_picture = Picture\n"
           "anki_field_frequency = FreqSort\n";
}

int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port)
{
    const char *colon = strchr(setting, ':');
    size_t n = colon ? (size_t)(colon - setting) : strlen(setting);
    int v = 0;
    if (!*setting)
        return VJO_ANKI_OFF;
    if (vjo_ieq(setting, "auto"))
        return VJO_ANKI_AUTO;
    if (n == 0 || n >= cap)
        return -1;
    for (size_t i = 0; i < n; i++) {
        char ch = setting[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' ||
              ch == '-'))
            return -1;
    }
    if (colon) {
        const char *s = colon + 1;
        if (!*s)
            return -1;
        for (; *s; s++) {
            if (*s < '0' || *s > '9')
                return -1;
            v = v * 10 + (*s - '0');
            if (v > 65535)
                return -1;
        }
        if (v == 0)
            return -1;
    }
    *port = colon ? v : VJO_ANKI_PORT;
    memcpy(host, setting, n);
    host[n] = '\0';
    return VJO_ANKI_MANUAL;
}

static void warn(VjoConfig *c, const char *fmt, const char *a, const char *b)
{
    if (c->n_warnings >= VJO_CONFIG_MAX_WARNINGS)
        return;
    vjo_snprintf(c->warnings[c->n_warnings++], sizeof(c->warnings[0]), fmt, a, b);
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

static int parse_int(const char *s, int *out)
{
    long v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return -1;
        v = v * 10 + (*s - '0');
    }
    *out = (int)v;
    return 0;
}

int vjo_dict_find(const char *id)
{
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        if (vjo_ieq(id, dicts[i].id))
            return i;
    return -1;
}

/* VJO_DICT_* whose API key `key` sets, or -1. */
static int api_key_dict(const char *key)
{
    if (vjo_ieq(key, "api_key")) /* the old name of jpdb_api_key (tools/migrate_config.py RENAMED) */
        return VJO_DICT_JPDB;
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        if (vjo_ieq(key, dicts[i].key_setting))
            return i;
    return -1;
}

/* Plain string settings. Deck, note type, tags and field names may
 * contain ';' and '#' (raw: no inline comment). */
typedef struct {
    const char *key;
    size_t offset, size;
    int required; /* an empty value keeps the default, with a warning */
    int raw;
} StrSetting;

#define STR_SETTING(name, required, raw) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), required, raw}
static const StrSetting str_settings[] = {
    STR_SETTING(log_host, 0, 0),
    STR_SETTING(anki_host, 0, 0),
    STR_SETTING(anki_deck, 1, 1),
    STR_SETTING(anki_note_type, 1, 1),
    STR_SETTING(anki_tags, 0, 1),
};
#define N_STR_SETTINGS ((int)(sizeof(str_settings) / sizeof(str_settings[0])))

static const StrSetting *find_str_setting(const char *key)
{
    for (int i = 0; i < N_STR_SETTINGS; i++)
        if (vjo_ieq(key, str_settings[i].key))
            return &str_settings[i];
    return NULL;
}

static int anki_field_index(const char *key)
{
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++)
        if (vjo_ieq(key, anki_fields[i].key))
            return i;
    return -1;
}

static int keeps_comment_chars(const char *key)
{
    const StrSetting *ss = find_str_setting(key);
    return (ss && ss->raw) || anki_field_index(key) >= 0;
}

static void set_str(VjoConfig *c, const char *key, const char *val, char *dst, size_t size)
{
    if (strlen(val) >= size)
        warn(c, "%s: value too long%s", key, "");
    else
        memcpy(dst, val, strlen(val) + 1);
}

static void set_font(VjoConfig *c, const char *key, const char *val, int *dst)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= 8 && v <= 40)
        *dst = v;
    else
        warn(c, "%s: invalid value '%s' (8-40)", key, val);
}

static void set_kv(VjoConfig *c, const char *key, const char *val)
{
    const StrSetting *ss;
    char host[64];
    int d, port;
    if (vjo_ieq(key, "dictionary")) {
        d = vjo_dict_find(val);
        if (d >= 0)
            c->dictionary = d;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((d = api_key_dict(key)) >= 0) {
        set_str(c, key, val, c->api_key[d], sizeof(c->api_key[d]));
    } else if (vjo_ieq(key, "frequency_filter") || vjo_ieq(key, "font_size") || vjo_ieq(key, "hw_jpeg")) {
        /* removed settings (tools/migrate_config.py REMOVED): no warning */
    } else if (vjo_ieq(key, "non_japanese_filter")) {
        if (vjo_ieq(val, "lines"))
            c->non_japanese_filter = VJO_FILTER_LINES;
        else if (vjo_ieq(val, "none"))
            c->non_japanese_filter = VJO_FILTER_NONE;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "font_size_ja")) {
        set_font(c, key, val, &c->font_size_ja);
    } else if (vjo_ieq(key, "font_size_en")) {
        set_font(c, key, val, &c->font_size_en);
    } else if (vjo_ieq(key, "toggle_button")) {
        int found = 0;
        for (int i = 0; i < VJO_TRIGGER_COUNT; i++) {
            if (vjo_ieq(val, trigger_names[i])) {
                c->toggle_button = i;
                found = 1;
            }
        }
        if (!found)
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_mode")) {
        if (vjo_ieq(val, "auto"))
            c->ocr_mode = VJO_OCR_AUTO;
        else if (vjo_ieq(val, "on_press"))
            c->ocr_mode = VJO_OCR_ON_PRESS;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((ss = find_str_setting(key)) != NULL) {
        char *dst = (char *)c + ss->offset;
        if (!*val && ss->required)
            warn(c, "%s: empty (keeping %s)", key, dst);
        else
            set_str(c, key, val, dst, ss->size);
        if (dst == c->anki_host && vjo_anki_endpoint(c->anki_host, host, sizeof(host), &port) < 0) {
            warn(c, "%s: invalid value '%s' (empty, auto, or IP[:port])", key, val);
            c->anki_host[0] = '\0';
        }
    } else if ((d = anki_field_index(key)) >= 0) {
        set_str(c, key, val, c->anki_field[d], sizeof(c->anki_field[d]));
    } else if (vjo_ieq(key, "log_file")) {
        if (vjo_ieq(val, "on"))
            c->log_file = 1;
        else if (vjo_ieq(val, "off"))
            c->log_file = 0;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else {
        warn(c, "unknown setting '%s'%s", key, "");
    }
}

void vjo_config_parse(VjoConfig *c, const char *text, size_t len)
{
    size_t i = 0;
    /* Skip a UTF-8 BOM (Notepad). */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        i = 3;
    while (i < len) {
        char line[256], key[64], val[192];
        size_t n = 0, s, e, eq;
        while (i < len && text[i] != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = text[i];
            i++;
        }
        i++;
        line[n] = '\0';

        s = 0;
        while (s < n && is_space(line[s]))
            s++;
        if (s == n || line[s] == ';' || line[s] == '#')
            continue;
        {
            char *p = strchr(line + s, '=');
            if (!p) {
                warn(c, "ignored line '%s'%s", line + s, "");
                continue;
            }
            eq = (size_t)(p - line);
        }
        e = eq;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (e - s >= sizeof(key))
            e = s + sizeof(key) - 1;
        memcpy(key, line + s, e - s);
        key[e - s] = '\0';

        /* Inline comment in the value: ';' or '#' preceded by whitespace. */
        if (!keeps_comment_chars(key)) {
            for (e = eq + 1; e < n; e++) {
                if ((line[e] == ';' || line[e] == '#') && is_space(line[e - 1])) {
                    n = e;
                    break;
                }
            }
        }
        s = eq + 1;
        while (s < n && is_space(line[s]))
            s++;
        e = n;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (e - s >= sizeof(val))
            e = s + sizeof(val) - 1;
        memcpy(val, line + s, e - s);
        val[e - s] = '\0';
        set_kv(c, key, val);
    }
}
