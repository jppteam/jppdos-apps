#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "slots_logic.h"

/* Vertical pitch of one symbol cell, and the length of a whole strip. */
#define SLOTS_CELL_H   21
#define SLOTS_STRIP_PX (SLOTS_CELL_H * (int32_t)SLOTS_STRIP_LEN)   /* 336 */

/* Ease-out curves. QUART has a much longer tail — that is the near-miss crawl. */
#define SLOTS_EASE_CUBIC 0u
#define SLOTS_EASE_QUART 1u

#define SLOTS_REEL_EV_CROSSED_CELL 0x01u
#define SLOTS_REEL_EV_STOPPED      0x02u

typedef struct {
    int32_t  start_px;   /* strip position when the spin began, [0, SLOTS_STRIP_PX) */
    int32_t  total_px;   /* distance this spin travels */
    int32_t  pos_px;     /* current position, [0, SLOTS_STRIP_PX) */
    uint32_t frame;      /* frames elapsed this spin */
    uint32_t frames;     /* frames this spin lasts */
    uint8_t  ease;
    uint8_t  target;
    int32_t  last_cell;
    bool     running;
} slots_reel_t;

/* Parks the reel with `index` on the payline. */
void slots_reel_init(slots_reel_t *reel, uint8_t index);

/* Starts a spin that lands `target` on the payline after exactly `frames`
   ticks, travelling `revolutions` whole strips plus the distance to target. */
void slots_reel_start(slots_reel_t *reel, uint8_t target, uint32_t frames,
                      uint8_t ease, uint32_t revolutions);

/* Advances one frame. Returns a mask of SLOTS_REEL_EV_*. */
uint32_t slots_reel_tick(slots_reel_t *reel);

/* Current scroll position in pixels along the strip, [0, SLOTS_STRIP_PX).
   The strip index on the payline is pos_px / SLOTS_CELL_H, and the sub-cell
   remainder is how far the lane has scrolled between cells. */
int32_t slots_reel_pos_px(const slots_reel_t *reel);

bool slots_reel_stopped(const slots_reel_t *reel);
