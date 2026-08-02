#include "mtp_store.h"

#include <stdio.h>
#include <string.h>

#pragma GCC visibility push(hidden)

/*
 * On-disk format. The magic and version exist so that a future layout change is
 * detected and the session simply discarded, rather than a stale file being read
 * as a valid key — which would present as an unexplained -404 from the DC.
 */
#define STORE_MAGIC   0x4A50544Du   /* "JPTM" */
#define STORE_VERSION 1u

#define STORE_DIR "/sd/apps/mtproto_client"

/* The last-used mode is small and non-critical, so it lives in the KV store
   rather than a file of its own. */
#define LAST_MODE_FILE STORE_DIR "/last_mode"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t  auth_key[MTP_AUTH_KEY_BYTES];
    uint64_t server_salt;
    int32_t  dc_id;
    int64_t  user_id;
    uint8_t  dh_prime_verified;
    uint8_t  logged_in;
    uint8_t  reserved[6];       /* keeps the struct 8-byte aligned and leaves
                                   room to add a field without a version bump */
} store_file_t;

static void session_path(mtp_mode_t mode, char *out, size_t out_cap)
{
    /* Named by mode index rather than by profile name: the name is user-facing
       text that could change, while the index is stable. */
    snprintf(out, out_cap, STORE_DIR "/session-%d.bin", (int)mode);
}

mtp_err_t mtp_store_load(mtp_mode_t mode, mtp_session_data_t *out)
{
    memset(out, 0, sizeof(*out));
    if (mode < 0 || mode >= MTP_MODE_COUNT) {
        return MTP_ERR_ARG;
    }

    char path[96];
    session_path(mode, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return MTP_ERR_STORE;
    }
    store_file_t rec;
    size_t got = fread(&rec, 1u, sizeof(rec), f);
    fclose(f);

    if (got != sizeof(rec) || rec.magic != STORE_MAGIC || rec.version != STORE_VERSION) {
        return MTP_ERR_STORE;
    }

    /* An all-zero key means a previous save was interrupted. Treat it as absent
       rather than handing mtp_sess_set_auth_key something it will reject. */
    bool all_zero = true;
    for (size_t i = 0u; i < MTP_AUTH_KEY_BYTES; i++) {
        if (rec.auth_key[i] != 0u) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return MTP_ERR_STORE;
    }

    memcpy(out->auth_key, rec.auth_key, MTP_AUTH_KEY_BYTES);
    out->server_salt = rec.server_salt;
    out->dc_id = rec.dc_id;
    out->user_id = rec.user_id;
    out->dh_prime_verified = rec.dh_prime_verified != 0u;
    out->logged_in = rec.logged_in != 0u;
    return MTP_OK;
}

mtp_err_t mtp_store_save(mtp_mode_t mode, const mtp_session_data_t *data)
{
    if (mode < 0 || mode >= MTP_MODE_COUNT) {
        return MTP_ERR_ARG;
    }

    store_file_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = STORE_MAGIC;
    rec.version = STORE_VERSION;
    memcpy(rec.auth_key, data->auth_key, MTP_AUTH_KEY_BYTES);
    rec.server_salt = data->server_salt;
    rec.dc_id = data->dc_id;
    rec.user_id = data->user_id;
    rec.dh_prime_verified = data->dh_prime_verified ? 1u : 0u;
    rec.logged_in = data->logged_in ? 1u : 0u;

    char path[96];
    session_path(mode, path, sizeof(path));

    /*
     * Write to a temporary file and rename over the target. An SD card pulled
     * mid-write would otherwise leave a half-written key, which reads back as a
     * valid file with a corrupt key — and that costs the user a re-login for no
     * visible reason. rename() is in the symbol table.
     */
    char tmp[112];   /* path plus ".tmp" */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return MTP_ERR_STORE;
    }
    size_t wrote = fwrite(&rec, 1u, sizeof(rec), f);
    int flushed = fflush(f);
    fclose(f);
    if (wrote != sizeof(rec) || flushed != 0) {
        remove(tmp);
        return MTP_ERR_STORE;
    }
    /* FATFS rename fails onto an existing name, so clear the way first. */
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return MTP_ERR_STORE;
    }
    return MTP_OK;
}

mtp_err_t mtp_store_clear(mtp_mode_t mode)
{
    if (mode < 0 || mode >= MTP_MODE_COUNT) {
        return MTP_ERR_ARG;
    }
    char path[96];
    session_path(mode, path, sizeof(path));
    /* A missing file is the desired end state, so absence is not an error. */
    (void)remove(path);
    return MTP_OK;
}

mtp_mode_t mtp_store_last_mode(void)
{
    FILE *f = fopen(LAST_MODE_FILE, "rb");
    if (f == NULL) {
        return MTP_MODE_COUNT;
    }
    int value = -1;
    unsigned char byte;
    if (fread(&byte, 1u, 1u, f) == 1u) {
        value = (int)byte - '0';
    }
    fclose(f);
    if (value < 0 || value >= (int)MTP_MODE_COUNT) {
        return MTP_MODE_COUNT;
    }
    return (mtp_mode_t)value;
}

void mtp_store_set_last_mode(mtp_mode_t mode)
{
    if (mode < 0 || mode >= MTP_MODE_COUNT) {
        return;
    }
    FILE *f = fopen(LAST_MODE_FILE, "wb");
    if (f == NULL) {
        return;
    }
    unsigned char byte = (unsigned char)('0' + (int)mode);
    (void)fwrite(&byte, 1u, 1u, f);
    fclose(f);
}

#pragma GCC visibility pop
