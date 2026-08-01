/*
 * ui_font — 5x7 text rendering with UTF-8 and Cyrillic support.
 *
 * The SDK offers no text primitive: `jpp_sdk_set_frame` draws 21 ASCII columns in
 * a fixed layout the firmware controls, and the canvas is raw pixels. A client
 * that wants message bubbles, or any Russian text at all, has to bring its own
 * font and blitter.
 *
 * At 5x7 plus one column of spacing, a 128-pixel line holds 21 characters and the
 * 64-pixel screen holds 8 rows — which is what the whole UI is laid out around.
 */
#pragma once

#include "mtp_common.h"

#define UI_FONT_W     5u   /* glyph width in pixels */
#define UI_FONT_H     7u   /* glyph height in pixels */
#define UI_FONT_BYTES 5u   /* column-major: one byte per column */
#define UI_FONT_ADV   6u   /* pen advance: glyph width plus one blank column */

/* One entry of the generated table. Sorted by codepoint for binary search. */
typedef struct {
    uint16_t cp;
    uint8_t  cols[UI_FONT_BYTES];
} ui_glyph_t;

extern const ui_glyph_t ui_font_glyphs[];
extern const size_t     ui_font_glyph_count;
extern const uint8_t    ui_font_notdef[UI_FONT_BYTES];

/*
 * Decode one UTF-8 sequence, advancing *pos. Returns the codepoint, or U+FFFD for
 * malformed input while still advancing by one byte — so a corrupt string costs
 * one wrong character rather than desynchronising the rest of the line.
 */
uint32_t ui_utf8_next(const char *s, size_t *pos);

/* Number of codepoints in a UTF-8 string — the count that matters for cursor
   movement and truncation, as opposed to strlen's byte count. */
size_t ui_utf8_len(const char *s);

/* Byte offset of codepoint index `n`, clamped to the string length. Lets the
   keyboard and text fields index by character without a second array. */
size_t ui_utf8_offset(const char *s, size_t n);

/* Columns of glyph data for a codepoint, or the notdef box when unsupported. */
const uint8_t *ui_font_glyph(uint32_t cp);

/* Rendered width of a string in pixels, excluding the trailing advance gap. */
int ui_font_width(const char *s);

/*
 * Width of the longest prefix of `s` that fits `max_px`, in bytes. Used
 * everywhere text meets a fixed-width slot: chat titles, message previews,
 * button labels.
 */
size_t ui_font_fit(const char *s, int max_px);
