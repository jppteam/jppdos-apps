/*
 * Host tests for the parts of the client that are pure computation.
 *
 * These are the pieces where a mistake is invisible on-device — a wrong TL
 * padding rule or a wrong fingerprint just produces "connection failed" with
 * nothing to debug. Run them before touching hardware:
 *
 *   ./test/run.sh
 */
#include <stdio.h>
#include <string.h>

#include <zlib.h>

#include "jpp_crypto_core.h"
#include "mtp_common.h"
#include "mtp_config.h"
#include "mtp_gzip.h"
#include "mtp_mem.h"
#include "mtp_scratch.h"
#include "mtp_pq.h"
#include "mtp_srp.h"
#include "mtp_tl.h"
#include "ui_font.h"
#include "ui_gfx.h"
#include "ui_keyboard.h"

#include "skip_vectors.h"

static int s_fail;
static int s_ran;

#define CHECK(cond, ...) do {                                     \
        s_ran++;                                                  \
        if (!(cond)) {                                            \
            s_fail++;                                             \
            printf("  FAIL %s:%d  ", __func__, __LINE__);          \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
        }                                                          \
    } while (0)

/* ---- TL ------------------------------------------------------------------ */

static void test_tl_scalars(void)
{
    uint8_t buf[64];
    mtp_w_t w;
    mtp_w_init(&w, buf, sizeof(buf));
    mtp_w_u32(&w, 0xDEADBEEFu);
    mtp_w_i32(&w, -2);
    mtp_w_u64(&w, 0x0123456789ABCDEFull);
    mtp_w_bool(&w, true);
    mtp_w_bool(&w, false);
    CHECK(mtp_w_ok(&w), "writer overflowed");
    CHECK(w.len == 4 + 4 + 8 + 4 + 4, "len=%zu", w.len);

    mtp_r_t r;
    mtp_r_init(&r, buf, w.len);
    CHECK(mtp_r_u32(&r) == 0xDEADBEEFu, "u32 mismatch");
    CHECK(mtp_r_i32(&r) == -2, "i32 mismatch");
    CHECK(mtp_r_u64(&r) == 0x0123456789ABCDEFull, "u64 mismatch");
    CHECK(mtp_r_bool(&r) == true, "bool true");
    CHECK(mtp_r_bool(&r) == false, "bool false");
    CHECK(mtp_r_ok(&r) && mtp_r_left(&r) == 0, "reader not clean");
}

/*
 * The length-prefix boundary is the classic TL bug: 253 bytes uses a 1-byte
 * header, 254 switches to the 4-byte form, and the zero padding is computed
 * from header+data so that the *total* is a multiple of 4.
 */
static void test_tl_bytes_padding(void)
{
    static const struct { size_t len; size_t expect; } cases[] = {
        { 0,   4   },   /* 1 + 0 + 3 pad */
        { 1,   4   },   /* 1 + 1 + 2 pad */
        { 2,   4   },
        { 3,   4   },
        { 4,   8   },   /* 1 + 4 + 3 pad */
        { 253, 256 },   /* 1 + 253 + 2 pad */
        { 254, 260 },   /* 4 + 254 + 2 pad */
        { 255, 260 },
        { 256, 260 },   /* 4 + 256 + 0 pad */
    };
    static uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 1);
    }

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t buf[600];
        mtp_w_t w;
        mtp_w_init(&w, buf, sizeof(buf));
        mtp_w_bytes(&w, data, cases[c].len);
        CHECK(mtp_w_ok(&w), "len %zu overflowed", cases[c].len);
        CHECK(w.len == cases[c].expect, "len %zu encoded to %zu, want %zu",
              cases[c].len, w.len, cases[c].expect);
        CHECK((w.len % 4) == 0, "len %zu not 4-aligned", cases[c].len);

        mtp_r_t r;
        mtp_r_init(&r, buf, w.len);
        size_t got = 0;
        const uint8_t *back = mtp_r_bytes(&r, &got);
        CHECK(back != NULL && got == cases[c].len, "len %zu round-trip got %zu",
              cases[c].len, got);
        CHECK(back != NULL && memcmp(back, data, cases[c].len) == 0,
              "len %zu data mismatch", cases[c].len);
        CHECK(mtp_r_ok(&r) && mtp_r_left(&r) == 0, "len %zu left %zu",
              cases[c].len, mtp_r_left(&r));
    }
}

/* A truncated or hostile buffer must set err, never read past the end. */
static void test_tl_reader_bounds(void)
{
    uint8_t buf[8] = { 0 };

    mtp_r_t r;
    mtp_r_init(&r, buf, 3);
    (void)mtp_r_u32(&r);
    CHECK(!mtp_r_ok(&r), "short u32 should error");

    /* Claims 200 bytes of payload inside a 4-byte buffer. */
    uint8_t evil[4] = { 200, 1, 2, 3 };
    mtp_r_init(&r, evil, sizeof(evil));
    size_t got = 0;
    CHECK(mtp_r_bytes(&r, &got) == NULL, "oversized bytes should fail");
    CHECK(!mtp_r_ok(&r), "oversized bytes should set err");

    /* A vector claiming more elements than could possibly fit. */
    uint8_t vec[8];
    mtp_wr_u32le(vec, MTP_ID_VECTOR);
    mtp_wr_u32le(vec + 4, 0xFFFFFFFFu);
    mtp_r_init(&r, vec, sizeof(vec));
    CHECK(mtp_r_vector(&r) == 0, "absurd vector count should be rejected");
    CHECK(!mtp_r_ok(&r), "absurd vector should set err");

    /* The 64-bit length form is refused rather than misparsed. */
    uint8_t huge[8] = { 0xFF, 0, 0, 0, 0, 0, 0, 0 };
    mtp_r_init(&r, huge, sizeof(huge));
    CHECK(mtp_r_bytes(&r, &got) == NULL, "0xFF length form should fail");
}

/* Truncation must not split a multi-byte UTF-8 sequence — chat titles are
   routinely Cyrillic, which is two bytes per character. */
static void test_tl_str_utf8(void)
{
    uint8_t buf[64];
    mtp_w_t w;
    mtp_w_init(&w, buf, sizeof(buf));
    /* "Привет" — 6 Cyrillic characters, 12 bytes. */
    const char *cyr = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    mtp_w_str(&w, cyr);
    CHECK(mtp_w_ok(&w), "writer overflowed");

    /* Full read. */
    char out[32];
    mtp_r_t r;
    mtp_r_init(&r, buf, w.len);
    mtp_r_str(&r, out, sizeof(out));
    CHECK(strcmp(out, cyr) == 0, "full read mismatch: %s", out);

    /* Truncating to 8 bytes of capacity leaves room for 7 payload bytes, which
       lands mid-character; the result must back off to 6. */
    mtp_r_init(&r, buf, w.len);
    mtp_r_str(&r, out, 8);
    size_t n = strlen(out);
    CHECK(n == 6, "truncated to %zu bytes, want 6", n);
    for (size_t i = 0; i < n; i++) {
        CHECK((out[i] & 0xC0) != 0x80 || i > 0, "leading continuation byte");
    }
    /* No trailing partial sequence: the last byte must not be a lead byte. */
    CHECK(n == 0 || ((unsigned char)out[n - 1] & 0xC0) != 0xC0,
          "ends on a dangling lead byte");
}

static void test_tl_pad_random(void)
{
    uint8_t buf[128];
    for (size_t body = 0; body <= 32; body += 4) {
        mtp_w_t w;
        mtp_w_init(&w, buf, sizeof(buf));
        w.len = body;
        mtp_w_pad_random(&w, 16, 12);
        CHECK(mtp_w_ok(&w), "body %zu overflowed", body);
        CHECK((w.len % 16) == 0, "body %zu -> %zu not 16-aligned", body, w.len);
        CHECK(w.len - body >= 12, "body %zu padded only %zu", body, w.len - body);
        CHECK(w.len - body < 12 + 16, "body %zu padded %zu, too much",
              body, w.len - body);
    }
}

/* ---- PQ factorisation ---------------------------------------------------- */

static void test_pq_factor(void)
{
    /*
     * Products of two primes in the range resPQ actually uses — both factors
     * around 2^31, so the product is 61-62 bits and trial division is hopeless.
     * Generated and verified independently.
     */
    static const struct { uint64_t pq; uint32_t p, q; } cases[] = {
        { 2949305686811325649ull, 1534802663u, 1921618823u },
        { 1603104959328425911ull, 1249524127u, 1282972393u },
        { 2156635589483185007ull, 1240430531u, 1738618597u },
        { 3862934972755802159ull, 1902222631u, 2030748089u },
        { 15ull,                  3u,          5u          },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t p = 0, q = 0;
        bool ok = mtp_pq_factor(cases[i].pq, &p, &q);
        CHECK(ok, "case %zu: factoring %llu failed", i,
              (unsigned long long)cases[i].pq);
        if (!ok) {
            continue;
        }
        CHECK(p < q || (p == q), "case %zu: p=%u not <= q=%u", i, p, q);
        CHECK((uint64_t)p * (uint64_t)q == cases[i].pq,
              "case %zu: %u * %u != %llu", i, p, q,
              (unsigned long long)cases[i].pq);
        if (cases[i].q != 0u) {
            CHECK(p == cases[i].p && q == cases[i].q,
                  "case %zu: got %u,%u want %u,%u", i, p, q, cases[i].p, cases[i].q);
        }
    }

    /* A prime input has no non-trivial split and must be reported as failure
       rather than answered with a guess. */
    uint32_t p = 0, q = 0;
    CHECK(!mtp_pq_factor(2147483647ull, &p, &q), "prime should not factor");
}

static void test_pq_mulmod(void)
{
    /* Values that overflow a naive 64-bit multiply, which is the whole point of
       the shift-and-add implementation. */
    uint64_t m = 0x7FFFFFFFFFFFFFC5ull;   /* just under 2^63 */
    uint64_t a = m - 3, b = m - 7;
    uint64_t r = mtp_pq_mulmod(a, b, m);
    /* (m-3)(m-7) = m^2 -10m +21 ≡ 21 (mod m) */
    CHECK(r == 21u, "mulmod near-2^63 gave %llu, want 21", (unsigned long long)r);
    CHECK(mtp_pq_mulmod(0, 12345, m) == 0, "0 * x should be 0");
    CHECK(mtp_pq_mulmod(1, 12345, m) == 12345u, "1 * x should be x");
}

/* ---- Byte helpers -------------------------------------------------------- */

static void test_be_trim(void)
{
    const uint8_t *out;
    size_t out_len;

    static const uint8_t a[4] = { 0x00, 0x00, 0x12, 0x34 };
    mtp_be_trim(a, sizeof(a), &out, &out_len);
    CHECK(out_len == 2 && out[0] == 0x12, "leading zeros not trimmed");

    static const uint8_t b[4] = { 0x80, 0, 0, 0 };
    mtp_be_trim(b, sizeof(b), &out, &out_len);
    CHECK(out_len == 4, "high-bit value should not be trimmed");

    /* All zeros must trim to a single zero byte, not to nothing: an empty TL
       string is a different thing from the integer zero. */
    static const uint8_t z[4] = { 0, 0, 0, 0 };
    mtp_be_trim(z, sizeof(z), &out, &out_len);
    CHECK(out_len == 1 && out[0] == 0, "zero trimmed to %zu bytes", out_len);
}

static void test_hex_decode(void)
{
    uint8_t out[8];
    CHECK(mtp_hex_decode("01020304", out, sizeof(out)) == 4 && out[0] == 1 && out[3] == 4,
          "plain hex failed");
    CHECK(mtp_hex_decode("AbCdEf", out, sizeof(out)) == 3 && out[0] == 0xAB,
          "mixed case failed");
    CHECK(mtp_hex_decode("01:02 03\n04", out, sizeof(out)) == 4,
          "separators should be tolerated");
    CHECK(mtp_hex_decode("012", out, sizeof(out)) == 0, "odd digit count must fail");
    CHECK(mtp_hex_decode("01g2", out, sizeof(out)) == 0, "bad char must fail");
    CHECK(mtp_hex_decode("0102030405060708090a", out, sizeof(out)) == 0,
          "overflow must fail");
}

static void test_parse_u32(void)
{
    uint32_t v = 0;
    CHECK(mtp_parse_u32("0", &v) && v == 0u, "zero");
    CHECK(mtp_parse_u32("4294967295", &v) && v == 4294967295u, "max u32");
    CHECK(!mtp_parse_u32("4294967296", &v), "overflow must fail");
    CHECK(!mtp_parse_u32("", &v), "empty must fail");
    CHECK(!mtp_parse_u32("12x", &v), "trailing junk must fail");
    CHECK(mtp_parse_u32(" 42 ", &v) && v == 42u, "surrounding spaces");
}

/* ---- RSA key fingerprint ------------------------------------------------- */

/*
 * The fingerprint is the low 64 bits of SHA-1 over the TL serialisation of
 * (modulus, exponent) with *no* constructor id — the detail implementations get
 * wrong first. Telegram's production key is known to fingerprint to
 * 0xd09d1d85de64fd85, so running the real mtp_config_init and checking the
 * result pins the algorithm, the embedded key bytes, and the TL writer at once.
 */
static void test_config_and_fingerprint(void)
{
    CHECK(mtp_config_init(NULL) == MTP_OK, "config init failed");

    const mtp_profile_t *tg = mtp_config_profile(MTP_MODE_TELEGRAM);
    CHECK(tg != NULL, "no Telegram profile");
    if (tg == NULL) {
        return;
    }
    CHECK(tg->key_count == 1, "key_count = %zu", tg->key_count);
    CHECK(tg->keys[0].modulus_len == 256, "modulus is %zu bytes, want 256",
          tg->keys[0].modulus_len);
    CHECK(tg->keys[0].fingerprint == 0xd09d1d85de64fd85ull,
          "fingerprint = 0x%016llx, want 0xd09d1d85de64fd85",
          (unsigned long long)tg->keys[0].fingerprint);
    CHECK(tg->dc_count == 5, "dc_count = %zu", tg->dc_count);
    CHECK(mtp_config_find_dc(tg, 2) != NULL, "DC2 should exist");
    CHECK(mtp_config_find_dc(tg, 9) == NULL, "DC9 should not exist");

    /* Key matching is by fingerprint overlap with the server's offered list. */
    uint64_t offered[3] = { 0x1111111111111111ull, 0xd09d1d85de64fd85ull, 0u };
    CHECK(mtp_config_match_key(tg, offered, 3) == &tg->keys[0],
          "should match the Telegram key");
    uint64_t none[1] = { 0x2222222222222222ull };
    CHECK(mtp_config_match_key(tg, none, 1) == NULL, "should not match");

    /*
     * The api credentials are unfilled placeholders in this tree, so the profile
     * must report itself unusable rather than let a handshake proceed. When the
     * real values are pasted in, this check flips to MTP_OK — which is the point.
     */
    mtp_err_t tg_check = mtp_config_check(MTP_MODE_TELEGRAM);
    CHECK(tg_check == MTP_OK || tg_check == MTP_ERR_NO_CONFIG,
          "unexpected Telegram check result %d", (int)tg_check);
    if (tg->api_id == 0u) {
        CHECK(tg_check == MTP_ERR_NO_CONFIG,
              "placeholder api_id must fail the config check");
    }

    /* j++gram ships entirely as placeholders and must never look configured. */
    CHECK(mtp_config_check(MTP_MODE_JPPGRAM) == MTP_ERR_NO_CONFIG,
          "placeholder j++gram must fail the config check");
}

/* ---- AES-256-IGE --------------------------------------------------------- */

static void test_aes_ige_roundtrip(void)
{
    uint8_t key[32], iv[32], plain[64], cipher[64], back[64];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)i; iv[i] = (uint8_t)(0xF0 - i); }
    for (int i = 0; i < 64; i++) { plain[i] = (uint8_t)(i * 3 + 1); }

    CHECK(jpp_crypto_aes256_ige_encrypt(plain, sizeof(plain), key, iv, cipher) ==
          JPP_CRYPTO_OK, "encrypt failed");
    CHECK(memcmp(cipher, plain, sizeof(plain)) != 0, "ciphertext equals plaintext");
    CHECK(jpp_crypto_aes256_ige_decrypt(cipher, sizeof(cipher), key, iv, back) ==
          JPP_CRYPTO_OK, "decrypt failed");
    CHECK(memcmp(back, plain, sizeof(plain)) == 0, "round-trip mismatch");

    /* In-place must give the same answer — the client relies on it. */
    uint8_t inplace[64];
    memcpy(inplace, plain, sizeof(plain));
    CHECK(jpp_crypto_aes256_ige_encrypt(inplace, sizeof(inplace), key, iv, inplace) ==
          JPP_CRYPTO_OK, "in-place encrypt failed");
    CHECK(memcmp(inplace, cipher, sizeof(cipher)) == 0, "in-place differs");

    /* A non-multiple of the block size must be refused, not truncated. */
    CHECK(jpp_crypto_aes256_ige_encrypt(plain, 20, key, iv, cipher) !=
          JPP_CRYPTO_OK, "unaligned length should fail");
}

/* ---- modexp -------------------------------------------------------------- */

static void test_modexp(void)
{
    /* 3^7 mod 1000 = 2187 mod 1000 = 187 */
    static const uint8_t base[1] = { 3 };
    static const uint8_t exp[1]  = { 7 };
    static const uint8_t mod[2]  = { 0x03, 0xE8 };   /* 1000 */
    uint8_t out[2];
    size_t out_len = 0;
    CHECK(jpp_crypto_modexp(base, 1, exp, 1, mod, 2, out, &out_len) == JPP_CRYPTO_OK,
          "modexp failed");
    CHECK(out_len == 2, "out_len = %zu", out_len);
    uint32_t v = ((uint32_t)out[0] << 8) | out[1];
    CHECK(v == 187u, "3^7 mod 1000 = %u, want 187", v);

    /* Fermat: for prime p and 0 < a < p, a^(p-1) mod p == 1. p = 65537. */
    static const uint8_t p[3]  = { 0x01, 0x00, 0x01 };
    static const uint8_t pm1[3] = { 0x01, 0x00, 0x00 };
    static const uint8_t a[1]  = { 5 };
    uint8_t out2[3];
    CHECK(jpp_crypto_modexp(a, 1, pm1, 3, p, 3, out2, &out_len) == JPP_CRYPTO_OK,
          "fermat modexp failed");
    CHECK(out2[0] == 0 && out2[1] == 0 && out2[2] == 1,
          "5^65536 mod 65537 = %02x%02x%02x, want 000001",
          out2[0], out2[1], out2[2]);
}

/* ---- gzip inflate -------------------------------------------------------- */

/*
 * Compressed with the host's zlib, decompressed with the client's own inflate.
 * Cross-checking against a real encoder is the only way to be confident about
 * DEFLATE: a subtly wrong Huffman table still produces plausible-looking output
 * for short inputs and then fails on a real dialog list.
 */
static void gzip_case(const char *what, const uint8_t *raw, size_t raw_len, int level)
{
    uint8_t packed[16384];
    uLongf packed_len = sizeof(packed);

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    /* windowBits 15+16 selects a gzip wrapper, which is what gzip_packed uses. */
    CHECK(deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK,
          "%s: deflateInit2 failed", what);
    zs.next_in = (Bytef *)raw;
    zs.avail_in = (uInt)raw_len;
    zs.next_out = packed;
    zs.avail_out = (uInt)packed_len;
    CHECK(deflate(&zs, Z_FINISH) == Z_STREAM_END, "%s: deflate did not finish", what);
    packed_len = zs.total_out;
    deflateEnd(&zs);

    static uint8_t out[16384];
    size_t out_len = 0;
    mtp_err_t err = mtp_gzip_inflate(packed, packed_len, out, sizeof(out), &out_len);
    CHECK(err == MTP_OK, "%s: inflate returned %d (%s)", what, (int)err, mtp_err_str(err));
    CHECK(out_len == raw_len, "%s: got %zu bytes, want %zu", what, out_len, raw_len);
    CHECK(out_len == raw_len && memcmp(out, raw, raw_len) == 0,
          "%s: content mismatch", what);
}

static void test_gzip_inflate(void)
{
    static uint8_t buf[8192];

    /* Highly repetitive — exercises long back-references and run copies. */
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)('A' + (i % 4));
    }
    gzip_case("repetitive", buf, sizeof(buf), 9);

    /* Incompressible — forces stored blocks, the byte-aligned path. */
    uint32_t rng = 0xABCDEF01u;
    for (size_t i = 0; i < sizeof(buf); i++) {
        rng = rng * 1103515245u + 12345u;
        buf[i] = (uint8_t)(rng >> 16);
    }
    gzip_case("random", buf, sizeof(buf), 0);
    gzip_case("random-lvl9", buf, sizeof(buf), 9);

    /* Mixed, and a size that is not a neat multiple of anything. */
    for (size_t i = 0; i < 3001; i++) {
        buf[i] = (uint8_t)((i * 31) ^ (i >> 5));
    }
    gzip_case("mixed", buf, 3001, 6);
    gzip_case("mixed-lvl1", buf, 3001, 1);

    /* Degenerate sizes. */
    gzip_case("one byte", (const uint8_t *)"x", 1, 9);
    gzip_case("empty", (const uint8_t *)"", 0, 9);

    /* A truncated stream must be rejected, not half-accepted. */
    uint8_t packed[512];
    uLongf plen = sizeof(packed);
    CHECK(compress2(packed, &plen, (const Bytef *)"hello hello hello hello", 23, 9) == Z_OK,
          "compress2 failed");
    static uint8_t out[512];
    size_t out_len = 0;
    /* compress2 makes a zlib stream, not gzip — the magic check must reject it. */
    CHECK(mtp_gzip_inflate(packed, plen, out, sizeof(out), &out_len) == MTP_ERR_PROTO,
          "zlib-wrapped stream should be rejected as not-gzip");

    /* Too small an output buffer reports overflow rather than truncating. */
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)('A' + (i % 4));
    }
    uint8_t big[16384];
    uLongf big_len = sizeof(big);
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = buf; zs.avail_in = (uInt)sizeof(buf);
    zs.next_out = big; zs.avail_out = (uInt)big_len;
    deflate(&zs, Z_FINISH);
    big_len = zs.total_out;
    deflateEnd(&zs);
    uint8_t tiny[64];
    CHECK(mtp_gzip_inflate(big, big_len, tiny, sizeof(tiny), &out_len) == MTP_ERR_OVERFLOW,
          "undersized output should report overflow");
}

/* ---- Font and text layout ----------------------------------------------- */

static void test_utf8_decode(void)
{
    /* "Привет!" — Cyrillic is two bytes per character, so byte length and
       character length differ, which is the whole reason this layer exists. */
    const char *s = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82!";
    CHECK(strlen(s) == 13, "byte length %zu", strlen(s));
    CHECK(ui_utf8_len(s) == 7, "char length %zu, want 7", ui_utf8_len(s));

    size_t pos = 0;
    CHECK(ui_utf8_next(s, &pos) == 0x041F && pos == 2, "first char should be U+041F");
    pos = ui_utf8_offset(s, 6);
    CHECK(pos == 12, "offset of char 6 = %zu, want 12", pos);
    CHECK(ui_utf8_next(s, &pos) == '!', "char 6 should be '!'");

    /* Past the end clamps rather than running off. */
    CHECK(ui_utf8_offset(s, 99) == 13, "clamped offset");

    /* Malformed input yields U+FFFD and advances, so the rest still decodes. */
    const char *bad = "a\xFF\xD0\x9Fz";
    pos = 0;
    CHECK(ui_utf8_next(bad, &pos) == 'a', "ascii before bad byte");
    CHECK(ui_utf8_next(bad, &pos) == 0xFFFD, "bad lead byte -> U+FFFD");
    CHECK(ui_utf8_next(bad, &pos) == 0x041F, "recovers on the next sequence");
    CHECK(ui_utf8_next(bad, &pos) == 'z', "and continues");

    /* A truncated sequence must not read past the terminator. */
    const char *trunc = "\xD0";
    pos = 0;
    CHECK(ui_utf8_next(trunc, &pos) == 0xFFFD, "truncated -> U+FFFD");
    CHECK(pos == 1, "truncated advanced to %zu", pos);
}

static void test_font_coverage(void)
{
    /* The table must be sorted, or the binary search silently misses glyphs. */
    for (size_t i = 1; i < ui_font_glyph_count; i++) {
        CHECK(ui_font_glyphs[i].cp > ui_font_glyphs[i - 1].cp,
              "table not sorted at index %zu", i);
    }
    /* Every printable ASCII and every Russian letter must resolve to a real
       glyph rather than the notdef box. */
    for (uint32_t cp = 0x20; cp < 0x7F; cp++) {
        CHECK(ui_font_glyph(cp) != ui_font_notdef, "ASCII U+%04X missing", cp);
    }
    for (uint32_t cp = 0x410; cp <= 0x44F; cp++) {
        CHECK(ui_font_glyph(cp) != ui_font_notdef, "Cyrillic U+%04X missing", cp);
    }
    CHECK(ui_font_glyph(0x401) != ui_font_notdef, "cap Yo missing");
    CHECK(ui_font_glyph(0x451) != ui_font_notdef, "lower yo missing");

    /* Something genuinely uncovered must fall back visibly. */
    CHECK(ui_font_glyph(0x4E00) == ui_font_notdef, "CJK should be notdef");
    CHECK(ui_font_glyph(0x1F600) == ui_font_notdef, "emoji should be notdef");

    /* 21 characters per 128-pixel line is the assumption the whole UI is laid
       out around; if the advance changes, the layouts need revisiting. */
    CHECK(UI_FONT_ADV == 6, "advance is %u", UI_FONT_ADV);
    CHECK(128 / (int)UI_FONT_ADV == 21, "chars per line changed");
}

static void test_font_fit(void)
{
    const char *s = "Hello world";
    CHECK(ui_font_width("") == 0, "empty width");
    CHECK(ui_font_width("A") == 5, "single glyph is %d, want 5", ui_font_width("A"));
    CHECK(ui_font_width("AB") == 11, "two glyphs is %d, want 11", ui_font_width("AB"));

    /* fit returns a byte count, and must never split a UTF-8 sequence. */
    const char *cyr = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    for (int px = 0; px < 80; px++) {
        size_t n = ui_font_fit(cyr, px);
        CHECK(n % 2 == 0, "fit(%d) = %zu split a 2-byte sequence", px, n);
        CHECK(n <= strlen(cyr), "fit(%d) = %zu past end", px, n);
    }
    /* Enough room for everything returns the whole string. */
    CHECK(ui_font_fit(s, 1000) == strlen(s), "generous fit should take all");
    CHECK(ui_font_fit(s, 0) == 0, "zero width fits nothing");
}

/*
 * The measured height and the drawn height must agree exactly. The chat view
 * lays bubbles out bottom-up, so a one-row disagreement misplaces every message
 * above it.
 */
static void test_wrap_agreement(void)
{
    static const char *cases[] = {
        "",
        "short",
        "a somewhat longer line that will certainly need wrapping at 100px",
        "supercalifragilisticexpialidocious_with_no_spaces_at_all_anywhere",
        "line one\nline two\nline three",
        "trailing spaces      and   multiple    gaps",
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 "
        "\xD0\xBC\xD0\xB8\xD1\x80 "
        "\xD1\x8D\xD1\x82\xD0\xBE \xD1\x82\xD0\xB5\xD1\x81\xD1\x82",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int w = 12; w <= 120; w += 9) {
            int measured = ui_gfx_wrap_rows(w, cases[i]);
            int drawn = ui_gfx_text_wrap(0, 0, w, 8, 64, cases[i], true);
            CHECK(measured == drawn,
                  "case %zu w=%d: measured %d rows, drew %d", i, w, measured, drawn);
            CHECK(measured <= 64, "case %zu w=%d: %d rows is implausible",
                  i, w, measured);
        }
    }
    /* A word longer than the line must still be emitted, broken mid-word. */
    int rows = ui_gfx_wrap_rows(20, "unbreakablelongword");
    CHECK(rows > 1, "long word should wrap onto %d rows", rows);
    /* max_lines must actually cap the output. */
    int capped = ui_gfx_text_wrap(0, 0, 30, 8, 2,
                                  "one two three four five six seven eight", true);
    CHECK(capped == 2, "max_lines ignored: drew %d", capped);
}

/* ---- Keyboard ------------------------------------------------------------ */

/* Walk focus to a specific row/col so a test can "press" a named key. */
static void kbd_focus(ui_kbd_t *k, int row, int col)
{
    while (k->row != row) {
        (void)ui_kbd_key(k, JPP_SDK_KEY_DOWN);
    }
    k->col = col;
}

static void test_keyboard_editing(void)
{
    char buf[64] = { 0 };
    ui_kbd_t k;
    ui_kbd_init(&k, buf, sizeof(buf), "Test");
    CHECK(k.len == 0 && k.caret == 0, "fresh buffer should be empty");

    /* The Russian layer is the default, and its first row is ЙЦУКЕН. Typing the
       first key must insert two bytes, not one. */
    kbd_focus(&k, 0, 0);
    CHECK(ui_kbd_key(&k, JPP_SDK_KEY_CENTER) == UI_KBD_CONTINUE, "typing continues");
    CHECK(k.len == 2, "Cyrillic insert wrote %zu bytes, want 2", k.len);
    CHECK(ui_utf8_len(buf) == 1, "should be one character");
    size_t pos = 0;
    CHECK(ui_utf8_next(buf, &pos) == 0x0439, "first RU key should be 'й'");

    /* Backspace must remove the whole sequence, leaving a valid string. */
    kbd_focus(&k, 3, 3);   /* function row, del */
    (void)ui_kbd_key(&k, JPP_SDK_KEY_CENTER);
    CHECK(k.len == 0, "backspace left %zu bytes", k.len);
    CHECK(buf[0] == '\0', "buffer not terminated after backspace");

    /* Backspace on an empty buffer must be a no-op, not an underflow. */
    (void)ui_kbd_key(&k, JPP_SDK_KEY_CENTER);
    CHECK(k.len == 0 && k.caret == 0, "backspace underflowed");

    /* Shift is one-shot and uppercases Cyrillic correctly. */
    kbd_focus(&k, 3, 0);   /* shift */
    (void)ui_kbd_key(&k, JPP_SDK_KEY_CENTER);
    CHECK(k.shift, "shift should latch");
    kbd_focus(&k, 0, 0);
    (void)ui_kbd_key(&k, JPP_SDK_KEY_CENTER);
    CHECK(!k.shift, "shift should clear after one character");
    pos = 0;
    CHECK(ui_utf8_next(buf, &pos) == 0x0419, "shifted 'й' should be 'Й'");

    /* OK commits, BACK cancels. */
    kbd_focus(&k, 3, 4);
    CHECK(ui_kbd_key(&k, JPP_SDK_KEY_CENTER) == UI_KBD_COMMIT, "OK should commit");
    CHECK(ui_kbd_key(&k, JPP_SDK_KEY_BACK) == UI_KBD_CANCEL, "BACK should cancel");
}

static void test_keyboard_overflow(void)
{
    /* A buffer too small for another Cyrillic character must refuse the insert
       rather than write one byte of a two-byte sequence. */
    char buf[6] = { 0 };
    ui_kbd_t k;
    ui_kbd_init(&k, buf, sizeof(buf), NULL);
    kbd_focus(&k, 0, 0);
    for (int i = 0; i < 20; i++) {
        (void)ui_kbd_key(&k, JPP_SDK_KEY_CENTER);
    }
    CHECK(k.len <= sizeof(buf) - 1, "len %zu exceeds capacity", k.len);
    CHECK(buf[sizeof(buf) - 1] == '\0' || k.len < sizeof(buf),
          "buffer not terminated");
    /* Whatever landed must still be valid UTF-8: every character decodes and the
       decoded length accounts for all the bytes. */
    size_t pos = 0;
    while (pos < k.len) {
        CHECK(ui_utf8_next(buf, &pos) != 0xFFFD,
              "overflow produced an invalid sequence");
    }
    CHECK(pos == k.len, "decode consumed %zu of %zu bytes", pos, k.len);
}

static void test_keyboard_navigation(void)
{
    char buf[32] = { 0 };
    ui_kbd_t k;
    ui_kbd_init(&k, buf, sizeof(buf), NULL);

    /* Focus must stay on a real key through every layer and every wrap. */
    for (int i = 0; i < 200; i++) {
        jpp_sdk_key_event_t ev;
        switch (i % 5) {
        case 0: ev = JPP_SDK_KEY_DOWN;  break;
        case 1: ev = JPP_SDK_KEY_RIGHT; break;
        case 2: ev = JPP_SDK_KEY_UP;    break;
        case 3: ev = JPP_SDK_KEY_LEFT;  break;
        default:
            /* Cycle the layer via the Lang key without committing. */
            k.row = 3; k.col = 1;
            ev = JPP_SDK_KEY_CENTER;
            break;
        }
        (void)ui_kbd_key(&k, ev);
        CHECK(k.row >= 0 && k.row < 4, "row out of range: %d", k.row);
        CHECK(k.col >= 0, "col negative: %d", k.col);
        CHECK(k.layer < UI_KBD_LAYER_COUNT, "layer out of range");
    }

    /* digits_only must never leave focus on a hidden row. */
    ui_kbd_init(&k, buf, sizeof(buf), NULL);
    k.digits_only = true;
    for (int i = 0; i < 40; i++) {
        (void)ui_kbd_key(&k, JPP_SDK_KEY_DOWN);
        CHECK(k.row == 0 || k.row == 3,
              "digits_only focused hidden row %d", k.row);
    }
}

/* ---- TL object skipping -------------------------------------------------- */

/*
 * The skipper is the piece most likely to break silently: mis-size one optional
 * field and the vector walk lands mid-object, producing garbage that still looks
 * like plausible TL. Each vector below was encoded independently from the schema,
 * so consuming exactly its length is a real check.
 */
static void test_skip_vectors(void)
{
    for (size_t i = 0; i < skip_case_count; i++) {
        const skip_case_t *c = skip_cases[i];
        mtp_r_t r;
        mtp_r_init(&r, c->data, c->len);
        mtp_skip_result_t res = mtp_skip(&r, c->type_index);

        if (c->expect_opaque) {
            CHECK(res == MTP_SKIP_OPAQUE,
                  "%s: expected OPAQUE, got %d", c->name, (int)res);
            continue;
        }
        CHECK(res == MTP_SKIP_OK, "%s: skip returned %d", c->name, (int)res);
        if (res != MTP_SKIP_OK) {
            continue;
        }
        CHECK(r.pos == c->len,
              "%s: consumed %zu of %zu bytes", c->name, r.pos, c->len);
        CHECK(mtp_r_ok(&r), "%s: reader errored", c->name);
    }
}

/* A truncated object must be refused, never read past the buffer. */
static void test_skip_truncated(void)
{
    for (size_t i = 0; i < skip_case_count; i++) {
        const skip_case_t *c = skip_cases[i];
        if (c->expect_opaque) {
            continue;
        }
        for (size_t cut = 1; cut < c->len; cut += 3) {
            mtp_r_t r;
            mtp_r_init(&r, c->data, cut);
            mtp_skip_result_t res = mtp_skip(&r, c->type_index);
            CHECK(res != MTP_SKIP_OK || r.pos <= cut,
                  "%s: truncated at %zu read past the end", c->name, cut);
            CHECK(r.pos <= cut, "%s: pos %zu exceeds %zu", c->name, r.pos, cut);
        }
    }
}

/* An unknown constructor is reported as opaque rather than mis-skipped — the
   behaviour that keeps a future layer bump degrading instead of breaking. */
static void test_skip_unknown_ctor(void)
{
    uint8_t buf[16];
    mtp_wr_u32le(buf, 0xDEADBEEFu);
    memset(buf + 4, 0, sizeof(buf) - 4);

    mtp_r_t r;
    mtp_r_init(&r, buf, sizeof(buf));
    CHECK(mtp_skip(&r, MTP_T_USER) == MTP_SKIP_OPAQUE,
          "unknown constructor should be opaque");

    /* Out-of-range type index must not index the table. */
    mtp_r_init(&r, buf, sizeof(buf));
    CHECK(mtp_skip(&r, 9999) == MTP_SKIP_OPAQUE,
          "bogus type index should not be dereferenced");
}

/* Every constructor in the table must be findable by id, and the per-type
   constructor lists must be sorted for the binary search to work. */
static void test_skip_table_integrity(void)
{
    for (size_t t = 0; t < mtp_skip_type_count; t++) {
        const mtp_skip_type_t *ty = &mtp_skip_types[t];
        for (unsigned i = 1; i < ty->n_ctors; i++) {
            CHECK(mtp_skip_ctors[ty->first_ctor + i].id >
                  mtp_skip_ctors[ty->first_ctor + i - 1].id,
                  "type %zu constructors not sorted at %u", t, i);
        }
        for (unsigned i = 0; i < ty->n_ctors; i++) {
            uint32_t id = mtp_skip_ctors[ty->first_ctor + i].id;
            CHECK(mtp_skip_find(t, id) == &mtp_skip_ctors[ty->first_ctor + i],
                  "type %zu ctor 0x%08x not findable", t, id);
        }
    }
}

/* ---- Two-factor key derivation ------------------------------------------ */

/*
 * PBKDF2-HMAC-SHA512 is the expensive half of the 2FA exchange and the half with
 * no feedback: a wrong derivation is indistinguishable from a wrong password. The
 * vectors below come from an independent implementation, so agreement means the
 * construction is right rather than merely self-consistent.
 */
static void test_hmac_sha512(void)
{
    /* RFC 4231 test case 1 */
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    static const uint8_t want1[64] = {
        0x87, 0xAA, 0x7C, 0xDE, 0xA5, 0xEF, 0x61, 0x9D, 0x4F, 0xF0, 0xB4, 0x24,
        0x1A, 0x1D, 0x6C, 0xB0, 0x23, 0x79, 0xF4, 0xE2, 0xCE, 0x4E, 0xC2, 0x78,
        0x7A, 0xD0, 0xB3, 0x05, 0x45, 0xE1, 0x7C, 0xDE, 0xDA, 0xA8, 0x33, 0xB7,
        0xD6, 0xB8, 0xA7, 0x02, 0x03, 0x8B, 0x27, 0x4E, 0xAE, 0xA3, 0xF4, 0xE4,
        0xBE, 0x9D, 0x91, 0x4E, 0xEB, 0x61, 0xF1, 0x70, 0x2E, 0x69, 0x6C, 0x20,
        0x3A, 0x12, 0x68, 0x54,
    };
    uint8_t got[64];
    CHECK(mtp_hmac_sha512(key1, sizeof(key1), (const uint8_t *)"Hi There", 8, got) == MTP_OK,
          "hmac case 1 failed");
    CHECK(memcmp(got, want1, sizeof(want1)) == 0, "hmac case 1 mismatch");

    /* RFC 4231 test case 2 — key shorter than the block, longer data */
    static const uint8_t want2[64] = {
        0x16, 0x4B, 0x7A, 0x7B, 0xFC, 0xF8, 0x19, 0xE2, 0xE3, 0x95, 0xFB, 0xE7,
        0x3B, 0x56, 0xE0, 0xA3, 0x87, 0xBD, 0x64, 0x22, 0x2E, 0x83, 0x1F, 0xD6,
        0x10, 0x27, 0x0C, 0xD7, 0xEA, 0x25, 0x05, 0x54, 0x97, 0x58, 0xBF, 0x75,
        0xC0, 0x5A, 0x99, 0x4A, 0x6D, 0x03, 0x4F, 0x65, 0xF8, 0xF0, 0xE6, 0xFD,
        0xCA, 0xEA, 0xB1, 0xA3, 0x4D, 0x4A, 0x6B, 0x4B, 0x63, 0x6E, 0x07, 0x0A,
        0x38, 0xBC, 0xE7, 0x37,
    };
    CHECK(mtp_hmac_sha512((const uint8_t *)"Jefe", 4,
                          (const uint8_t *)"what do ya want for nothing?", 28,
                          got) == MTP_OK, "hmac case 2 failed");
    CHECK(memcmp(got, want2, sizeof(want2)) == 0, "hmac case 2 mismatch");

    /* Key longer than the 128-byte block, which must be hashed first */
    uint8_t key3[200];
    memset(key3, 0xaa, sizeof(key3));
    static const uint8_t want3[64] = {
        0x9D, 0xC6, 0x33, 0x0F, 0x4C, 0x96, 0x6B, 0x62, 0xB7, 0x35, 0xD5, 0x65,
        0x34, 0x3C, 0xB7, 0x74, 0x13, 0xDE, 0xCC, 0xDF, 0x42, 0xA9, 0x2D, 0x9E,
        0xF5, 0xE4, 0xE2, 0xAE, 0x33, 0xF6, 0xC9, 0x24, 0xBB, 0xC8, 0xE3, 0x4C,
        0x47, 0x11, 0x1B, 0xC0, 0x69, 0x48, 0x2D, 0x4D, 0xBC, 0xFE, 0xE1, 0x48,
        0x41, 0x9A, 0x65, 0x47, 0xF2, 0xD0, 0x15, 0x00, 0xE8, 0x16, 0x0B, 0x39,
        0xCC, 0x2E, 0x4A, 0xE8,
    };
    static const char *msg3 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK(mtp_hmac_sha512(key3, sizeof(key3), (const uint8_t *)msg3, strlen(msg3),
                          got) == MTP_OK, "hmac long-key failed");
    CHECK(memcmp(got, want3, sizeof(want3)) == 0, "hmac long-key mismatch");
}

static void test_pbkdf2_sha512(void)
{
    uint8_t got[64];
    /* pw='password' salt='salt' iterations=1 */
    static const uint8_t want0[64] = {
        0x86, 0x7F, 0x70, 0xCF, 0x1A, 0xDE, 0x02, 0xCF, 0xF3, 0x75, 0x25, 0x99,
        0xA3, 0xA5, 0x3D, 0xC4, 0xAF, 0x34, 0xC7, 0xA6, 0x69, 0x81, 0x5A, 0xE5,
        0xD5, 0x13, 0x55, 0x4E, 0x1C, 0x8C, 0xF2, 0x52, 0xC0, 0x2D, 0x47, 0x0A,
        0x28, 0x5A, 0x05, 0x01, 0xBA, 0xD9, 0x99, 0xBF, 0xE9, 0x43, 0xC0, 0x8F,
        0x05, 0x02, 0x35, 0xD7, 0xD6, 0x8B, 0x1D, 0xA5, 0x5E, 0x63, 0xF7, 0x3B,
        0x60, 0xA5, 0x7F, 0xCE,
    };
    CHECK(mtp_pbkdf2_sha512((const uint8_t *)"password", 8,
                            (const uint8_t *)"salt", 4,
                            1, got, sizeof(got), NULL, NULL) == MTP_OK,
          "pbkdf2 case 0 failed");
    CHECK(memcmp(got, want0, sizeof(want0)) == 0, "pbkdf2 case 0 mismatch");

    /* pw='password' salt='salt' iterations=2 */
    static const uint8_t want1[64] = {
        0xE1, 0xD9, 0xC1, 0x6A, 0xA6, 0x81, 0x70, 0x8A, 0x45, 0xF5, 0xC7, 0xC4,
        0xE2, 0x15, 0xCE, 0xB6, 0x6E, 0x01, 0x1A, 0x2E, 0x9F, 0x00, 0x40, 0x71,
        0x3F, 0x18, 0xAE, 0xFD, 0xB8, 0x66, 0xD5, 0x3C, 0xF7, 0x6C, 0xAB, 0x28,
        0x68, 0xA3, 0x9B, 0x9F, 0x78, 0x40, 0xED, 0xCE, 0x4F, 0xEF, 0x5A, 0x82,
        0xBE, 0x67, 0x33, 0x5C, 0x77, 0xA6, 0x06, 0x8E, 0x04, 0x11, 0x27, 0x54,
        0xF2, 0x7C, 0xCF, 0x4E,
    };
    CHECK(mtp_pbkdf2_sha512((const uint8_t *)"password", 8,
                            (const uint8_t *)"salt", 4,
                            2, got, sizeof(got), NULL, NULL) == MTP_OK,
          "pbkdf2 case 1 failed");
    CHECK(memcmp(got, want1, sizeof(want1)) == 0, "pbkdf2 case 1 mismatch");

    /* pw='password' salt='salt' iterations=4096 */
    static const uint8_t want2[64] = {
        0xD1, 0x97, 0xB1, 0xB3, 0x3D, 0xB0, 0x14, 0x3E, 0x01, 0x8B, 0x12, 0xF3,
        0xD1, 0xD1, 0x47, 0x9E, 0x6C, 0xDE, 0xBD, 0xCC, 0x97, 0xC5, 0xC0, 0xF8,
        0x7F, 0x69, 0x02, 0xE0, 0x72, 0xF4, 0x57, 0xB5, 0x14, 0x3F, 0x30, 0x60,
        0x26, 0x41, 0xB3, 0xD5, 0x5C, 0xD3, 0x35, 0x98, 0x8C, 0xB3, 0x6B, 0x84,
        0x37, 0x60, 0x60, 0xEC, 0xD5, 0x32, 0xE0, 0x39, 0xB7, 0x42, 0xA2, 0x39,
        0x43, 0x4A, 0xF2, 0xD5,
    };
    CHECK(mtp_pbkdf2_sha512((const uint8_t *)"password", 8,
                            (const uint8_t *)"salt", 4,
                            4096, got, sizeof(got), NULL, NULL) == MTP_OK,
          "pbkdf2 case 2 failed");
    CHECK(memcmp(got, want2, sizeof(want2)) == 0, "pbkdf2 case 2 mismatch");

    /* A short output length must take a prefix, not re-derive. */
    uint8_t short_out[20];
    CHECK(mtp_pbkdf2_sha512((const uint8_t *)"password", 8,
                            (const uint8_t *)"salt", 4, 2, short_out,
                            sizeof(short_out), NULL, NULL) == MTP_OK,
          "pbkdf2 short output failed");
    CHECK(memcmp(short_out, want1, sizeof(short_out)) == 0,
          "pbkdf2 short output should prefix the full derivation");
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    printf("mtproto host tests\n");
    /* The arena, the model tables and the framebuffer live in one heap block on
       device (see mtp_mem.h); the tests need the same block for the same
       reason. */
    if (mtp_mem_init() != MTP_OK ||
        mtp_scratch_mem_init() != MTP_OK ||
        ui_gfx_mem_init() != MTP_OK) {
        printf("mtp_mem_init failed\n");
        return 1;
    }
    test_tl_scalars();
    test_tl_bytes_padding();
    test_tl_reader_bounds();
    test_tl_str_utf8();
    test_tl_pad_random();
    test_pq_mulmod();
    test_pq_factor();
    test_be_trim();
    test_hex_decode();
    test_parse_u32();
    test_config_and_fingerprint();
    test_aes_ige_roundtrip();
    test_modexp();
    test_gzip_inflate();
    test_utf8_decode();
    test_font_coverage();
    test_font_fit();
    test_wrap_agreement();
    test_keyboard_editing();
    test_keyboard_overflow();
    test_keyboard_navigation();
    test_skip_vectors();
    test_skip_truncated();
    test_skip_unknown_ctor();
    test_skip_table_integrity();
    test_hmac_sha512();
    test_pbkdf2_sha512();

    printf("%d checks, %d failed\n", s_ran, s_fail);
    return s_fail == 0 ? 0 : 1;
}
