#include "mtp_session.h"

#include <string.h>

#include "jpp_crypto_core.h"
#include "mtp_mem.h"
#include "mtp_time.h"
#include "mtp_tl.h"

/*
 * Buffer layout. The frame is what goes on the wire; the 32-byte lead-in in
 * front of it exists only so the msg_key digest can be taken over one span:
 *
 *   [ 0 .. 32 )        KDF prefix — auth_key slice, written just before hashing
 *   [ 8 .. 32 )        frame header: auth_key_id (8) + msg_key (16)
 *   [ 32 .. 32+n )     plaintext / ciphertext
 *
 * The two regions overlap deliberately. msg_key is computed first (using
 * [0,32) as the prefix), and only then is the frame header written over
 * [8,32) — by which point the prefix has served its purpose.
 */
#define SESS_LEAD  32u
#define FRAME_OFF  (SESS_LEAD - 24u)   /* = 8 */

_Static_assert(MTP_SESS_TX_BYTES == SESS_LEAD + MTP_TX_MAX, "TX size mismatch");
_Static_assert(MTP_SESS_RX_BYTES == SESS_LEAD + MTP_RX_MAX, "RX size mismatch");

/* On the heap, not in the app pool — see mtp_mem.h. */
static uint8_t *s_tx;
static uint8_t *s_rx;

mtp_err_t mtp_sess_mem_init(void)
{
    s_tx = mtp_mem_take(MTP_SESS_TX_BYTES);
    s_rx = mtp_mem_take(MTP_SESS_RX_BYTES);
    return (s_tx != NULL && s_rx != NULL) ? MTP_OK : MTP_ERR_OVERFLOW;
}

void mtp_sess_mem_clear(void)
{
    s_tx = NULL;
    s_rx = NULL;
}

static uint8_t  s_auth_key[MTP_AUTH_KEY_BYTES];
static uint64_t s_auth_key_id;
static bool     s_have_key;
static uint64_t s_salt;
static uint64_t s_session_id;
static bool     s_key_rejected;

/* ---- Key and session state ---------------------------------------------- */

void mtp_sess_reset(void)
{
    memset(s_auth_key, 0, sizeof(s_auth_key));
    s_auth_key_id = 0u;
    s_have_key = false;
    s_salt = 0u;
    s_session_id = 0u;
    s_key_rejected = false;
}

mtp_err_t mtp_sess_set_auth_key(const uint8_t key[MTP_AUTH_KEY_BYTES])
{
    /* A zeroed key is the signature of a handshake that failed somewhere without
       reporting it. Refusing it here keeps that bug from turning into traffic
       encrypted under a known key. */
    bool all_zero = true;
    for (size_t i = 0u; i < MTP_AUTH_KEY_BYTES; i++) {
        if (key[i] != 0u) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return MTP_ERR_AUTH_KEY;
    }

    uint8_t digest[JPP_CRYPTO_SHA1_BYTES];
    if (jpp_crypto_sha1(key, MTP_AUTH_KEY_BYTES, digest) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    memcpy(s_auth_key, key, MTP_AUTH_KEY_BYTES);
    /* auth_key_id is the *low* 64 bits of the digest, i.e. its last 8 bytes,
       read little-endian like every other MTProto long. */
    s_auth_key_id = mtp_rd_u64le(digest + 12);
    s_have_key = true;
    s_key_rejected = false;
    return MTP_OK;
}

bool           mtp_sess_has_auth_key(void) { return s_have_key; }
const uint8_t *mtp_sess_auth_key(void)     { return s_auth_key; }
uint64_t       mtp_sess_auth_key_id(void)  { return s_auth_key_id; }
void           mtp_sess_set_salt(uint64_t salt) { s_salt = salt; }
uint64_t       mtp_sess_salt(void)         { return s_salt; }
uint64_t       mtp_sess_session_id(void)   { return s_session_id; }

void mtp_sess_new_session(void)
{
    randombytes_buf(&s_session_id, sizeof(s_session_id));
    mtp_time_reset_session();
}

uint8_t *mtp_sess_body_buf(size_t *out_cap)
{
    /*
     * Bodies are built where the plaintext will live, minus the room the inner
     * envelope (salt, session, msg_id, seq_no, length = 32 bytes) and the random
     * padding will need. Handing out a smaller capacity here means an oversized
     * request is caught by the TL writer while building, not after.
     *
     * The padding reserve is 32, not the 1024 the spec permits: mtp_w_pad_random
     * is called with min_pad 12 rounded to a 16-byte boundary, so it emits at
     * most 27 bytes. Reserving the spec's ceiling for padding this client never
     * generates would cost a kilobyte of the pool and hand callers a *smaller*
     * body capacity than the buffer actually has.
     */
    *out_cap = MTP_TX_MAX - 32u - 32u;
    return s_tx + SESS_LEAD + 32u;
}

/* ---- MTProto 2.0 key derivation ----------------------------------------- */

/*
 * msg_key = middle 16 bytes of SHA256(auth_key[88+x : +32] || plaintext), where
 * x is 0 for messages we send and 8 for messages we receive. The prefix is
 * written into the buffer's lead-in so the hash covers one contiguous range.
 */
static mtp_err_t compute_msg_key(uint8_t *buf, size_t plain_off, size_t plain_len,
                                 bool from_client, uint8_t out_msg_key[16])
{
    size_t x = from_client ? 0u : 8u;
    memcpy(buf + plain_off - 32u, s_auth_key + 88u + x, 32u);

    uint8_t large[JPP_CRYPTO_SHA256_BYTES];
    if (jpp_crypto_sha256(buf + plain_off - 32u, 32u + plain_len, large) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    memcpy(out_msg_key, large + 8, 16u);
    return MTP_OK;
}

/*
 * The AES key and IV, interleaved from two SHA-256 digests exactly as the spec
 * lays out. The interleaving is not decorative: it is what stops a msg_key from
 * being reusable across the two directions.
 */
static mtp_err_t derive_aes(const uint8_t msg_key[16], bool from_client,
                            uint8_t key[32], uint8_t iv[32])
{
    size_t x = from_client ? 0u : 8u;
    uint8_t buf[52];
    uint8_t sha_a[JPP_CRYPTO_SHA256_BYTES];
    uint8_t sha_b[JPP_CRYPTO_SHA256_BYTES];

    /* sha_a = SHA256(msg_key || auth_key[x : x+36]) */
    memcpy(buf, msg_key, 16u);
    memcpy(buf + 16, s_auth_key + x, 36u);
    if (jpp_crypto_sha256(buf, sizeof(buf), sha_a) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    /* sha_b = SHA256(auth_key[40+x : +36] || msg_key) */
    memcpy(buf, s_auth_key + 40u + x, 36u);
    memcpy(buf + 36, msg_key, 16u);
    if (jpp_crypto_sha256(buf, sizeof(buf), sha_b) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    memcpy(key,      sha_a,      8u);
    memcpy(key + 8,  sha_b + 8,  16u);
    memcpy(key + 24, sha_a + 24, 8u);
    memcpy(iv,       sha_b,      8u);
    memcpy(iv + 8,   sha_a + 8,  16u);
    memcpy(iv + 24,  sha_b + 24, 8u);
    return MTP_OK;
}

/* ---- Plaintext messages ------------------------------------------------- */

mtp_err_t mtp_sess_send_plain(jpp_sdk_context_t *ctx, const uint8_t *body, size_t len)
{
    /* Envelope is auth_key_id(0) + msg_id + length, so the body sits 20 bytes
       into the frame. Build the whole thing in the TX buffer's frame area. */
    uint8_t *frame = s_tx + FRAME_OFF;
    size_t   cap   = MTP_SESS_TX_BYTES - FRAME_OFF;

    mtp_w_t w;
    mtp_w_init(&w, frame, cap);
    mtp_w_u64(&w, 0u);                    /* auth_key_id = 0 → unencrypted */
    mtp_w_u64(&w, mtp_time_msg_id());
    mtp_w_u32(&w, (uint32_t)len);
    if (!mtp_w_ok(&w) || w.len + len > cap) {
        return MTP_ERR_OVERFLOW;
    }
    /*
     * memmove, and deliberately not the writer's mtp_w_raw. The handshake builds
     * its bodies in mtp_sess_body_buf(), which is further into this same buffer
     * than the plaintext envelope ends — so this copy moves data backwards over
     * itself, which memcpy is not allowed to do.
     */
    memmove(frame + w.len, body, len);
    w.len += len;

    return mtp_tp_send(ctx, frame, w.len);
}

mtp_err_t mtp_sess_recv_plain(jpp_sdk_context_t *ctx, const uint8_t **out_body,
                              size_t *out_len, uint32_t timeout_ms)
{
    *out_body = NULL;
    *out_len = 0u;

    uint8_t *frame = s_rx + FRAME_OFF;
    size_t   got   = 0u;
    mtp_err_t err = mtp_tp_recv(ctx, frame, MTP_SESS_RX_BYTES - FRAME_OFF, &got, timeout_ms);
    if (err != MTP_OK) {
        return err;
    }

    int32_t tp_code;
    if (mtp_tp_frame_is_error(frame, got, &tp_code)) {
        return MTP_ERR_PROTO;
    }
    if (got < 20u) {
        return MTP_ERR_PROTO;
    }
    /* A plaintext reply must itself be plaintext; a nonzero auth_key_id here
       means we are not where we think we are in the handshake. */
    if (mtp_rd_u64le(frame) != 0u) {
        return MTP_ERR_PROTO;
    }

    uint64_t msg_id = mtp_rd_u64le(frame + 8);
    uint32_t len    = mtp_rd_u32le(frame + 16);
    if ((size_t)len + 20u > got) {
        return MTP_ERR_PROTO;
    }
    mtp_time_latch(msg_id);

    *out_body = frame + 20;
    *out_len = len;
    return MTP_OK;
}

/* ---- Encrypted messages ------------------------------------------------- */

mtp_err_t mtp_sess_send(jpp_sdk_context_t *ctx, const uint8_t *body, size_t len,
                        bool content_related, uint64_t *out_msg_id)
{
    if (!s_have_key) {
        return MTP_ERR_AUTH_KEY;
    }

    uint8_t *plain = s_tx + SESS_LEAD;
    size_t   cap   = MTP_SESS_TX_BYTES - SESS_LEAD;
    uint64_t msg_id = mtp_time_msg_id();

    mtp_w_t w;
    mtp_w_init(&w, plain, cap);
    mtp_w_u64(&w, s_salt);
    mtp_w_u64(&w, s_session_id);
    mtp_w_u64(&w, msg_id);
    mtp_w_u32(&w, mtp_time_seq_no(content_related));
    mtp_w_u32(&w, (uint32_t)len);
    /*
     * memmove, not memcpy: callers build the body in mtp_sess_body_buf(), which
     * is inside this same buffer at exactly the offset the envelope ends at.
     * The regions coincide rather than overlap in the general case, but a
     * shorter-than-expected envelope would make them overlap for real.
     */
    if (w.len + len <= cap) {
        memmove(plain + w.len, body, len);
        w.len += len;
    } else {
        w.ovf = true;
    }
    /* 12..1024 random bytes, rounded so the plaintext is a multiple of 16. */
    mtp_w_pad_random(&w, 16u, 12u);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    uint8_t msg_key[16];
    mtp_err_t err = compute_msg_key(s_tx, SESS_LEAD, w.len, true, msg_key);
    if (err != MTP_OK) {
        return err;
    }

    uint8_t aes_key[32], aes_iv[32];
    err = derive_aes(msg_key, true, aes_key, aes_iv);
    if (err != MTP_OK) {
        return err;
    }
    if (jpp_crypto_aes256_ige_encrypt(plain, w.len, aes_key, aes_iv, plain) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /* Now the header can go in, over the KDF prefix we no longer need. */
    uint8_t *frame = s_tx + FRAME_OFF;
    mtp_wr_u64le(frame, s_auth_key_id);
    memcpy(frame + 8, msg_key, 16u);

    if (out_msg_id != NULL) {
        *out_msg_id = msg_id;
    }
    return mtp_tp_send(ctx, frame, 24u + w.len);
}

mtp_err_t mtp_sess_recv(jpp_sdk_context_t *ctx, mtp_msg_t *out, uint32_t timeout_ms)
{
    memset(out, 0, sizeof(*out));
    if (!s_have_key) {
        return MTP_ERR_AUTH_KEY;
    }

    uint8_t *frame = s_rx + FRAME_OFF;
    size_t   got   = 0u;
    mtp_err_t err = mtp_tp_recv(ctx, frame, MTP_SESS_RX_BYTES - FRAME_OFF, &got, timeout_ms);
    if (err != MTP_OK) {
        return err;
    }

    int32_t tp_code;
    if (mtp_tp_frame_is_error(frame, got, &tp_code)) {
        if (tp_code == MTP_TP_ERR_AUTH_KEY_UNKNOWN) {
            s_key_rejected = true;
            return MTP_ERR_AUTH_KEY;
        }
        return MTP_ERR_PROTO;
    }
    /* 24-byte header plus at least one 16-byte cipher block. */
    if (got < 24u + 16u || ((got - 24u) & 15u) != 0u) {
        return MTP_ERR_PROTO;
    }
    if (mtp_rd_u64le(frame) != s_auth_key_id) {
        return MTP_ERR_PROTO;
    }

    uint8_t msg_key[16];
    memcpy(msg_key, frame + 8, 16u);

    uint8_t aes_key[32], aes_iv[32];
    err = derive_aes(msg_key, false, aes_key, aes_iv);
    if (err != MTP_OK) {
        return err;
    }

    uint8_t *plain = s_rx + SESS_LEAD;
    size_t   plain_len = got - 24u;
    if (jpp_crypto_aes256_ige_decrypt(plain, plain_len, aes_key, aes_iv, plain) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /*
     * Recompute msg_key over the decrypted plaintext and compare. This is the
     * integrity check — AES-IGE on its own is malleable, so skipping it would
     * let anyone flip bits in a message and have it accepted. Constant-time
     * compare so a mismatch does not leak where it diverged.
     */
    uint8_t expect[16];
    err = compute_msg_key(s_rx, SESS_LEAD, plain_len, false, expect);
    if (err != MTP_OK) {
        return err;
    }
    if (!mtp_ct_eq(expect, msg_key, sizeof(expect))) {
        return MTP_ERR_PROTO;
    }

    if (mtp_rd_u64le(plain + 8) != s_session_id) {
        return MTP_ERR_PROTO;
    }

    uint64_t msg_id  = mtp_rd_u64le(plain + 16);
    uint32_t seq_no  = mtp_rd_u32le(plain + 24);
    uint32_t inner   = mtp_rd_u32le(plain + 28);
    /*
     * The declared length must fit inside the padding-inclusive plaintext. The
     * padding is at least 12 bytes, but accepting anything that merely fits is
     * enough to keep the reader inside the buffer.
     */
    if ((size_t)inner + 32u > plain_len) {
        return MTP_ERR_PROTO;
    }
    mtp_time_latch(msg_id);

    out->msg_id = msg_id;
    out->seq_no = seq_no;
    out->body   = plain + 32;
    out->len    = inner;
    return MTP_OK;
}

bool mtp_sess_auth_key_rejected(void) { return s_key_rejected; }
