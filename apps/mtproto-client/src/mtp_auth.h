/*
 * mtp_auth — the authorization-key handshake (PQ proof-of-work + Diffie-Hellman).
 *
 * Run once per datacentre. The resulting 256-byte auth_key is what every later
 * message is encrypted under, so it is persisted and this whole exchange is
 * skipped on subsequent launches.
 *
 * Two details separate this from the naive implementation:
 *
 *  - req_DH_params uses RSA_PAD, the padding scheme Telegram moved to in 2021.
 *    The older "SHA1 || data || zero padding" form is what most tutorials show
 *    and current servers reject it. RSA_PAD reverses the padded plaintext,
 *    wraps it in AES-256-IGE under a random throwaway key, and folds that key
 *    into the block XORed with the ciphertext's own hash — then retries if the
 *    result is numerically larger than the modulus.
 *
 *  - The server's DH parameters are checked, not trusted. A malicious dh_prime
 *    or g_a would let the server pick the shared secret. A full safe-prime test
 *    on 2048 bits is far too slow here, so the common case is recognised by
 *    hashing: Telegram's long-standing prime is verified offline and identified
 *    on-device by its SHA-256. Anything else gets the structural checks only,
 *    and is flagged as unverified rather than silently accepted.
 */
#pragma once

#include "mtp_common.h"
#include "mtp_config.h"
#include "jpp_sdk_bridge.h"

/* Progress callback so the login screen can show which step is running; the
   handshake takes a few seconds, most of it in two modular exponentiations. */
typedef void (*mtp_auth_progress_fn)(void *user, const char *step, int percent);

typedef struct {
    uint8_t  auth_key[MTP_AUTH_KEY_BYTES];
    uint64_t server_salt;
    /* True when dh_prime was recognised as the offline-verified safe prime.
       False means only the structural checks passed — worth surfacing, since it
       is the difference between "proven safe" and "not obviously broken". */
    bool     dh_prime_verified;
} mtp_auth_result_t;

/*
 * Perform the full exchange on an already-connected transport. Returns
 * MTP_ERR_AUTH_KEY if the server refused or the parameter checks failed, and
 * MTP_ERR_PROTO for a malformed reply.
 */
mtp_err_t mtp_auth_handshake(jpp_sdk_context_t *ctx,
                             const mtp_profile_t *profile,
                             int dc_id,
                             mtp_auth_progress_fn progress,
                             void *progress_user,
                             mtp_auth_result_t *out);
