#include "mtp_client.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mtp_schema.h"
#include "mtp_session.h"
#include "mtp_time.h"
#include "mtp_tl.h"
#include "mtp_transport.h"

#define CALL_TIMEOUT_MS 20000u

/* Telegram drops a connection that goes quiet. Ping well inside that, and ask
   the server to hang up on us after 75 s of silence so a dead WiFi link does not
   leave a socket that looks alive. */
#define PING_INTERVAL_MS   30000u
#define PING_DISCONNECT_S  75u

static jpp_sdk_context_t *s_ctx;
static mtp_conn_state_t   s_state;
static mtp_mode_t         s_mode = MTP_MODE_COUNT;
static mtp_session_data_t s_session;
static bool               s_layer_sent;
static uint32_t           s_last_ping_ms;
static bool               s_migrating;

/*
 * Where every request is staged, and the only copy of it that exists.
 *
 * Three things share this one buffer, which is why it is laid out rather than
 * simply declared:
 *
 *   [ 0 .. WRAP_MAX )         room for the invokeWithLayer(initConnection(...))
 *                             header, written *backwards* from WRAP_MAX so it
 *                             ends exactly where the body begins. The header is
 *                             variable-length, so it cannot start at a fixed
 *                             offset, but it can end at one.
 *   [ WRAP_MAX .. end )       the request body, where callers serialise via
 *                             mtp_client_req_buf().
 *
 * It also *is* the resend copy mtp_rpc holds on to: mtp_sess_send frames into
 * the TX buffer and never touches this one, so the bytes are still here when a
 * bad_server_salt or clock correction asks for the request again. That is why
 * requests are not built directly in mtp_sess_body_buf(), which would be one
 * buffer fewer but would leave nothing to resend.
 */
#define WRAP_MAX 160u
#define REQ_MAX  512u

static uint8_t s_stage[WRAP_MAX + REQ_MAX];

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

void mtp_client_init(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
    s_state = MTP_CONN_OFFLINE;
    s_mode = MTP_MODE_COUNT;
    s_layer_sent = false;
    s_migrating = false;
    memset(&s_session, 0, sizeof(s_session));
    mtp_sess_reset();
    mtp_rpc_reset();
}

mtp_conn_state_t mtp_client_state(void)      { return s_state; }
mtp_mode_t       mtp_client_mode(void)       { return s_mode; }
bool             mtp_client_is_logged_in(void) { return s_session.logged_in; }
const mtp_session_data_t *mtp_client_session(void) { return &s_session; }
void             mtp_client_reset_layer(void) { s_layer_sent = false; }

uint8_t *mtp_client_req_buf(size_t *cap)
{
    *cap = REQ_MAX;
    return s_stage + WRAP_MAX;
}

mtp_err_t mtp_client_save(void)
{
    if (s_mode >= MTP_MODE_COUNT) {
        return MTP_ERR_ARG;
    }
    /* Keep the persisted salt current: it rotates during a session, and a stale
       one costs an extra round trip on the next connect. */
    s_session.server_salt = mtp_sess_salt();
    return mtp_store_save(s_mode, &s_session);
}

void mtp_client_forget(void)
{
    if (s_mode < MTP_MODE_COUNT) {
        (void)mtp_store_clear(s_mode);
    }
    memset(&s_session, 0, sizeof(s_session));
    mtp_sess_reset();
    mtp_rpc_reset();
    s_layer_sent = false;
}

/* ---- Connect ------------------------------------------------------------- */

/* Open the socket and get an auth_key in place, reusing the stored one if it is
   for this DC. Does not touch login state. */
static mtp_err_t connect_dc(const mtp_profile_t *profile, int dc_id,
                            mtp_client_progress_fn progress, void *user)
{
    const mtp_dc_t *dc = mtp_config_find_dc(profile, dc_id);
    if (dc == NULL) {
        dc = mtp_config_find_dc(profile, profile->default_dc);
    }
    if (dc == NULL) {
        return MTP_ERR_NO_CONFIG;
    }

    mtp_tp_close(s_ctx);
    mtp_rpc_reset();
    s_layer_sent = false;

    mtp_err_t err = mtp_tp_connect(s_ctx, dc->host, dc->port);
    if (err != MTP_OK) {
        return err;
    }

    /* A stored key belongs to the DC it was negotiated with; using it elsewhere
       earns transport error -404. */
    bool reuse = s_session.dc_id == dc->id &&
                 mtp_sess_set_auth_key(s_session.auth_key) == MTP_OK;

    if (!reuse) {
        mtp_auth_result_t result;
        err = mtp_auth_handshake(s_ctx, profile, dc->id, progress, user, &result);
        if (err != MTP_OK) {
            mtp_tp_close(s_ctx);
            return err;
        }
        err = mtp_sess_set_auth_key(result.auth_key);
        if (err != MTP_OK) {
            mtp_tp_close(s_ctx);
            return err;
        }
        memcpy(s_session.auth_key, result.auth_key, MTP_AUTH_KEY_BYTES);
        s_session.server_salt = result.server_salt;
        s_session.dc_id = dc->id;
        s_session.dh_prime_verified = result.dh_prime_verified;
        /* A new key means no authorization on this DC yet. */
        s_session.logged_in = false;
        s_session.user_id = 0;
    }

    mtp_sess_set_salt(s_session.server_salt);
    mtp_sess_new_session();
    s_last_ping_ms = now_ms();
    return MTP_OK;
}

mtp_err_t mtp_client_connect(mtp_mode_t mode,
                            mtp_client_progress_fn progress, void *user)
{
    mtp_err_t err = mtp_config_check(mode);
    if (err != MTP_OK) {
        return err;
    }
    const mtp_profile_t *profile = mtp_config_profile(mode);

    s_state = MTP_CONN_CONNECTING;
    s_mode = mode;

    /* A stored session tells us which DC to go to directly, skipping a
       migration round trip. */
    mtp_session_data_t stored;
    if (mtp_store_load(mode, &stored) == MTP_OK) {
        s_session = stored;
    } else {
        memset(&s_session, 0, sizeof(s_session));
        s_session.dc_id = profile->default_dc;
    }

    int dc = s_session.dc_id != 0 ? s_session.dc_id : profile->default_dc;
    err = connect_dc(profile, dc, progress, user);
    if (err != MTP_OK) {
        s_state = MTP_CONN_OFFLINE;
        return err;
    }

    s_state = MTP_CONN_READY;
    mtp_store_set_last_mode(mode);
    (void)mtp_client_save();
    return MTP_OK;
}

void mtp_client_disconnect(void)
{
    if (s_ctx != NULL) {
        (void)mtp_rpc_flush_acks(s_ctx);
        mtp_tp_close(s_ctx);
    }
    mtp_rpc_reset();
    s_layer_sent = false;
    s_state = MTP_CONN_OFFLINE;
}

/* ---- DC migration -------------------------------------------------------- */

/*
 * Move to the DC the server named.
 *
 * Before login this is just "reconnect and handshake there" — there is no
 * authorization to carry. Once signed in, the authorization has to be exported
 * from the current DC and imported at the new one, or the user would be silently
 * logged out by what is meant to be a transparent redirect.
 */
static mtp_err_t migrate(int target_dc)
{
    const mtp_profile_t *profile = mtp_config_profile(s_mode);
    if (profile == NULL) {
        return MTP_ERR_ARG;
    }
    /*
     * One level only. Migration issues its own requests through
     * mtp_client_invoke, so a server that answers those with another _MIGRATE_
     * would otherwise recurse — and each level holds a saved copy of the request
     * it interrupted. A second redirect while handling the first is a server
     * fault, not something to chase.
     */
    if (s_migrating) {
        return MTP_ERR_MIGRATE;
    }
    s_migrating = true;

    uint8_t  export_bytes[256];
    size_t   export_len = 0u;
    int64_t  export_id = 0;
    bool     carry_auth = false;

    size_t cap;
    uint8_t *req = mtp_client_req_buf(&cap);

    if (s_session.logged_in) {
        mtp_w_t w;
        mtp_w_init(&w, req, cap);
        mtp_w_u32(&w, MTP_ID_AUTH_EXPORTAUTHORIZATION);
        mtp_w_i32(&w, target_dc);

        const uint8_t *res;
        size_t res_len;
        if (mtp_client_invoke(req, w.len, &res, &res_len) == MTP_OK) {
            mtp_r_t r;
            mtp_r_init(&r, res, res_len);
            if (mtp_r_u32(&r) == MTP_ID_AUTH_EXPORTEDAUTHORIZATION) {
                export_id = mtp_r_i64(&r);
                size_t blen = 0u;
                const uint8_t *b = mtp_r_bytes(&r, &blen);
                if (b != NULL && blen <= sizeof(export_bytes)) {
                    memcpy(export_bytes, b, blen);
                    export_len = blen;
                    carry_auth = true;
                }
            }
        }
        /* If the export failed there is nothing to carry; the user will have to
           sign in again on the new DC, which is the honest outcome rather than a
           silent half-authorized state. */
    }

    /* The key is per-DC, so force a fresh handshake rather than reusing. */
    s_session.dc_id = target_dc;
    memset(s_session.auth_key, 0, sizeof(s_session.auth_key));
    s_session.logged_in = false;
    s_session.user_id = 0;

    mtp_err_t err = connect_dc(profile, target_dc, NULL, NULL);
    if (err != MTP_OK) {
        s_migrating = false;
        return err;
    }

    if (carry_auth) {
        mtp_w_t w;
        mtp_w_init(&w, req, cap);
        mtp_w_u32(&w, MTP_ID_AUTH_IMPORTAUTHORIZATION);
        mtp_w_i64(&w, export_id);
        mtp_w_bytes(&w, export_bytes, export_len);

        const uint8_t *res;
        size_t res_len;
        if (mtp_client_invoke(req, w.len, &res, &res_len) == MTP_OK) {
            mtp_r_t r;
            mtp_r_init(&r, res, res_len);
            if (mtp_r_u32(&r) == MTP_ID_AUTH_AUTHORIZATION) {
                s_session.logged_in = true;
            }
        }
    }
    (void)mtp_client_save();
    s_migrating = false;
    return MTP_OK;
}

/* ---- Invoke -------------------------------------------------------------- */

/*
 * Wrap a request in invokeWithLayer(initConnection(...)).
 *
 * Telegram answers almost nothing on a fresh connection until it has been told
 * the layer and who is calling, so the first request of every connection carries
 * this header. Subsequent ones go bare — repeating it would just waste bytes.
 *
 * The header is built in a local and then placed so that it *ends* at the body's
 * start, in the room s_stage reserves ahead of it. Building it in place is not
 * possible — its length depends on the profile strings — and copying the body
 * instead would mean a second buffer the size of the largest request.
 */
static mtp_err_t wrap_request(const uint8_t *body, size_t len,
                              const uint8_t **out_body, size_t *out_len)
{
    if (s_layer_sent) {
        *out_body = body;
        *out_len = len;
        return MTP_OK;
    }
    const mtp_profile_t *p = mtp_config_profile(s_mode);
    if (p == NULL) {
        return MTP_ERR_ARG;
    }

    uint8_t hdr[WRAP_MAX];
    mtp_w_t w;
    mtp_w_init(&w, hdr, sizeof(hdr));
    mtp_w_u32(&w, MTP_ID_INVOKEWITHLAYER);
    mtp_w_i32(&w, MTP_LAYER);
    mtp_w_u32(&w, MTP_ID_INITCONNECTION);
    mtp_w_u32(&w, 0u);                       /* flags: no proxy, no params */
    mtp_w_i32(&w, (int32_t)p->api_id);
    mtp_w_str(&w, p->device_model);
    mtp_w_str(&w, p->system_version);
    mtp_w_str(&w, p->app_version);
    mtp_w_str(&w, p->lang_code);             /* system_lang_code */
    mtp_w_str(&w, "");                       /* lang_pack: official clients only */
    mtp_w_str(&w, p->lang_code);
    if (!mtp_w_ok(&w)) {
        /* Only reachable if a profile carries strings long enough to overflow
           WRAP_MAX — a configuration problem, not a runtime one. */
        return MTP_ERR_OVERFLOW;
    }
    /* The body must be where mtp_client_req_buf put it, or there is no reserved
       room in front of it to write into. */
    if (body != s_stage + WRAP_MAX || len > REQ_MAX) {
        return MTP_ERR_ARG;
    }

    uint8_t *start = s_stage + WRAP_MAX - w.len;
    memcpy(start, hdr, w.len);

    *out_body = start;
    *out_len = w.len + len;
    return MTP_OK;
}

mtp_err_t mtp_client_invoke(const uint8_t *body, size_t len,
                            const uint8_t **out_result, size_t *out_len)
{
    *out_result = NULL;
    *out_len = 0u;
    if (s_state != MTP_CONN_READY || !mtp_tp_is_open()) {
        return MTP_ERR_NET;
    }

    /*
     * Two attempts. The second exists for the case where the connection died
     * between requests — common on a device that sleeps — so that every caller
     * does not need its own reconnect-and-retry.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        const uint8_t *wrapped;
        size_t wrapped_len;
        mtp_err_t err = wrap_request(body, len, &wrapped, &wrapped_len);
        if (err != MTP_OK) {
            return err;
        }

        err = mtp_rpc_call(s_ctx, wrapped, wrapped_len, out_result, out_len,
                           CALL_TIMEOUT_MS);
        if (err == MTP_OK) {
            s_layer_sent = true;
            return MTP_OK;
        }
        if (err == MTP_ERR_MIGRATE) {
            int target = mtp_rpc_migrate_dc();
            if (target <= 0 || attempt > 0) {
                return err;
            }
            /*
             * Migration issues export/importAuthorization through this same
             * staging buffer, so the request being redirected has to be set
             * aside — otherwise the reissue below would send whatever migration
             * left behind. Stack rather than another static: it is live only
             * across this one call, and migrate() refuses to nest.
             */
            uint8_t saved[REQ_MAX];
            if (body != s_stage + WRAP_MAX || len > sizeof(saved)) {
                return MTP_ERR_ARG;
            }
            memcpy(saved, body, len);

            mtp_err_t merr = migrate(target);
            if (merr != MTP_OK) {
                return merr;
            }
            memcpy(s_stage + WRAP_MAX, saved, len);
            continue;   /* reissue on the new DC */
        }
        if (err == MTP_ERR_AUTH_KEY && mtp_sess_auth_key_rejected()) {
            /* The DC has forgotten our key. Nothing to do but start over. */
            mtp_client_forget();
            s_state = MTP_CONN_OFFLINE;
            return MTP_ERR_AUTH_KEY;
        }
        if (err == MTP_ERR_RPC || err == MTP_ERR_OVERFLOW || err == MTP_ERR_PROTO) {
            /* The server answered; retrying would get the same answer. */
            return err;
        }
        if (attempt > 0) {
            return err;
        }

        /* Transport-level failure — rebuild the connection and try once more. */
        const mtp_profile_t *profile = mtp_config_profile(s_mode);
        if (profile == NULL || connect_dc(profile, s_session.dc_id, NULL, NULL) != MTP_OK) {
            s_state = MTP_CONN_OFFLINE;
            return err;
        }
    }
    return MTP_ERR_NET;
}

/* ---- Main-loop servicing ------------------------------------------------- */

void mtp_client_pump(void)
{
    if (s_state != MTP_CONN_READY || !mtp_tp_is_open()) {
        return;
    }

    /* Poll, don't block: the caller is also reading the keypad. */
    mtp_err_t err = mtp_rpc_poll(s_ctx, 0u);
    if (err != MTP_OK && err != MTP_ERR_TIMEOUT) {
        if (err == MTP_ERR_AUTH_KEY && mtp_sess_auth_key_rejected()) {
            mtp_client_forget();
        }
        mtp_tp_close(s_ctx);
        s_state = MTP_CONN_OFFLINE;
        return;
    }

    uint32_t now = now_ms();
    if (now - s_last_ping_ms >= PING_INTERVAL_MS) {
        s_last_ping_ms = now;
        if (mtp_rpc_ping(s_ctx, PING_DISCONNECT_S) != MTP_OK) {
            mtp_tp_close(s_ctx);
            s_state = MTP_CONN_OFFLINE;
        }
    }
}
