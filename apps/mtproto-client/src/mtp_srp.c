#include "mtp_srp.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "jpp_crypto_core.h"
#include "mtp_scratch.h"

/* libsodium's SHA-512, the only hash in the symbol table besides jpp_crypto_*. */
extern int crypto_hash_sha512(unsigned char *out, const unsigned char *in,
                              unsigned long long inlen);

#pragma GCC visibility push(hidden)

#define SHA512_BYTES 64u
#define SHA512_BLOCK 128u

/* ---- HMAC-SHA512 --------------------------------------------------------- */

mtp_err_t mtp_hmac_sha512(const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len,
                          uint8_t out[SHA512_BYTES])
{
    uint8_t k[SHA512_BLOCK];
    memset(k, 0, sizeof(k));

    if (key_len > SHA512_BLOCK) {
        if (crypto_hash_sha512(k, key, key_len) != 0) {
            return MTP_ERR_CRYPTO;
        }
    } else {
        memcpy(k, key, key_len);
    }

    /* Inner and outer padded keys, per RFC 2104. */
    uint8_t block[SHA512_BLOCK + SHA512_BYTES];
    for (size_t i = 0u; i < SHA512_BLOCK; i++) {
        block[i] = (uint8_t)(k[i] ^ 0x36u);
    }
    /*
     * The inner hash is over ipad || data. Data here is never more than 64 bytes
     * (a PBKDF2 block or the previous digest), so it is concatenated into one
     * buffer rather than needing a streaming hash API — which the symbol table
     * does not offer anyway.
     */
    if (data_len > SHA512_BYTES) {
        return MTP_ERR_ARG;
    }
    memcpy(block + SHA512_BLOCK, data, data_len);

    uint8_t inner[SHA512_BYTES];
    if (crypto_hash_sha512(inner, block, SHA512_BLOCK + data_len) != 0) {
        return MTP_ERR_CRYPTO;
    }

    for (size_t i = 0u; i < SHA512_BLOCK; i++) {
        block[i] = (uint8_t)(k[i] ^ 0x5Cu);
    }
    memcpy(block + SHA512_BLOCK, inner, SHA512_BYTES);
    if (crypto_hash_sha512(out, block, SHA512_BLOCK + SHA512_BYTES) != 0) {
        return MTP_ERR_CRYPTO;
    }
    return MTP_OK;
}

/* ---- PBKDF2-HMAC-SHA512 -------------------------------------------------- */

mtp_err_t mtp_pbkdf2_sha512(const uint8_t *password, size_t password_len,
                            const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t *out, size_t out_len,
                            mtp_srp_progress_fn progress, void *user)
{
    /*
     * Telegram only ever asks for a single 64-byte block, so the block-index loop
     * of the general algorithm collapses to one pass. Keeping it to that keeps
     * the salt buffer small.
     */
    if (out_len > SHA512_BYTES || salt_len > 96u) {
        return MTP_ERR_ARG;
    }

    uint8_t block[100];
    memcpy(block, salt, salt_len);
    /* INT_32_BE(1) — the block index. */
    block[salt_len + 0u] = 0u;
    block[salt_len + 1u] = 0u;
    block[salt_len + 2u] = 0u;
    block[salt_len + 3u] = 1u;

    uint8_t u[SHA512_BYTES];
    uint8_t acc[SHA512_BYTES];

    /* U1 = HMAC(password, salt || INT(1)) — the only iteration whose input is
       longer than a digest, so it is done with a direct hash. */
    {
        uint8_t k[SHA512_BLOCK];
        memset(k, 0, sizeof(k));
        if (password_len > SHA512_BLOCK) {
            if (crypto_hash_sha512(k, password, password_len) != 0) {
                return MTP_ERR_CRYPTO;
            }
        } else {
            memcpy(k, password, password_len);
        }
        uint8_t buf[SHA512_BLOCK + sizeof(block)];
        for (size_t i = 0u; i < SHA512_BLOCK; i++) {
            buf[i] = (uint8_t)(k[i] ^ 0x36u);
        }
        memcpy(buf + SHA512_BLOCK, block, salt_len + 4u);
        uint8_t inner[SHA512_BYTES];
        if (crypto_hash_sha512(inner, buf, SHA512_BLOCK + salt_len + 4u) != 0) {
            return MTP_ERR_CRYPTO;
        }
        for (size_t i = 0u; i < SHA512_BLOCK; i++) {
            buf[i] = (uint8_t)(k[i] ^ 0x5Cu);
        }
        memcpy(buf + SHA512_BLOCK, inner, SHA512_BYTES);
        if (crypto_hash_sha512(u, buf, SHA512_BLOCK + SHA512_BYTES) != 0) {
            return MTP_ERR_CRYPTO;
        }
    }
    memcpy(acc, u, SHA512_BYTES);

    for (uint32_t i = 1u; i < iterations; i++) {
        mtp_err_t err = mtp_hmac_sha512(password, password_len, u, SHA512_BYTES, u);
        if (err != MTP_OK) {
            return err;
        }
        for (size_t b = 0u; b < SHA512_BYTES; b++) {
            acc[b] ^= u[b];
        }
        /*
         * Yield periodically. This loop runs for about ten seconds on this chip;
         * without a yield the idle task never runs and the watchdog fires, and
         * the UI would be frozen with no indication of progress either.
         */
        if ((i & 0x3FFu) == 0u) {
            if (progress != NULL) {
                progress(user, (int)((uint64_t)i * 100u / iterations));
            }
            vTaskDelay(1);
        }
    }
    if (progress != NULL) {
        progress(user, 100);
    }
    memcpy(out, acc, out_len);
    return MTP_OK;
}

/* ---- Minimal bignum ------------------------------------------------------ */

/*
 * SRP needs a modular multiply and a couple of additions, which the SDK does not
 * expose — jpp_crypto_modexp is exponentiation only. Operands are fixed at
 * 2048 bits, and products at 4096, so everything below is fixed-width with no
 * allocation.
 */
#define LIMBS  (MTP_SRP_BYTES / 4u)     /* 64 limbs of 32 bits */
#define LIMBS2 (LIMBS * 2u)

typedef struct { uint32_t v[LIMBS2]; } bn_t;

static void bn_load(bn_t *a, const uint8_t *be, size_t len)
{
    memset(a->v, 0, sizeof(a->v));
    for (size_t i = 0u; i < len; i++) {
        size_t bit = (len - 1u - i) * 8u;
        size_t limb = bit / 32u;
        if (limb < LIMBS2) {
            a->v[limb] |= (uint32_t)be[i] << (bit % 32u);
        }
    }
}

static void bn_store(const bn_t *a, uint8_t *be, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        size_t bit = (len - 1u - i) * 8u;
        size_t limb = bit / 32u;
        be[i] = (limb < LIMBS2) ? (uint8_t)(a->v[limb] >> (bit % 32u)) : 0u;
    }
}

static int bn_cmp(const bn_t *a, const bn_t *b)
{
    for (size_t i = LIMBS2; i-- > 0u;) {
        if (a->v[i] != b->v[i]) {
            return a->v[i] < b->v[i] ? -1 : 1;
        }
    }
    return 0;
}

static void bn_sub(bn_t *a, const bn_t *b)
{
    uint64_t borrow = 0u;
    for (size_t i = 0u; i < LIMBS2; i++) {
        uint64_t d = (uint64_t)a->v[i] - b->v[i] - borrow;
        a->v[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
}

static void bn_add(bn_t *a, const bn_t *b)
{
    uint64_t carry = 0u;
    for (size_t i = 0u; i < LIMBS2; i++) {
        uint64_t s = (uint64_t)a->v[i] + b->v[i] + carry;
        a->v[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

static void bn_shl1(bn_t *a)
{
    uint32_t carry = 0u;
    for (size_t i = 0u; i < LIMBS2; i++) {
        uint32_t next = a->v[i] >> 31;
        a->v[i] = (a->v[i] << 1) | carry;
        carry = next;
    }
}

static size_t bn_bits(const bn_t *a)
{
    for (size_t i = LIMBS2; i-- > 0u;) {
        if (a->v[i] != 0u) {
            uint32_t w = a->v[i];
            size_t b = 0u;
            while (w != 0u) { w >>= 1; b++; }
            return i * 32u + b;
        }
    }
    return 0u;
}

/* a %= m, by shift-and-subtract. Not fast, but this runs three times per login. */
static void bn_mod(bn_t *a, const bn_t *m)
{
    size_t abits = bn_bits(a), mbits = bn_bits(m);
    if (mbits == 0u || abits < mbits) {
        return;
    }
    bn_t shifted = *m;
    size_t shift = abits - mbits;
    for (size_t i = 0u; i < shift; i++) {
        bn_shl1(&shifted);
    }
    for (size_t i = 0u; i <= shift; i++) {
        if (bn_cmp(a, &shifted) >= 0) {
            bn_sub(a, &shifted);
        }
        /* Shift right by one: done by halving through a temporary, since only a
           left shift is needed elsewhere. */
        uint32_t carry = 0u;
        for (size_t j = LIMBS2; j-- > 0u;) {
            uint32_t next = shifted.v[j] & 1u;
            shifted.v[j] = (shifted.v[j] >> 1) | (carry << 31);
            carry = next;
        }
    }
}

/* out = a * b, truncated to 4096 bits (operands are 2048, so nothing is lost). */
static void bn_mul(const bn_t *a, const bn_t *b, bn_t *out)
{
    bn_t r;
    memset(r.v, 0, sizeof(r.v));
    for (size_t i = 0u; i < LIMBS; i++) {
        if (a->v[i] == 0u) {
            continue;
        }
        uint64_t carry = 0u;
        for (size_t j = 0u; j + i < LIMBS2; j++) {
            uint64_t cur = (uint64_t)r.v[i + j] + carry;
            if (j < LIMBS) {
                cur += (uint64_t)a->v[i] * b->v[j];
            }
            r.v[i + j] = (uint32_t)cur;
            carry = cur >> 32;
            if (carry == 0u && j >= LIMBS) {
                break;
            }
        }
    }
    *out = r;
}

/*
 * Everything the exchange needs at once: eleven 512-byte bignums plus the M1
 * input. Borrowed from the shared arena rather than held statically, because it
 * is only live for the few seconds of a password check and the handshake wants
 * the same bytes at a different time. See mtp_scratch.h.
 */
typedef struct {
    bn_t    k, gx, p, kv, t1, B, t2, u, x, a, e;
    uint8_t m1_src[32 + 32 + 32 + MTP_SRP_BYTES * 2u + 32];
} srp_work_t;

_Static_assert(sizeof(srp_work_t) <= MTP_SCRATCH_BYTES,
               "SRP working set outgrew the shared arena");

/* ---- SRP ----------------------------------------------------------------- */

/* SH(data, salt) = SHA256(salt | data | salt). */
static mtp_err_t sh(const uint8_t *data, size_t data_len,
                    const uint8_t *salt, size_t salt_len, uint8_t out[32])
{
    uint8_t buf[256];
    if (salt_len * 2u + data_len > sizeof(buf)) {
        return MTP_ERR_ARG;
    }
    memcpy(buf, salt, salt_len);
    memcpy(buf + salt_len, data, data_len);
    memcpy(buf + salt_len + data_len, salt, salt_len);
    return jpp_crypto_sha256(buf, salt_len * 2u + data_len, out) == JPP_CRYPTO_OK
           ? MTP_OK : MTP_ERR_CRYPTO;
}

/* Hash of two 256-byte operands, each left-padded — the form every SRP hash of
   big numbers takes. */
static mtp_err_t h_pair(const uint8_t *a, const uint8_t *b, uint8_t out[32])
{
    uint8_t buf[MTP_SRP_BYTES * 2u];
    memcpy(buf, a, MTP_SRP_BYTES);
    memcpy(buf + MTP_SRP_BYTES, b, MTP_SRP_BYTES);
    return jpp_crypto_sha256(buf, sizeof(buf), out) == JPP_CRYPTO_OK
           ? MTP_OK : MTP_ERR_CRYPTO;
}

static mtp_err_t srp_compute_body(const mtp_srp_params_t *params,
                                  const char *password,
                                  mtp_srp_progress_fn progress, void *user,
                                  mtp_srp_proof_t *out, srp_work_t *w)
{
    out->srp_id = params->srp_id;

    const size_t N = MTP_SRP_BYTES;
    size_t pw_len = strlen(password);

    /*
     * x = PH2(password, salt1, salt2)
     *   PH1 = SH(SH(password, salt1), salt2)
     *   PH2 = SH(PBKDF2(PH1, salt1, 100000), salt2)
     */
    uint8_t h1[32], ph1[32], pbk[64], x[32];
    mtp_err_t err = sh((const uint8_t *)password, pw_len,
                       params->salt1, params->salt1_len, h1);
    if (err != MTP_OK) {
        return err;
    }
    err = sh(h1, sizeof(h1), params->salt2, params->salt2_len, ph1);
    if (err != MTP_OK) {
        return err;
    }
    err = mtp_pbkdf2_sha512(ph1, sizeof(ph1), params->salt1, params->salt1_len,
                            100000u, pbk, sizeof(pbk), progress, user);
    if (err != MTP_OK) {
        return err;
    }
    err = sh(pbk, sizeof(pbk), params->salt2, params->salt2_len, x);
    if (err != MTP_OK) {
        return err;
    }

    /* g as a padded 256-byte big-endian value, which is how it is hashed. */
    uint8_t g_pad[MTP_SRP_BYTES];
    memset(g_pad, 0, sizeof(g_pad));
    g_pad[N - 4u] = (uint8_t)(params->g >> 24);
    g_pad[N - 3u] = (uint8_t)(params->g >> 16);
    g_pad[N - 2u] = (uint8_t)(params->g >> 8);
    g_pad[N - 1u] = (uint8_t)params->g;

    uint8_t g_be[4] = {
        (uint8_t)(params->g >> 24), (uint8_t)(params->g >> 16),
        (uint8_t)(params->g >> 8),  (uint8_t)params->g,
    };
    const uint8_t *gp; size_t gl;
    mtp_be_trim(g_be, sizeof(g_be), &gp, &gl);

    /* g_x = g^x mod p */
    uint8_t g_x[MTP_SRP_BYTES];
    size_t olen = 0u;
    if (jpp_crypto_dh_compute(gp, gl, x, sizeof(x), params->p, N, g_x, &olen)
            != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /* k = H(p | g), k_v = k * g_x mod p */
    uint8_t k[32];
    err = h_pair(params->p, g_pad, k);
    if (err != MTP_OK) {
        return err;
    }

    bn_load(&w->k, k, sizeof(k));
    bn_load(&w->gx, g_x, N);
    bn_load(&w->p, params->p, N);
    bn_mul(&w->k, &w->gx, &w->kv);
    bn_mod(&w->kv, &w->p);

    /* a random, A = g^a mod p */
    uint8_t a[MTP_SRP_BYTES];
    randombytes_buf(a, sizeof(a));
    if (jpp_crypto_dh_compute(gp, gl, a, sizeof(a), params->p, N, out->A, &olen)
            != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /* u = H(A | B) */
    uint8_t u[32];
    err = h_pair(out->A, params->srp_B, u);
    if (err != MTP_OK) {
        return err;
    }

    /* t = (B - k_v) mod p, taking care that B may be the smaller of the two. */
    bn_load(&w->B, params->srp_B, N);
    bn_mod(&w->B, &w->p);
    w->t1 = w->B;
    if (bn_cmp(&w->t1, &w->kv) < 0) {
        bn_add(&w->t1, &w->p);
    }
    bn_sub(&w->t1, &w->kv);
    bn_mod(&w->t1, &w->p);

    uint8_t t_be[MTP_SRP_BYTES];
    bn_store(&w->t1, t_be, N);

    /* exponent = a + u * x, up to ~2304 bits, which modexp accepts as-is. */
    bn_load(&w->u, u, sizeof(u));
    bn_load(&w->x, x, sizeof(x));
    bn_load(&w->a, a, sizeof(a));
    bn_mul(&w->u, &w->x, &w->t2);
    w->e = w->a;
    bn_add(&w->e, &w->t2);

    uint8_t exp_be[MTP_SRP_BYTES * 2u];
    bn_store(&w->e, exp_be, sizeof(exp_be));
    const uint8_t *ep; size_t el;
    mtp_be_trim(exp_be, sizeof(exp_be), &ep, &el);

    /* S = t^(a + u*x) mod p, then K = H(S). */
    uint8_t s_a[MTP_SRP_BYTES];
    if (jpp_crypto_dh_compute(t_be, N, ep, el, params->p, N, s_a, &olen)
            != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    uint8_t k_a[32];
    if (jpp_crypto_sha256(s_a, N, k_a) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /* M1 = H( (H(p) xor H(g)) | H(salt1) | H(salt2) | A | B | K ) */
    uint8_t h_p[32], h_g[32], h_s1[32], h_s2[32];
    if (jpp_crypto_sha256(params->p, N, h_p) != JPP_CRYPTO_OK ||
        jpp_crypto_sha256(g_pad, N, h_g) != JPP_CRYPTO_OK ||
        jpp_crypto_sha256(params->salt1, params->salt1_len, h_s1) != JPP_CRYPTO_OK ||
        jpp_crypto_sha256(params->salt2, params->salt2_len, h_s2) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    for (size_t i = 0u; i < 32u; i++) {
        h_p[i] ^= h_g[i];
    }

    uint8_t *m1_src = w->m1_src;
    size_t off = 0u;
    memcpy(m1_src + off, h_p, 32u);           off += 32u;
    memcpy(m1_src + off, h_s1, 32u);          off += 32u;
    memcpy(m1_src + off, h_s2, 32u);          off += 32u;
    memcpy(m1_src + off, out->A, N);          off += N;
    memcpy(m1_src + off, params->srp_B, N);   off += N;
    memcpy(m1_src + off, k_a, 32u);           off += 32u;
    if (jpp_crypto_sha256(m1_src, off, out->M1) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }

    /* The password-derived material has no reason to stay in memory. The bignums
       are wiped by mtp_scratch_release; these are the locals it does not cover. */
    memset(x, 0, sizeof(x));
    memset(pbk, 0, sizeof(pbk));
    memset(ph1, 0, sizeof(ph1));
    memset(a, 0, sizeof(a));
    return MTP_OK;
}

mtp_err_t mtp_srp_compute(const mtp_srp_params_t *params, const char *password,
                          mtp_srp_progress_fn progress, void *user,
                          mtp_srp_proof_t *out)
{
    memset(out, 0, sizeof(*out));

    srp_work_t *w = mtp_scratch_acquire(sizeof(srp_work_t));
    if (w == NULL) {
        return MTP_ERR_CRYPTO;
    }
    mtp_err_t err = srp_compute_body(params, password, progress, user, out, w);
    mtp_scratch_release(w);
    return err;
}

#pragma GCC visibility pop
