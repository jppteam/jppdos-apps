/*
 * mtp_login — the sign-in exchange.
 *
 * Mirrors what a phone does: send the number, receive a code, sign in with it,
 * and if the account has a cloud password, prove it with SRP. Each step is a
 * separate call so the UI can own the screens in between.
 *
 * Telegram's error strings drive real branching here — PHONE_MIGRATE_n moves the
 * account to another datacentre, SESSION_PASSWORD_NEEDED means 2FA, FLOOD_WAIT_n
 * means stop asking. Turning those into typed results is most of this module's
 * job.
 */
#pragma once

#include "mtp_client.h"
#include "mtp_srp.h"

typedef enum {
    MTP_LOGIN_OK = 0,          /* signed in */
    MTP_LOGIN_CODE_SENT,       /* a code is on its way */
    MTP_LOGIN_NEEDS_PASSWORD,  /* correct code, but 2FA is enabled */
    MTP_LOGIN_NEEDS_SIGNUP,    /* no account for this number */
    MTP_LOGIN_BAD_CODE,
    MTP_LOGIN_BAD_PASSWORD,
    MTP_LOGIN_EXPIRED,         /* the code timed out; send a new one */
    MTP_LOGIN_FLOOD,           /* rate-limited; see mtp_login_flood_seconds */
    MTP_LOGIN_BAD_PHONE,
    MTP_LOGIN_ERROR,           /* transport or unexpected server error */
} mtp_login_result_t;

/* How the code was delivered, so the UI can say where to look for it. */
typedef enum {
    MTP_CODE_UNKNOWN = 0,
    MTP_CODE_APP,       /* another Telegram session */
    MTP_CODE_SMS,
    MTP_CODE_CALL,
    MTP_CODE_MISSED_CALL,
    MTP_CODE_EMAIL,
    MTP_CODE_FRAGMENT,
} mtp_code_kind_t;

typedef struct {
    mtp_code_kind_t kind;
    int             length;         /* expected digits, 0 if unspecified */
    char            hint[32];       /* pattern or address, when the server gives one */
} mtp_code_info_t;

/* Clear any half-finished attempt. */
void mtp_login_reset(void);

/*
 * Request a login code. Handles PHONE_MIGRATE by reconnecting to the right
 * datacentre and retrying, which is invisible to the user beyond a pause.
 */
mtp_login_result_t mtp_login_send_code(const char *phone, mtp_code_info_t *out);

/* Ask for the code again over a different channel, after the timeout elapses. */
mtp_login_result_t mtp_login_resend_code(mtp_code_info_t *out);

mtp_login_result_t mtp_login_sign_in(const char *code);

/*
 * Complete a 2FA sign-in. Slow — the SRP key derivation runs for several seconds
 * — so `progress` is called throughout and must keep the UI drawing.
 */
mtp_login_result_t mtp_login_check_password(const char *password,
                                            mtp_srp_progress_fn progress,
                                            void *user);

/* The 2FA hint the account owner set, or "" if none. Valid after a
   MTP_LOGIN_NEEDS_PASSWORD result. */
const char *mtp_login_password_hint(void);

/* Seconds to wait after MTP_LOGIN_FLOOD. */
int mtp_login_flood_seconds(void);

/* Server error text for the last failure, for the toast line. */
const char *mtp_login_error(void);

/* Sign out and forget the local session. */
mtp_err_t mtp_login_log_out(void);
