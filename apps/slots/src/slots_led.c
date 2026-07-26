#include "slots_led.h"

#include "slots_audio.h"

/* A single WS2812 at full white next to a dim OLED is unpleasant to sit
   beside, so everyday states stay well below full brightness. The headroom
   is what makes the jackpot land. */
#define IDLE_MIN     4u
#define IDLE_MAX    40u
#define IDLE_PERIOD 76u    /* frames for a full breath, ~2.5 s */
#define SPIN_BASE   12u
#define PULSE_DECAY  4u    /* frames for a pulse to fall back to base */

/* Win: solid green through the arrival thunk's window, then one pulse per
   jingle note. The jingle is 120/120/200 ms, which at 33 ms/frame is 4/4/6
   frames — so the pulse onsets land on the note onsets. */
#define WIN_SETTLE  30u
static const uint8_t k_win_len[3]  = {   4u,   4u,   6u };
static const uint8_t k_win_peak[3] = { 255u, 190u, 130u };

static jpp_sdk_context_t *s_ctx;
static uint8_t s_last_r, s_last_g, s_last_b;
static bool    s_have_last;
static uint32_t s_pulse;
static uint32_t s_pulse_peak;

void slots_led_init(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
    s_have_last = false;
    s_pulse = 0u;
    s_pulse_peak = 0u;
}

/* Skips the SDK call when nothing changed, so a steady state costs nothing. */
static void emit(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_ctx == NULL) {
        return;
    }
    if (s_have_last && r == s_last_r && g == s_last_g && b == s_last_b) {
        return;
    }
    s_last_r = r;
    s_last_g = g;
    s_last_b = b;
    s_have_last = true;
    jpp_sdk_led_set_color(s_ctx, r, g, b);
}

static uint8_t scale(uint8_t channel, uint32_t level)
{
    return (uint8_t)(((uint32_t)channel * level) / 255u);
}

/* Triangle wave 0..255 over `period` frames, squared. Perceived brightness
   goes roughly as the square of drive, so squaring the ramp is what makes the
   breath look like an even fade instead of a fast rise and a long dim tail. */
static uint32_t breath(uint32_t frame, uint32_t period)
{
    uint32_t t = frame % period;
    uint32_t half = period / 2u;
    uint32_t up = (t < half) ? t : (period - t);
    uint32_t tri = (up * 255u) / half;
    return (tri * tri) / 255u;
}

void slots_led_tick(slots_led_mode_t mode, uint32_t frame, uint8_t pulse)
{
    /* A pulse belongs to the mode that raised it. Dropping it on a mode change
       keeps that explicit: the reel-arrival kick from the end of one spin must
       not decay into the start of the next, which an immediate re-spin makes a
       routine sequence. It happens to be unreachable today (the pulse decays in
       PULSE_DECAY frames and the shortest result dwell is longer), but nothing
       in either module enforces that margin. */
    static slots_led_mode_t s_last_mode = SLOTS_LED_IDLE;
    if (mode != s_last_mode) {
        s_last_mode = mode;
        s_pulse = 0u;
        s_pulse_peak = 0u;
    }

    if (pulse > SLOTS_LED_PULSE_NONE) {
        s_pulse = PULSE_DECAY;
        s_pulse_peak = pulse;
    } else if (s_pulse > 0u) {
        s_pulse--;
    }

    switch (mode) {
    case SLOTS_LED_IDLE: {
        uint32_t b = breath(frame, IDLE_PERIOD);
        uint32_t level = IDLE_MIN + ((IDLE_MAX - IDLE_MIN) * b) / 255u;
        /* Amber: full red, roughly half green. */
        emit((uint8_t)level, (uint8_t)(level / 2u), 0u);
        break;
    }
    case SLOTS_LED_SPIN: {
        uint32_t peak = (s_pulse_peak > SPIN_BASE) ? s_pulse_peak : SPIN_BASE;
        uint32_t level = SPIN_BASE + ((peak - SPIN_BASE) * s_pulse) / PULSE_DECAY;
        /* Cool white-blue. */
        emit((uint8_t)(level / 2u), (uint8_t)((level * 3u) / 4u), (uint8_t)level);
        break;
    }
    case SLOTS_LED_CRAWL: {
        /* Two quick throbs then a pause, ~1 Hz. Red appears nowhere else, so
           it reads unambiguously as tension. */
        uint32_t t = frame % 30u;
        uint32_t level = (t < 4u || (t >= 8u && t < 12u)) ? 200u : 30u;
        emit((uint8_t)level, (uint8_t)(level / 8u), 0u);
        break;
    }
    case SLOTS_LED_WIN: {
        /* Hold full green while the third reel's thunk still owns the buzzer,
           then pulse once per jingle note. The jingle is fired at
           SLOTS_AUDIO_SUPPRESS_FRAMES, so the pulses start there too. */
        uint32_t level = WIN_SETTLE;
        if (frame < SLOTS_AUDIO_SUPPRESS_FRAMES) {
            level = 255u;
        } else {
            uint32_t t = frame - SLOTS_AUDIO_SUPPRESS_FRAMES;
            uint32_t start = 0u;
            for (uint32_t i = 0u; i < 3u; i++) {
                if (t < start + k_win_len[i]) {
                    uint32_t peak = k_win_peak[i];
                    level = peak - ((peak - WIN_SETTLE) * (t - start)) / k_win_len[i];
                    break;
                }
                start += k_win_len[i];
            }
        }
        emit(0u, (uint8_t)level, 0u);
        break;
    }
    case SLOTS_LED_JACKPOT: {
        static const uint8_t k_cycle[6][3] = {
            { 255,   0,   0 }, { 255, 200,   0 }, {   0, 255,   0 },
            {   0, 255, 255 }, {   0,   0, 255 }, { 255,   0, 255 },
        };
        const uint8_t *c = k_cycle[(frame / 4u) % 6u];
        emit(c[0], c[1], c[2]);
        break;
    }
    case SLOTS_LED_LOSS:
    default: {
        uint32_t level = (frame < 10u) ? (10u - frame) * 4u : 0u;
        emit(scale(255u, level), scale(128u, level), 0u);
        break;
    }
    }
}

void slots_led_off(void)
{
    if (s_ctx != NULL) {
        jpp_sdk_led_off(s_ctx);
    }
    s_have_last = false;
}
