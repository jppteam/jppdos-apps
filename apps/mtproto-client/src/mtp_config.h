/*
 * mtp_config — the three server profiles.
 *
 * A profile is everything that differs between "which Telegram is this": the
 * datacentre list, the RSA public keys the DC will accept for the handshake, and
 * the api_id/api_hash the account layer authenticates the *application* with.
 *
 * Telegram's profile is complete and real. j++gram's values are not knowable
 * from this repository, so they are named placeholder constants gathered in one
 * block at the top of mtp_config.c — see the comment there. Custom is read from
 * the SD card at runtime.
 *
 * Key fingerprints are computed at startup rather than hardcoded: the
 * fingerprint is the low 64 bits of SHA-1 over the TL-serialised (modulus,
 * exponent) pair, so deriving it from the key we actually carry removes a
 * transcription error that would only ever show up as an unexplained handshake
 * failure.
 */
#pragma once

#include "mtp_common.h"
#include "jpp_sdk_bridge.h"

typedef enum {
    MTP_MODE_TELEGRAM = 0,
    MTP_MODE_JPPGRAM  = 1,
    MTP_MODE_CUSTOM   = 2,
    MTP_MODE_COUNT    = 3,
} mtp_mode_t;

typedef struct {
    const uint8_t *modulus;
    size_t         modulus_len;
    const uint8_t *exponent;
    size_t         exponent_len;
    uint64_t       fingerprint;   /* filled by mtp_config_init */
} mtp_rsa_key_t;

typedef struct {
    int         id;               /* DC number, as the server refers to it */
    const char *host;
    uint16_t    port;
} mtp_dc_t;

typedef struct {
    const char          *name;         /* shown in the mode picker */
    const mtp_dc_t      *dcs;
    size_t               dc_count;
    int                  default_dc;
    const mtp_rsa_key_t *keys;
    size_t               key_count;
    uint32_t             api_id;
    const char          *api_hash;
    /* Reported to the server in initConnection; shows up in the user's list of
       active sessions, so it should say what this device actually is. */
    const char          *device_model;
    const char          *system_version;
    const char          *app_version;
    const char          *lang_code;
} mtp_profile_t;

/*
 * Compute every key fingerprint and load the Custom profile from
 * /sd/apps/mtproto_client/custom.conf if it is present. A missing or malformed
 * custom.conf is not an error here — it only makes MTP_MODE_CUSTOM unusable,
 * which mtp_config_check reports when that mode is actually selected.
 */
mtp_err_t mtp_config_init(jpp_sdk_context_t *ctx);

const mtp_profile_t *mtp_config_profile(mtp_mode_t mode);

/*
 * Whether a profile is actually usable: real api credentials, at least one DC
 * and at least one RSA key. Returns MTP_ERR_NO_CONFIG for a profile still
 * carrying placeholders, so the UI can say "not configured" instead of letting
 * the user watch a handshake fail for no visible reason.
 */
mtp_err_t mtp_config_check(mtp_mode_t mode);

/* Look up a DC by id; NULL when the profile does not list it. Used on migration,
   where the server names a DC number and expects us to know its address. */
const mtp_dc_t *mtp_config_find_dc(const mtp_profile_t *profile, int dc_id);

/* Find the key matching one of the fingerprints the server offered. NULL when
   there is no overlap, which means this build cannot talk to that DC. */
const mtp_rsa_key_t *mtp_config_match_key(const mtp_profile_t *profile,
                                          const uint64_t *fingerprints,
                                          size_t count);

/* Human-readable reason the Custom profile failed to load, for the settings
   screen. Empty string when it loaded or was simply absent. */
const char *mtp_config_custom_error(void);
