/*
 * scr_login — the sign-in sequence, shaped like the mobile clients'.
 *
 * Welcome with the logo, pick a server, type the number, type the code, and a
 * password step only when the account has one. The wording follows Telegram's
 * because the flow is the one users already know; the layout is what fits 128x64.
 */
#include "ui_screens.h"

#include <stdio.h>
#include <string.h>

#include "mtp_api.h"
#include "mtp_client.h"
#include "mtp_config.h"
#include "mtp_store.h"
#include "ui_gfx.h"
#include "ui_icons.h"
#include "ui_widgets.h"

#pragma GCC visibility push(hidden)

/* Remembered across a "back" from the code step, so a retry does not force the
   user to retype the number. jpp_sdk_input() never prefills a field, so this
   can only be shown back to them as a placeholder, not restored into the box. */
static char       s_phone[24];
static ui_list_t  s_mode_list;

/* Progress state for the long operations, drawn by the render callback. */
static char s_step[24];
static int  s_percent;

void ui_screens_status(const char *title, bool clock)
{
    ui_status_t st;
    memset(&st, 0, sizeof(st));
    st.title = title;
    st.connected = mtp_client_state() == MTP_CONN_READY;
    st.show_clock = clock;
    st.battery_pct = -1;

    int unread = 0;
    for (int i = 0; i < mtp_dialogs_count(); i++) {
        const mtp_dialog_t *d = mtp_dialog_at(i);
        if (d != NULL) {
            unread += d->unread_count;
        }
    }
    st.unread = unread;
    ui_status_draw(&st);
}

/* Shared progress callback: repaints from inside a blocking call. */
static void progress_cb(void *user, const char *step, int percent)
{
    (void)user;
    if (step != NULL) {
        snprintf(s_step, sizeof(s_step), "%s", step);
    }
    s_percent = percent;
    mtp_app_render_now();
}

static void srp_progress_cb(void *user, int percent)
{
    (void)user;
    snprintf(s_step, sizeof(s_step), "Checking password");
    s_percent = percent;
    mtp_app_render_now();
}

/* ---- Welcome ------------------------------------------------------------- */

void scr_welcome_draw(void)
{
    ui_gfx_clear();
    ui_gfx_bitmap((UI_W - ui_icon_logo.w) / 2, 4,
                  ui_icon_logo.w, ui_icon_logo.h, ui_icon_logo.bits, true);
    (void)ui_gfx_text_center(0, 34, UI_W, "MTProto", true);
    (void)ui_gfx_text_center(0, 44, UI_W, "Press CENTER to start", true);
    ui_toast_draw();
}

void scr_welcome_key(jpp_sdk_key_event_t ev)
{
    if (ev == JPP_SDK_KEY_CENTER) {
        scr_mode_enter();
        mtp_app_goto(SCR_MODE_PICK);
    }
}

/* ---- Server mode --------------------------------------------------------- */

void scr_mode_enter(void)
{
    ui_list_init(&s_mode_list, UI_STATUS_H + 2, UI_H - UI_STATUS_H - 4, 13);
    ui_list_set_count(&s_mode_list, MTP_MODE_COUNT);
    /* Start on whichever mode was used last, so a returning user just presses
       CENTER twice. */
    mtp_mode_t last = mtp_store_last_mode();
    if (last < MTP_MODE_COUNT) {
        s_mode_list.selected = (int)last;
    }
}

static void mode_row(void *user, int index, int y, int row_h, bool selected)
{
    (void)user;
    const mtp_profile_t *p = mtp_config_profile((mtp_mode_t)index);
    if (p == NULL) {
        return;
    }
    bool usable = mtp_config_check((mtp_mode_t)index) == MTP_OK;

    (void)ui_gfx_text_ellipsis(4, y + 1, UI_W - 10, p->name, true);

    /* Second line says why a mode cannot be used, rather than leaving the user to
       discover it by selecting it. */
    const char *note;
    if (usable) {
        note = (mtp_store_last_mode() == (mtp_mode_t)index) ? "last used" : "";
    } else if (index == MTP_MODE_CUSTOM) {
        note = mtp_config_custom_error();
        if (note[0] == '\0') {
            note = "no custom.conf";
        }
    } else {
        note = "not configured";
    }
    if (note[0] != '\0') {
        (void)ui_gfx_text_ellipsis(4, y + 9, UI_W - 10, note, true);
    }

    if (selected) {
        ui_gfx_invert(0, y, UI_W - 3, row_h);
    }
}

void scr_mode_draw(void)
{
    ui_gfx_clear();
    ui_screens_status("Choose server", false);
    ui_list_draw(&s_mode_list, mode_row, NULL);
    ui_toast_draw();
}

/* Connect, then decide where the user lands: straight to the chats if the stored
   session is still authorized, otherwise into the login flow. */
static void connect_and_continue(mtp_mode_t mode)
{
    s_step[0] = '\0';
    s_percent = 0;
    mtp_app_goto(SCR_CONNECTING);
    mtp_app_render_now();

    mtp_err_t err = mtp_client_connect(mode, progress_cb, NULL);
    if (err != MTP_OK) {
        mtp_app_set_error("Cannot connect", mtp_err_str(err));
        mtp_app_goto(SCR_ERROR);
        return;
    }

    if (mtp_client_is_logged_in()) {
        /* Confirm the authorization actually survived; a key can outlive it. */
        snprintf(s_step, sizeof(s_step), "Signing in");
        s_percent = -1;
        mtp_app_render_now();
        if (mtp_api_get_self() == MTP_OK) {
            scr_dialogs_enter();
            mtp_app_goto(SCR_DIALOGS);
            return;
        }
        /* The server no longer knows us — fall through to a fresh login. */
    }
    scr_login_run();
}

void scr_mode_key(jpp_sdk_key_event_t ev)
{
    if (ui_list_key(&s_mode_list, ev)) {
        return;
    }
    if (ev == JPP_SDK_KEY_BACK) {
        mtp_app_goto(SCR_WELCOME);
        return;
    }
    if (ev != JPP_SDK_KEY_CENTER) {
        return;
    }

    mtp_mode_t mode = (mtp_mode_t)s_mode_list.selected;
    mtp_err_t check = mtp_config_check(mode);
    if (check != MTP_OK) {
        /*
         * Say what is missing rather than failing at connect time. For the two
         * built-in profiles this means the api credentials were never filled in;
         * for Custom it means custom.conf.
         */
        if (mode == MTP_MODE_CUSTOM) {
            const char *why = mtp_config_custom_error();
            mtp_app_set_error("Custom not set up",
                              why[0] != '\0' ? why
                                             : "Add custom.conf over WebDAV");
        } else {
            mtp_app_set_error("Not configured",
                              "This build has no API credentials for this server");
        }
        mtp_app_goto(SCR_ERROR);
        return;
    }
    connect_and_continue(mode);
}

/* ---- Connecting ---------------------------------------------------------- */

/*
 * Drawn by the app loop and by progress_cb; shared so both look identical.
 *
 * The long operation it reports on usually follows an jpp_sdk_input() modal —
 * in the 2FA case, the password prompt right before the SRP derivation — and
 * every modal helper drops fullscreen back to the 48-row windowed canvas. The
 * progress bar sits at y=46, so on a windowed canvas its lower half would be
 * clipped off-screen. Re-enable fullscreen here so the hashing bar, and every
 * other progress screen, renders on the full 128x64 display. See the canvas
 * note in mtp_app.h.
 */
void scr_connecting_draw(void)
{
    (void)jpp_sdk_canvas_fullscreen(mtp_app_ctx(), true);
    ui_screen_progress("Connecting", s_step[0] != '\0' ? s_step : NULL, s_percent);
}

/* Where to look for the code, in the words the mobile clients use. */
static const char *code_source(const mtp_code_info_t *info)
{
    switch (info->kind) {
    case MTP_CODE_APP:         return "Sent to your Telegram app";
    case MTP_CODE_SMS:         return "Sent by SMS";
    case MTP_CODE_CALL:        return "You will get a call";
    case MTP_CODE_MISSED_CALL: return "Check the calling number";
    case MTP_CODE_EMAIL:       return "Sent to your email";
    case MTP_CODE_FRAGMENT:    return "Sent via Fragment";
    default:                   return "Enter the code you received";
    }
}

/*
 * scr_login_run — phone, code, and (if needed) two-factor password, all via the
 * App SDK's jpp_sdk_input(). Each step is a blocking modal: no key-queue drain,
 * no mtp_client_pump() runs while it is up. That is an accepted tradeoff for
 * this app — see ui_widgets.h for why every *other* screen avoids it — since
 * nothing is connected yet during login (there is nothing to pump until the
 * phone number is sent), and jpp_sdk_input() is the only entry surface the App
 * SDK offers. Loops rather than recurses on "back"/retry so bouncing between
 * steps cannot grow the stack.
 */
void scr_login_run(void)
{
    enum { STEP_PHONE, STEP_CODE, STEP_PASSWORD } step = STEP_PHONE;
    mtp_code_info_t code_info;
    memset(&code_info, 0, sizeof(code_info));
    char code_title[40] = "Enter the code";

    for (;;) {
        jpp_sdk_ui_result_t res;

        switch (step) {
        case STEP_PHONE: {
            char phone[24] = { 0 };
            (void)jpp_sdk_input(mtp_app_ctx(), "Your phone number",
                                s_phone[0] != '\0' ? s_phone : "+1234567890",
                                JPP_SDK_INPUT_NUMBER, phone, sizeof(phone), &res);
            if (res == JPP_SDK_UI_BACK) {
                scr_mode_enter();
                mtp_app_goto(SCR_MODE_PICK);
                return;
            }
            /*
             * The remembered number is shown to the user as the box's
             * placeholder. jpp_sdk_input() never prefills the field, so
             * confirming without typing anything comes back here as an empty
             * string — which, when we have a remembered number, means "use the
             * number that is already on screen".
             */
            if (phone[0] == '\0' && s_phone[0] != '\0') {
                snprintf(phone, sizeof(phone), "%s", s_phone);
            }
            if (strlen(phone) < 6) {
                continue;
            }
            snprintf(s_phone, sizeof(s_phone), "%s", phone);
            mtp_log("login_phone");

            snprintf(s_step, sizeof(s_step), "Sending code");
            s_percent = -1;
            mtp_app_goto(SCR_CONNECTING);
            mtp_app_render_now();

            switch (mtp_login_send_code(s_phone, &code_info)) {
            case MTP_LOGIN_CODE_SENT:
                mtp_log("login_code_sent");
                snprintf(code_title, sizeof(code_title), "%s",
                         code_source(&code_info));
                step = STEP_CODE;
                continue;
            case MTP_LOGIN_OK:
                scr_dialogs_enter();
                mtp_app_goto(SCR_DIALOGS);
                return;
            case MTP_LOGIN_FLOOD: {
                char msg[48];
                mtp_log("login_flood");
                snprintf(msg, sizeof(msg), "Try again in %d s",
                         mtp_login_flood_seconds());
                mtp_app_set_error("Too many attempts", msg);
                mtp_app_goto(SCR_ERROR);
                return;
            }
            case MTP_LOGIN_BAD_PHONE:
                mtp_log("login_bad_phone");
                continue;
            default:
                mtp_app_set_error("Could not send code", mtp_login_error());
                mtp_app_goto(SCR_ERROR);
                return;
            }
        }

        case STEP_CODE: {
            char code[12] = { 0 };
            (void)jpp_sdk_input(mtp_app_ctx(), code_title, NULL,
                                JPP_SDK_INPUT_NUMBER, code, sizeof(code), &res);
            if (res == JPP_SDK_UI_BACK) {
                step = STEP_PHONE;
                continue;
            }
            if (strlen(code) < 4) {
                continue;
            }

            snprintf(s_step, sizeof(s_step), "Signing in");
            s_percent = -1;
            mtp_app_goto(SCR_CONNECTING);
            mtp_app_render_now();

            switch (mtp_login_sign_in(code)) {
            case MTP_LOGIN_OK:
                mtp_log("login_signed_in");
                scr_dialogs_enter();
                mtp_app_goto(SCR_DIALOGS);
                return;
            case MTP_LOGIN_NEEDS_PASSWORD:
                mtp_log("login_needs_password");
                step = STEP_PASSWORD;
                continue;
            case MTP_LOGIN_BAD_CODE:
                mtp_log("login_bad_code");
                continue;
            case MTP_LOGIN_EXPIRED:
                mtp_log("login_code_expired");
                step = STEP_PHONE;
                continue;
            case MTP_LOGIN_NEEDS_SIGNUP:
                mtp_log("login_needs_signup");
                mtp_app_set_error("No account",
                                  "This number has no account. Register on a phone first.");
                mtp_app_goto(SCR_ERROR);
                return;
            default:
                mtp_app_set_error("Sign-in failed", mtp_login_error());
                mtp_app_goto(SCR_ERROR);
                return;
            }
        }

        case STEP_PASSWORD: {
            /* The App SDK's on-screen keyboard has no masked/dots mode, so
               the password is shown in the clear while typed — unlike the
               previous custom keyboard. Accepted along with the rest of the
               jpp_sdk_input() switch. */
            char password[64] = { 0 };
            const char *hint = mtp_login_password_hint();
            char title[48];
            snprintf(title, sizeof(title), "%s",
                     hint[0] != '\0' ? hint : "Two-step password");
            (void)jpp_sdk_input(mtp_app_ctx(), title, NULL,
                                JPP_SDK_INPUT_TEXT, password, sizeof(password), &res);
            if (res == JPP_SDK_UI_BACK) {
                step = STEP_CODE;
                continue;
            }
            if (password[0] == '\0') {
                continue;
            }

            mtp_log("login_2fa_hashing");
            /*
             * The key derivation runs for several seconds and cannot be
             * interrupted, so the screen switches to progress before it
             * starts and srp_progress_cb keeps the bar moving from inside it.
             */
            snprintf(s_step, sizeof(s_step), "Checking password");
            s_percent = 0;
            mtp_app_goto(SCR_CONNECTING);
            mtp_app_render_now();

            mtp_login_result_t pres =
                mtp_login_check_password(password, srp_progress_cb, NULL);

            /* The plaintext password has served its purpose. */
            memset(password, 0, sizeof(password));

            switch (pres) {
            case MTP_LOGIN_OK:
                mtp_log("login_2fa_ok");
                scr_dialogs_enter();
                mtp_app_goto(SCR_DIALOGS);
                return;
            case MTP_LOGIN_BAD_PASSWORD:
                mtp_log("login_2fa_bad");
                continue;
            case MTP_LOGIN_FLOOD: {
                char msg[48];
                mtp_log("login_2fa_flood");
                snprintf(msg, sizeof(msg), "Try again in %d s",
                         mtp_login_flood_seconds());
                mtp_app_set_error("Too many attempts", msg);
                mtp_app_goto(SCR_ERROR);
                return;
            }
            default:
                mtp_app_set_error("Sign-in failed", mtp_login_error());
                mtp_app_goto(SCR_ERROR);
                return;
            }
        }
        }
    }
}

#pragma GCC visibility pop
