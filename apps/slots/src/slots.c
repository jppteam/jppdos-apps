#include "slots.h"

#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "slots_audio.h"
#include "slots_fx.h"
#include "slots_gfx.h"
#include "slots_led.h"
#include "slots_logic.h"
#include "slots_reel.h"
#include "slots_rng.h"

#define FRAME_MS       33u    /* ~30 fps */

/* Layout — see the design doc. Reel area is x 0..95 in three 32 px lanes;
   the right panel from x 97 carries the jackpot counter. */
#define LANE_W         32
#define REEL_AREA_W    96
#define LANE_SYM_X(l)  ((l) * LANE_W + 8)
#define PANEL_X        99
#define PANEL_W        (SLOTS_GFX_COLS - PANEL_X)   /* 29 px */
#define PAYLINE_Y      31
#define PAYLINE_CELL_Y SLOTS_PAYLINE_Y0             /* top of the payline cell */
/* A 16 px symbol cannot be centred in a 21 px cell — 5 px of slack splits
   unevenly. Biasing 2 px down puts the symbol at rows 23..38, i.e. half a
   pixel above the payline at y=31, which beats sitting half a pixel below. */
#define SYM_INSET      2

/* Per-reel frame budgets. These are *ceilings*, not spin lengths: a reel stops
   the moment its eased position reaches the target, which is always earlier —
   so the landing frame in the comments is the number that matters, and raising
   a budget lengthens that reel's approach. The stagger is what makes reels
   arrive one at a time; drama pushes reel 2 out further and eases it harder.
   Landings are measured by tests/test_slots_logic.py's reel harness. */
#define FRAMES_R0      27u    /* lands frame 25, ~0.83 s */
#define FRAMES_R1      44u    /* lands frame 40, ~1.32 s */
#define FRAMES_R2      61u    /* lands frame 55, ~1.82 s */
#define FRAMES_R2_DRAMA 106u  /* lands frame 88, ~2.90 s */
#define REVS_R0         3u
#define REVS_R1         4u
#define REVS_R2         5u

typedef enum {
    ST_IDLE = 0,
    ST_SPIN,
    ST_RESULT,
} slots_state_t;

typedef struct {
    jpp_sdk_context_t *ctx;
    slots_reel_t       reel[SLOTS_REEL_COUNT];
    slots_spin_plan_t  plan;
    slots_state_t      state;
    uint32_t           state_frame;
    uint32_t           jackpots;
    uint32_t           shown_jackpots;   /* what the counter displays; rolls up */
    slots_led_mode_t   led_mode;
    uint32_t           led_frame;
    bool               pending_spin;         /* CENTER pressed during ST_RESULT */
    bool               result_sound_fired;   /* win/loss sound already played */
    bool               milking;              /* reels 0+1 matched: drag out reel 2 */
} slots_app_t;

/* ---- Rendering ----------------------------------------------------------- */

static void draw_reels(const slots_app_t *app)
{
    for (uint8_t lane = 0u; lane < SLOTS_REEL_COUNT; lane++) {
        int32_t pos    = slots_reel_pos_px(&app->reel[lane]);
        int32_t base   = pos / SLOTS_CELL_H;      /* strip index on the payline */
        int32_t offset = pos % SLOTS_CELL_H;      /* how far between cells */

        /* Four cells cover the window with room for partials at both edges. */
        for (int k = -1; k <= 2; k++) {
            int cell_top = PAYLINE_CELL_Y - (int)offset + k * SLOTS_CELL_H;
            if (cell_top > (int)SLOTS_GFX_ROWS || cell_top + SLOTS_CELL_H < 0) {
                continue;
            }
            uint8_t index = (uint8_t)((base + k + SLOTS_STRIP_LEN) % SLOTS_STRIP_LEN);
            slots_gfx_draw_symbol(LANE_SYM_X(lane), cell_top + SYM_INSET,
                                  slots_logic_symbol_at(lane, index));
        }
    }
}

static void draw_frame_furniture(const slots_app_t *app)
{
    slots_gfx_vline(LANE_W, 0, SLOTS_GFX_ROWS - 1, true);
    slots_gfx_vline(LANE_W * 2, 0, SLOTS_GFX_ROWS - 1, true);
    slots_gfx_vline(REEL_AREA_W, 0, SLOTS_GFX_ROWS - 1, true);

    /* Payline chevrons. In IDLE they pulse so a resting machine doesn't look
       frozen; during a spin they stay solid. */
    bool show = (app->state != ST_IDLE) || ((app->state_frame / 12u) % 4u) != 0u;
    if (show) {
        for (int i = 0; i < 4; i++) {
            slots_gfx_vline(i, PAYLINE_Y - (3 - i), PAYLINE_Y + (3 - i), true);
            slots_gfx_vline(REEL_AREA_W - 4 + i, PAYLINE_Y - i, PAYLINE_Y + i, true);
        }
    }
}

static void draw_panel(const slots_app_t *app)
{
    char buf[8];
    uint32_t shown = app->shown_jackpots;
    if (shown > 9999u) {
        shown = 9999u;      /* the panel is PANEL_W px wide; the stored count keeps going */
    }
    snprintf(buf, sizeof(buf), "%u", (unsigned)shown);

    slots_gfx_text(PANEL_X + 2, 18, "JP", 2);
    int scale = (shown >= 100u) ? 1 : 2;
    int w = slots_gfx_text_w(buf, scale);
    slots_gfx_text(PANEL_X + (PANEL_W - w) / 2, 36, buf, scale);
}

static void render(const slots_app_t *app)
{
    slots_gfx_clear();
    draw_reels(app);
    draw_frame_furniture(app);
    draw_panel(app);
    slots_fx_draw();
    slots_gfx_flush(app->ctx);
}

/* ---- Spin ---------------------------------------------------------------- */

static void begin_spin(slots_app_t *app)
{
    app->plan = slots_logic_plan_spin();

    /* Milk the last reel whenever the first two agree — what the player can
       see, not what the planner decided. Keying this off the outcome would
       make the slow reel a reliable announcement that the spin has already
       lost, since a win shows the same two matching reels beforehand. */
    app->milking = slots_logic_symbol_at(0u, app->plan.target[0]) ==
                   slots_logic_symbol_at(1u, app->plan.target[1]);

    slots_reel_start(&app->reel[0], app->plan.target[0], FRAMES_R0,
                     SLOTS_EASE_CUBIC, REVS_R0);
    slots_reel_start(&app->reel[1], app->plan.target[1], FRAMES_R1,
                     SLOTS_EASE_CUBIC, REVS_R1);
    slots_reel_start(&app->reel[2], app->plan.target[2],
                     app->milking ? FRAMES_R2_DRAMA : FRAMES_R2,
                     app->milking ? SLOTS_EASE_QUART : SLOTS_EASE_CUBIC, REVS_R2);

    app->state = ST_SPIN;
    app->state_frame = 0u;
    app->led_mode = SLOTS_LED_SPIN;
    app->led_frame = 0u;
    app->pending_spin = false;
}

static void persist_jackpots(slots_app_t *app)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)app->jackpots);
    if (jpp_sdk_kv_set(app->ctx, "jackpots", buf) != JPP_SDK_STATUS_OK) {
        jpp_sdk_log(app->ctx, "slots_kv_set_failed");
    }
}

static void enter_result(slots_app_t *app)
{
    app->state = ST_RESULT;
    app->state_frame = 0u;
    app->led_frame = 0u;
    /* No sound here. The third reel's arrival thunk was submitted on this very
       frame and owns the buzzer; the result sound is fired from ST_RESULT once
       the suppression window has elapsed. */
    app->result_sound_fired = false;

    if (app->plan.result == SLOTS_RESULT_JACKPOT) {
        app->jackpots++;
        persist_jackpots(app);
        slots_fx_start_jackpot();
        app->led_mode = SLOTS_LED_JACKPOT;
    } else if (app->plan.result == SLOTS_RESULT_WIN) {
        slots_fx_start_win();
        app->led_mode = SLOTS_LED_WIN;
    } else {
        app->led_mode = SLOTS_LED_LOSS;
    }
}

/* Advances the three reels, turning their events into sound and LED pulses.
   Returns true once all three have stopped. */
static bool tick_reels(slots_app_t *app, uint8_t *out_pulse)
{
    static const uint8_t k_stop_pulse[SLOTS_REEL_COUNT] = {
        SLOTS_LED_PULSE_STOP0, SLOTS_LED_PULSE_STOP1, SLOTS_LED_PULSE_STOP2,
    };
    bool all_stopped = true;
    uint8_t pulse = SLOTS_LED_PULSE_NONE;
    bool ticked = false;

    for (uint8_t i = 0u; i < SLOTS_REEL_COUNT; i++) {
        uint32_t ev = slots_reel_tick(&app->reel[i]);

        /* At most one blip per frame, taken from the lowest-numbered reel that
           crossed a cell this frame. The blip carries no reel identity, so
           which reel sourced it does not matter — what matters is that the
           rate tracks whichever reels are still moving, which it does because
           crossings are proportional to velocity. */
        if ((ev & SLOTS_REEL_EV_CROSSED_CELL) && !ticked) {
            slots_audio_tick_blip();
            ticked = true;
            if (pulse < SLOTS_LED_PULSE_TICK) {
                pulse = SLOTS_LED_PULSE_TICK;
            }
        }
        if (ev & SLOTS_REEL_EV_STOPPED) {
            slots_audio_reel_stop(i);
            if (pulse < k_stop_pulse[i]) {
                pulse = k_stop_pulse[i];   /* arrival escalates with reel index */
            }
        }
        if (!slots_reel_stopped(&app->reel[i])) {
            all_stopped = false;
        }
    }

    *out_pulse = pulse;
    return all_stopped;
}

/* The tension is on screen exactly when the first two reels have landed
   matching and the last one is still creeping — win or lose. */
static bool in_crawl(const slots_app_t *app)
{
    return app->milking &&
           slots_reel_stopped(&app->reel[0]) &&
           slots_reel_stopped(&app->reel[1]) &&
           !slots_reel_stopped(&app->reel[2]);
}

/* ---- Entry --------------------------------------------------------------- */

void slots_run(jpp_sdk_context_t *ctx)
{
    static slots_app_t app;

    app.ctx = ctx;
    app.state = ST_IDLE;
    app.state_frame = 0u;
    app.led_mode = SLOTS_LED_IDLE;
    app.led_frame = 0u;
    app.pending_spin = false;
    app.result_sound_fired = true;
    app.milking = false;

    slots_rng_seed((uint32_t)xTaskGetTickCount() * 2654435761u + 1u);
    slots_audio_init(ctx);
    slots_led_init(ctx);

    app.jackpots = 0u;
    char buf[16];
    if (jpp_sdk_kv_get(ctx, "jackpots", buf, sizeof(buf)) == JPP_SDK_STATUS_OK) {
        app.jackpots = (uint32_t)atoi(buf);   /* strtoul is not in the loader whitelist */
    }
    app.shown_jackpots = app.jackpots;

    /* Park the reels on a combination that does not pay. Three independent
       random indices line up 3-of-a-kind about 6% of the time (244/4096), so
       without this the app opens on an uncelebrated win — and once in 4096, on
       an unearned 7-7-7 that never incremented the counter. */
    uint8_t park[SLOTS_REEL_COUNT];
    do {
        for (uint8_t i = 0u; i < SLOTS_REEL_COUNT; i++) {
            park[i] = (uint8_t)slots_rng(SLOTS_STRIP_LEN);
        }
    } while (slots_logic_evaluate(slots_logic_symbol_at(0u, park[0]),
                                  slots_logic_symbol_at(1u, park[1]),
                                  slots_logic_symbol_at(2u, park[2]))
             != SLOTS_RESULT_NONE);
    for (uint8_t i = 0u; i < SLOTS_REEL_COUNT; i++) {
        slots_reel_init(&app.reel[i], park[i]);
    }

    jpp_sdk_wakelock_acquire(ctx);
    jpp_sdk_canvas_fullscreen(ctx, true);

    bool running = true;
    while (running && !ctx->close_requested) {
        /* Input — drain the queue so buffered presses aren't lost. */
        bool spin_requested = false;
        bool any_key = false;
        jpp_sdk_key_event_t key;
        while (jpp_sdk_poll_key(ctx, &key) == JPP_SDK_STATUS_OK &&
               key != JPP_SDK_KEY_NONE) {
            if (key == JPP_SDK_KEY_CENTER_LONG) {
                running = false;
            } else {
                any_key = true;
                if (key == JPP_SDK_KEY_CENTER) {
                    spin_requested = true;
                    /* Queue only from ST_RESULT. Making this a general sticky
                       flag would turn mashing CENTER mid-spin into an unwanted
                       auto-respin; from ST_RESULT it always means "skip the
                       celebration, then spin again". */
                    if (app.state == ST_RESULT) {
                        app.pending_spin = true;
                    }
                }
            }
        }
        if (!running) {
            break;
        }

        slots_audio_frame();
        uint8_t pulse = SLOTS_LED_PULSE_NONE;

        switch (app.state) {
        case ST_IDLE:
            if (spin_requested) {
                begin_spin(&app);
            }
            break;

        case ST_SPIN:
            if (tick_reels(&app, &pulse)) {
                enter_result(&app);
            } else if (in_crawl(&app) && app.led_mode != SLOTS_LED_CRAWL) {
                /* slots_led_tick reads `frame` as frames since the mode began,
                   and the heartbeat is frame % 30 — so it has to restart here
                   or the throbs begin at an arbitrary phase. Guarded on the
                   mode so it resets on the transition, not every crawl frame. */
                app.led_mode = SLOTS_LED_CRAWL;
                app.led_frame = 0u;
            }
            break;

        case ST_RESULT:
            /* Any key skips the celebration. */
            if (any_key && slots_fx_active()) {
                slots_fx_skip();
                app.result_sound_fired = true;   /* a skip cancels it too */
            }

            /* state_frame is 1 on the first ST_RESULT frame, so this lands on
               exactly the frame slots_audio's suppression countdown reaches
               zero — the earliest point the result sound will not truncate (or
               outright replace) the third reel's arrival thunk. The jackpot
               fanfare is not fired here; slots_fx.c owns its bar schedule. */
            if (!app.result_sound_fired &&
                app.state_frame == SLOTS_AUDIO_SUPPRESS_FRAMES) {
                app.result_sound_fired = true;
                if (app.plan.result == SLOTS_RESULT_WIN) {
                    slots_audio_win();
                } else if (app.plan.result == SLOTS_RESULT_NONE) {
                    slots_audio_loss_click();
                }
            }

            slots_fx_tick();

            /* Roll the counter up over the last stretch of the jackpot show:
               hold the old value, flicker between old and new, then settle. */
            if (app.plan.result == SLOTS_RESULT_JACKPOT && slots_fx_active()) {
                uint32_t f = slots_fx_frame();
                if (f < 90u) {
                    app.shown_jackpots = app.jackpots - 1u;
                } else if (f < 105u) {
                    app.shown_jackpots = ((f / 3u) % 2u == 0u)
                                         ? app.jackpots - 1u : app.jackpots;
                } else {
                    app.shown_jackpots = app.jackpots;
                }
            }

            /* Dwell long enough for the sound fired above to have started. */
            if (!slots_fx_active() &&
                app.state_frame >= (app.plan.result == SLOTS_RESULT_NONE
                                    ? SLOTS_AUDIO_SUPPRESS_FRAMES + 1u
                                    : SLOTS_AUDIO_SUPPRESS_FRAMES + 5u)) {
                app.shown_jackpots = app.jackpots;
                app.state = ST_IDLE;
                app.state_frame = 0u;
                app.led_mode = SLOTS_LED_IDLE;
                app.led_frame = 0u;
                /* A CENTER press at any point during the result — including
                   the plain-loss window, which is only a few frames long —
                   re-spins immediately. */
                if (app.pending_spin) {
                    begin_spin(&app);
                }
            }
            break;
        }

        slots_led_tick(app.led_mode, app.led_frame, pulse);
        render(&app);

        app.state_frame++;
        app.led_frame++;
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }

    slots_audio_stop();
    slots_led_off();
    jpp_sdk_wakelock_release(ctx);
}
