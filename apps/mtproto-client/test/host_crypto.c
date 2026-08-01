/*
 * Host-only stand-ins for the firmware's crypto and SDK symbols.
 *
 * These exist so the protocol code can be exercised on a development machine,
 * where a failure is a stack trace instead of a device that silently fails to
 * connect. SHA-1/SHA-256/AES come from CommonCrypto, which is always present on
 * macOS; modexp is a plain square-and-multiply over 32-bit limbs, chosen for
 * being obviously correct rather than fast — on target this call lands on the
 * ESP32-C6's hardware-accelerated mbedTLS MPI instead.
 *
 * Not compiled into the device build: jppd-build only globs sources under src/.
 */
#include <stdlib.h>
#include <string.h>

#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonDigest.h>

#include "jpp_crypto_core.h"
#include "jpp_sdk_bridge.h"

/* ---- Hashes -------------------------------------------------------------- */

jpp_crypto_status_t jpp_crypto_sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    CC_SHA1(msg, (CC_LONG)len, out);
    return JPP_CRYPTO_OK;
}

jpp_crypto_status_t jpp_crypto_sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    CC_SHA256(msg, (CC_LONG)len, out);
    return JPP_CRYPTO_OK;
}

/* libsodium's SHA-512, which the firmware exports and mtp_srp builds HMAC on. */
int crypto_hash_sha512(unsigned char *out, const unsigned char *in,
                       unsigned long long inlen)
{
    CC_SHA512(in, (CC_LONG)inlen, out);
    return 0;
}

/* ---- AES-256-IGE --------------------------------------------------------- */

static void aes_ecb(const uint8_t key[32], const uint8_t in[16], uint8_t out[16], int encrypt)
{
    size_t moved = 0;
    CCCryptorRef cryptor = NULL;
    CCCryptorCreateWithMode(encrypt ? kCCEncrypt : kCCDecrypt, kCCModeECB,
                            kCCAlgorithmAES, ccNoPadding,
                            NULL, key, 32, NULL, 0, 0, 0, &cryptor);
    CCCryptorUpdate(cryptor, in, 16, out, 16, &moved);
    CCCryptorRelease(cryptor);
}

/*
 * IGE: each block is XORed with the previous plaintext before the cipher and
 * with the previous ciphertext after it. The 32-byte IV supplies both "previous"
 * values for the first block.
 */
jpp_crypto_status_t jpp_crypto_aes256_ige_encrypt(const uint8_t *in, size_t len,
                                                  const uint8_t key[32],
                                                  const uint8_t iv[32], uint8_t *out)
{
    if (len == 0 || (len % 16) != 0) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }
    uint8_t prev_c[16], prev_p[16];
    memcpy(prev_c, iv, 16);
    memcpy(prev_p, iv + 16, 16);

    for (size_t off = 0; off < len; off += 16) {
        uint8_t cur_p[16], tmp[16], cur_c[16];
        memcpy(cur_p, in + off, 16);
        for (int i = 0; i < 16; i++) {
            tmp[i] = cur_p[i] ^ prev_c[i];
        }
        aes_ecb(key, tmp, cur_c, 1);
        for (int i = 0; i < 16; i++) {
            cur_c[i] ^= prev_p[i];
        }
        memcpy(out + off, cur_c, 16);
        memcpy(prev_c, cur_c, 16);
        memcpy(prev_p, cur_p, 16);
    }
    return JPP_CRYPTO_OK;
}

jpp_crypto_status_t jpp_crypto_aes256_ige_decrypt(const uint8_t *in, size_t len,
                                                  const uint8_t key[32],
                                                  const uint8_t iv[32], uint8_t *out)
{
    if (len == 0 || (len % 16) != 0) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }
    uint8_t prev_c[16], prev_p[16];
    memcpy(prev_c, iv, 16);
    memcpy(prev_p, iv + 16, 16);

    for (size_t off = 0; off < len; off += 16) {
        uint8_t cur_c[16], tmp[16], cur_p[16];
        memcpy(cur_c, in + off, 16);
        for (int i = 0; i < 16; i++) {
            tmp[i] = cur_c[i] ^ prev_p[i];
        }
        aes_ecb(key, tmp, cur_p, 0);
        for (int i = 0; i < 16; i++) {
            cur_p[i] ^= prev_c[i];
        }
        memcpy(out + off, cur_p, 16);
        memcpy(prev_c, cur_c, 16);
        memcpy(prev_p, cur_p, 16);
    }
    return JPP_CRYPTO_OK;
}

/* ---- Bignum modexp ------------------------------------------------------- */

#define LIMBS 128   /* 128 * 32 bits = 4096, enough headroom for 2048-bit squares */

typedef struct { uint32_t v[LIMBS]; int n; } bn_t;

static void bn_zero(bn_t *a) { memset(a->v, 0, sizeof(a->v)); a->n = 0; }

static void bn_trim(bn_t *a)
{
    while (a->n > 0 && a->v[a->n - 1] == 0) {
        a->n--;
    }
}

static void bn_from_be(bn_t *a, const uint8_t *b, size_t len)
{
    bn_zero(a);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        size_t limb = bit / 32;
        if (limb < LIMBS) {
            a->v[limb] |= (uint32_t)b[i] << (bit % 32);
            if ((int)limb + 1 > a->n) {
                a->n = (int)limb + 1;
            }
        }
    }
    bn_trim(a);
}

static void bn_to_be(const bn_t *a, uint8_t *out, size_t len)
{
    memset(out, 0, len);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        size_t limb = bit / 32;
        if (limb < (size_t)a->n) {
            out[i] = (uint8_t)(a->v[limb] >> (bit % 32));
        }
    }
}

static int bn_cmp(const bn_t *a, const bn_t *b)
{
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) {
            return a->v[i] < b->v[i] ? -1 : 1;
        }
    }
    return 0;
}

static void bn_sub(bn_t *a, const bn_t *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t d = (int64_t)a->v[i] - (i < b->n ? b->v[i] : 0) - borrow;
        if (d < 0) { d += (int64_t)1 << 32; borrow = 1; } else { borrow = 0; }
        a->v[i] = (uint32_t)d;
    }
    bn_trim(a);
}

static void bn_mul(const bn_t *a, const bn_t *b, bn_t *out)
{
    bn_t r;
    bn_zero(&r);
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->n || carry; j++) {
            if (i + j >= LIMBS) break;
            uint64_t cur = (uint64_t)r.v[i + j] + carry +
                           (j < b->n ? (uint64_t)a->v[i] * b->v[j] : 0);
            r.v[i + j] = (uint32_t)cur;
            carry = cur >> 32;
            if (i + j + 1 > r.n) r.n = i + j + 1;
        }
    }
    bn_trim(&r);
    *out = r;
}

/* Schoolbook remainder by repeated shift-and-subtract. Slow, but this is a test
   harness and correctness is the only requirement. */
static void bn_mod(bn_t *a, const bn_t *m)
{
    if (bn_cmp(a, m) < 0) return;
    int shift = (a->n - m->n) * 32 + 32;
    for (int s = shift; s >= 0; s--) {
        bn_t t = *m;
        /* t <<= s */
        int limb_shift = s / 32, bit_shift = s % 32;
        bn_t sh; bn_zero(&sh);
        for (int i = t.n - 1; i >= 0; i--) {
            int di = i + limb_shift;
            if (di >= LIMBS) continue;
            uint64_t val = (uint64_t)t.v[i] << bit_shift;
            sh.v[di] |= (uint32_t)val;
            if (di + 1 < LIMBS && (val >> 32)) sh.v[di + 1] |= (uint32_t)(val >> 32);
            if (di + 2 > sh.n) sh.n = di + 2;
        }
        bn_trim(&sh);
        if (sh.n > 0 && bn_cmp(a, &sh) >= 0) {
            bn_sub(a, &sh);
        }
    }
}

jpp_crypto_status_t jpp_crypto_modexp(const uint8_t *base, size_t base_len,
                                      const uint8_t *exp, size_t exp_len,
                                      const uint8_t *mod, size_t mod_len,
                                      uint8_t *out, size_t *out_len)
{
    bn_t b, m, r, one;
    bn_from_be(&b, base, base_len);
    bn_from_be(&m, mod, mod_len);
    if (m.n == 0) {
        return JPP_CRYPTO_ERR_INVALID_ARG;
    }
    bn_zero(&one); one.v[0] = 1; one.n = 1;
    r = one;
    bn_mod(&b, &m);

    /* Square-and-multiply, most significant exponent bit first. */
    for (size_t i = 0; i < exp_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bn_t t;
            bn_mul(&r, &r, &t); bn_mod(&t, &m); r = t;
            if ((exp[i] >> bit) & 1) {
                bn_mul(&r, &b, &t); bn_mod(&t, &m); r = t;
            }
        }
    }
    bn_to_be(&r, out, mod_len);
    *out_len = mod_len;
    return JPP_CRYPTO_OK;
}

jpp_crypto_status_t jpp_crypto_rsa_encrypt(const uint8_t *data, size_t data_len,
                                           const uint8_t *modulus, size_t modulus_len,
                                           const uint8_t *exponent, size_t exponent_len,
                                           uint8_t *out, size_t *out_len)
{
    return jpp_crypto_modexp(data, data_len, exponent, exponent_len,
                             modulus, modulus_len, out, out_len);
}

jpp_crypto_status_t jpp_crypto_dh_compute(const uint8_t *base, size_t base_len,
                                          const uint8_t *exp, size_t exp_len,
                                          const uint8_t *prime, size_t prime_len,
                                          uint8_t *out, size_t *out_len)
{
    return jpp_crypto_modexp(base, base_len, exp, exp_len, prime, prime_len,
                             out, out_len);
}

/* ---- Other firmware symbols the code under test references --------------- */

/* Deterministic by default so a failing test reproduces; host_test_seed_random
   lets a case ask for real entropy. */
static uint32_t s_rng = 0x12345678u;

void randombytes_buf(void *buf, size_t size)
{
    uint8_t *p = buf;
    for (size_t i = 0; i < size; i++) {
        s_rng = s_rng * 1103515245u + 12345u;
        p[i] = (uint8_t)(s_rng >> 16);
    }
}

void host_test_seed_random(uint32_t seed) { s_rng = seed; }

/*
 * ui_gfx_flush is the only SDK call reached from a tested module. Stubbed as a
 * no-op: the tests exercise layout arithmetic, and nothing here has a display.
 */
jpp_sdk_status_t jpp_sdk_canvas_write(jpp_sdk_context_t *ctx, uint8_t row,
                                     const uint8_t *pixels)
{
    (void)ctx; (void)row; (void)pixels;
    return JPP_SDK_STATUS_OK;
}

uint32_t xTaskGetTickCount(void)
{
    static uint32_t ticks = 0;
    return ticks++;
}

void vTaskDelay(uint32_t t) { (void)t; }
