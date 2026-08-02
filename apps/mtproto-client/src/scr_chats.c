/*
 * scr_chats — the dialog list and the conversation view.
 *
 * The layout borrows Telegram's shape as far as 128x64 allows: rows with an
 * avatar square, a title, a preview line and an unread badge; then bubbles that
 * sit right for outgoing and left for incoming, with ticks on our own messages.
 *
 * The conversation is laid out bottom-up. Bubble heights depend on wrapped text,
 * so the newest message is placed against the bottom edge and earlier ones are
 * stacked upward until they run off the top — which is also what makes a new
 * message push the view without a scroll calculation.
 */
#include "ui_screens.h"

#include <stdio.h>
#include <string.h>

#include "mtp_api.h"
#include "mtp_client.h"
#include "mtp_time.h"
#include "ui_font.h"
#include "ui_gfx.h"
#include "ui_icons.h"
#include "ui_widgets.h"

#pragma GCC visibility push(hidden)

#define DIALOG_ROW_H 14
#define AVATAR_W     11

static ui_list_t s_dialogs;
static int       s_chat_peer = -1;
static int       s_chat_scroll;          /* messages skipped from the bottom */
static bool      s_chat_loading;

static void compose_and_send(void);

/* ---- Dialog list --------------------------------------------------------- */

void scr_dialogs_refresh(void)
{
    mtp_err_t err = mtp_api_get_dialogs();
    if (err != MTP_OK) {
        char ev[48];
        snprintf(ev, sizeof(ev), "dialogs_ui_error_%d", (int)err);
        mtp_log(ev);
        ui_toast(mtp_err_str(err));
    }
    ui_list_set_count(&s_dialogs, mtp_dialogs_count());
}

void scr_dialogs_enter(void)
{
    ui_list_init(&s_dialogs, UI_STATUS_H + 1, UI_H - UI_STATUS_H - 1, DIALOG_ROW_H);
    ui_list_set_count(&s_dialogs, mtp_dialogs_count());
    if (mtp_dialogs_count() == 0) {
        scr_dialogs_refresh();
    }
}

/* The avatar square with the chat's first letter — Telegram's placeholder, and
   the cheapest way to make rows scannable without bitmaps per chat. */
static void draw_avatar(int x, int y, const mtp_peer_t *peer)
{
    ui_gfx_rrect(x, y, AVATAR_W, AVATAR_W, true);
    if (peer == NULL || peer->name[0] == '\0') {
        ui_gfx_bitmap(x + 2, y + 2, ui_icon_person.w, ui_icon_person.h,
                      ui_icon_person.bits, true);
        return;
    }
    size_t pos = 0u;
    uint32_t cp = ui_utf8_next(peer->name, &pos);
    (void)ui_gfx_glyph(x + 3, y + 2, cp, true);
}

static void dialog_row(void *user, int index, int y, int row_h, bool selected)
{
    (void)user;
    const mtp_dialog_t *d = mtp_dialog_at(index);
    if (d == NULL) {
        return;
    }
    const mtp_peer_t *peer = mtp_peer_at(d->peer);

    draw_avatar(1, y + 1, peer);

    /* Right-hand furniture first, so the title knows how much room it has. */
    int right = UI_W - 4;
    if (d->unread_count > 0) {
        char badge[6];
        /* int32_t is `long int` on this target, so it needs an explicit cast
           rather than %d — the value is already clamped to 99. */
        snprintf(badge, sizeof(badge),
                 d->unread_count > 99 ? "99+" : "%d", (int)d->unread_count);
        int bw = ui_font_width(badge) + 4;
        right -= bw;
        ui_gfx_rfill(right, y + 8, bw, 8, true);
        (void)ui_gfx_text(right + 2, y + 9, badge, false);
        right -= 2;
    }

    int title_right = UI_W - 4;
    if (d->date != 0u && mtp_time_is_synced()) {
        char when[8];
        mtp_time_fmt_hhmm(d->date, when, sizeof(when));
        title_right -= ui_font_width(when) + 2;
        (void)ui_gfx_text(title_right, y + 1, when, true);
    }

    const char *title = (peer != NULL && peer->name[0] != '\0')
                        ? peer->name : "Unknown";
    if (peer != NULL && peer->is_self) {
        title = "Saved Messages";
    }
    (void)ui_gfx_text_ellipsis(AVATAR_W + 4, y + 1,
                               title_right - AVATAR_W - 6, title, true);

    /* Preview line, prefixed when the last message is ours — the same cue
       Telegram uses to show you spoke last. */
    char preview[MTP_PREVIEW_MAX + 4];
    if (d->preview[0] != '\0') {
        snprintf(preview, sizeof(preview), "%s%s",
                 d->preview_out ? "> " : "", d->preview);
    } else {
        snprintf(preview, sizeof(preview), " ");
    }
    (void)ui_gfx_text_ellipsis(AVATAR_W + 4, y + 8,
                               right - AVATAR_W - 6, preview, true);

    if (selected) {
        ui_gfx_invert(0, y, UI_W - 3, row_h);
    }
}

void scr_dialogs_draw(void)
{
    ui_gfx_clear();
    ui_screens_status("Chats", true);

    if (mtp_dialogs_count() == 0) {
        (void)ui_gfx_text_center(0, 28, UI_W,
                                 s_chat_loading ? "Loading..." : "No chats", true);
    } else {
        ui_list_draw(&s_dialogs, dialog_row, NULL);
    }
    ui_toast_draw();
}

void scr_dialogs_key(jpp_sdk_key_event_t ev)
{
    if (ui_list_key(&s_dialogs, ev)) {
        return;
    }
    switch (ev) {
    case JPP_SDK_KEY_CENTER: {
        const mtp_dialog_t *d = mtp_dialog_at(s_dialogs.selected);
        if (d != NULL && d->peer >= 0) {
            scr_chat_enter(d->peer);
            mtp_app_goto(SCR_CHAT);
        }
        return;
    }
    case JPP_SDK_KEY_RIGHT:
        scr_settings_enter();
        mtp_app_goto(SCR_SETTINGS);
        return;
    case JPP_SDK_KEY_LEFT:
        /* Pull-to-refresh has no gesture here; LEFT is the spare key. */
        ui_toast("Refreshing");
        mtp_app_render_now();
        scr_dialogs_refresh();
        return;
    case JPP_SDK_KEY_BACK:
        mtp_app_goto(SCR_WELCOME);
        return;
    default:
        return;
    }
}

/* ---- Conversation -------------------------------------------------------- */

void scr_chat_enter(int peer_index)
{
    s_chat_peer = peer_index;
    s_chat_scroll = 0;
    mtp_app_set_open_chat(peer_index);

    if (mtp_messages_peer() != peer_index) {
        mtp_messages_clear(peer_index);
        s_chat_loading = true;
        mtp_app_render_now();
        int32_t stopped = 0;
        (void)mtp_api_get_history(peer_index, 0, &stopped);
        s_chat_loading = false;
    }

    /* Clear the unread badge for what is now on screen. */
    const mtp_message_t *newest = mtp_message_at(mtp_messages_count() - 1);
    if (newest != NULL) {
        (void)mtp_api_read_history(peer_index, newest->id);
        int at = mtp_dialog_find_peer(peer_index);
        if (at >= 0) {
            mtp_dialog_t *d = (mtp_dialog_t *)mtp_dialog_at(at);
            if (d != NULL) {
                d->unread_count = 0;
            }
        }
    }
}

void scr_chat_on_new_message(int peer_index)
{
    if (peer_index == s_chat_peer) {
        /* Stick to the bottom so an arriving message is visible. */
        s_chat_scroll = 0;
    }
}

/* Height of one bubble, including its padding and the gap under it. */
static int bubble_height(const mtp_message_t *msg, int text_w)
{
    int rows = ui_gfx_wrap_rows(text_w, msg->text);
    if (rows < 1) {
        rows = 1;
    }
    return rows * 8 + 4;
}

void scr_chat_draw(void)
{
    ui_gfx_clear();

    const mtp_peer_t *peer = mtp_peer_at(s_chat_peer);
    const char *title = (peer != NULL && peer->name[0] != '\0')
                        ? peer->name : "Chat";
    if (peer != NULL && peer->is_self) {
        title = "Saved Messages";
    }
    ui_screens_status(title, true);

    if (s_chat_loading) {
        (void)ui_gfx_text_center(0, 28, UI_W, "Loading...", true);
        ui_toast_draw();
        return;
    }
    int count = mtp_messages_count();
    if (count == 0) {
        (void)ui_gfx_text_center(0, 24, UI_W, "No messages yet", true);
        (void)ui_gfx_text_center(0, 34, UI_W, "CENTER to write", true);
        ui_toast_draw();
        return;
    }

    /*
     * Bottom-up layout. Start at the last message minus the scroll offset and
     * stack upward; a bubble whose top goes above the status bar is drawn anyway
     * and clipped, which is what makes partial scrolling look right.
     */
    const int bubble_max_w = 96;
    const int text_w = bubble_max_w - 6;
    int y = UI_H - 2;

    for (int i = count - 1 - s_chat_scroll; i >= 0; i--) {
        const mtp_message_t *msg = mtp_message_at(i);
        if (msg == NULL) {
            continue;
        }
        int h = bubble_height(msg, text_w);
        y -= h + 2;
        if (y + h < UI_STATUS_H) {
            break;      /* fully above the visible area */
        }

        int rows = ui_gfx_wrap_rows(text_w, msg->text);
        if (rows < 1) {
            rows = 1;
        }
        /* Shrink the bubble to the text when it is narrower than the maximum, so
           short messages do not look like empty boxes. */
        int w = (rows > 1) ? bubble_max_w
                           : ui_font_width(msg->text) + 6;
        if (w > bubble_max_w) {
            w = bubble_max_w;
        }
        if (w < 12) {
            w = 12;
        }
        int x = msg->out ? (UI_W - w - 4) : 2;

        if (msg->out) {
            /* Outgoing bubbles are filled and their text knocked out — the
               strongest contrast available on a 1-bit panel, and it reads as the
               accent colour a real client would use. */
            ui_gfx_rfill(x, y, w, h, true);
            (void)ui_gfx_text_wrap(x + 3, y + 2, text_w, 8, rows, msg->text, false);
        } else {
            ui_gfx_rfill(x, y, w, h, false);   /* clear anything underneath */
            ui_gfx_rrect(x, y, w, h, true);
            (void)ui_gfx_text_wrap(x + 3, y + 2, text_w, 8, rows, msg->text, true);
        }

        /* Read state, on our own messages only, as Telegram does. */
        if (msg->out) {
            int at = mtp_dialog_find_peer(s_chat_peer);
            const mtp_dialog_t *d = at >= 0 ? mtp_dialog_at(at) : NULL;
            bool read = d != NULL && msg->id <= d->read_outbox_max_id;
            const ui_icon_t *tick = read ? &ui_icon_dcheck : &ui_icon_check;
            ui_gfx_bitmap(x - tick->w - 1, y + h - tick->h - 1,
                          tick->w, tick->h, tick->bits, true);
        }

        if (y <= UI_STATUS_H) {
            break;
        }
    }

    /* Mask anything that spilled into the status bar. */
    ui_gfx_fill(0, 0, UI_W, UI_STATUS_H, false);
    ui_screens_status(title, true);
    ui_toast_draw();
}

void scr_chat_key(jpp_sdk_key_event_t ev)
{
    int count = mtp_messages_count();

    switch (ev) {
    case JPP_SDK_KEY_UP:
        if (s_chat_scroll < count - 1) {
            s_chat_scroll++;
        } else if (count > 0) {
            /*
             * At the top of what is held: fetch an older page. mtp_api_get_history
             * reports where it stopped when a message could not be traversed, and
             * resuming from that id is what keeps paging past media.
             */
            int32_t oldest = mtp_messages_oldest_id();
            s_chat_loading = true;
            mtp_app_render_now();
            int32_t stopped = 0;
            (void)mtp_api_get_history(s_chat_peer, oldest, &stopped);
            if (stopped != 0 && stopped == oldest) {
                (void)mtp_api_get_history(s_chat_peer, stopped, &stopped);
            }
            s_chat_loading = false;
        }
        return;

    case JPP_SDK_KEY_DOWN:
        if (s_chat_scroll > 0) {
            s_chat_scroll--;
        }
        return;

    case JPP_SDK_KEY_CENTER:
        compose_and_send();
        return;

    case JPP_SDK_KEY_LEFT:
    case JPP_SDK_KEY_BACK:
        mtp_app_set_open_chat(-1);
        scr_dialogs_enter();
        mtp_app_goto(SCR_DIALOGS);
        return;

    default:
        return;
    }
}

/*
 * ---- Composer --------------------------------------------------------------
 * A single blocking jpp_sdk_input() call rather than a driven screen. That
 * means mtp_client_pump() does not run for as long as the user is typing —
 * an accepted tradeoff (see ui_widgets.h for why every other screen here
 * avoids blocking modals) in exchange for the App SDK's own keyboard instead
 * of a hand-rolled one. It also caps a message at JPP_SDK_INPUT_VALUE_MAX (64)
 * characters and ASCII only, down from the 224-byte UTF-8 buffer used before.
 */
static void compose_and_send(void)
{
    /* Tell the peer we are typing, exactly as a phone would. Best effort: a
       failure here is not worth interrupting the user for. */
    (void)mtp_api_set_typing(s_chat_peer, true);

    char text[JPP_SDK_INPUT_VALUE_MAX + 1] = { 0 };
    jpp_sdk_ui_result_t res;
    (void)jpp_sdk_input(mtp_app_ctx(), "Message", NULL, JPP_SDK_INPUT_TEXT,
                        text, sizeof(text), &res);

    (void)mtp_api_set_typing(s_chat_peer, false);

    if (res == JPP_SDK_UI_BACK || text[0] == '\0') {
        return;
    }

    mtp_err_t err = mtp_api_send_message(s_chat_peer, text);
    if (err != MTP_OK) {
        ui_toast(mtp_err_str(err));
    } else {
        s_chat_scroll = 0;
    }
}

#pragma GCC visibility pop
