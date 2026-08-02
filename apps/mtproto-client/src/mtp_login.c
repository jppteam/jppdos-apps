#include "mtp_login.h"

#include <stdio.h>
#include <string.h>

#include "mtp_api.h"
#include "mtp_model.h"
#include "mtp_rpc.h"
#include "mtp_schema.h"
#include "mtp_tl.h"

#pragma GCC visibility push(hidden)

static char s_phone[24];
static char s_code_hash[64];
static char s_hint[32];
static int  s_flood;
static char s_error[64];

void mtp_login_reset(void)
{
    s_phone[0] = '\0';
    s_code_hash[0] = '\0';
    s_hint[0] = '\0';
    s_flood = 0;
    s_error[0] = '\0';
}

const char *mtp_login_password_hint(void) { return s_hint; }
int         mtp_login_flood_seconds(void) { return s_flood; }
const char *mtp_login_error(void)         { return s_error; }

/* Turn an rpc_error into a typed result, recording the parts the UI needs. */
static mtp_login_result_t classify(void)
{
    const char *text = mtp_rpc_error_text();
    snprintf(s_error, sizeof(s_error), "%s", text);
    s_flood = mtp_rpc_flood_wait();

    if (strncmp(text, "FLOOD_WAIT_", 11) == 0) {
        return MTP_LOGIN_FLOOD;
    }
    if (strcmp(text, "SESSION_PASSWORD_NEEDED") == 0) {
        return MTP_LOGIN_NEEDS_PASSWORD;
    }
    if (strcmp(text, "PHONE_CODE_INVALID") == 0 ||
        strcmp(text, "PHONE_CODE_EMPTY") == 0) {
        return MTP_LOGIN_BAD_CODE;
    }
    if (strcmp(text, "PHONE_CODE_EXPIRED") == 0) {
        return MTP_LOGIN_EXPIRED;
    }
    if (strcmp(text, "PASSWORD_HASH_INVALID") == 0) {
        return MTP_LOGIN_BAD_PASSWORD;
    }
    if (strncmp(text, "PHONE_NUMBER_", 13) == 0) {
        return MTP_LOGIN_BAD_PHONE;
    }
    return MTP_LOGIN_ERROR;
}

/* ---- auth.sendCode ------------------------------------------------------- */

/* Decode auth.SentCodeType into something the UI can phrase. */
static void read_code_type(mtp_r_t *r, mtp_code_info_t *info)
{
    info->kind = MTP_CODE_UNKNOWN;
    info->length = 0;
    info->hint[0] = '\0';

    uint32_t id = mtp_r_u32(r);
    switch (id) {
    case MTP_ID_AUTH_SENTCODETYPEAPP:
        info->kind = MTP_CODE_APP;
        info->length = mtp_r_i32(r);
        break;
    case MTP_ID_AUTH_SENTCODETYPESMS:
        info->kind = MTP_CODE_SMS;
        info->length = mtp_r_i32(r);
        break;
    case MTP_ID_AUTH_SENTCODETYPECALL:
        info->kind = MTP_CODE_CALL;
        info->length = mtp_r_i32(r);
        break;
    case MTP_ID_AUTH_SENTCODETYPEMISSEDCALL:
        info->kind = MTP_CODE_MISSED_CALL;
        mtp_r_str(r, info->hint, sizeof(info->hint));   /* prefix to dial */
        info->length = mtp_r_i32(r);
        break;
    case MTP_ID_AUTH_SENTCODETYPEFRAGMENTSMS:
        info->kind = MTP_CODE_FRAGMENT;
        mtp_r_str(r, info->hint, sizeof(info->hint));   /* url */
        info->length = mtp_r_i32(r);
        break;
    default:
        /*
         * A delivery method this build does not model — email verification, a
         * Firebase push, a word or phrase instead of digits. The code still
         * arrives somehow, so the flow continues with a generic prompt rather
         * than refusing to let the user type it.
         */
        break;
    }
}

static mtp_login_result_t parse_sent_code(const uint8_t *res, size_t len,
                                          mtp_code_info_t *out)
{
    mtp_r_t r;
    mtp_r_init(&r, res, len);
    uint32_t id = mtp_r_u32(&r);

    if (id == MTP_ID_AUTH_SENTCODESUCCESS) {
        /* Already authorized — some flows short-circuit. */
        return MTP_LOGIN_OK;
    }
    if (id != MTP_ID_AUTH_SENTCODE) {
        return MTP_LOGIN_ERROR;
    }
    (void)mtp_r_u32(&r);                    /* flags */
    if (out != NULL) {
        read_code_type(&r, out);
    } else {
        mtp_code_info_t ignored;
        read_code_type(&r, &ignored);
    }
    mtp_r_str(&r, s_code_hash, sizeof(s_code_hash));
    if (!mtp_r_ok(&r) || s_code_hash[0] == '\0') {
        return MTP_LOGIN_ERROR;
    }
    return MTP_LOGIN_CODE_SENT;
}

mtp_login_result_t mtp_login_send_code(const char *phone, mtp_code_info_t *out)
{
    const mtp_profile_t *profile = mtp_config_profile(mtp_client_mode());
    if (profile == NULL) {
        return MTP_LOGIN_ERROR;
    }
    snprintf(s_phone, sizeof(s_phone), "%s", phone);
    s_error[0] = '\0';

    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_AUTH_SENDCODE);
    mtp_w_str(&w, s_phone);
    mtp_w_i32(&w, (int32_t)profile->api_id);
    mtp_w_str(&w, profile->api_hash);
    /* codeSettings with every option off: this device cannot receive a flash
       call, read an SMS, or run the app-hash flow. */
    mtp_w_u32(&w, MTP_ID_CODESETTINGS);
    mtp_w_u32(&w, 0u);
    if (!mtp_w_ok(&w)) {
        return MTP_LOGIN_ERROR;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err == MTP_ERR_RPC) {
        mtp_log("send_code_rpc_error");
        return classify();
    }
    if (err != MTP_OK) {
        char ev[48];
        snprintf(ev, sizeof(ev), "send_code_fail_%d", (int)err);
        mtp_log(ev);
        snprintf(s_error, sizeof(s_error), "%s", mtp_err_str(err));
        return MTP_LOGIN_ERROR;
    }
    return parse_sent_code(res, res_len, out);
}

mtp_login_result_t mtp_login_resend_code(mtp_code_info_t *out)
{
    if (s_code_hash[0] == '\0') {
        return MTP_LOGIN_ERROR;
    }
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_AUTH_RESENDCODE);
    mtp_w_u32(&w, 0u);                       /* flags: no reason given */
    mtp_w_str(&w, s_phone);
    mtp_w_str(&w, s_code_hash);
    if (!mtp_w_ok(&w)) {
        return MTP_LOGIN_ERROR;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err == MTP_ERR_RPC) {
        return classify();
    }
    if (err != MTP_OK) {
        return MTP_LOGIN_ERROR;
    }
    return parse_sent_code(res, res_len, out);
}

/* ---- Authorization result ------------------------------------------------ */

/*
 * auth.authorization carries the signed-in user. Recording it marks the session
 * as logged in and gives the model our own peer, which the dialog list needs to
 * label outgoing messages and find Saved Messages.
 */
static mtp_login_result_t absorb_authorization(const uint8_t *res, size_t len)
{
    mtp_r_t r;
    mtp_r_init(&r, res, len);
    uint32_t id = mtp_r_u32(&r);

    if (id == MTP_ID_AUTH_AUTHORIZATIONSIGNUPREQUIRED) {
        return MTP_LOGIN_NEEDS_SIGNUP;
    }
    if (id != MTP_ID_AUTH_AUTHORIZATION) {
        return MTP_LOGIN_ERROR;
    }

    uint32_t flags = mtp_r_u32(&r);
    if ((flags & (1u << 1)) != 0u) {
        (void)mtp_r_i32(&r);        /* otherwise_relogin_days */
    }
    if ((flags & (1u << 0)) != 0u) {
        (void)mtp_r_i32(&r);        /* tmp_sessions */
    }
    if ((flags & (1u << 2)) != 0u) {
        mtp_r_skip_bytes(&r);       /* future_auth_token */
    }
    if (!mtp_r_ok(&r)) {
        return MTP_LOGIN_ERROR;
    }
    (void)mtp_parse_user(&r);

    mtp_session_data_t *sess = (mtp_session_data_t *)mtp_client_session();
    sess->logged_in = true;
    /* Record which account this is, so a stored session can be matched to it. */
    for (int i = 0; i < MTP_MAX_PEERS; i++) {
        const mtp_peer_t *p = mtp_peer_at(i);
        if (p == NULL) {
            break;
        }
        if (p->is_self) {
            sess->user_id = p->id;
            break;
        }
    }
    (void)mtp_client_save();
    return MTP_LOGIN_OK;
}

mtp_login_result_t mtp_login_sign_in(const char *code)
{
    if (s_code_hash[0] == '\0') {
        return MTP_LOGIN_ERROR;
    }
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_AUTH_SIGNIN);
    mtp_w_u32(&w, 1u);                      /* flags.0: phone_code present */
    mtp_w_str(&w, s_phone);
    mtp_w_str(&w, s_code_hash);
    mtp_w_str(&w, code);
    if (!mtp_w_ok(&w)) {
        return MTP_LOGIN_ERROR;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err == MTP_ERR_RPC) {
        mtp_login_result_t r = classify();
        if (r == MTP_LOGIN_NEEDS_PASSWORD) {
            /* Fetch the hint now so the password screen can show it. */
            s_hint[0] = '\0';
        }
        return r;
    }
    if (err != MTP_OK) {
        char ev[48];
        snprintf(ev, sizeof(ev), "sign_in_fail_%d", (int)err);
        mtp_log(ev);
        snprintf(s_error, sizeof(s_error), "%s", mtp_err_str(err));
        return MTP_LOGIN_ERROR;
    }
    return absorb_authorization(res, res_len);
}

/* ---- Two-factor password ------------------------------------------------- */

/* Parse account.password into the SRP parameters. */
static bool read_password_params(const uint8_t *res, size_t len,
                                 mtp_srp_params_t *out)
{
    memset(out, 0, sizeof(*out));

    mtp_r_t r;
    mtp_r_init(&r, res, len);
    if (mtp_r_u32(&r) != MTP_ID_ACCOUNT_PASSWORD) {
        return false;
    }
    uint32_t flags = mtp_r_u32(&r);

    /*
     * flags.2 (has_password) gates current_algo, srp_B and srp_id together. If it
     * is clear the account has no cloud password and we should not be here.
     */
    if ((flags & (1u << 2)) == 0u) {
        return false;
    }

    /* current_algo */
    uint32_t algo = mtp_r_u32(&r);
    if (algo != MTP_ID_PASSWORDKDFALGOSHA256SHA256PBKDF2HMACSHA512ITER100000SHA256MODPOW) {
        /* The only KDF Telegram currently issues. An unknown one cannot be
           computed, and guessing would just produce a wrong proof. */
        return false;
    }
    size_t n = 0u;
    const uint8_t *s1 = mtp_r_bytes(&r, &n);
    if (s1 == NULL || n > sizeof(out->salt1)) {
        return false;
    }
    memcpy(out->salt1, s1, n);
    out->salt1_len = n;

    const uint8_t *s2 = mtp_r_bytes(&r, &n);
    if (s2 == NULL || n > sizeof(out->salt2)) {
        return false;
    }
    memcpy(out->salt2, s2, n);
    out->salt2_len = n;

    out->g = (uint32_t)mtp_r_i32(&r);

    const uint8_t *p = mtp_r_bytes(&r, &n);
    if (p == NULL || n != MTP_SRP_BYTES) {
        return false;
    }
    memcpy(out->p, p, n);

    /* srp_B is a big-endian integer and may have fewer than 256 significant
       bytes; left-pad so the arithmetic sees a fixed width. */
    const uint8_t *b = mtp_r_bytes(&r, &n);
    if (b == NULL || n > MTP_SRP_BYTES) {
        return false;
    }
    memcpy(out->srp_B + (MTP_SRP_BYTES - n), b, n);

    out->srp_id = mtp_r_i64(&r);

    if ((flags & (1u << 3)) != 0u) {
        mtp_r_str(&r, s_hint, sizeof(s_hint));
    }
    return mtp_r_ok(&r);
}

mtp_login_result_t mtp_login_check_password(const char *password,
                                            mtp_srp_progress_fn progress,
                                            void *user)
{
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    /* The parameters are fetched fresh every attempt: srp_B is single-use, so a
       retry with a stale one always fails. */
    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_ACCOUNT_GETPASSWORD);

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err != MTP_OK) {
        snprintf(s_error, sizeof(s_error), "%s", mtp_err_str(err));
        return MTP_LOGIN_ERROR;
    }

    /*
     * On the stack, not static: together these are about 900 bytes that would
     * otherwise sit resident for the whole session to serve one screen the user
     * may never reach. The deepest call below them is mtp_srp_compute, which
     * keeps its own bignums in the shared arena rather than on the stack, so the
     * 12 KB task stack has ample room.
     *
     * `params` must be filled before mtp_srp_compute borrows the arena: `res`
     * points into it whenever the response arrived gzipped, and the borrow wipes
     * it. read_password_params copies everything out, so the ordering here is
     * the ordering that matters.
     */
    mtp_srp_params_t params;
    if (!read_password_params(res, res_len, &params)) {
        snprintf(s_error, sizeof(s_error), "Unsupported password setup");
        return MTP_LOGIN_ERROR;
    }

    mtp_srp_proof_t proof;
    if (mtp_srp_compute(&params, password, progress, user, &proof) != MTP_OK) {
        snprintf(s_error, sizeof(s_error), "Key derivation failed");
        return MTP_LOGIN_ERROR;
    }

    buf = mtp_client_req_buf(&cap);
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_AUTH_CHECKPASSWORD);
    mtp_w_u32(&w, MTP_ID_INPUTCHECKPASSWORDSRP);
    mtp_w_i64(&w, proof.srp_id);
    mtp_w_bytes(&w, proof.A, sizeof(proof.A));
    mtp_w_bytes(&w, proof.M1, sizeof(proof.M1));
    if (!mtp_w_ok(&w)) {
        return MTP_LOGIN_ERROR;
    }

    err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err == MTP_ERR_RPC) {
        return classify();
    }
    if (err != MTP_OK) {
        char ev[48];
        snprintf(ev, sizeof(ev), "check_password_fail_%d", (int)err);
        mtp_log(ev);
        snprintf(s_error, sizeof(s_error), "%s", mtp_err_str(err));
        return MTP_LOGIN_ERROR;
    }
    return absorb_authorization(res, res_len);
}

mtp_err_t mtp_login_log_out(void)
{
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_AUTH_LOGOUT);

    const uint8_t *res;
    size_t res_len;
    /* Best effort: the local session is discarded either way, since a user who
       asked to log out should not still be logged in if the network is down. */
    (void)mtp_client_invoke(buf, w.len, &res, &res_len);

    mtp_client_forget();
    mtp_model_reset();
    mtp_login_reset();
    return MTP_OK;
}

#pragma GCC visibility pop
