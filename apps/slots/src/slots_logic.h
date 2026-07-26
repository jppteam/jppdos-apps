#pragma once

#include <stdint.h>

#define SLOTS_STRIP_LEN  16u
#define SLOTS_REEL_COUNT 3u

typedef enum {
    SLOTS_SYMBOL_CHERRY = 0,
    SLOTS_SYMBOL_BELL,
    SLOTS_SYMBOL_BAR,
    SLOTS_SYMBOL_STAR,
    SLOTS_SYMBOL_SEVEN,
    SLOTS_SYMBOL_COUNT,
} slots_symbol_t;

typedef enum {
    SLOTS_RESULT_NONE = 0,
    SLOTS_RESULT_WIN,
    SLOTS_RESULT_JACKPOT,
} slots_spin_result_t;

typedef enum {
    SLOTS_DRAMA_NONE = 0,
    SLOTS_DRAMA_NEAR_MISS,      /* reels 0 and 1 match; reel 2 stops one cell off */
    SLOTS_DRAMA_JACKPOT_TEASE,  /* reels 0 and 1 are both SEVEN; reel 2 stops one off */
} slots_drama_t;

typedef struct {
    uint8_t             target[SLOTS_REEL_COUNT]; /* strip index landing on the payline */
    slots_spin_result_t result;
    slots_drama_t       drama;
} slots_spin_plan_t;

/* Symbol at a strip index on a given reel. Index wraps; reel is 0..2. */
slots_symbol_t slots_logic_symbol_at(uint8_t reel, uint8_t index);

/* Any 3-of-a-kind on the payline wins; three SLOTS_SYMBOL_SEVEN is the jackpot. */
slots_spin_result_t slots_logic_evaluate(slots_symbol_t a, slots_symbol_t b, slots_symbol_t c);

/* Decides the outcome up front and picks per-reel strip targets that realise
   it. Uses slots_rng() — call slots_rng_seed() once before the first spin. */
slots_spin_plan_t slots_logic_plan_spin(void);
