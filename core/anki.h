/* Anki notes over AnkiConnect (API version 6, plain HTTP on the LAN): note
 * fields, request bodies, and the calls the host CLI and the shell make.
 * Fields, deck and note type come from the anki_* settings; where Anki is
 * (anki_host) is parsed by vjo_anki_endpoint in config.h. */
#ifndef VJO_ANKI_H
#define VJO_ANKI_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "config.h"
#include "entries.h"
#include "net.h"

#define VJO_ANKI_MAX_RESPONSE (64u * 1024u)

/* ---- field values (HTML, as Anki stores fields) ---- */

/* The text, escaped, with bytes [hl_start, hl_end) in <b>...</b> (none if
 * hl_start < 0) and line breaks as <br>. */
void vjo_anki_sentence(VjoBuf *b, const char *text, int hl_start, int hl_end);
/* "言葉[ことば]" (one reading for the whole word), or just the spelling when it
 * has no kanji or equals the reading. */
void vjo_anki_furigana(VjoBuf *b, const char *spelling, const char *reading);
/* <ol><li>meaning</li>...</ol>, escaped */
void vjo_anki_definition(VjoBuf *b, const char *const *meanings, int n);

/* One entry, copied out of the result arena (which is reused while the
 * note is being sent). */
typedef struct {
    const char *spelling, *reading;
    int rank; /* VJO_NO_RANK if unknown */
    const char **meanings;
    int n_meanings;
    const char *sentence; /* VjoEntryList.header */
    int hl_start, hl_end; /* the word in sentence, -1 if none */
} VjoAnkiNote;

int vjo_anki_note_from_entry(VjoArena *a, const VjoEntryList *l, int entry, VjoAnkiNote *out);

/* What × adds besides the entry's text (NULL / 0 = none): the screenshot,
 * a JPEG sent in the request, and the word's audio, a URL Anki downloads.
 * File names from vjo_anki_media_name. */
typedef struct {
    const uint8_t *picture;
    size_t picture_len;
    const char *picture_name;
    const char *audio_url, *audio_name;
} VjoAnkiMedia;

/* "vitajp_<unix ms>.<ext>" */
void vjo_anki_media_name(char *out, size_t cap, uint64_t unix_ms, const char *ext);

/* ---- requests ---- */

/* A request body: prefix, then `picture` base64-encoded (when picture_len is
 * not 0), then suffix. */
typedef struct {
    const char *prefix;
    size_t prefix_len;
    const uint8_t *picture;
    size_t picture_len;
    const char *suffix;
    size_t suffix_len;
} VjoAnkiBody;

/* canAddNotesWithErrorDetail for the first n entries, the word in the word
 * field; NULL when anki_field_word is empty (nothing to check) or on OOM. */
char *vjo_anki_can_add_request(VjoArena *a, const VjoConfig *cfg, const VjoEntryList *l, int n);
/* addNote, with the media the note has a field for. With a picture, the
 * body ends in it (out->picture/picture_len, base64-encoded when sent).
 * Returns 0, or -1 on OOM. */
int vjo_anki_add_request(VjoArena *a, const VjoConfig *cfg, const VjoAnkiNote *n, const VjoAnkiMedia *m,
                         VjoAnkiBody *out);

/* ---- word audio ----
 * anki_audio_url is a Yomitan custom audio source: it lists recordings of
 * a word, best first. */
#define VJO_ANKI_AUDIO_MAX_RESPONSE (32u * 1024u)

/* Both anki_audio_url and anki_field_audio are set. */
int vjo_anki_audio_enabled(const VjoConfig *cfg);
/* The request path: the URL's path template (see vjo_anki_audio_endpoint)
 * with {term} and {reading} percent-encoded in ({language}: ja). NULL on
 * OOM. */
char *vjo_anki_audio_path(VjoArena *a, const char *path_template, const char *term, const char *reading);
/* The first http(s) URL of an audioSourceList, or NULL in *url when it
 * has none. */
int vjo_anki_audio_parse(VjoArena *a, const char *json, size_t len, const char **url);
/* The audio file extension of a URL ("opus", ...), "mp3" when it has none
 * that is known (text-to-speech). */
const char *vjo_anki_audio_ext(const char *url);

/* ---- calls (one connection each) ----
 * Results: VJO_OK; VJO_E_ANKI_DUPLICATE or VJO_E_ANKI (VjoErr.detail) when
 * AnkiConnect refuses; VJO_E_PARSE when the reply is not AnkiConnect's;
 * or a transport error (VJO_E_NET, VJO_E_STATUS, ...). */

/* VJO_OK if host:port is AnkiConnect (version 6). */
int vjo_anki_probe(VjoArena *a, const VjoPlatform *p, const char *host, int port, VjoErr *err);
/* Sends a vjo_anki_can_add_request body for n entries: marks[i] = 1 for an
 * entry already in the deck, 0 otherwise (addable, or refused for another
 * reason, such as a deck that does not exist yet). */
int vjo_anki_check(VjoArena *a, const VjoPlatform *p, const char *host, int port, const char *request,
                   uint8_t *marks, int n, VjoErr *err);
/* addNote with the media (see vjo_anki_add_request); a missing deck is
 * created, then the note is sent again. */
int vjo_anki_add(VjoArena *a, const VjoPlatform *p, const char *host, int port, const VjoConfig *cfg,
                 const VjoAnkiNote *n, const VjoAnkiMedia *m, VjoErr *err);

/* Asks the audio source (anki_audio_url) for the word: its best recording
 * in m->audio_url and m->audio_name (both NULL when there is none, or on
 * an error). Only those two strings stay in the arena. The source's
 * errors are transport ones (VJO_E_NET, VJO_E_TLS, VJO_E_STATUS, ...) or
 * VJO_E_PARSE (not an audioSourceList, or an invalid anki_audio_url). */
int vjo_anki_find_audio(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const VjoAnkiNote *n,
                        uint64_t unix_ms, VjoAnkiMedia *m, VjoErr *err);
/* Why there is no audio, after vjo_anki_find_audio: "no recording" for
 * VJO_OK, else the error ("HTTP 401", ...). */
const char *vjo_anki_audio_err_text(VjoArena *a, const VjoErr *err);

/* Short user-facing description of a call's result ("" for VJO_OK). */
const char *vjo_anki_err_text(VjoArena *a, const VjoErr *err);
/* After this result from a saved (auto-found) host, search the network
 * again: anything but an answer from AnkiConnect itself. */
int vjo_anki_host_stale(int rc);

/* The candidate hosts of a network search: the /24 around ip (or the
 * smaller subnet of mask), without the network, broadcast and own
 * addresses, in ascending order. Addresses in host byte order. Returns the
 * count (at most cap). */
int vjo_anki_scan_candidates(uint32_t ip, uint32_t mask, uint32_t *out, int cap);

#endif
