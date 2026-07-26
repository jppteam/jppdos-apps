#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ~4 s and ~1.2 s at 33 ms/frame. */
#define SLOTS_FX_JACKPOT_FRAMES 120u
#define SLOTS_FX_WIN_FRAMES      36u

void slots_fx_start_win(void);
void slots_fx_start_jackpot(void);

/* Advances particles and fires the fanfare bars. Call once per frame. */
void slots_fx_tick(void);

/* Draws into the slots_gfx framebuffer. Call after the reels are drawn. */
void slots_fx_draw(void);

bool     slots_fx_active(void);
uint32_t slots_fx_frame(void);

/* Jumps to the end — any key press during a celebration. */
void slots_fx_skip(void);
