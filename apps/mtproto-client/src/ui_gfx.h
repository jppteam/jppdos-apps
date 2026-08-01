/*
 * ui_gfx — 1-bit framebuffer and drawing primitives for the 128x64 panel.
 *
 * Generalised from apps/slots/src/slots_gfx.c, which already had the framebuffer,
 * clipped pixel plotting, a Bayer dither and the flush-to-canvas loop. What is
 * added here is what a messenger UI needs and a slot machine did not: horizontal
 * lines, filled and outlined rectangles with optional rounded corners for message
 * bubbles, region inversion for selection highlighting, and text.
 *
 * Every primitive clips. Screen-edge clipping is not an optional nicety when the
 * UI scrolls — a chat view is a list of bubbles sliding past a 64-pixel window,
 * so partially visible shapes are the normal case rather than the exception.
 *
 * The firmware blits ctx->canvas to the OLED on its own 100 ms cadence, so
 * ui_gfx_flush copies the framebuffer into the canvas and returns; there is no
 * synchronous display write to wait on.
 */
#pragma once

#include "mtp_common.h"
#include "jpp_sdk_bridge.h"

#define UI_W 128
#define UI_H 64

/* Framebuffer size, for mtp_mem.c to include in the heap block. */
#define UI_FB_BYTES (UI_H * (UI_W / 8))

/* Claim and drop the framebuffer. Called only by mtp_mem_init/_release. */
mtp_err_t ui_gfx_mem_init(void);
void      ui_gfx_mem_clear(void);

/* Height of the status bar, and hence where content starts. Matches the panel's
   8-pixel page height so the bar aligns with a hardware row boundary. */
#define UI_STATUS_H 9

void ui_gfx_clear(void);
void ui_gfx_pixel(int x, int y, bool on);
void ui_gfx_hline(int x0, int x1, int y, bool on);
void ui_gfx_vline(int x, int y0, int y1, bool on);

/* Outlined and filled rectangles. w/h are sizes, not end coordinates. */
void ui_gfx_rect(int x, int y, int w, int h, bool on);
void ui_gfx_fill(int x, int y, int w, int h, bool on);

/*
 * The same with the four corner pixels omitted — the cheapest rounding that
 * reads as a bubble at this scale, and the shape Telegram's own bubbles suggest.
 */
void ui_gfx_rrect(int x, int y, int w, int h, bool on);
void ui_gfx_rfill(int x, int y, int w, int h, bool on);

/* Invert a region. This is how selection is shown: the highlighted row is drawn
   normally and then flipped, so text stays legible against the fill. */
void ui_gfx_invert(int x, int y, int w, int h);

/* 4x4 ordered dither at the given brightness — the only way to suggest a mid
   tone on a 1-bit panel. Used for disabled text and the "typing" shimmer. */
void ui_gfx_dither(int x, int y, int w, int h, uint8_t brightness);

/* ---- Text ---------------------------------------------------------------- */

/* Draw one glyph by codepoint. Returns the pen advance. */
int ui_gfx_glyph(int x, int y, uint32_t cp, bool on);

/* Draw a UTF-8 string. Returns the x coordinate just past the last glyph. */
int ui_gfx_text(int x, int y, const char *s, bool on);

/*
 * Draw at most `max_px` worth of `s`, appending an ellipsis if it did not fit.
 * This is the workhorse for chat titles and message previews, where the text is
 * arbitrary and the slot is fixed.
 */
int ui_gfx_text_ellipsis(int x, int y, int max_px, const char *s, bool on);

/* Centred within [x, x+w). */
int ui_gfx_text_center(int x, int y, int w, const char *s, bool on);

/* Right-aligned so the string ends at `x_right`. */
int ui_gfx_text_right(int x_right, int y, const char *s, bool on);

/*
 * Word-wrap into a fixed width, drawing at most `max_lines` rows of `line_h`.
 * Breaks on spaces where it can and mid-word where it cannot, so a long
 * unbroken string still renders instead of vanishing. Returns lines drawn.
 */
int ui_gfx_text_wrap(int x, int y, int w, int line_h, int max_lines,
                     const char *s, bool on);

/* Rows a string would occupy if wrapped to `w` — needed to lay out a chat view
   bottom-up, where a bubble's height has to be known before it is placed. */
int ui_gfx_wrap_rows(int w, const char *s);

/* Pixel width of the final wrapped line, which is where a caret belongs. */
int ui_gfx_wrap_last_width(int w, const char *s);

/* ---- Bitmaps ------------------------------------------------------------- */

/*
 * Blit a row-major 1-bit bitmap, MSB leftmost, each row padded to a byte
 * boundary — the layout the icon tables in ui_icons.c use.
 */
void ui_gfx_bitmap(int x, int y, int w, int h, const uint8_t *bits, bool on);

/* Copy the framebuffer into the SDK canvas. */
jpp_sdk_status_t ui_gfx_flush(jpp_sdk_context_t *ctx);
