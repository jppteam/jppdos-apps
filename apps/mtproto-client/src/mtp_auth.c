#include "mtp_auth.h"

#include <string.h>

#include "jpp_crypto_core.h"
#include "mtp_pq.h"
#include "mtp_scratch.h"
#include "mtp_session.h"
#include "mtp_tl.h"

/* ---- Constructor ids ----------------------------------------------------- */

#define ID_REQ_PQ_MULTI          0xbe7e8ef1u
#define ID_RES_PQ                0x05162463u
#define ID_P_Q_INNER_DATA_DC     0xa9f55f95u
#define ID_REQ_DH_PARAMS         0xd712e4beu
#define ID_SERVER_DH_PARAMS_FAIL 0x79cb045du
#define ID_SERVER_DH_PARAMS_OK   0xd0e8075cu
#define ID_SERVER_DH_INNER_DATA  0xb5890dbau
#define ID_CLIENT_DH_INNER_DATA  0x6643b654u
#define ID_SET_CLIENT_DH_PARAMS  0xf5045f1fu
#define ID_DH_GEN_OK             0x3bcbf734u
#define ID_DH_GEN_RETRY          0x46dc1fb9u
#define ID_DH_GEN_FAIL           0xa69dae02u

#define HANDSHAKE_TIMEOUT_MS 15000u

/* RSA_PAD fixed sizes, straight from the spec. */
#define PAD_DATA_MAX  144u   /* p_q_inner_data_dc must fit this */
#define PAD_TOTAL     192u   /* data + random padding */
#define PAD_WITH_HASH 224u   /* reversed padded data + SHA-256 */
#define PAD_BLOCK     256u   /* the RSA input block */

/*
 * SHA-256 of Telegram's long-standing 2048-bit DH prime. That prime was checked
 * offline to be prime with (p-1)/2 also prime; recognising it by hash costs one
 * SHA-256 instead of a multi-second Miller-Rabin run on-device.
 */
static const uint8_t KNOWN_DH_PRIME_SHA256[32] = {
    0x02, 0xF8, 0x5E, 0x76, 0x87, 0xFC, 0x6F, 0x33,
    0xBA, 0x67, 0x82, 0x26, 0xA9, 0x63, 0xB3, 0xC8,
    0xA1, 0x91, 0xB4, 0x7C, 0x89, 0x0C, 0xF3, 0x0D,
    0xEB, 0xE1, 0x7C, 0x1D, 0x62, 0x3B, 0x5A, 0xF1,
};

/*
 * Handshake scratch — ~3.2 KB, mostly 256-byte bignum operands. Far too large
 * for the 12 KB task stack, and borrowed from the shared arena rather than
 * owning static storage: it is dead the moment the handshake returns, and the
 * SRP exchange needs the same bytes later. See mtp_scratch.h.
 */
typedef struct {
    uint8_t nonce[16];
    uint8_t server_nonce[16];
    uint8_t new_nonce[32];
    uint8_t tmp_key[32];
    uint8_t tmp_iv[32];
    uint8_t dh_prime[MTP_KEY_BYTES];
    uint8_t g_a[MTP_KEY_BYTES];
    uint8_t b[MTP_KEY_BYTES];
    uint8_t g_b[MTP_KEY_BYTES];
    uint8_t auth_key[MTP_AUTH_KEY_BYTES];
    /* RSA_PAD working set */
    uint8_t padded[PAD_TOTAL];
    uint8_t reversed[PAD_TOTAL];
    uint8_t with_hash[PAD_WITH_HASH];
    uint8_t block[PAD_BLOCK];
    uint8_t encrypted[PAD_BLOCK];
    /* client_DH_inner_data and its SHA1-prefixed, padded form. Here rather than
       static in step_set_client_dh for the same reason as everything else in
       this struct: they are dead once the handshake returns. */
    uint8_t inner[MTP_KEY_BYTES + 64];
    uint8_t padded_dh[MTP_KEY_BYTES + 96];
    uint32_t g;
    size_t   dh_prime_len;
    size_t   g_a_len;
} auth_scratch_t;

_Static_assert(sizeof(auth_scratch_t) <= MTP_SCRATCH_BYTES,
               "handshake scratch outgrew the shared arena");

/* Points into the arena for the duration of a handshake. */
static auth_scratch_t *sp;
#define s (*sp)

static void report(mtp_auth_progress_fn fn, void *user, const char *step, int pct)
{
    if (fn != NULL) {
        fn(user, step, pct);
    }
}

/* ---- Big-endian helpers -------------------------------------------------- */

/* Compare two same-length big-endian magnitudes. */
static int be_cmp(const uint8_t *a, const uint8_t *b, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/*
 * Is `v` strictly inside (2^k, p - 2^k) where k = bits(p) - 64?
 *
 * Telegram requires this of both g_a and g_b. Rejecting values near the ends of
 * the range is what stops a server from steering the shared secret into a small
 * subgroup — a g_a of 1 or p-1 would make auth_key trivially predictable.
 */
static bool dh_operand_in_range(const uint8_t *v, const uint8_t *p, size_t len)
{
    /* v > 1: the leading 8 bytes must not all be zero, which for a 2048-bit
       modulus is exactly "at least 2^(len*8-64)". */
    bool low = true;
    for (size_t i = 0u; i < 8u && i < len; i++) {
        if (v[i] != 0u) {
            low = false;
            break;
        }
    }
    if (low) {
        return false;
    }
    /* v < p - 2^k: equivalently the top 8 bytes of (p - v) must be nonzero.
       Computed as a borrow-propagating subtract, most significant byte last. */
    uint8_t diff[MTP_KEY_BYTES];
    int borrow = 0;
    for (size_t i = len; i-- > 0u;) {
        int d = (int)p[i] - (int)v[i] - borrow;
        if (d < 0) {
            d += 256;
            borrow = 1;
        } else {
            borrow = 0;
        }
        diff[i] = (uint8_t)d;
    }
    if (borrow != 0) {
        return false;   /* v > p */
    }
    for (size_t i = 0u; i < 8u && i < len; i++) {
        if (diff[i] != 0u) {
            return true;
        }
    }
    return false;
}

/* ---- Temporary AES key for the DH answer -------------------------------- */

/*
 * tmp_aes_key = SHA1(new_nonce+server_nonce) || SHA1(server_nonce+new_nonce)[0:12]
 * tmp_aes_iv  = SHA1(server_nonce+new_nonce)[12:20] || SHA1(new_nonce+new_nonce)
 *               || new_nonce[0:4]
 */
static mtp_err_t derive_tmp_aes(void)
{
    uint8_t cat[64];
    uint8_t h_ns[20], h_sn[20], h_nn[20];

    memcpy(cat, s.new_nonce, 32u);
    memcpy(cat + 32, s.server_nonce, 16u);
    if (jpp_crypto_sha1(cat, 48u, h_ns) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    memcpy(cat, s.server_nonce, 16u);
    memcpy(cat + 16, s.new_nonce, 32u);
    if (jpp_crypto_sha1(cat, 48u, h_sn) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    memcpy(cat, s.new_nonce, 32u);
    memcpy(cat + 32, s.new_nonce, 32u);
    if (jpp_crypto_sha1(cat, 64u, h_nn) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    memcpy(s.tmp_key,      h_ns,      20u);
    memcpy(s.tmp_key + 20, h_sn,      12u);
    memcpy(s.tmp_iv,       h_sn + 12, 8u);
    memcpy(s.tmp_iv + 8,   h_nn,      20u);
    memcpy(s.tmp_iv + 28,  s.new_nonce, 4u);
    return MTP_OK;
}

/* ---- RSA_PAD ------------------------------------------------------------- */

/*
 * Wrap `data` for req_DH_params and encrypt it to the server's public key.
 * See the header for why the older SHA1 padding is not an option.
 */
static mtp_err_t rsa_pad_encrypt(const uint8_t *data, size_t data_len,
                                 const mtp_rsa_key_t *key,
                                 uint8_t *out, size_t *out_len)
{
    if (data_len > PAD_DATA_MAX) {
        return MTP_ERR_OVERFLOW;
    }

    memcpy(s.padded, data, data_len);
    randombytes_buf(s.padded + data_len, PAD_TOTAL - data_len);

    for (size_t i = 0u; i < PAD_TOTAL; i++) {
        s.reversed[i] = s.padded[PAD_TOTAL - 1u - i];
    }

    /*
     * Retry until the block is numerically below the modulus. RSA is only
     * defined on values in [0, n), and the block's leading byte comes from a
     * hash, so a small fraction of attempts land too high.
     */
    for (int attempt = 0; attempt < 64; attempt++) {
        uint8_t temp_key[32];
        randombytes_buf(temp_key, sizeof(temp_key));

        /* with_hash = reversed(padded) || SHA256(temp_key || padded) */
        uint8_t hash_src[32 + PAD_TOTAL];
        memcpy(hash_src, temp_key, 32u);
        memcpy(hash_src + 32, s.padded, PAD_TOTAL);
        memcpy(s.with_hash, s.reversed, PAD_TOTAL);
        if (jpp_crypto_sha256(hash_src, sizeof(hash_src),
                              s.with_hash + PAD_TOTAL) != JPP_CRYPTO_OK) {
            return MTP_ERR_CRYPTO;
        }

        /* AES-256-IGE with an all-zero IV, as the scheme specifies. */
        uint8_t zero_iv[32] = { 0 };
        uint8_t aes_enc[PAD_WITH_HASH];
        if (jpp_crypto_aes256_ige_encrypt(s.with_hash, PAD_WITH_HASH,
                                          temp_key, zero_iv, aes_enc) != JPP_CRYPTO_OK) {
            return MTP_ERR_CRYPTO;
        }

        /* block = (temp_key XOR SHA256(aes_enc)) || aes_enc */
        uint8_t enc_hash[32];
        if (jpp_crypto_sha256(aes_enc, PAD_WITH_HASH, enc_hash) != JPP_CRYPTO_OK) {
            return MTP_ERR_CRYPTO;
        }
        for (size_t i = 0u; i < 32u; i++) {
            s.block[i] = (uint8_t)(temp_key[i] ^ enc_hash[i]);
        }
        memcpy(s.block + 32, aes_enc, PAD_WITH_HASH);

        /* The comparison needs equal widths; a modulus shorter than the block
           would mean a key smaller than 2048 bits, which we do not support. */
        if (key->modulus_len != PAD_BLOCK) {
            return MTP_ERR_ARG;
        }
        if (be_cmp(s.block, key->modulus, PAD_BLOCK) >= 0) {
            continue;
        }

        size_t enc_len = 0u;
        if (jpp_crypto_rsa_encrypt(s.block, PAD_BLOCK,
                                   key->modulus, key->modulus_len,
                                   key->exponent, key->exponent_len,
                                   out, &enc_len) != JPP_CRYPTO_OK) {
            return MTP_ERR_CRYPTO;
        }
        *out_len = enc_len;
        return MTP_OK;
    }
    /* 64 consecutive over-modulus blocks is not statistically possible; treat it
       as a broken key rather than looping forever. */
    return MTP_ERR_CRYPTO;
}

/* ---- Step 1: req_pq_multi ----------------------------------------------- */

static mtp_err_t step_req_pq(jpp_sdk_context_t *ctx, const mtp_profile_t *profile,
                             uint64_t *out_pq, const mtp_rsa_key_t **out_key)
{
    uint8_t body[32];
    mtp_w_t w;

    randombytes_buf(s.nonce, sizeof(s.nonce));
    mtp_w_init(&w, body, sizeof(body));
    mtp_w_u32(&w, ID_REQ_PQ_MULTI);
    mtp_w_raw(&w, s.nonce, sizeof(s.nonce));
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    mtp_err_t err = mtp_sess_send_plain(ctx, body, w.len);
    if (err != MTP_OK) {
        return err;
    }

    const uint8_t *resp;
    size_t resp_len;
    err = mtp_sess_recv_plain(ctx, &resp, &resp_len, HANDSHAKE_TIMEOUT_MS);
    if (err != MTP_OK) {
        return err;
    }

    mtp_r_t r;
    mtp_r_init(&r, resp, resp_len);
    if (mtp_r_u32(&r) != ID_RES_PQ) {
        return MTP_ERR_PROTO;
    }
    uint8_t echo[16];
    mtp_r_raw(&r, echo, sizeof(echo));
    /* The nonce echo is the only thing tying this reply to our request. */
    if (!mtp_r_ok(&r) || memcmp(echo, s.nonce, sizeof(echo)) != 0) {
        return MTP_ERR_PROTO;
    }
    mtp_r_raw(&r, s.server_nonce, sizeof(s.server_nonce));

    size_t pq_len = 0u;
    const uint8_t *pq = mtp_r_bytes(&r, &pq_len);
    if (pq == NULL || pq_len == 0u || pq_len > 8u) {
        return MTP_ERR_PROTO;
    }
    /* pq is a big-endian integer of up to 8 bytes. */
    uint64_t pq_val = 0u;
    for (size_t i = 0u; i < pq_len; i++) {
        pq_val = (pq_val << 8) | (uint64_t)pq[i];
    }

    uint32_t count = mtp_r_vector(&r);
    if (!mtp_r_ok(&r) || count == 0u) {
        return MTP_ERR_PROTO;
    }
    /* Read the fingerprints the server will accept and pick one we hold. The
       list is short in practice; cap it so a bogus count cannot spin. */
    uint64_t fps[16];
    size_t n = count < 16u ? count : 16u;
    for (size_t i = 0u; i < n; i++) {
        fps[i] = mtp_r_u64(&r);
    }
    if (!mtp_r_ok(&r)) {
        return MTP_ERR_PROTO;
    }

    const mtp_rsa_key_t *key = mtp_config_match_key(profile, fps, n);
    if (key == NULL) {
        /* This build has no key the DC trusts — a configuration problem, not a
           transient failure, so say so distinctly. */
        return MTP_ERR_NO_CONFIG;
    }

    *out_pq = pq_val;
    *out_key = key;
    return MTP_OK;
}

/* ---- Step 2: req_DH_params ---------------------------------------------- */

static mtp_err_t step_req_dh(jpp_sdk_context_t *ctx, int dc_id, uint64_t pq_val,
                             const mtp_rsa_key_t *key,
                             const uint8_t **out_answer, size_t *out_answer_len)
{
    uint32_t p32, q32;
    if (!mtp_pq_factor(pq_val, &p32, &q32)) {
        return MTP_ERR_PROTO;
    }

    /* p, q and pq travel as big-endian byte strings with no leading zeros. */
    uint8_t pq_be[8], p_be[4], q_be[4];
    mtp_wr_u64be(pq_be, pq_val);
    for (size_t i = 0u; i < 4u; i++) {
        p_be[3u - i] = (uint8_t)(p32 >> (i * 8u));
        q_be[3u - i] = (uint8_t)(q32 >> (i * 8u));
    }
    const uint8_t *pqp; size_t pql;
    const uint8_t *pp;  size_t pl;
    const uint8_t *qp;  size_t ql;
    mtp_be_trim(pq_be, sizeof(pq_be), &pqp, &pql);
    mtp_be_trim(p_be, sizeof(p_be), &pp, &pl);
    mtp_be_trim(q_be, sizeof(q_be), &qp, &ql);

    randombytes_buf(s.new_nonce, sizeof(s.new_nonce));

    /* p_q_inner_data_dc — the _dc variant carries the datacentre number, which
       lets the server detect a client that was redirected mid-handshake. */
    uint8_t inner[PAD_DATA_MAX];
    mtp_w_t w;
    mtp_w_init(&w, inner, sizeof(inner));
    mtp_w_u32(&w, ID_P_Q_INNER_DATA_DC);
    mtp_w_bytes(&w, pqp, pql);
    mtp_w_bytes(&w, pp, pl);
    mtp_w_bytes(&w, qp, ql);
    mtp_w_raw(&w, s.nonce, 16u);
    mtp_w_raw(&w, s.server_nonce, 16u);
    mtp_w_raw(&w, s.new_nonce, 32u);
    mtp_w_i32(&w, dc_id);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    size_t enc_len = 0u;
    mtp_err_t err = rsa_pad_encrypt(inner, w.len, key, s.encrypted, &enc_len);
    if (err != MTP_OK) {
        return err;
    }

    size_t body_cap;
    uint8_t *body = mtp_sess_body_buf(&body_cap);
    mtp_w_init(&w, body, body_cap);
    mtp_w_u32(&w, ID_REQ_DH_PARAMS);
    mtp_w_raw(&w, s.nonce, 16u);
    mtp_w_raw(&w, s.server_nonce, 16u);
    mtp_w_bytes(&w, pp, pl);
    mtp_w_bytes(&w, qp, ql);
    mtp_w_u64(&w, key->fingerprint);
    mtp_w_bytes(&w, s.encrypted, enc_len);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    err = mtp_sess_send_plain(ctx, body, w.len);
    if (err != MTP_OK) {
        return err;
    }

    const uint8_t *resp;
    size_t resp_len;
    err = mtp_sess_recv_plain(ctx, &resp, &resp_len, HANDSHAKE_TIMEOUT_MS);
    if (err != MTP_OK) {
        return err;
    }

    mtp_r_t r;
    mtp_r_init(&r, resp, resp_len);
    uint32_t id = mtp_r_u32(&r);
    if (id == ID_SERVER_DH_PARAMS_FAIL) {
        return MTP_ERR_AUTH_KEY;
    }
    if (id != ID_SERVER_DH_PARAMS_OK) {
        return MTP_ERR_PROTO;
    }
    uint8_t echo_n[16], echo_sn[16];
    mtp_r_raw(&r, echo_n, sizeof(echo_n));
    mtp_r_raw(&r, echo_sn, sizeof(echo_sn));
    if (!mtp_r_ok(&r) || memcmp(echo_n, s.nonce, 16u) != 0 ||
        memcmp(echo_sn, s.server_nonce, 16u) != 0) {
        return MTP_ERR_PROTO;
    }

    size_t ans_len = 0u;
    const uint8_t *answer = mtp_r_bytes(&r, &ans_len);
    if (answer == NULL || ans_len < 16u || (ans_len & 15u) != 0u) {
        return MTP_ERR_PROTO;
    }
    *out_answer = answer;
    *out_answer_len = ans_len;
    return MTP_OK;
}

/* ---- Step 3: decrypt and validate the DH parameters -------------------- */

static mtp_err_t parse_dh_inner(uint8_t *answer, size_t ans_len, bool *out_verified)
{
    mtp_err_t err = derive_tmp_aes();
    if (err != MTP_OK) {
        return err;
    }
    if (jpp_crypto_aes256_ige_decrypt(answer, ans_len, s.tmp_key, s.tmp_iv,
                                      answer) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /* Layout is SHA1(inner) || inner || padding. The inner length is not sent,
       so it is recovered from how far the TL reader advanced. */
    mtp_r_t r;
    mtp_r_init(&r, answer + 20, ans_len - 20u);
    if (mtp_r_u32(&r) != ID_SERVER_DH_INNER_DATA) {
        return MTP_ERR_PROTO;
    }
    uint8_t echo_n[16], echo_sn[16];
    mtp_r_raw(&r, echo_n, sizeof(echo_n));
    mtp_r_raw(&r, echo_sn, sizeof(echo_sn));
    if (!mtp_r_ok(&r) || memcmp(echo_n, s.nonce, 16u) != 0 ||
        memcmp(echo_sn, s.server_nonce, 16u) != 0) {
        return MTP_ERR_PROTO;
    }

    s.g = (uint32_t)mtp_r_i32(&r);

    size_t prime_len = 0u;
    const uint8_t *prime = mtp_r_bytes(&r, &prime_len);
    size_t ga_len = 0u;
    const uint8_t *ga = mtp_r_bytes(&r, &ga_len);
    (void)mtp_r_i32(&r);   /* server_time — the clock is latched from msg_id */
    if (!mtp_r_ok(&r) || prime == NULL || ga == NULL) {
        return MTP_ERR_PROTO;
    }

    /* Now the hash check, over exactly the bytes the reader consumed. */
    uint8_t digest[JPP_CRYPTO_SHA1_BYTES];
    if (jpp_crypto_sha1(answer + 20, r.pos, digest) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    if (!mtp_ct_eq(digest, answer, sizeof(digest))) {
        return MTP_ERR_PROTO;
    }

    /*
     * Parameter validation. Everything below is what keeps a hostile server from
     * choosing the shared secret for us.
     */
    if (prime_len != MTP_KEY_BYTES) {
        return MTP_ERR_AUTH_KEY;          /* must be exactly 2048 bits */
    }
    if ((prime[0] & 0x80u) == 0u) {
        return MTP_ERR_AUTH_KEY;          /* top bit set, i.e. really 2048 bits */
    }
    if ((prime[prime_len - 1u] & 1u) == 0u) {
        return MTP_ERR_AUTH_KEY;          /* a prime this size cannot be even */
    }
    /* g is a small generator; the spec permits only these. */
    if (s.g < 2u || s.g > 7u) {
        return MTP_ERR_AUTH_KEY;
    }
    if (ga_len == 0u || ga_len > MTP_KEY_BYTES) {
        return MTP_ERR_AUTH_KEY;
    }

    memcpy(s.dh_prime, prime, prime_len);
    s.dh_prime_len = prime_len;
    /* Left-pad g_a to the modulus width so the range check and modexp both see
       equal-width operands. */
    memset(s.g_a, 0, sizeof(s.g_a));
    memcpy(s.g_a + (MTP_KEY_BYTES - ga_len), ga, ga_len);
    s.g_a_len = MTP_KEY_BYTES;

    if (!dh_operand_in_range(s.g_a, s.dh_prime, MTP_KEY_BYTES)) {
        return MTP_ERR_AUTH_KEY;
    }

    /* Recognise the offline-verified safe prime. A match means the parameters
       are known-good; a miss leaves only the structural checks above. */
    uint8_t prime_hash[JPP_CRYPTO_SHA256_BYTES];
    if (jpp_crypto_sha256(s.dh_prime, s.dh_prime_len, prime_hash) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    *out_verified = mtp_ct_eq(prime_hash, KNOWN_DH_PRIME_SHA256, sizeof(prime_hash));
    return MTP_OK;
}

/* ---- Step 4: compute the key and confirm ------------------------------- */

static mtp_err_t step_set_client_dh(jpp_sdk_context_t *ctx,
                                    mtp_auth_progress_fn progress, void *user)
{
    /* b is the client's secret exponent; 2048 random bits. */
    randombytes_buf(s.b, sizeof(s.b));

    uint8_t g_be[4] = {
        (uint8_t)(s.g >> 24), (uint8_t)(s.g >> 16),
        (uint8_t)(s.g >> 8),  (uint8_t)s.g,
    };
    const uint8_t *gp; size_t gl;
    mtp_be_trim(g_be, sizeof(g_be), &gp, &gl);

    report(progress, user, "Exchanging keys", 55);

    /* g_b = g^b mod p — the value the server needs. */
    size_t out_len = 0u;
    if (jpp_crypto_dh_compute(gp, gl, s.b, sizeof(s.b),
                              s.dh_prime, s.dh_prime_len,
                              s.g_b, &out_len) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    if (out_len != MTP_KEY_BYTES || !dh_operand_in_range(s.g_b, s.dh_prime, MTP_KEY_BYTES)) {
        return MTP_ERR_AUTH_KEY;
    }

    report(progress, user, "Exchanging keys", 70);

    /* auth_key = g_a^b mod p — the shared secret. */
    if (jpp_crypto_dh_compute(s.g_a, s.g_a_len, s.b, sizeof(s.b),
                              s.dh_prime, s.dh_prime_len,
                              s.auth_key, &out_len) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    if (out_len != MTP_AUTH_KEY_BYTES) {
        return MTP_ERR_AUTH_KEY;
    }

    report(progress, user, "Finishing", 85);

    /* client_DH_inner_data, then SHA1 || data || random padding to a multiple
       of 16, encrypted under the same temporary key. */
    const uint8_t *gbp; size_t gbl;
    mtp_be_trim(s.g_b, MTP_KEY_BYTES, &gbp, &gbl);

    mtp_w_t w;
    mtp_w_init(&w, s.inner, sizeof(s.inner));
    mtp_w_u32(&w, ID_CLIENT_DH_INNER_DATA);
    mtp_w_raw(&w, s.nonce, 16u);
    mtp_w_raw(&w, s.server_nonce, 16u);
    mtp_w_u64(&w, 0u);            /* retry_id: 0 on the first attempt */
    mtp_w_bytes(&w, gbp, gbl);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    uint8_t *padded = s.padded_dh;
    if (20u + w.len + 16u > sizeof(s.padded_dh)) {
        return MTP_ERR_OVERFLOW;
    }
    if (jpp_crypto_sha1(s.inner, w.len, padded) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    memcpy(padded + 20, s.inner, w.len);
    size_t total = 20u + w.len;
    size_t pad = (16u - (total % 16u)) % 16u;
    randombytes_buf(padded + total, pad);
    total += pad;

    if (jpp_crypto_aes256_ige_encrypt(padded, total, s.tmp_key, s.tmp_iv,
                                      padded) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    size_t body_cap;
    uint8_t *body = mtp_sess_body_buf(&body_cap);
    mtp_w_init(&w, body, body_cap);
    mtp_w_u32(&w, ID_SET_CLIENT_DH_PARAMS);
    mtp_w_raw(&w, s.nonce, 16u);
    mtp_w_raw(&w, s.server_nonce, 16u);
    mtp_w_bytes(&w, padded, total);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    mtp_err_t err = mtp_sess_send_plain(ctx, body, w.len);
    if (err != MTP_OK) {
        return err;
    }

    const uint8_t *resp;
    size_t resp_len;
    err = mtp_sess_recv_plain(ctx, &resp, &resp_len, HANDSHAKE_TIMEOUT_MS);
    if (err != MTP_OK) {
        return err;
    }

    mtp_r_t r;
    mtp_r_init(&r, resp, resp_len);
    uint32_t id = mtp_r_u32(&r);
    if (id == ID_DH_GEN_RETRY || id == ID_DH_GEN_FAIL) {
        return MTP_ERR_AUTH_KEY;
    }
    if (id != ID_DH_GEN_OK) {
        return MTP_ERR_PROTO;
    }
    uint8_t echo_n[16], echo_sn[16], hash1[16];
    mtp_r_raw(&r, echo_n, sizeof(echo_n));
    mtp_r_raw(&r, echo_sn, sizeof(echo_sn));
    mtp_r_raw(&r, hash1, sizeof(hash1));
    if (!mtp_r_ok(&r) || memcmp(echo_n, s.nonce, 16u) != 0 ||
        memcmp(echo_sn, s.server_nonce, 16u) != 0) {
        return MTP_ERR_PROTO;
    }

    /*
     * Confirm the server derived the same key:
     *   new_nonce_hash1 = SHA1(new_nonce || 1 || SHA1(auth_key)[0:8])[4:20]
     * Without this the client would happily adopt a key the server never had.
     */
    uint8_t ak_hash[JPP_CRYPTO_SHA1_BYTES];
    if (jpp_crypto_sha1(s.auth_key, MTP_AUTH_KEY_BYTES, ak_hash) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    uint8_t cat[32 + 1 + 8];
    memcpy(cat, s.new_nonce, 32u);
    cat[32] = 1u;
    memcpy(cat + 33, ak_hash, 8u);
    uint8_t expect[JPP_CRYPTO_SHA1_BYTES];
    if (jpp_crypto_sha1(cat, sizeof(cat), expect) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    if (!mtp_ct_eq(expect + 4, hash1, 16u)) {
        return MTP_ERR_AUTH_KEY;
    }
    return MTP_OK;
}

/* ---- Entry point -------------------------------------------------------- */

/*
 * The exchange proper. Wrapped by mtp_auth_handshake so that the arena is
 * released on every path out, of which there are a dozen.
 */
static mtp_err_t handshake_body(jpp_sdk_context_t *ctx,
                                const mtp_profile_t *profile,
                                int dc_id,
                                mtp_auth_progress_fn progress,
                                void *progress_user,
                                mtp_auth_result_t *out)
{
    report(progress, progress_user, "Connecting", 5);

    uint64_t pq_val = 0u;
    const mtp_rsa_key_t *key = NULL;
    mtp_err_t err = step_req_pq(ctx, profile, &pq_val, &key);
    if (err != MTP_OK) {
        return err;
    }

    report(progress, progress_user, "Verifying server", 25);

    const uint8_t *answer = NULL;
    size_t ans_len = 0u;
    err = step_req_dh(ctx, dc_id, pq_val, key, &answer, &ans_len);
    if (err != MTP_OK) {
        return err;
    }

    report(progress, progress_user, "Checking parameters", 40);

    /*
     * parse_dh_inner decrypts in place. The buffer belongs to the session's RX
     * area and is not needed again, so casting away const is safe here and
     * avoids another 600-byte copy.
     */
    bool verified = false;
    err = parse_dh_inner((uint8_t *)answer, ans_len, &verified);
    if (err != MTP_OK) {
        return err;
    }

    err = step_set_client_dh(ctx, progress, progress_user);
    if (err != MTP_OK) {
        return err;
    }

    /* server_salt = new_nonce[0:8] XOR server_nonce[0:8] */
    uint8_t salt_bytes[8];
    for (size_t i = 0u; i < 8u; i++) {
        salt_bytes[i] = (uint8_t)(s.new_nonce[i] ^ s.server_nonce[i]);
    }

    memcpy(out->auth_key, s.auth_key, MTP_AUTH_KEY_BYTES);
    out->server_salt = mtp_rd_u64le(salt_bytes);
    out->dh_prime_verified = verified;

    report(progress, progress_user, "Connected", 100);
    return MTP_OK;
}

mtp_err_t mtp_auth_handshake(jpp_sdk_context_t *ctx,
                             const mtp_profile_t *profile,
                             int dc_id,
                             mtp_auth_progress_fn progress,
                             void *progress_user,
                             mtp_auth_result_t *out)
{
    memset(out, 0, sizeof(*out));

    sp = mtp_scratch_acquire(sizeof(auth_scratch_t));
    if (sp == NULL) {
        return MTP_ERR_CRYPTO;
    }
    mtp_err_t err = handshake_body(ctx, profile, dc_id, progress, progress_user, out);
    /* Releasing zeroes the arena, which is what clears the secret exponent and
       the derived key; the caller has its own copy of what it needs. */
    mtp_scratch_release(sp);
    sp = NULL;
    return err;
}
