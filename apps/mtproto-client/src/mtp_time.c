#include "mtp_time.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#pragma GCC visibility push(hidden)

/* Unixtime of the RTC seed, and the tick count when it was taken. Elapsed time
   since the seed comes from the tick, which is what gives sub-second msg_ids. */
static uint32_t s_seed_unix;
static uint32_t s_seed_tick_ms;
static bool     s_seeded;

/* Correction from server msg_ids, applied on top of the seed. Signed because the
   unknown timezone can put the seed either side of the truth. */
static int32_t  s_offset;
static bool     s_synced;

/* msg_id floor and the content-related message count, both per session. */
static uint64_t s_last_msg_id;
static uint32_t s_seq;

static uint32_t tick_ms(void)
{
    /* portTICK_PERIOD_MS keeps this correct whatever CONFIG_FREERTOS_HZ is; the
       wrap at ~49 days is harmless because only differences are used. */
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

/*
 * Days since 1970-01-01 from a proleptic-Gregorian date (Howard Hinnant's
 * days_from_civil). Integer-only — there is no math.h on the app include path,
 * and no mktime in the symbol table.
 */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2u) ? 1 : 0;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);                       /* 0..399   */
    uint32_t doy = (153u * (m + (m > 2u ? -3u : 9u)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        /* 0..146096 */
    return era * 146097 + (int32_t)doe - 719468;
}

/* Two digits at a fixed offset. The RTC format is fixed-width, so there is no
   need for a general parser — and no strtol to reach for anyway. */
static bool two_digits(const char *s, uint32_t *out)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
        return false;
    }
    *out = (uint32_t)(s[0] - '0') * 10u + (uint32_t)(s[1] - '0');
    return true;
}

/* Parses exactly "YYYY-MM-DD HH:mm" as produced by rtc_reader_cb. */
static bool parse_rtc(const char *text, uint32_t *out_unix)
{
    if (text == NULL || strlen(text) < 16u) {
        return false;
    }
    uint32_t yhi, ylo, mon, day, hour, min;
    if (!two_digits(text + 0, &yhi) || !two_digits(text + 2, &ylo) ||
        !two_digits(text + 5, &mon) || !two_digits(text + 8, &day) ||
        !two_digits(text + 11, &hour) || !two_digits(text + 14, &min)) {
        return false;
    }
    int32_t year = (int32_t)(yhi * 100u + ylo);
    if (mon < 1u || mon > 12u || day < 1u || day > 31u || hour > 23u || min > 59u) {
        return false;
    }
    int32_t days = days_from_civil(year, mon, day);
    if (days < 0) {
        return false;
    }
    *out_unix = (uint32_t)days * 86400u + hour * 3600u + min * 60u;
    return true;
}

mtp_err_t mtp_time_init(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t result;
    memset(&result, 0, sizeof(result));

    s_seeded = false;
    if (jpp_sdk_get_time(ctx, &result) == JPP_SDK_STATUS_OK && result.ok) {
        const char *text = jpp_broker_result_get(&result, "text");
        uint32_t unix_sec;
        if (parse_rtc(text, &unix_sec)) {
            s_seed_unix = unix_sec;
            s_seed_tick_ms = tick_ms();
            s_seeded = true;
        }
    }
    if (!s_seeded) {
        /* No RTC. Start from zero and let the first server response set the
           clock — the handshake does not depend on this being right. */
        s_seed_unix = 0u;
        s_seed_tick_ms = tick_ms();
    }
    return s_seeded ? MTP_OK : MTP_ERR_NO_TIME;
}

/* Milliseconds since the seed, as a 64-bit value so the split into seconds and
   remainder below cannot overflow on a long session. */
static uint64_t elapsed_ms(void)
{
    return (uint64_t)(tick_ms() - s_seed_tick_ms);
}

uint32_t mtp_time_now(void)
{
    uint64_t secs = (uint64_t)s_seed_unix + elapsed_ms() / 1000u;
    return (uint32_t)((int64_t)secs + (int64_t)s_offset);
}

void mtp_time_latch(uint64_t server_msg_id)
{
    uint32_t server_unix = (uint32_t)(server_msg_id >> 32);
    /* Guard against latching from a corrupt id: anything before 2020 or after
       2100 is not a plausible server clock and would poison every later msg_id. */
    if (server_unix < 1577836800u || server_unix > 4102444800u) {
        return;
    }
    uint64_t local = (uint64_t)s_seed_unix + elapsed_ms() / 1000u;
    s_offset = (int32_t)((int64_t)server_unix - (int64_t)local);
    s_synced = true;
}

bool mtp_time_is_synced(void) { return s_synced; }

uint64_t mtp_time_msg_id(void)
{
    uint64_t ms = elapsed_ms();
    uint64_t secs = (uint64_t)s_seed_unix + ms / 1000u;
    secs = (uint64_t)((int64_t)secs + (int64_t)s_offset);

    /*
     * The low 32 bits are the fraction of a second, scaled to 2^32. The bottom
     * two bits are then cleared: the spec reserves them to distinguish client
     * ids (0 mod 4) from server ones, and a client id that is not divisible by
     * 4 is rejected outright.
     */
    uint32_t frac = (uint32_t)(((ms % 1000u) << 32) / 1000u);
    uint64_t id = (secs << 32) | (uint64_t)(frac & ~3u);

    /*
     * Strict monotonicity matters more than accuracy here. The tick has 10 ms
     * resolution at the default CONFIG_FREERTOS_HZ, so two requests issued in
     * the same tick would otherwise collide and the server would drop the
     * second as a duplicate.
     */
    if (id <= s_last_msg_id) {
        id = s_last_msg_id + 4u;
    }
    s_last_msg_id = id;
    return id;
}

uint32_t mtp_time_seq_no(bool content_related)
{
    if (content_related) {
        uint32_t seq = s_seq * 2u + 1u;
        s_seq++;
        return seq;
    }
    return s_seq * 2u;
}

void mtp_time_reset_session(void)
{
    s_seq = 0u;
    s_last_msg_id = 0u;
}

void mtp_time_fmt_hhmm(uint32_t unixtime, char *out, size_t out_cap)
{
    if (out_cap < 6u) {
        if (out_cap > 0u) {
            out[0] = '\0';
        }
        return;
    }
    if (!s_seeded && !s_synced) {
        memcpy(out, "--:--", 6u);
        return;
    }
    uint32_t sod = unixtime % 86400u;
    uint32_t hh = sod / 3600u;
    uint32_t mm = (sod % 3600u) / 60u;
    out[0] = (char)('0' + hh / 10u);
    out[1] = (char)('0' + hh % 10u);
    out[2] = ':';
    out[3] = (char)('0' + mm / 10u);
    out[4] = (char)('0' + mm % 10u);
    out[5] = '\0';
}

int32_t mtp_time_day_index(uint32_t unixtime)
{
    return (int32_t)(unixtime / 86400u);
}

#pragma GCC visibility pop
