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
#include "ui_keyboard.h"
#include "ui_widgets.h"

static ui_kbd_t   s_kbd;
static char       s_phone[24];
static char       s_code[12];
static char       s_password[64];
static ui_list_t  s_mode_list;
static mtp_code_info_t s_code_info;

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
    scr_phone_enter();
    mtp_app_goto(SCR_PHONE);
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

/* Drawn by the app loop and by progress_cb; shared so both look identical. */
void scr_connecting_draw(void)
{
    ui_screen_progress("Connecting", s_step[0] != '\0' ? s_step : NULL, s_percent);
}

/* ---- Phone number -------------------------------------------------------- */

void scr_phone_enter(void)
{
    if (s_phone[0] == '\0') {
        /* Seed with a leading + so the required format is obvious. */
        snprintf(s_phone, sizeof(s_phone), "+");
    }
    ui_kbd_init(&s_kbd, s_phone, sizeof(s_phone), "Your phone number");
    s_kbd.digits_only = true;
}

void scr_phone_draw(void)
{
    ui_kbd_draw(&s_kbd);
    ui_toast_draw();
}

void scr_phone_key(jpp_sdk_key_event_t ev)
{
    switch (ui_kbd_key(&s_kbd, ev)) {
    case UI_KBD_CANCEL:
        scr_mode_enter();
        mtp_app_goto(SCR_MODE_PICK);
        return;
    case UI_KBD_COMMIT:
        break;
    default:
        return;
    }

    if (strlen(s_phone) < 6) {
        ui_toast("Number looks too short");
        return;
    }

    snprintf(s_step, sizeof(s_step), "Sending code");
    s_percent = -1;
    mtp_app_goto(SCR_CONNECTING);
    mtp_app_render_now();

    mtp_login_result_t res = mtp_login_send_code(s_phone, &s_code_info);
    switch (res) {
    case MTP_LOGIN_CODE_SENT:
        scr_code_enter(&s_code_info);
        mtp_app_goto(SCR_CODE);
        return;
    case MTP_LOGIN_OK:
        scr_dialogs_enter();
        mtp_app_goto(SCR_DIALOGS);
        return;
    case MTP_LOGIN_FLOOD: {
        char msg[48];
        snprintf(msg, sizeof(msg), "Try again in %d s", mtp_login_flood_seconds());
        mtp_app_set_error("Too many attempts", msg);
        mtp_app_goto(SCR_ERROR);
        return;
    }
    case MTP_LOGIN_BAD_PHONE:
        ui_toast("Invalid number");
        mtp_app_goto(SCR_PHONE);
        return;
    default:
        mtp_app_set_error("Could not send code", mtp_login_error());
        mtp_app_goto(SCR_ERROR);
        return;
    }
}

/* ---- Login code ---------------------------------------------------------- */

void scr_code_enter(const mtp_code_info_t *info)
{
    if (info != NULL) {
        s_code_info = *info;
    }
    s_code[0] = '\0';
    ui_kbd_init(&s_kbd, s_code, sizeof(s_code), "Enter the code");
    s_kbd.digits_only = true;
}

/* Where to look for the code, in the words the mobile clients use. */
static const char *code_source(void)
{
    switch (s_code_info.kind) {
    case MTP_CODE_APP:         return "Sent to your Telegram app";
    case MTP_CODE_SMS:         return "Sent by SMS";
    case MTP_CODE_CALL:        return "You will get a call";
    case MTP_CODE_MISSED_CALL: return "Check the calling number";
    case MTP_CODE_EMAIL:       return "Sent to your email";
    case MTP_CODE_FRAGMENT:    return "Sent via Fragment";
    default:                   return "Enter the code you received";
    }
}

void scr_code_draw(void)
{
    ui_kbd_draw(&s_kbd);
    /*
     * Overwrite the keyboard's title band with the delivery hint: on this screen
     * knowing where the code went matters more than a title repeating what the
     * keypad already shows.
     */
    ui_gfx_fill(0, 0, UI_W, 9, false);
    (void)ui_gfx_text_ellipsis(1, 1, UI_W - 2, code_source(), true);
    ui_gfx_hline(0, UI_W - 1, 9, true);
    ui_toast_draw();
}

void scr_code_key(jpp_sdk_key_event_t ev)
{
    /* RIGHT on the function row is a natural place for "resend", but the
       keyboard owns RIGHT. Long-press BACK returns to the number instead. */
    switch (ui_kbd_key(&s_kbd, ev)) {
    case UI_KBD_CANCEL:
        scr_phone_enter();
        mtp_app_goto(SCR_PHONE);
        return;
    case UI_KBD_COMMIT:
        break;
    default:
        return;
    }

    if (strlen(s_code) < 4) {
        ui_toast("Code too short");
        return;
    }

    snprintf(s_step, sizeof(s_step), "Signing in");
    s_percent = -1;
    mtp_app_goto(SCR_CONNECTING);
    mtp_app_render_now();

    switch (mtp_login_sign_in(s_code)) {
    case MTP_LOGIN_OK:
        scr_dialogs_enter();
        mtp_app_goto(SCR_DIALOGS);
        return;
    case MTP_LOGIN_NEEDS_PASSWORD:
        scr_password_enter();
        mtp_app_goto(SCR_PASSWORD);
        return;
    case MTP_LOGIN_BAD_CODE:
        ui_toast("Wrong code");
        s_code[0] = '\0';
        scr_code_enter(NULL);
        mtp_app_goto(SCR_CODE);
        return;
    case MTP_LOGIN_EXPIRED:
        ui_toast("Code expired");
        scr_phone_enter();
        mtp_app_goto(SCR_PHONE);
        return;
    case MTP_LOGIN_NEEDS_SIGNUP:
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

/* ---- Two-factor password ------------------------------------------------- */

void scr_password_enter(void)
{
    s_password[0] = '\0';
    ui_kbd_init(&s_kbd, s_password, sizeof(s_password), "Password");
    s_kbd.password = true;
}

void scr_password_draw(void)
{
    ui_kbd_draw(&s_kbd);

    /* Replace the title band with the lock and the account's own hint, which is
       the only clue the user gets about which password this is. */
    ui_gfx_fill(0, 0, UI_W, 9, false);
    ui_gfx_bitmap(1, 1, ui_icon_lock.w, ui_icon_lock.h, ui_icon_lock.bits, true);
    const char *hint = mtp_login_password_hint();
    if (hint[0] != '\0') {
        (void)ui_gfx_text_ellipsis(ui_icon_lock.w + 4, 1,
                                   UI_W - ui_icon_lock.w - 6, hint, true);
    } else {
        (void)ui_gfx_text_ellipsis(ui_icon_lock.w + 4, 1,
                                   UI_W - ui_icon_lock.w - 6,
                                   "Two-step password", true);
    }
    ui_gfx_hline(0, UI_W - 1, 9, true);
    ui_toast_draw();
}

void scr_password_key(jpp_sdk_key_event_t ev)
{
    switch (ui_kbd_key(&s_kbd, ev)) {
    case UI_KBD_CANCEL:
        scr_code_enter(NULL);
        mtp_app_goto(SCR_CODE);
        return;
    case UI_KBD_COMMIT:
        break;
    default:
        return;
    }

    if (s_password[0] == '\0') {
        ui_toast("Enter your password");
        return;
    }

    /*
     * The key derivation runs for several seconds and cannot be interrupted, so
     * the screen switches to progress before it starts and srp_progress_cb keeps
     * the bar moving from inside it.
     */
    snprintf(s_step, sizeof(s_step), "Checking password");
    s_percent = 0;
    mtp_app_goto(SCR_CONNECTING);
    mtp_app_render_now();

    mtp_login_result_t res =
        mtp_login_check_password(s_password, srp_progress_cb, NULL);

    /* The plaintext password has served its purpose. */
    memset(s_password, 0, sizeof(s_password));

    switch (res) {
    case MTP_LOGIN_OK:
        scr_dialogs_enter();
        mtp_app_goto(SCR_DIALOGS);
        return;
    case MTP_LOGIN_BAD_PASSWORD:
        ui_toast("Wrong password");
        scr_password_enter();
        mtp_app_goto(SCR_PASSWORD);
        return;
    case MTP_LOGIN_FLOOD: {
        char msg[48];
        snprintf(msg, sizeof(msg), "Try again in %d s", mtp_login_flood_seconds());
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
