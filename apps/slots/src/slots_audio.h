#pragma once

#include <stdint.h>

#include "jpp_sdk_bridge.h"

/* The jackpot fanfare is submitted one bar at a time so it plays over the
   celebration instead of blocking before it. */
#define SLOTS_AUDIO_JACKPOT_BARS        4u
#define SLOTS_AUDIO_JACKPOT_BAR_FRAMES 30u   /* ~1 s at 33 ms/frame */

/* Frames a just-submitted sequence owns the buzzer for. The buzzer is a single
   channel and a new async sequence preempts the current one, so anything that
   would follow a reel-stop thunk has to wait this long or it truncates it —
   or, if the firmware's player task has not woken between the two
   submissions, replaces it outright (ulTaskNotifyTake clears the counter).
   Callers that schedule sound after a thunk key off this. */
#define SLOTS_AUDIO_SUPPRESS_FRAMES 5u

void slots_audio_init(jpp_sdk_context_t *ctx);

/* Call once per frame — drives the tick-suppression countdown. */
void slots_audio_frame(void);

/* Spin tick. Suppressed for a few frames after a reel-stop thunk so the two
   never preempt each other on the single buzzer channel. */
void slots_audio_tick_blip(void);

/* Reel arrival, pitched by reel index (0..2) so arrival escalates. */
void slots_audio_reel_stop(uint8_t reel);

void slots_audio_loss_click(void);
void slots_audio_win(void);
void slots_audio_jackpot_bar(uint8_t bar);   /* 0..SLOTS_AUDIO_JACKPOT_BARS-1 */
void slots_audio_stop(void);
