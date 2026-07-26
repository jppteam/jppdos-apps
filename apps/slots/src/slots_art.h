#pragma once

#include <stdint.h>

#include "slots_logic.h"

/* 16x16 symbol glyphs. Row-major; bit 15 (MSB) is the leftmost column.
   The binary literals in slots_art.c are laid out so the source reads as
   the picture itself — keep it that way when editing. */
extern const uint16_t slots_art_symbol[SLOTS_SYMBOL_COUNT][16];

/* 5x5 coin sprites for the jackpot money. Bit 4 is the leftmost column. */
#define SLOTS_ART_COIN_ROWS 5
extern const uint8_t slots_art_coin_face[SLOTS_ART_COIN_ROWS];
extern const uint8_t slots_art_coin_edge[SLOTS_ART_COIN_ROWS];
