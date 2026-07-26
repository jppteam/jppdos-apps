#include "slots_fx.h"

#include "slots_art.h"
#include "slots_audio.h"
#include "slots_gfx.h"
#include "slots_rng.h"

#define MAX_SPARKS 32u
#define MAX_COINS  12u
#define ROCKETS     6u
#define PILE_MAX    8      /* px — a taller pile eats the reels */
#define GRAVITY    12      /* 8.8 fixed point px/frame^2 */

/* 8.8 fixed point throughout: x/y in pixels, vx/vy in px/frame. */
typedef struct {
    int32_t x, y, vx, vy;
    uint8_t life;        /* frames remaining; 0 = free slot */
    uint8_t life0;       /* life at birth, for the fade ramp */
} spark_t;

typedef struct {
    int32_t x, y, vx, vy;
    bool    landed;
    bool    active;
} coin_t;

/* A rocket rises, decelerates under gravity, and bursts at its apex. Without
   the rising streak the bursts read as random pops rather than fireworks. */
typedef struct {
    int32_t x, y, vy;
    bool    active;
} rocket_t;

static spark_t  s_sparks[MAX_SPARKS];
static coin_t   s_coins[MAX_COINS];
static rocket_t s_rockets[ROCKETS];
static uint8_t  s_pile[SLOTS_GFX_COLS];
static uint32_t s_frame;
static uint32_t s_total;
static bool     s_active;
static bool     s_jackpot;
static uint8_t  s_bar;

static int32_t fp(int v) { return v << 8; }

static void spawn_spark(int32_t x, int32_t y, int32_t vx, int32_t vy, uint8_t life)
{
    for (uint32_t i = 0u; i < MAX_SPARKS; i++) {
        if (s_sparks[i].life == 0u) {
            s_sparks[i].x = x;
            s_sparks[i].y = y;
            s_sparks[i].vx = vx;
            s_sparks[i].vy = vy;
            s_sparks[i].life = life;
            s_sparks[i].life0 = life;
            return;
        }
    }
}

static void burst(int32_t x, int32_t y)
{
    /* 12 sparks on a rough circle — a table would cost flash for no gain at
       this size, so the directions come from the RNG. */
    for (uint32_t i = 0u; i < 12u; i++) {
        int32_t vx = (int32_t)slots_rng(512u) - 256;
        int32_t vy = (int32_t)slots_rng(512u) - 320;
        spawn_spark(x, y, vx, vy, (uint8_t)(18u + slots_rng(10u)));
    }
}

static void launch_rocket(void)
{
    for (uint32_t i = 0u; i < ROCKETS; i++) {
        if (!s_rockets[i].active) {
            s_rockets[i].active = true;
            s_rockets[i].x  = fp((int)(10u + slots_rng(108u)));
            s_rockets[i].y  = fp(SLOTS_GFX_ROWS - 1);
            s_rockets[i].vy = -(int32_t)(360u + slots_rng(160u));
            return;
        }
    }
}

static void reset(void)
{
    for (uint32_t i = 0u; i < MAX_SPARKS; i++) { s_sparks[i].life = 0u; }
    for (uint32_t i = 0u; i < MAX_COINS; i++)  { s_coins[i].active = false; }
    for (uint32_t i = 0u; i < ROCKETS; i++)    { s_rockets[i].active = false; }
    for (uint32_t i = 0u; i < SLOTS_GFX_COLS; i++) { s_pile[i] = 0u; }
    s_frame = 0u;
    s_bar = 0u;
}

void slots_fx_start_win(void)
{
    reset();
    s_active = true;
    s_jackpot = false;
    s_total = SLOTS_FX_WIN_FRAMES;
    /* A handful of sparks popping along the payline. */
    for (uint32_t i = 0u; i < 6u; i++) {
        spawn_spark(fp((int)(8u + slots_rng(80u))), fp(31),
                    (int32_t)slots_rng(300u) - 150,
                    -(int32_t)slots_rng(200u) - 60,
                    (uint8_t)(14u + slots_rng(8u)));
    }
}

void slots_fx_start_jackpot(void)
{
    reset();
    s_active = true;
    s_jackpot = true;
    s_total = SLOTS_FX_JACKPOT_FRAMES;
    for (uint32_t i = 0u; i < MAX_COINS; i++) {
        s_coins[i].active = true;
        s_coins[i].landed = false;
        s_coins[i].x  = fp((int)slots_rng(SLOTS_GFX_COLS - 5u));
        s_coins[i].y  = fp(-6 - (int)slots_rng(60u));
        s_coins[i].vx = (int32_t)slots_rng(80u) - 40;
        s_coins[i].vy = (int32_t)slots_rng(120u) + 60;
    }
}

bool slots_fx_active(void) { return s_active; }
uint32_t slots_fx_frame(void) { return s_frame; }

void slots_fx_skip(void)
{
    s_active = false;
    slots_audio_stop();
}

void slots_fx_tick(void)
{
    if (!s_active) {
        return;
    }

    if (s_jackpot) {
        /* Re-arm the fanfare one bar at a time so it runs under the visuals,
           offset by the suppression window so bar 0 does not land inside the
           third reel's arrival thunk and cut it off. */
        if (s_bar < SLOTS_AUDIO_JACKPOT_BARS &&
            s_frame == s_bar * SLOTS_AUDIO_JACKPOT_BAR_FRAMES
                       + SLOTS_AUDIO_SUPPRESS_FRAMES) {
            slots_audio_jackpot_bar(s_bar);
            s_bar++;
        }
        /* Launch across the fireworks window; each rocket bursts at its own
           apex, so bursts are spread in both time and height. */
        if (s_frame >= 15u && s_frame < 78u && (s_frame % 11u) == 0u) {
            launch_rocket();
        }
    }

    for (uint32_t i = 0u; i < ROCKETS; i++) {
        rocket_t *r = &s_rockets[i];
        if (!r->active) {
            continue;
        }
        r->vy += GRAVITY;
        r->y  += r->vy;
        if (r->vy >= 0 || (r->y >> 8) <= 8) {
            /* Apex (or the ceiling) — burst. */
            burst(r->x, r->y);
            r->active = false;
        }
    }

    for (uint32_t i = 0u; i < MAX_SPARKS; i++) {
        if (s_sparks[i].life == 0u) {
            continue;
        }
        s_sparks[i].vy += GRAVITY;
        s_sparks[i].x  += s_sparks[i].vx;
        s_sparks[i].y  += s_sparks[i].vy;
        s_sparks[i].life--;
    }

    for (uint32_t i = 0u; i < MAX_COINS; i++) {
        coin_t *c = &s_coins[i];
        if (!c->active || c->landed) {
            continue;
        }
        c->vy += GRAVITY / 2;
        c->x  += c->vx;
        c->y  += c->vy;

        int px = c->x >> 8;
        int py = c->y >> 8;
        if (px < 0) { px = 0; c->x = 0; c->vx = -c->vx; }
        if (px > (int)SLOTS_GFX_COLS - 5) { px = SLOTS_GFX_COLS - 5; c->x = fp(px); c->vx = -c->vx; }

        uint8_t floor_h = s_pile[px];
        if (py + 5 >= (int)SLOTS_GFX_ROWS - (int)floor_h) {
            /* Land and add to the pile, so the money visibly accumulates. */
            c->landed = true;
            for (int k = 0; k < 5; k++) {
                int col = px + k;
                if (col >= 0 && col < (int)SLOTS_GFX_COLS && s_pile[col] < PILE_MAX) {
                    s_pile[col] = (uint8_t)(s_pile[col] + 2u > PILE_MAX
                                            ? PILE_MAX : s_pile[col] + 2u);
                }
            }
        }
    }

    s_frame++;
    if (s_frame >= s_total) {
        s_active = false;
    }
}

void slots_fx_draw(void)
{
    if (!s_active) {
        return;
    }

    /* Opening flash: three inversions on alternate frames. The jackpot takes
       the whole screen; a win gets the same beat confined to the payline
       band, which is what reads as a flash on a 1-bit panel. */
    if (s_frame < 15u && ((s_frame / 3u) % 2u) == 0u) {
        if (s_jackpot) {
            slots_gfx_invert();
        } else {
            slots_gfx_invert_band(SLOTS_PAYLINE_Y0, SLOTS_PAYLINE_Y1);
        }
    }

    /* Rockets: the head plus a two-pixel trail, so the rise is visible. */
    for (uint32_t i = 0u; i < ROCKETS; i++) {
        if (!s_rockets[i].active) {
            continue;
        }
        int x = s_rockets[i].x >> 8;
        int y = s_rockets[i].y >> 8;
        slots_gfx_set_pixel(x, y, true);
        slots_gfx_set_pixel(x, y + 1, true);
        if (slots_gfx_dither(x, y + 2, 128u)) {
            slots_gfx_set_pixel(x, y + 2, true);
        }
    }

    for (uint32_t i = 0u; i < MAX_SPARKS; i++) {
        if (s_sparks[i].life == 0u) {
            continue;
        }
        int x = s_sparks[i].x >> 8;
        int y = s_sparks[i].y >> 8;
        /* Fade by ordered dither — how a 1-bit panel does brightness. */
        uint8_t brightness = (uint8_t)((s_sparks[i].life * 255u) / s_sparks[i].life0);
        if (slots_gfx_dither(x, y, brightness)) {
            slots_gfx_set_pixel(x, y, true);
        }
    }

    if (s_jackpot) {
        for (uint32_t i = 0u; i < MAX_COINS; i++) {
            const coin_t *c = &s_coins[i];
            if (!c->active || c->landed) {
                continue;
            }
            /* Alternating face and edge sells the spin cheaply. */
            const uint8_t *sprite = (((s_frame + i) / 4u) % 2u == 0u)
                                    ? slots_art_coin_face : slots_art_coin_edge;
            slots_gfx_draw_sprite5(c->x >> 8, c->y >> 8, sprite);
        }

        for (int x = 0; x < (int)SLOTS_GFX_COLS; x++) {
            if (s_pile[x] > 0u) {
                slots_gfx_vline(x, (int)SLOTS_GFX_ROWS - (int)s_pile[x],
                                (int)SLOTS_GFX_ROWS - 1, true);
            }
        }

        if (s_frame >= 30u && s_frame < 110u) {
            /* Bob +/-2 px. The halo keeps it readable over the fireworks. */
            int bob = (int)((s_frame / 6u) % 4u) - 2;
            int w = slots_gfx_text_w("JACKPOT", 2);
            slots_gfx_text_halo((SLOTS_GFX_COLS - w) / 2, 26 + bob, "JACKPOT", 2);
        }
    }
}
