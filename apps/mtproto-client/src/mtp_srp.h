/*
 * mtp_srp — the two-factor password exchange (auth.checkPassword).
 *
 * Telegram never sends the password. The client proves knowledge of it with an
 * SRP-2048 exchange whose key-derivation step is PBKDF2-HMAC-SHA512 at 100,000
 * iterations, deliberately expensive to make an intercepted srp_B useless for
 * offline guessing.
 *
 * On this hardware that expense is very visible. The ESP32-C6's crypto
 * accelerator covers SHA-1 and SHA-256 but not SHA-512, so the KDF runs in
 * software and takes on the order of ten seconds. Hence the progress callback,
 * and hence mtp_srp_derive yielding to the scheduler as it goes — a ten-second
 * busy loop would otherwise starve the connection and trip the watchdog.
 *
 * The SDK provides modexp but no modular multiply or addition, so the handful of
 * bignum operations SRP needs beyond exponentiation are implemented here.
 */
#pragma once

#include "mtp_common.h"

#define MTP_SRP_BYTES 256u   /* 2048-bit operands */

/* Server-supplied parameters, from account.password. */
typedef struct {
    uint8_t  p[MTP_SRP_BYTES];
    uint32_t g;
    uint8_t  salt1[64];
    size_t   salt1_len;
    uint8_t  salt2[64];
    size_t   salt2_len;
    uint8_t  srp_B[MTP_SRP_BYTES];
    int64_t  srp_id;
} mtp_srp_params_t;

/* What goes into inputCheckPasswordSRP. */
typedef struct {
    int64_t srp_id;
    uint8_t A[MTP_SRP_BYTES];
    uint8_t M1[32];
} mtp_srp_proof_t;

/* Reports KDF progress 0..100 so the login screen can show a bar. */
typedef void (*mtp_srp_progress_fn)(void *user, int percent);

/*
 * Compute the proof for `password`. Takes seconds; call from the app task with a
 * progress callback, never from anywhere that must stay responsive.
 */
mtp_err_t mtp_srp_compute(const mtp_srp_params_t *params, const char *password,
                          mtp_srp_progress_fn progress, void *user,
                          mtp_srp_proof_t *out);

/*
 * PBKDF2-HMAC-SHA512, exposed for the host tests — it is the part with published
 * vectors, and a wrong KDF is otherwise indistinguishable from a wrong password.
 */
mtp_err_t mtp_pbkdf2_sha512(const uint8_t *password, size_t password_len,
                            const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t *out, size_t out_len,
                            mtp_srp_progress_fn progress, void *user);

/* HMAC-SHA512, likewise. */
mtp_err_t mtp_hmac_sha512(const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len,
                          uint8_t out[64]);
