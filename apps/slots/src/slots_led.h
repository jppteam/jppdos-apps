#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

/* Pulse strengths handed to slots_led_tick(). A cell crossing sits at the
   sustained spin ceiling; reel arrivals escalate with reel index and
   deliberately exceed it — an arrival is a momentary event, not a resting
   level, so the ceiling that keeps the pixel comfortable to sit beside still
   governs everything the eye reads as steady. */
#define SLOTS_LED_PULSE_NONE    0u
#define SLOTS_LED_PULSE_TICK   64u
#define SLOTS_LED_PULSE_STOP0  96u
#define SLOTS_LED_PULSE_STOP1 160u
#define SLOTS_LED_PULSE_STOP2 240u

typedef enum {
    SLOTS_LED_IDLE = 0,   /* amber breathing */
    SLOTS_LED_SPIN,       /* cool white-blue, pulses on each reel tick */
    SLOTS_LED_CRAWL,      /* red heartbeat — the near-miss tease */
    SLOTS_LED_WIN,        /* green flash then decaying pulses */
    SLOTS_LED_JACKPOT,    /* full-saturation colour cycle */
    SLOTS_LED_LOSS,       /* fade to black */
} slots_led_mode_t;

void slots_led_init(jpp_sdk_context_t *ctx);

/* Call once per frame. `frame` counts frames since the current mode began;
   `pulse` is a SLOTS_LED_PULSE_* strength requesting a brightness kick (a
   reel cell crossing or arrival), or SLOTS_LED_PULSE_NONE for no kick. */
void slots_led_tick(slots_led_mode_t mode, uint32_t frame, uint8_t pulse);

void slots_led_off(void);
