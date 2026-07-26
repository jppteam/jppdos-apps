#include "slots_audio.h"

static jpp_sdk_context_t *s_ctx;
static uint32_t s_suppress;

void slots_audio_init(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
    s_suppress = 0u;
}

void slots_audio_frame(void)
{
    if (s_suppress > 0u) {
        s_suppress--;
    }
}

static void play(const jpp_buzzer_note_t *notes, size_t count)
{
    if (s_ctx != NULL) {
        jpp_sdk_buzzer_play_sequence_async(s_ctx, notes, count);
    }
}

void slots_audio_tick_blip(void)
{
    if (s_suppress > 0u) {
        return;
    }
    static const jpp_buzzer_note_t k_tick[] = { { 1200u, 10u } };
    play(k_tick, 1u);
}

void slots_audio_reel_stop(uint8_t reel)
{
    /* Rising pitch per reel: the third reel arriving is the loudest event of
       an ordinary spin. */
    static const jpp_buzzer_note_t k_thunk0[] = { { 330u, 40u }, { 220u, 70u } };
    static const jpp_buzzer_note_t k_thunk1[] = { { 440u, 40u }, { 294u, 70u } };
    static const jpp_buzzer_note_t k_thunk2[] = { { 587u, 40u }, { 392u, 90u } };

    switch (reel) {
    case 0u:  play(k_thunk0, 2u); break;
    case 1u:  play(k_thunk1, 2u); break;
    default:  play(k_thunk2, 2u); break;
    }
    s_suppress = SLOTS_AUDIO_SUPPRESS_FRAMES;
}

void slots_audio_loss_click(void)
{
    static const jpp_buzzer_note_t k_click[] = { { 180u, 25u } };
    play(k_click, 1u);
}

void slots_audio_win(void)
{
    static const jpp_buzzer_note_t k_win[] = {
        { 659u, 120u }, { 784u, 120u }, { 988u, 200u },
    };
    play(k_win, 3u);
    s_suppress = SLOTS_AUDIO_SUPPRESS_FRAMES;
}

void slots_audio_jackpot_bar(uint8_t bar)
{
    static const jpp_buzzer_note_t k_bar0[] = {
        { 523u, 100u }, { 659u, 100u }, { 784u, 100u }, { 1047u, 240u },
    };
    static const jpp_buzzer_note_t k_bar1[] = {
        { 587u, 100u }, { 740u, 100u }, { 880u, 100u }, { 1175u, 240u },
    };
    static const jpp_buzzer_note_t k_bar2[] = {
        { 659u, 90u }, { 784u, 90u }, { 988u, 90u }, { 1319u, 130u }, { 988u, 130u },
    };
    static const jpp_buzzer_note_t k_bar3[] = {
        { 1047u, 90u }, { 1319u, 90u }, { 1568u, 300u }, { 0u, 60u }, { 1568u, 260u },
    };

    switch (bar) {
    case 0u:  play(k_bar0, 4u); break;
    case 1u:  play(k_bar1, 4u); break;
    case 2u:  play(k_bar2, 5u); break;
    default:  play(k_bar3, 5u); break;
    }
    s_suppress = SLOTS_AUDIO_SUPPRESS_FRAMES;
}

void slots_audio_stop(void)
{
    if (s_ctx != NULL) {
        jpp_sdk_buzzer_stop(s_ctx);
    }
}
