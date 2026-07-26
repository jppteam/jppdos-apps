#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"
#include "slots_logic.h"

#define SLOTS_GFX_ROWS 64
#define SLOTS_GFX_COLS 128

/* The payline cell band. Lives here rather than in slots.c so slots_fx.c can
   reach it for the win flash. */
#define SLOTS_PAYLINE_Y0 21
#define SLOTS_PAYLINE_Y1 41

void slots_gfx_clear(void);
void slots_gfx_set_pixel(int x, int y, bool on);
void slots_gfx_vline(int x, int y0, int y1, bool on);

/* Blits a 16x16 symbol with its top-left at (x0, y0). Clipped on all four
   edges — partial symbols scrolling through the reel window depend on this. */
void slots_gfx_draw_symbol(int x0, int y0, slots_symbol_t symbol);

/* Blits a 5-row, 5-column sprite (bit 4 = leftmost). Clipped. */
void slots_gfx_draw_sprite5(int x0, int y0, const uint8_t rows[5]);

/* 3x5 font scaled by an integer factor. _halo clears a 1px border first so
   text stays readable over particles. */
void slots_gfx_text(int x, int y, const char *s, int scale);
void slots_gfx_text_halo(int x, int y, const char *s, int scale);
int  slots_gfx_text_w(const char *s, int scale);

/* Inverts every pixel — the jackpot flash. */
void slots_gfx_invert(void);

/* Inverts rows y0..y1 inclusive — the win flash, which is the same effect
   confined to the payline band. Clipped to the framebuffer. */
void slots_gfx_invert_band(int y0, int y1);

/* 4x4 ordered-dither test: true when a pixel at (x, y) should be lit for the
   given brightness (0 = off, 255 = solid). This is how a 1-bit panel fades. */
bool slots_gfx_dither(int x, int y, uint8_t brightness);

/* Pushes all 64 rows to the canvas. */
jpp_sdk_status_t slots_gfx_flush(jpp_sdk_context_t *ctx);
