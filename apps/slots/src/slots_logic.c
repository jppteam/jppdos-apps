#include "slots_logic.h"
#include "slots_rng.h"

/* ---- Reel strips ---------------------------------------------------------
   Each strip holds the same multiset — Cherry x5, Bell x4, Bar x3, Star x3,
   Seven x1 — in a different order, so the three lanes never scroll in visual
   lockstep and the sevens sit at different indices. Symbol frequency lives
   here; the planner never samples symbols uniformly. */

#define C SLOTS_SYMBOL_CHERRY
#define B SLOTS_SYMBOL_BELL
#define R SLOTS_SYMBOL_BAR
#define S SLOTS_SYMBOL_STAR
#define V SLOTS_SYMBOL_SEVEN

static const uint8_t k_strip[SLOTS_REEL_COUNT][SLOTS_STRIP_LEN] = {
    { C, B, C, R, S, C, B, V, C, R, B, S, C, R, B, S },
    { B, C, S, C, R, V, C, B, R, C, S, B, C, R, S, B },
    { C, R, B, S, C, B, C, R, S, V, B, C, S, R, C, B },
};

#undef C
#undef B
#undef R
#undef S
#undef V

/* ---- Odds ----------------------------------------------------------------
   One roll of 0..999 picks the outcome; a second roll promotes some losses to
   drama. Tune the game here and nowhere else. */

#define ODDS_JACKPOT_MAX     19u   /* 0..19    ->  2% jackpot   */
#define ODDS_WIN_MAX        139u   /* 20..139  -> 12% win       */
                                   /* 140..999 -> 86% loss      */
#define ODDS_NEAR_MISS_MAX  299u   /* of losses: 0..299   -> 30% near miss     */
#define ODDS_TEASE_MAX      349u   /* of losses: 300..349 ->  5% jackpot tease */

slots_symbol_t slots_logic_symbol_at(uint8_t reel, uint8_t index)
{
    if (reel >= SLOTS_REEL_COUNT) {
        return SLOTS_SYMBOL_CHERRY;
    }
    return (slots_symbol_t)k_strip[reel][index % SLOTS_STRIP_LEN];
}

slots_spin_result_t slots_logic_evaluate(slots_symbol_t a, slots_symbol_t b, slots_symbol_t c)
{
    if (a != b || b != c) {
        return SLOTS_RESULT_NONE;
    }
    return (a == SLOTS_SYMBOL_SEVEN) ? SLOTS_RESULT_JACKPOT : SLOTS_RESULT_WIN;
}

/* Nth index on `reel` carrying `symbol`, where n wraps over however many of
   that symbol the strip holds. Every symbol appears on every strip, so this
   always finds one. */
static uint8_t index_of_symbol(uint8_t reel, slots_symbol_t symbol, uint32_t n)
{
    uint8_t found[SLOTS_STRIP_LEN];
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < SLOTS_STRIP_LEN; i++) {
        if (k_strip[reel][i] == (uint8_t)symbol) {
            found[count++] = i;
        }
    }
    if (count == 0u) {
        return 0u;              /* unreachable with the strips above */
    }
    return found[n % count];
}

/* A symbol to line up for an ordinary win, weighted toward the common ones so
   wins usually show cherries and bells rather than stars. */
static slots_symbol_t pick_win_symbol(void)
{
    uint32_t r = slots_rng(100u);
    if (r < 40u) { return SLOTS_SYMBOL_CHERRY; }
    if (r < 70u) { return SLOTS_SYMBOL_BELL; }
    if (r < 88u) { return SLOTS_SYMBOL_BAR; }
    return SLOTS_SYMBOL_STAR;
}

/* Lands `reel` one cell away from an index carrying `symbol`, so the match
   ends up directly above or below the payline instead of on it. */
static uint8_t near_miss_target(uint8_t reel, slots_symbol_t symbol)
{
    uint8_t hit = index_of_symbol(reel, symbol, slots_rng(SLOTS_STRIP_LEN));
    /* Offsetting by -1 puts the match one cell below the payline, +1 above. */
    int8_t offset = (slots_rng(2u) == 0u) ? -1 : 1;
    return (uint8_t)((hit + SLOTS_STRIP_LEN + (uint8_t)(offset & 0xF)) % SLOTS_STRIP_LEN);
}

slots_spin_plan_t slots_logic_plan_spin(void)
{
    slots_spin_plan_t plan;
    plan.drama = SLOTS_DRAMA_NONE;

    uint32_t roll = slots_rng(1000u);

    if (roll <= ODDS_JACKPOT_MAX) {
        plan.result = SLOTS_RESULT_JACKPOT;
        for (uint8_t r = 0u; r < SLOTS_REEL_COUNT; r++) {
            plan.target[r] = index_of_symbol(r, SLOTS_SYMBOL_SEVEN, 0u);
        }
        return plan;
    }

    if (roll <= ODDS_WIN_MAX) {
        plan.result = SLOTS_RESULT_WIN;
        slots_symbol_t symbol = pick_win_symbol();
        for (uint8_t r = 0u; r < SLOTS_REEL_COUNT; r++) {
            plan.target[r] = index_of_symbol(r, symbol, slots_rng(SLOTS_STRIP_LEN));
        }
        return plan;
    }

    /* Loss. Some losses are promoted to drama. */
    plan.result = SLOTS_RESULT_NONE;
    uint32_t drama_roll = slots_rng(1000u);

    if (drama_roll <= ODDS_NEAR_MISS_MAX) {
        slots_symbol_t symbol = pick_win_symbol();
        plan.target[0] = index_of_symbol(0u, symbol, slots_rng(SLOTS_STRIP_LEN));
        plan.target[1] = index_of_symbol(1u, symbol, slots_rng(SLOTS_STRIP_LEN));
        plan.target[2] = near_miss_target(2u, symbol);
        plan.drama = SLOTS_DRAMA_NEAR_MISS;
    } else if (drama_roll <= ODDS_TEASE_MAX) {
        plan.target[0] = index_of_symbol(0u, SLOTS_SYMBOL_SEVEN, 0u);
        plan.target[1] = index_of_symbol(1u, SLOTS_SYMBOL_SEVEN, 0u);
        plan.target[2] = near_miss_target(2u, SLOTS_SYMBOL_SEVEN);
        plan.drama = SLOTS_DRAMA_JACKPOT_TEASE;
    } else {
        for (uint8_t r = 0u; r < SLOTS_REEL_COUNT; r++) {
            plan.target[r] = (uint8_t)slots_rng(SLOTS_STRIP_LEN);
        }
        /* Reels 0 and 1 agreeing is what makes the machine milk the last reel,
           so it must never happen by accident on a plain loss — otherwise the
           tease fires on spins that were never going anywhere. Every strip
           holds five distinct symbols, so a non-matching index always exists. */
        while (slots_logic_symbol_at(0u, plan.target[0]) ==
               slots_logic_symbol_at(1u, plan.target[1])) {
            plan.target[1] = (uint8_t)((plan.target[1] + 1u) % SLOTS_STRIP_LEN);
        }
    }

    /* A plain or dramatic loss must never accidentally line up three of a
       kind — nudge reel 2 until it doesn't. */
    while (slots_logic_evaluate(slots_logic_symbol_at(0u, plan.target[0]),
                                slots_logic_symbol_at(1u, plan.target[1]),
                                slots_logic_symbol_at(2u, plan.target[2]))
           != SLOTS_RESULT_NONE) {
        plan.target[2] = (uint8_t)((plan.target[2] + 1u) % SLOTS_STRIP_LEN);
    }

    return plan;
}
