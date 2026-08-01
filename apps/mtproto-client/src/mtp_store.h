/*
 * mtp_store — persisting the authorization key across launches.
 *
 * The PQ/DH handshake costs a few seconds and two 2048-bit modular
 * exponentiations, and the login flow costs the user an SMS. Neither should
 * happen twice, so the auth_key, salt and home DC are written to the app's
 * scoped storage and reloaded on start.
 *
 * Written with fopen/fwrite rather than jpp_sdk_file_write, which takes a
 * NUL-terminated `const char *` and so cannot store a 256-byte key — any key
 * containing a zero byte would be silently truncated. The raw stdio calls are in
 * the firmware's symbol table and are binary-safe.
 *
 * One file per server profile: switching between Telegram and a custom server
 * should not log you out of either.
 */
#pragma once

#include "mtp_common.h"
#include "mtp_config.h"

typedef struct {
    uint8_t  auth_key[MTP_AUTH_KEY_BYTES];
    uint64_t server_salt;
    int32_t  dc_id;
    int64_t  user_id;        /* 0 until the login flow completes */
    bool     dh_prime_verified;
    bool     logged_in;      /* auth_key exists *and* an account is signed in */
} mtp_session_data_t;

/*
 * Load the stored session for a mode. Returns MTP_ERR_STORE when there is
 * nothing to load or the file is unusable — both meaning "start fresh", which is
 * a normal state rather than a failure worth showing the user.
 */
mtp_err_t mtp_store_load(mtp_mode_t mode, mtp_session_data_t *out);

mtp_err_t mtp_store_save(mtp_mode_t mode, const mtp_session_data_t *data);

/* Forget a session — the log-out path, and the recovery path when a DC rejects
   our key with transport error -404. */
mtp_err_t mtp_store_clear(mtp_mode_t mode);

/* Which mode was last used, so the app can reconnect without asking. Returns
   MTP_MODE_COUNT when nothing has been chosen yet. */
mtp_mode_t mtp_store_last_mode(void);
void       mtp_store_set_last_mode(mtp_mode_t mode);
