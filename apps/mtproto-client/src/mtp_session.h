/*
 * mtp_session — auth_key state and MTProto 2.0 message framing.
 *
 * This layer owns the two transfer buffers for the whole client, because it is
 * the one that has to encrypt and decrypt in place. Each buffer carries a
 * 32-byte lead-in ahead of the frame: msg_key is SHA-256 over a slice of
 * auth_key immediately followed by the plaintext, so writing that slice into the
 * lead-in lets the digest run across one contiguous span instead of copying the
 * whole message into a scratch buffer it would not fit in.
 */
#pragma once

#include "mtp_common.h"
#include "mtp_transport.h"

/* One decoded message body, pointing into the session's own RX buffer — valid
   only until the next receive. */
typedef struct {
    uint64_t       msg_id;
    uint32_t       seq_no;
    const uint8_t *body;
    size_t         len;
} mtp_msg_t;

/* ---- Key and session state ---------------------------------------------- */

/* Forget the auth_key and everything derived from it. */
void mtp_sess_reset(void);

/*
 * Install a 256-byte auth_key and derive its id (the low 64 bits of its SHA-1).
 * Rejects an all-zero key, which is what a failed handshake leaves behind and
 * would otherwise be used to encrypt real traffic.
 */
mtp_err_t mtp_sess_set_auth_key(const uint8_t key[MTP_AUTH_KEY_BYTES]);

bool            mtp_sess_has_auth_key(void);
const uint8_t  *mtp_sess_auth_key(void);
uint64_t        mtp_sess_auth_key_id(void);

/* The salt the server told us to use. Updated from bad_server_salt and from
   new_session_created; persisted so a reconnect does not need a round trip. */
void     mtp_sess_set_salt(uint64_t salt);
uint64_t mtp_sess_salt(void);

/*
 * Start a new MTProto session (a fresh random session_id and a reset seq_no).
 * Required after a reconnect: reusing a session_id with restarted sequence
 * numbers makes the server reject everything as replayed.
 */
void     mtp_sess_new_session(void);
uint64_t mtp_sess_session_id(void);

/* ---- Building request bodies -------------------------------------------- */

/*
 * The scratch space to serialise a request body into, positioned so that
 * mtp_sess_send can frame it without moving the bytes.
 */
uint8_t *mtp_sess_body_buf(size_t *out_cap);

/* ---- Plaintext messages (handshake only) -------------------------------- */

/*
 * auth_key_id = 0 messages, used for the PQ/DH exchange before a key exists.
 * Unencrypted and unauthenticated, which is why they are confined to the
 * handshake — the DH result is what makes the exchange trustworthy.
 */
mtp_err_t mtp_sess_send_plain(jpp_sdk_context_t *ctx, const uint8_t *body, size_t len);
mtp_err_t mtp_sess_recv_plain(jpp_sdk_context_t *ctx, const uint8_t **out_body,
                              size_t *out_len, uint32_t timeout_ms);

/* ---- Encrypted messages ------------------------------------------------- */

/*
 * Frame, encrypt and send one message. `content_related` distinguishes real
 * queries from acks and pings, which MTProto excludes from the sequence count.
 * The assigned msg_id is returned so the caller can match the response.
 */
mtp_err_t mtp_sess_send(jpp_sdk_context_t *ctx, const uint8_t *body, size_t len,
                        bool content_related, uint64_t *out_msg_id);

/*
 * Receive, decrypt and validate one message.
 *
 * Rejects a mismatched auth_key_id, a msg_key that does not match the decrypted
 * plaintext (the integrity check — without it the IGE ciphertext is malleable),
 * a session_id that is not ours, and an inner length that does not fit the
 * frame. Also latches the server clock from the msg_id on the way through.
 *
 * Returns MTP_ERR_TIMEOUT when nothing was waiting, so a 0 timeout makes this a
 * poll suitable for the update loop.
 */
mtp_err_t mtp_sess_recv(jpp_sdk_context_t *ctx, mtp_msg_t *out, uint32_t timeout_ms);

/*
 * True when the last mtp_sess_recv failed because the DC does not recognise our
 * auth_key (transport error -404). Distinct from a generic protocol error: the
 * only recovery is to discard the stored key and log in again.
 */
bool mtp_sess_auth_key_rejected(void);

/*
 * The transfer buffers' true sizes, including the 32-byte lead-in the msg_key
 * digest runs over (see the layout note at the top of mtp_session.c). Exposed so
 * mtp_mem.c can size the heap block without duplicating the arithmetic.
 */
#define MTP_SESS_TX_BYTES (32u + MTP_TX_MAX)
#define MTP_SESS_RX_BYTES (32u + MTP_RX_MAX)

/* Claim and drop the transfer buffers. Called only by mtp_mem_init/_release. */
mtp_err_t mtp_sess_mem_init(void);
void      mtp_sess_mem_clear(void);
