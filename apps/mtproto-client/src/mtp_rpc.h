/*
 * mtp_rpc — request/response matching and the service-message layer.
 *
 * MTProto does not hand back a clean reply per request. What arrives is a stream
 * that may contain containers of several messages, gzipped bodies, corrections
 * the client is obliged to act on, acknowledgements, and updates unrelated to
 * anything outstanding. This layer flattens all of that into "call a method, get
 * a result", and routes anything unsolicited to the update handler.
 *
 * Three service messages are corrections rather than information, and handling
 * them is what makes the client work on a device with no real clock:
 *
 *   bad_server_salt        the salt rotated; adopt the new one and resend
 *   bad_msg_notification   our msg_id was outside the server's window; re-anchor
 *                          the clock from the server's own msg_id and resend
 *   new_session_created    the server dropped our session; adopt the new salt
 *
 * Without the resend, every one of these would surface as a lost request.
 */
#pragma once

#include "mtp_common.h"
#include "mtp_session.h"

/*
 * Called for each message that is not a reply to an outstanding request —
 * updates, mostly. The body points into a shared buffer and is only valid for
 * the duration of the call.
 */
typedef void (*mtp_rpc_update_fn)(void *user, const uint8_t *body, size_t len);

void mtp_rpc_set_update_handler(mtp_rpc_update_fn fn, void *user);

/* Clear per-connection state: pending acks and any half-finished call. The
   auth_key and the clock correction survive, since both outlive a socket. */
void mtp_rpc_reset(void);

/*
 * Invoke one method and wait for its result.
 *
 * `out_result`/`out_len` point at the response body — inside either the session
 * RX buffer or the inflate buffer — and stay valid only until the next call.
 *
 * Returns MTP_ERR_RPC when the server answered with rpc_error; the code and text
 * are then available below. Returns MTP_ERR_MIGRATE for the *_MIGRATE_n family,
 * with the target DC in mtp_rpc_migrate_dc().
 */
mtp_err_t mtp_rpc_call(jpp_sdk_context_t *ctx,
                       const uint8_t *body, size_t len,
                       const uint8_t **out_result, size_t *out_len,
                       uint32_t timeout_ms);

/*
 * Drain whatever has arrived without an outstanding request, dispatching updates
 * and flushing acks. `timeout_ms` of 0 polls, which is what lets the main loop
 * stay responsive to the keypad while a connection is live.
 * Returns MTP_ERR_TIMEOUT when nothing was waiting — an expected, benign result.
 */
mtp_err_t mtp_rpc_poll(jpp_sdk_context_t *ctx, uint32_t timeout_ms);

/*
 * Keepalive. Telegram closes an idle connection, and ping_delay_disconnect also
 * tells the server to drop us after `disconnect_delay` seconds of silence, which
 * is what stops a half-open socket from looking alive after a WiFi drop.
 */
mtp_err_t mtp_rpc_ping(jpp_sdk_context_t *ctx, uint32_t disconnect_delay_s);

/* Send any queued acknowledgements. Called automatically by call/poll; exposed
   for the shutdown path, which should not leave the server waiting. */
mtp_err_t mtp_rpc_flush_acks(jpp_sdk_context_t *ctx);

/* Details of the last MTP_ERR_RPC. */
int         mtp_rpc_error_code(void);
const char *mtp_rpc_error_text(void);

/* Target DC of the last MTP_ERR_MIGRATE. */
int mtp_rpc_migrate_dc(void);

/*
 * Seconds to wait, from the last FLOOD_WAIT_n error. Telegram rate-limits
 * aggressively during login, and retrying before this elapses only extends it.
 */
int mtp_rpc_flood_wait(void);
