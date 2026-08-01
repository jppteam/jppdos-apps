/*
 * mtp_transport — MTProto's "intermediate" TCP transport.
 *
 * Of the four framings Telegram offers, intermediate is the one worth having
 * here: the length prefix is a plain 4-byte little-endian *byte* count, where
 * abridged encodes a word count across one or four bytes and full adds a
 * sequence number and a CRC32 this client would have to carry code for.
 *
 * The layer is deliberately dumb — it owns the socket and the framing and
 * nothing else. Buffers belong to mtp_session, which decrypts in place, so
 * nothing here needs to be sized against MTP_RX_MAX.
 */
#pragma once

#include "mtp_common.h"
#include "jpp_sdk_bridge.h"

/*
 * Connect and send the transport-identifying magic. `host` may be a name or a
 * dotted quad; the firmware resolves it. Distinguishes the failure the user can
 * act on (MTP_ERR_SERVER_ACTIVE — WebDAV or the LRV server holds the network)
 * from one they cannot.
 */
mtp_err_t mtp_tp_connect(jpp_sdk_context_t *ctx, const char *host, uint16_t port);

void mtp_tp_close(jpp_sdk_context_t *ctx);
bool mtp_tp_is_open(void);

/* Frame and send one payload. `len` must be a multiple of 4. Sends completely or
   fails — the firmware's net_send loops internally. */
mtp_err_t mtp_tp_send(jpp_sdk_context_t *ctx, const uint8_t *payload, size_t len);

/*
 * Read exactly one frame.
 *
 * `timeout_ms` applies only to the wait for the frame's first byte, and 0 means
 * poll and return immediately (which is how the update loop stays responsive to
 * the keypad). Once a header has started arriving the remainder is read with a
 * fixed internal timeout instead: abandoning a half-read frame would leave the
 * stream desynchronised, and every later frame would be garbage.
 *
 * Returns MTP_ERR_TIMEOUT when no frame was waiting, MTP_ERR_CLOSED when the
 * peer hung up, and MTP_ERR_OVERFLOW when the frame will not fit `cap` (the
 * socket is closed in that case, since the excess cannot be skipped safely).
 */
mtp_err_t mtp_tp_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t cap,
                      size_t *out_len, uint32_t timeout_ms);

/*
 * Transport-level errors arrive as a lone 4-byte frame holding a negative
 * int32 — outside the MTProto message layer entirely, so the session cannot
 * decrypt it. -404 (auth_key not found on this DC) is the one that matters: it
 * means the stored key is dead and login has to start over.
 */
bool mtp_tp_frame_is_error(const uint8_t *buf, size_t len, int32_t *out_code);

#define MTP_TP_ERR_AUTH_KEY_UNKNOWN (-404)
