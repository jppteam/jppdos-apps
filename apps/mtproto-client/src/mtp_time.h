/*
 * mtp_time — the clock MTProto needs, on a device that does not have one.
 *
 * MTProto stamps every message with a msg_id of (unixtime << 32 | subsecond),
 * requires them to increase strictly within a session, and lets the server
 * reject anything more than ~300 s away from its own clock.  This device offers
 * `jpp_sdk_get_time`, which returns "YYYY-MM-DD HH:mm" — no seconds, no epoch,
 * and already shifted into local time by the firmware's TZ offset, which the app
 * cannot read back.  There is no time() or mktime() in the symbol table either.
 *
 * So the clock here is a seed plus a correction:
 *
 *   1. Seed from the RTC string, giving a value that is within a minute of right
 *      but potentially hours out because of the unknown timezone. Good enough to
 *      start the handshake, since plaintext (auth_key_id = 0) messages are not
 *      time-checked.
 *   2. Latch the truth from the server. Every server msg_id carries the server's
 *      own unixtime in its high 32 bits, so the first response fixes the offset
 *      exactly — including the timezone error.
 *   3. Re-latch whenever the server says so, via bad_msg_notification 16/17/20.
 *
 * Sub-second resolution comes from the FreeRTOS tick, so msg_ids keep advancing
 * between RTC minutes. A device with no RTC at all still works: it starts at
 * offset zero and is correct from the first server response onward.
 */
#pragma once

#include "mtp_common.h"
#include "jpp_sdk_bridge.h"

/*
 * Seed from the RTC. Safe to call with no RTC present — that just leaves the
 * clock unseeded, which step 2 above repairs. Returns MTP_ERR_NO_TIME when the
 * seed failed, for callers that want to say so in the UI; the client itself
 * carries on regardless.
 */
mtp_err_t mtp_time_init(jpp_sdk_context_t *ctx);

/* Current best estimate of server unixtime. */
uint32_t mtp_time_now(void);

/*
 * Adopt the server's clock from a msg_id it generated. Idempotent and cheap —
 * called on every server message, so the offset tracks drift for free.
 */
void mtp_time_latch(uint64_t server_msg_id);

/* True once a server message has corrected the clock. Until then, timestamps
   shown in the UI carry the timezone error and are not worth displaying. */
bool mtp_time_is_synced(void);

/*
 * Next client msg_id: time-derived, divisible by 4 as the spec requires of
 * client messages, and strictly greater than every id this session has issued.
 */
uint64_t mtp_time_msg_id(void);

/*
 * Session sequence number. MTProto counts only "content-related" messages (real
 * queries, not acks or pings), and the count is carried in seq_no as n*2+1 for
 * those and 2n for the rest — hence the flag rather than a plain counter.
 */
uint32_t mtp_time_seq_no(bool content_related);

/* Drop per-session state (seq_no and the msg_id floor) without discarding the
   clock correction, which stays valid across reconnects. */
void mtp_time_reset_session(void);

/* Format a unixtime as "HH:MM" for the UI, in whatever zone the RTC seed
   implied. Writes "--:--" when the clock is not usable. */
void mtp_time_fmt_hhmm(uint32_t unixtime, char *out, size_t out_cap);

/* Calendar day number for a unixtime — used only to decide where the chat view
   draws a day separator, so the arbitrary epoch does not matter. */
int32_t mtp_time_day_index(uint32_t unixtime);
