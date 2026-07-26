#include "slots_reel.h"

/* Progress is 0..1024 rather than 0..1 — everything here is integer math.
   1024^3 == 2^30, so the cubic term stays inside an int32_t. */
#define UNIT 1024

static int32_t ease_out(uint8_t kind, int32_t t)
{
    int32_t u = UNIT - t;                    /* 1024 .. 0 */
    if (kind == SLOTS_EASE_QUART) {
        int32_t u2 = (u * u) >> 10;          /* <= 1024 */
        int32_t u4 = (u2 * u2) >> 10;        /* <= 1024 */
        return UNIT - u4;
    }
    int32_t u3 = (((u * u) >> 10) * u) >> 10;
    return UNIT - u3;
}

void slots_reel_init(slots_reel_t *reel, uint8_t index)
{
    reel->start_px  = (int32_t)(index % SLOTS_STRIP_LEN) * SLOTS_CELL_H;
    reel->pos_px    = reel->start_px;
    reel->total_px  = 0;
    reel->frame     = 0u;
    reel->frames    = 0u;
    reel->ease      = SLOTS_EASE_CUBIC;
    reel->target    = (uint8_t)(index % SLOTS_STRIP_LEN);
    reel->last_cell = reel->pos_px / SLOTS_CELL_H;
    reel->running   = false;
}

void slots_reel_start(slots_reel_t *reel, uint8_t target, uint32_t frames,
                      uint8_t ease, uint32_t revolutions)
{
    int32_t stop_px = (int32_t)(target % SLOTS_STRIP_LEN) * SLOTS_CELL_H;
    int32_t delta   = stop_px - reel->pos_px;
    if (delta < 0) {
        delta += SLOTS_STRIP_PX;             /* always travel forwards */
    }

    reel->start_px = reel->pos_px;
    reel->total_px = (int32_t)revolutions * SLOTS_STRIP_PX + delta;
    reel->frame    = 0u;
    reel->frames   = frames > 0u ? frames : 1u;
    reel->ease     = ease;
    reel->target   = (uint8_t)(target % SLOTS_STRIP_LEN);
    reel->running  = true;
}

uint32_t slots_reel_tick(slots_reel_t *reel)
{
    if (!reel->running) {
        return 0u;
    }

    uint32_t events = 0u;
    reel->frame++;

    if (reel->frame >= reel->frames) {
        /* Land exactly. No rounding, no visible snap. */
        reel->pos_px = (reel->start_px + reel->total_px) % SLOTS_STRIP_PX;
        reel->running = false;
        events |= SLOTS_REEL_EV_STOPPED;
    } else {
        int32_t t = (int32_t)((reel->frame * (uint32_t)UNIT) / reel->frames);
        int32_t travelled = (int32_t)(((int64_t)reel->total_px * ease_out(reel->ease, t)) >> 10);
        /* The ease tail truncates to zero movement well before the frame
           budget expires, so stop the moment the eased position actually
           reaches the target rather than sitting motionless until `frames`.
           Otherwise the crawl goes still — and, with no cell crossings, silent
           — for half a second with the outcome already readable. */
        if (travelled >= reel->total_px) {
            reel->pos_px = (reel->start_px + reel->total_px) % SLOTS_STRIP_PX;
            reel->running = false;
            events |= SLOTS_REEL_EV_STOPPED;
        } else {
            reel->pos_px = (reel->start_px + travelled) % SLOTS_STRIP_PX;
        }
    }

    int32_t cell = reel->pos_px / SLOTS_CELL_H;
    if (cell != reel->last_cell) {
        reel->last_cell = cell;
        events |= SLOTS_REEL_EV_CROSSED_CELL;
    }
    return events;
}

int32_t slots_reel_pos_px(const slots_reel_t *reel)
{
    return reel->pos_px;
}

bool slots_reel_stopped(const slots_reel_t *reel)
{
    return !reel->running;
}
