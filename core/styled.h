/* Styled text for ScePaf ui::Text widgets: UTF-16 text plus spans of size
 * and color, applied with Text::SetStyleAttribute. Sizes are in pixels and
 * text positions come from the widget itself (GetCharInfo/GetLineInfo), so
 * nothing here estimates layout. */
#ifndef VJO_STYLED_H
#define VJO_STYLED_H

#include <stdint.h>

#include "arena.h"
#include "entries.h"

#define VJO_RGB_TEXT      0xFFFFFFu
#define VJO_RGB_HIGHLIGHT 0xFFD24Au
#define VJO_RGB_READING   0x9FD0FFu
#define VJO_RGB_RANK      0x8A94A6u
#define VJO_RGB_DIM       0xB8C0CCu
#define VJO_RGB_ERROR     0xFF8A80u
#define VJO_RGB_OK        0x7CD992u /* in Anki */
/* The in-Anki mark: √ (JIS X 0208, in the system font; ✓ is not and
 * renders as "_"). */
#define VJO_ANKI_MARK     "√"

typedef struct {
    uint32_t start, len; /* UTF-16 units */
    uint32_t rgb;
    int px;
} VjoSpan;

typedef struct {
    uint16_t *text;      /* UTF-16, NUL-terminated */
    uint32_t len, cap;
    VjoSpan *spans;
    int n_spans, cap_spans;
    int truncated;
} VjoStyled;

/* config font_size_ja/_en (8..40, Android sp as on a ~400 dpi phone) ->
 * pixels: ~1.5 px per sp gives a comparable size on the Vita's 220 dpi
 * screen. */
int vjo_font_px(int font_size);

/* Fixed capacity from the arena; text beyond it is dropped (truncated = 1).
 * Returns -1 on OOM. */
int vjo_styled_init(VjoStyled *s, VjoArena *a, uint32_t cap_units, int cap_spans);
/* Appends UTF-8 text (n bytes) as one span. */
void vjo_styled_add(VjoStyled *s, const char *utf8, size_t n, uint32_t rgb, int px);
void vjo_styled_puts(VjoStyled *s, const char *utf8, uint32_t rgb, int px);

/* UTF-16 index of byte offset `off` in UTF-8 text. */
uint32_t vjo_utf16_index(const char *utf8, size_t off);

/* Header: the OCR text in one span (highlights are applied separately). */
void vjo_styled_header(VjoStyled *s, const VjoEntryList *l, int ja_px);

/* UTF-16 range of an entry's highlight in the header; 0 if it has none. */
int vjo_entry_header_range(const VjoEntryList *l, int entry, uint32_t *start, uint32_t *len);

/* Body: the selected entry — headword and reading at ja_px, rank and
 * meanings at en_px; a green VJO_ANKI_MARK after the rank when in_anki. */
void vjo_styled_entry(VjoStyled *s, const VjoEntryList *l, int selected, int ja_px, int en_px, int in_anki);

#endif
