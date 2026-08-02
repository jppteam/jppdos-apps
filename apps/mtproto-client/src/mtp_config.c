#include "mtp_config.h"

#include <stdio.h>
#include <string.h>

#include "jpp_crypto_core.h"
#include "mtp_scratch.h"
#include "mtp_tl.h"

#pragma GCC visibility push(hidden)

/* ========================================================================== *
 *                        VALUES TO FILL IN BEFORE RELEASE                    *
 * ========================================================================== *
 *
 * Everything in this block is a placeholder that cannot be determined from this
 * repository. The client compiles and runs with them as-is — the affected mode
 * simply reports "Profile not configured" instead of attempting a handshake that
 * could not succeed. Replace each value and the mode starts working; no other
 * file needs touching.
 *
 * --- Telegram ---------------------------------------------------------------
 * Register an application at https://my.telegram.org/apps and paste the two
 * values it issues. They identify this *client*, not the user's account, so one
 * pair covers every device running this build. The api_hash is a 32-character
 * lowercase hex string.
 */
#define MTP_TELEGRAM_API_ID   28690885u
#define MTP_TELEGRAM_API_HASH "b0f34bebbd7142cadbefb4a0a74ff4cb"

/*
 * --- j++gram ----------------------------------------------------------------
 * The endpoint and public key of the j++gram deployment, plus its own api
 * credentials. The profile is shaped exactly like Telegram's below, so filling
 * these in is the only step.
 *
 *   HOST/PORT  the DC the client should connect to first.
 *   RSA_MODULUS_HEX  the server's 2048-bit RSA public modulus as 512 hex
 *       characters (whitespace and colons are tolerated). To extract it from a
 *       PEM public key:
 *           openssl rsa -pubin -in jppgram.pem -RSAPublicKey_out -text -noout
 *       and concatenate the "Modulus:" bytes. The exponent is almost always
 *       65537, which is the default below.
 *   API_ID/API_HASH  whatever the deployment issues; if it does not check them,
 *       any nonzero api_id and a 32-character hash will do.
 */
#define MTP_JPPGRAM_DC_ID          2
#define MTP_JPPGRAM_DC_HOST        "PUT_JPPGRAM_HOST_HERE"
#define MTP_JPPGRAM_DC_PORT        443u
#define MTP_JPPGRAM_RSA_MODULUS_HEX "PUT_JPPGRAM_RSA_MODULUS_HEX_HERE"
#define MTP_JPPGRAM_RSA_EXPONENT_HEX "010001"
#define MTP_JPPGRAM_API_ID         0u
#define MTP_JPPGRAM_API_HASH       "PUT_JPPGRAM_API_HASH_HERE"

/* ========================================================================== *
 *                          end of values to fill in                          *
 * ========================================================================== */

/* Where the Custom profile reads its settings from. Scoped storage, so it is
   reachable over the device's WebDAV server without files.full. */
#define CUSTOM_CONF_PATH "/sd/apps/mtproto_client/custom.conf"

/* Reported in initConnection and shown in the user's active-session list. */
#define DEV_MODEL   "J++Device"
#define DEV_SYSTEM  "JPPDOS"
#define DEV_APP     "0.2.0"
#define DEV_LANG    "en"

/* ---- Telegram ------------------------------------------------------------ */

/*
 * Telegram's production RSA public key — public data, shipped with every
 * Telegram client, and required to start the handshake. Its fingerprint works
 * out to 0xd09d1d85de64fd85, but that is derived at startup rather than written
 * down here (see mtp_config_init).
 */
static const uint8_t TG_MODULUS[256] = {
    0xE8, 0xBB, 0x33, 0x05, 0xC0, 0xB5, 0x2C, 0x6C, 0xF2, 0xAF, 0xDF, 0x76,
    0x37, 0x31, 0x34, 0x89, 0xE6, 0x3E, 0x05, 0x26, 0x8E, 0x5B, 0xAD, 0xB6,
    0x01, 0xAF, 0x41, 0x77, 0x86, 0x47, 0x2E, 0x5F, 0x93, 0xB8, 0x54, 0x38,
    0x96, 0x8E, 0x20, 0xE6, 0x72, 0x9A, 0x30, 0x1C, 0x0A, 0xFC, 0x12, 0x1B,
    0xF7, 0x15, 0x1F, 0x83, 0x44, 0x36, 0xF7, 0xFD, 0xA6, 0x80, 0x84, 0x7A,
    0x66, 0xBF, 0x64, 0xAC, 0xCE, 0xC7, 0x8E, 0xE2, 0x1C, 0x0B, 0x31, 0x6F,
    0x0E, 0xDA, 0xFE, 0x2F, 0x41, 0x90, 0x8D, 0xA7, 0xBD, 0x1F, 0x4A, 0x51,
    0x07, 0x63, 0x8E, 0xEB, 0x67, 0x04, 0x0A, 0xCE, 0x47, 0x2A, 0x14, 0xF9,
    0x0D, 0x9F, 0x7C, 0x2B, 0x7D, 0xEF, 0x99, 0x68, 0x8B, 0xA3, 0x07, 0x3A,
    0xDB, 0x57, 0x50, 0xBB, 0x02, 0x96, 0x49, 0x02, 0xA3, 0x59, 0xFE, 0x74,
    0x5D, 0x81, 0x70, 0xE3, 0x68, 0x76, 0xD4, 0xFD, 0x8A, 0x5D, 0x41, 0xB2,
    0xA7, 0x6C, 0xBF, 0xF9, 0xA1, 0x32, 0x67, 0xEB, 0x95, 0x80, 0xB2, 0xD0,
    0x6D, 0x10, 0x35, 0x74, 0x48, 0xD2, 0x0D, 0x9D, 0xA2, 0x19, 0x1C, 0xB5,
    0xD8, 0xC9, 0x39, 0x82, 0x96, 0x1C, 0xDF, 0xDE, 0xDA, 0x62, 0x9E, 0x37,
    0xF1, 0xFB, 0x09, 0xA0, 0x72, 0x20, 0x27, 0x69, 0x60, 0x32, 0xFE, 0x61,
    0xED, 0x66, 0x3D, 0xB7, 0xA3, 0x7F, 0x6F, 0x26, 0x3D, 0x37, 0x0F, 0x69,
    0xDB, 0x53, 0xA0, 0xDC, 0x0A, 0x17, 0x48, 0xBD, 0xAA, 0xFF, 0x62, 0x09,
    0xD5, 0x64, 0x54, 0x85, 0xE6, 0xE0, 0x01, 0xD1, 0x95, 0x32, 0x55, 0x75,
    0x7E, 0x4B, 0x8E, 0x42, 0x81, 0x33, 0x47, 0xB1, 0x1D, 0xA6, 0xAB, 0x50,
    0x0F, 0xD0, 0xAC, 0xE7, 0xE6, 0xDF, 0xA3, 0x73, 0x61, 0x99, 0xCC, 0xAF,
    0x93, 0x97, 0xED, 0x07, 0x45, 0xA4, 0x27, 0xDC, 0xFA, 0x6C, 0xD6, 0x7B,
    0xCB, 0x1A, 0xCF, 0xF3,
};

static const uint8_t E_65537[3] = { 0x01, 0x00, 0x01 };

static mtp_rsa_key_t s_tg_keys[] = {
    { TG_MODULUS, sizeof(TG_MODULUS), E_65537, sizeof(E_65537), 0u },
};

/* Telegram's production datacentres. DC2 is the one to try first: it is where
   accounts without a known home DC are steered, and the server redirects with
   PHONE_MIGRATE_n if the user belongs elsewhere. */
static const mtp_dc_t s_tg_dcs[] = {
    { 1, "149.154.175.53",  443u },
    { 2, "149.154.167.51",  443u },
    { 3, "149.154.175.100", 443u },
    { 4, "149.154.167.91",  443u },
    { 5, "91.108.56.130",   443u },
};

/* ---- j++gram ------------------------------------------------------------- */

/* Filled by mtp_config_init from the hex constants above. */
static uint8_t       s_jg_modulus[MTP_KEY_BYTES];
static size_t        s_jg_modulus_len;
static uint8_t       s_jg_exponent[8];
static size_t        s_jg_exponent_len;
static mtp_rsa_key_t s_jg_keys[1];
static size_t        s_jg_key_count;

static const mtp_dc_t s_jg_dcs[] = {
    { MTP_JPPGRAM_DC_ID, MTP_JPPGRAM_DC_HOST, MTP_JPPGRAM_DC_PORT },
};

/* ---- Custom -------------------------------------------------------------- */

static char          s_cu_host[64];
static uint16_t      s_cu_port;
static int           s_cu_dc_id = 2;
static uint8_t       s_cu_modulus[MTP_KEY_BYTES];
static size_t        s_cu_modulus_len;
static uint8_t       s_cu_exponent[8];
static size_t        s_cu_exponent_len;
static char          s_cu_api_hash[40];
static uint32_t      s_cu_api_id;
static mtp_rsa_key_t s_cu_keys[1];
static size_t        s_cu_key_count;
static mtp_dc_t      s_cu_dcs[1];
static char          s_cu_error[48];

/* ---- Profiles ------------------------------------------------------------ */

static mtp_profile_t s_profiles[MTP_MODE_COUNT];

static void init_profiles(void)
{
    s_profiles[MTP_MODE_TELEGRAM] = (mtp_profile_t){
        .name = "Telegram",
        .dcs = s_tg_dcs, .dc_count = sizeof(s_tg_dcs) / sizeof(s_tg_dcs[0]),
        .default_dc = 2,
        .keys = s_tg_keys, .key_count = sizeof(s_tg_keys) / sizeof(s_tg_keys[0]),
        .api_id = MTP_TELEGRAM_API_ID, .api_hash = MTP_TELEGRAM_API_HASH,
        .device_model = DEV_MODEL, .system_version = DEV_SYSTEM,
        .app_version = DEV_APP, .lang_code = DEV_LANG,
    };
    s_profiles[MTP_MODE_JPPGRAM] = (mtp_profile_t){
        .name = "j++gram",
        .dcs = s_jg_dcs, .dc_count = sizeof(s_jg_dcs) / sizeof(s_jg_dcs[0]),
        .default_dc = MTP_JPPGRAM_DC_ID,
        .keys = s_jg_keys, .key_count = 0u,   /* set below if the hex parsed */
        .api_id = MTP_JPPGRAM_API_ID, .api_hash = MTP_JPPGRAM_API_HASH,
        .device_model = DEV_MODEL, .system_version = DEV_SYSTEM,
        .app_version = DEV_APP, .lang_code = DEV_LANG,
    };
    s_profiles[MTP_MODE_CUSTOM] = (mtp_profile_t){
        .name = "Custom",
        .dcs = s_cu_dcs, .dc_count = 0u,
        .default_dc = 2,
        .keys = s_cu_keys, .key_count = 0u,
        .api_id = 0u, .api_hash = "",
        .device_model = DEV_MODEL, .system_version = DEV_SYSTEM,
        .app_version = DEV_APP, .lang_code = DEV_LANG,
    };
}

/*
 * Fingerprint = low 64 bits of SHA-1 over the TL serialisation of
 * (modulus, exponent) as two `bytes` fields — with no constructor id, which is
 * the detail most implementations get wrong first.
 */
static mtp_err_t compute_fingerprint(mtp_rsa_key_t *key)
{
    uint8_t buf[300];
    mtp_w_t w;
    mtp_w_init(&w, buf, sizeof(buf));
    mtp_w_bytes(&w, key->modulus, key->modulus_len);
    mtp_w_bytes(&w, key->exponent, key->exponent_len);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }
    uint8_t digest[JPP_CRYPTO_SHA1_BYTES];
    if (jpp_crypto_sha1(buf, w.len, digest) != JPP_CRYPTO_OK) {
        return MTP_ERR_CRYPTO;
    }
    key->fingerprint = mtp_rd_u64le(digest + 12);
    return MTP_OK;
}

/* True when a string still holds one of the PUT_..._HERE placeholders. */
static bool is_placeholder(const char *s)
{
    return s == NULL || s[0] == '\0' || strncmp(s, "PUT_", 4) == 0;
}

/* ---- custom.conf --------------------------------------------------------- */

/*
 * Everything the parse needs, borrowed from the shared arena rather than owned.
 *
 * This runs once at startup, long before a handshake or an SRP proof, so nothing
 * here has any business being resident for the life of the app — and it is too
 * much to put on the 12 KB task stack either, most of it because a 2048-bit
 * modulus is 512 hex characters and the line holding it has to be at least that.
 * See mtp_scratch.h for the ownership rules.
 */
typedef struct {
    char text[1400];        /* the file, read whole: there is no fgets */
    char line[640];
    char mod_hex[600];
    char host[64];
    char exp_hex[24];
    char api_hash[40];
} conf_work_t;

_Static_assert(sizeof(conf_work_t) <= MTP_SCRATCH_BYTES,
               "custom.conf working set outgrew the shared arena");

static mtp_err_t parse_custom(conf_work_t *k, size_t got)
{
    char    *host     = k->host;
    char    *mod_hex  = k->mod_hex;
    char    *exp_hex  = k->exp_hex;
    char    *api_hash = k->api_hash;
    char    *line     = k->line;
    char    *text     = k->text;
    uint32_t api_id = 0u;
    uint32_t port = 443u;
    uint32_t dc_id = 2u;

    snprintf(exp_hex, sizeof(k->exp_hex), "010001");

    size_t pos = 0u;
    while (pos < got) {
        size_t end = pos;
        while (end < got && text[end] != '\n') {
            end++;
        }
        size_t len = end - pos;
        if (len >= sizeof(k->line)) {
            len = sizeof(k->line) - 1u;
        }
        memcpy(line, text + pos, len);
        line[len] = '\0';
        pos = end + 1u;

        /* Strip trailing whitespace, including the \r of a CRLF file. */
        while (len > 0u && (line[len - 1u] == '\r' || line[len - 1u] == ' ' ||
                            line[len - 1u] == '\t')) {
            line[--len] = '\0';
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* Trim the key's trailing and the value's leading whitespace, so
           "api_id = 12345" works as well as "api_id=12345". */
        size_t klen = strlen(key);
        while (klen > 0u && (key[klen - 1u] == ' ' || key[klen - 1u] == '\t')) {
            key[--klen] = '\0';
        }
        while (*val == ' ' || *val == '\t') {
            val++;
        }

        if (strcmp(key, "dc_host") == 0) {
            snprintf(host, sizeof(k->host), "%s", val);
        } else if (strcmp(key, "dc_port") == 0) {
            (void)mtp_parse_u32(val, &port);
        } else if (strcmp(key, "dc_id") == 0) {
            (void)mtp_parse_u32(val, &dc_id);
        } else if (strcmp(key, "rsa_modulus") == 0) {
            snprintf(mod_hex, sizeof(k->mod_hex), "%s", val);
        } else if (strcmp(key, "rsa_exponent") == 0) {
            snprintf(exp_hex, sizeof(k->exp_hex), "%s", val);
        } else if (strcmp(key, "api_id") == 0) {
            (void)mtp_parse_u32(val, &api_id);
        } else if (strcmp(key, "api_hash") == 0) {
            snprintf(api_hash, sizeof(k->api_hash), "%s", val);
        }
        /* Unknown keys are ignored rather than fatal — a config file from a
           newer build should not brick an older one. */
    }

    /* Validate, and say precisely what is wrong: this file is hand-edited over
       WebDAV, so a specific message is worth far more than a generic failure. */
    if (host[0] == '\0') {
        snprintf(s_cu_error, sizeof(s_cu_error), "dc_host missing");
        return MTP_ERR_NO_CONFIG;
    }
    if (port == 0u || port > 65535u) {
        snprintf(s_cu_error, sizeof(s_cu_error), "dc_port invalid");
        return MTP_ERR_NO_CONFIG;
    }
    size_t mlen = mtp_hex_decode(mod_hex, s_cu_modulus, sizeof(s_cu_modulus));
    if (mlen == 0u) {
        snprintf(s_cu_error, sizeof(s_cu_error), "rsa_modulus invalid");
        return MTP_ERR_NO_CONFIG;
    }
    size_t elen = mtp_hex_decode(exp_hex, s_cu_exponent, sizeof(s_cu_exponent));
    if (elen == 0u) {
        snprintf(s_cu_error, sizeof(s_cu_error), "rsa_exponent invalid");
        return MTP_ERR_NO_CONFIG;
    }
    if (api_id == 0u || strlen(api_hash) == 0u) {
        snprintf(s_cu_error, sizeof(s_cu_error), "api_id/api_hash missing");
        return MTP_ERR_NO_CONFIG;
    }

    snprintf(s_cu_host, sizeof(s_cu_host), "%s", host);
    snprintf(s_cu_api_hash, sizeof(s_cu_api_hash), "%s", api_hash);
    s_cu_port = (uint16_t)port;
    s_cu_dc_id = (int)dc_id;
    s_cu_modulus_len = mlen;
    s_cu_exponent_len = elen;
    s_cu_api_id = api_id;

    s_cu_dcs[0] = (mtp_dc_t){ s_cu_dc_id, s_cu_host, s_cu_port };
    s_cu_keys[0] = (mtp_rsa_key_t){ s_cu_modulus, s_cu_modulus_len,
                                    s_cu_exponent, s_cu_exponent_len, 0u };
    s_cu_key_count = 1u;

    s_profiles[MTP_MODE_CUSTOM].dcs = s_cu_dcs;
    s_profiles[MTP_MODE_CUSTOM].dc_count = 1u;
    s_profiles[MTP_MODE_CUSTOM].default_dc = s_cu_dc_id;
    s_profiles[MTP_MODE_CUSTOM].keys = s_cu_keys;
    s_profiles[MTP_MODE_CUSTOM].key_count = 1u;
    s_profiles[MTP_MODE_CUSTOM].api_id = s_cu_api_id;
    s_profiles[MTP_MODE_CUSTOM].api_hash = s_cu_api_hash;
    return MTP_OK;
}

/*
 * Read with fopen rather than jpp_sdk_file_read: the SDK's reader stops at the
 * first NUL and truncates at 4095 bytes, and a 512-hex-character modulus plus
 * the rest of the file sits uncomfortably close to that.
 *
 * Split from parse_custom so the borrowed arena is returned on every exit path —
 * the parse has a validation failure for each field and would otherwise need a
 * release at each one.
 */
static mtp_err_t load_custom(void)
{
    s_cu_error[0] = '\0';

    FILE *f = fopen(CUSTOM_CONF_PATH, "r");
    if (f == NULL) {
        /* Absent is normal — most users never set up a custom server. */
        return MTP_OK;
    }

    conf_work_t *k = mtp_scratch_acquire(sizeof(*k));
    if (k == NULL) {
        fclose(f);
        snprintf(s_cu_error, sizeof(s_cu_error), "No scratch space");
        return MTP_ERR_OVERFLOW;
    }

    size_t got = fread(k->text, 1u, sizeof(k->text) - 1u, f);
    fclose(f);
    k->text[got] = '\0';

    mtp_err_t err = parse_custom(k, got);
    mtp_scratch_release(k);
    return err;
}

/* ---- Public API ---------------------------------------------------------- */

mtp_err_t mtp_config_init(jpp_sdk_context_t *ctx)
{
    (void)ctx;
    init_profiles();

    /* j++gram: decode the hex constants, if they have been filled in. */
    if (!is_placeholder(MTP_JPPGRAM_RSA_MODULUS_HEX)) {
        s_jg_modulus_len = mtp_hex_decode(MTP_JPPGRAM_RSA_MODULUS_HEX,
                                          s_jg_modulus, sizeof(s_jg_modulus));
        s_jg_exponent_len = mtp_hex_decode(MTP_JPPGRAM_RSA_EXPONENT_HEX,
                                           s_jg_exponent, sizeof(s_jg_exponent));
        if (s_jg_modulus_len > 0u && s_jg_exponent_len > 0u) {
            s_jg_keys[0] = (mtp_rsa_key_t){ s_jg_modulus, s_jg_modulus_len,
                                            s_jg_exponent, s_jg_exponent_len, 0u };
            s_jg_key_count = 1u;
            s_profiles[MTP_MODE_JPPGRAM].key_count = 1u;
        }
    }

    (void)load_custom();

    /* Derive every fingerprint now so the handshake never has to. */
    for (size_t m = 0u; m < MTP_MODE_COUNT; m++) {
        mtp_profile_t *p = &s_profiles[m];
        for (size_t k = 0u; k < p->key_count; k++) {
            /* Cast away const: the table is ours, and fingerprint is the one
               field that is computed rather than declared. */
            mtp_rsa_key_t *key = (mtp_rsa_key_t *)&p->keys[k];
            mtp_err_t err = compute_fingerprint(key);
            if (err != MTP_OK) {
                return err;
            }
        }
    }
    return MTP_OK;
}

const mtp_profile_t *mtp_config_profile(mtp_mode_t mode)
{
    if (mode < 0 || mode >= MTP_MODE_COUNT) {
        return NULL;
    }
    return &s_profiles[mode];
}

mtp_err_t mtp_config_check(mtp_mode_t mode)
{
    const mtp_profile_t *p = mtp_config_profile(mode);
    if (p == NULL) {
        return MTP_ERR_ARG;
    }
    if (p->dc_count == 0u || p->key_count == 0u) {
        return MTP_ERR_NO_CONFIG;
    }
    if (p->api_id == 0u || is_placeholder(p->api_hash)) {
        return MTP_ERR_NO_CONFIG;
    }
    for (size_t i = 0u; i < p->dc_count; i++) {
        if (is_placeholder(p->dcs[i].host)) {
            return MTP_ERR_NO_CONFIG;
        }
    }
    return MTP_OK;
}

const mtp_dc_t *mtp_config_find_dc(const mtp_profile_t *profile, int dc_id)
{
    if (profile == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < profile->dc_count; i++) {
        if (profile->dcs[i].id == dc_id) {
            return &profile->dcs[i];
        }
    }
    return NULL;
}

const mtp_rsa_key_t *mtp_config_match_key(const mtp_profile_t *profile,
                                          const uint64_t *fingerprints,
                                          size_t count)
{
    if (profile == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < count; i++) {
        for (size_t k = 0u; k < profile->key_count; k++) {
            if (profile->keys[k].fingerprint == fingerprints[i]) {
                return &profile->keys[k];
            }
        }
    }
    return NULL;
}

const char *mtp_config_custom_error(void) { return s_cu_error; }

#pragma GCC visibility pop
