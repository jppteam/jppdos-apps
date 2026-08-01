/*
 * scr_settings — account, server and diagnostics.
 *
 * Short by design. Everything here either tells the user something they cannot
 * see elsewhere (which server, whether the DH parameters were verified) or is an
 * action with consequences (switch server, log out).
 */
#include "ui_screens.h"

#include <stdio.h>
#include <string.h>

#include "mtp_client.h"
#include "mtp_config.h"
#include "mtp_model.h"
#include "mtp_time.h"
#include "ui_gfx.h"
#include "ui_widgets.h"

typedef enum {
    SET_ACCOUNT = 0,
    SET_SERVER,
    SET_SECURITY,
    SET_SWITCH,
    SET_LOGOUT,
    SET_COUNT,
} setting_row_t;

static ui_list_t s_list;
static bool      s_confirm_logout;

void scr_settings_enter(void)
{
    ui_list_init(&s_list, UI_STATUS_H + 1, UI_H - UI_STATUS_H - 1, 13);
    ui_list_set_count(&s_list, SET_COUNT);
    s_confirm_logout = false;
}

/* Fill `title` and `detail` for a row. Split out so the row renderer stays
   layout-only. */
static void row_text(int index, char *title, size_t tcap,
                     char *detail, size_t dcap)
{
    const mtp_profile_t *p = mtp_config_profile(mtp_client_mode());
    const mtp_session_data_t *sess = mtp_client_session();

    switch (index) {
    case SET_ACCOUNT: {
        snprintf(title, tcap, "Account");
        const char *name = NULL;
        for (int i = 0; i < MTP_MAX_PEERS; i++) {
            const mtp_peer_t *peer = mtp_peer_at(i);
            if (peer == NULL) {
                break;
            }
            if (peer->is_self && peer->name[0] != '\0') {
                name = peer->name;
                break;
            }
        }
        snprintf(detail, dcap, "%s", name != NULL ? name : "signed in");
        break;
    }
    case SET_SERVER:
        snprintf(title, tcap, "Server");
        snprintf(detail, dcap, "%s, DC%d",
                 p != NULL ? p->name : "?", (int)sess->dc_id);
        break;
    case SET_SECURITY:
        snprintf(title, tcap, "Key exchange");
        /*
         * Worth surfacing: a verified prime means the DH parameters matched the
         * known-good safe prime, an unverified one means only the structural
         * checks passed. See mtp_auth.
         */
        snprintf(detail, dcap, "%s",
                 sess->dh_prime_verified ? "verified prime" : "unverified prime");
        break;
    case SET_SWITCH:
        snprintf(title, tcap, "Switch server");
        snprintf(detail, dcap, "keeps this session");
        break;
    case SET_LOGOUT:
        snprintf(title, tcap, "Log out");
        snprintf(detail, dcap, "%s",
                 s_confirm_logout ? "press again to confirm" : "sign out of this device");
        break;
    default:
        title[0] = '\0';
        detail[0] = '\0';
        break;
    }
}

static void settings_row(void *user, int index, int y, int row_h, bool selected)
{
    (void)user;
    char title[32], detail[40];
    row_text(index, title, sizeof(title), detail, sizeof(detail));

    (void)ui_gfx_text_ellipsis(3, y + 1, UI_W - 8, title, true);
    (void)ui_gfx_text_ellipsis(3, y + 8, UI_W - 8, detail, true);
    if (selected) {
        ui_gfx_invert(0, y, UI_W - 3, row_h);
    }
}

void scr_settings_draw(void)
{
    ui_gfx_clear();
    ui_screens_status("Settings", true);
    ui_list_draw(&s_list, settings_row, NULL);
    ui_toast_draw();
}

void scr_settings_key(jpp_sdk_key_event_t ev)
{
    if (ui_list_key(&s_list, ev)) {
        /* Moving away from Log out cancels the pending confirmation, so a stray
           double press cannot sign the user out. */
        s_confirm_logout = false;
        return;
    }
    if (ev == JPP_SDK_KEY_LEFT || ev == JPP_SDK_KEY_BACK) {
        scr_dialogs_enter();
        mtp_app_goto(SCR_DIALOGS);
        return;
    }
    if (ev != JPP_SDK_KEY_CENTER) {
        return;
    }

    switch ((setting_row_t)s_list.selected) {
    case SET_SWITCH:
        mtp_client_disconnect();
        scr_mode_enter();
        mtp_app_goto(SCR_MODE_PICK);
        return;

    case SET_LOGOUT:
        if (!s_confirm_logout) {
            /* Two presses, because this is the one irreversible action on the
               device and CENTER is easy to hit by accident. */
            s_confirm_logout = true;
            return;
        }
        ui_toast("Signing out");
        mtp_app_render_now();
        (void)mtp_login_log_out();
        mtp_app_goto(SCR_WELCOME);
        return;

    default:
        /* The informational rows have nothing to activate. */
        return;
    }
}
