#include "mtp_app.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mtp_api.h"
#include "mtp_client.h"
#include "mtp_config.h"
#include "mtp_mem.h"
#include "mtp_model.h"
#include "mtp_scratch.h"
#include "mtp_session.h"
#include "mtp_store.h"
#include "mtp_time.h"
#include "mtp_updates.h"
#include "ui_gfx.h"
#include "ui_icons.h"
#include "ui_screens.h"
#include "ui_widgets.h"

/* Frame period. The firmware repaints the panel every 100 ms, so drawing faster
   than that would only burn cycles the connection wants. */
#define FRAME_MS 60u

/* How often to re-fetch the dialog list while it is on screen, as a backstop for
   updates the client could not parse. */
#define DIALOG_REFRESH_MS 120000u

static jpp_sdk_context_t *s_ctx;
static mtp_screen_t       s_screen = SCR_WELCOME;
static bool               s_exit;
static char               s_err_headline[32];
static char               s_err_detail[96];
static int                s_open_chat = -1;
static uint32_t           s_last_refresh;

/* Declared in scr_login.c; shared because the progress screen is drawn from
   inside blocking calls there. */
void scr_connecting_draw(void);

jpp_sdk_context_t *mtp_app_ctx(void)      { return s_ctx; }
mtp_screen_t       mtp_app_screen(void)   { return s_screen; }
void               mtp_app_goto(mtp_screen_t s) { s_screen = s; }
int                mtp_app_open_chat(void) { return s_open_chat; }
void               mtp_app_set_open_chat(int peer) { s_open_chat = peer; }

void mtp_app_set_error(const char *headline, const char *detail)
{
    snprintf(s_err_headline, sizeof(s_err_headline), "%s",
             headline != NULL ? headline : "Error");
    snprintf(s_err_detail, sizeof(s_err_detail), "%s",
             detail != NULL ? detail : "");
}

/* ---- Drawing ------------------------------------------------------------- */

static void draw_error(void)
{
    ui_screen_message(s_err_headline, s_err_detail, NULL);
    (void)ui_gfx_text_center(0, UI_H - 9, UI_W, "CENTER to go back", true);
}

static void draw(void)
{
    switch (s_screen) {
    case SCR_WELCOME:    scr_welcome_draw();    break;
    case SCR_MODE_PICK:  scr_mode_draw();       break;
    case SCR_CONNECTING: scr_connecting_draw(); break;
    case SCR_PHONE:      scr_phone_draw();      break;
    case SCR_CODE:       scr_code_draw();       break;
    case SCR_PASSWORD:   scr_password_draw();   break;
    case SCR_DIALOGS:    scr_dialogs_draw();    break;
    case SCR_CHAT:       scr_chat_draw();       break;
    case SCR_COMPOSE:    scr_compose_draw();    break;
    case SCR_SETTINGS:   scr_settings_draw();   break;
    case SCR_ERROR:      draw_error();          break;
    default:             ui_gfx_clear();        break;
    }
}

void mtp_app_render_now(void)
{
    draw();
    (void)ui_gfx_flush(s_ctx);
    /*
     * Give the firmware's render loop a tick to pick the frame up. Without this
     * a progress callback firing in a tight loop would redraw faster than the
     * panel updates and the user would see nothing move.
     */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---- Input --------------------------------------------------------------- */

static void handle_key(jpp_sdk_key_event_t ev)
{
    switch (s_screen) {
    case SCR_WELCOME:
        if (ev == JPP_SDK_KEY_BACK) {
            s_exit = true;
            return;
        }
        scr_welcome_key(ev);
        return;
    case SCR_MODE_PICK:  scr_mode_key(ev);     return;
    case SCR_PHONE:      scr_phone_key(ev);    return;
    case SCR_CODE:       scr_code_key(ev);     return;
    case SCR_PASSWORD:   scr_password_key(ev); return;
    case SCR_DIALOGS:    scr_dialogs_key(ev);  return;
    case SCR_CHAT:       scr_chat_key(ev);     return;
    case SCR_COMPOSE:    scr_compose_key(ev);  return;
    case SCR_SETTINGS:   scr_settings_key(ev); return;

    case SCR_ERROR:
        if (ev == JPP_SDK_KEY_CENTER || ev == JPP_SDK_KEY_BACK) {
            /* Back to wherever makes sense: the chats if we have a session, the
               server picker otherwise. */
            if (mtp_client_is_logged_in() &&
                mtp_client_state() == MTP_CONN_READY) {
                scr_dialogs_enter();
                s_screen = SCR_DIALOGS;
            } else {
                scr_mode_enter();
                s_screen = SCR_MODE_PICK;
            }
        }
        return;

    case SCR_CONNECTING:
        /* Modal by nature — the operation it reports on is synchronous, so no
           key can arrive while it is genuinely running. */
        return;

    default:
        return;
    }
}

/* ---- Notifications ------------------------------------------------------- */

/*
 * Signal an incoming message. Suppressed for the chat the user is already
 * looking at, which is what every messenger does and what stops the device
 * buzzing at someone mid-conversation.
 */
static void notify(int peer)
{
    if (peer == s_open_chat && s_screen == SCR_CHAT) {
        return;
    }
    (void)jpp_sdk_buzzer_play(s_ctx, JPP_BUZZER_SOUND_NOTIFY);
    (void)jpp_sdk_led_set_color(s_ctx, 0u, 40u, 90u);

    const mtp_peer_t *p = mtp_peer_at(peer);
    if (p != NULL && p->name[0] != '\0') {
        char msg[48];
        snprintf(msg, sizeof(msg), "%s", p->name);
        ui_toast(msg);
    } else {
        ui_toast("New message");
    }
}

/* ---- Memory -------------------------------------------------------------- */

/*
 * Take the one static block and hand every region out of it, here rather than
 * inside mtp_mem.c so that file stays a leaf the host tests can link.
 *
 * All of it up front, in one place: a shortfall then shows up at startup, where
 * it can be reported, instead of the first time some screen is drawn. The order
 * does not matter — mtp_mem_take is a bump pointer, not a layout.
 */
static mtp_err_t mem_setup(void)
{
    if (mtp_mem_init() != MTP_OK) {
        return MTP_ERR_OVERFLOW;
    }
    if (mtp_scratch_mem_init() != MTP_OK ||
        mtp_sess_mem_init()    != MTP_OK ||
        mtp_model_mem_init()   != MTP_OK ||
        ui_gfx_mem_init()      != MTP_OK) {
        mtp_mem_release();
        return MTP_ERR_OVERFLOW;
    }
    return MTP_OK;
}

static void mem_teardown(void)
{
    /* Drop the modules' pointers before wiping the block, so a use after
       teardown is a null dereference at the point of the bug rather than a
       read of stale (or zeroed) data. */
    mtp_scratch_mem_clear();
    mtp_sess_mem_clear();
    mtp_model_mem_clear();
    ui_gfx_mem_clear();
    mtp_mem_release();
}

/* ---- Main loop ----------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

void mtp_app_run(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
    s_exit = false;
    s_screen = SCR_WELCOME;

    /*
     * The big buffers first, and before the canvas: without them there is no
     * framebuffer to draw an error into, so the failure has to be reported
     * through the firmware's own text frame instead.
     *
     * This is now a build-time invariant, not a runtime condition — mtp_mem's
     * block is static and part of the app's own image, not the shared heap, so
     * nothing else running on the device can starve it. The only way in here
     * is MTP_MEM_BYTES drifting out of step with what the modules actually ask
     * mtp_mem_take for, which should have been caught by a _Static_assert
     * before this ever ships. Kept as a backstop rather than removed outright.
     */
    if (mem_setup() != MTP_OK) {
        static const char *const lines[] = {
            "MTProto",
            "",
            "Internal error:",
            "memory layout",
            "mismatch.",
        };
        (void)jpp_sdk_set_frame(ctx, lines, sizeof(lines) / sizeof(lines[0]));
        jpp_sdk_log(ctx, "mtproto_oom");
        /* Leave the frame up long enough to be read, then hand back to the
           launcher. Nothing to clean up: no canvas, no wakelock, no socket —
           mem_setup is the first thing that runs, and it released its own block
           before failing. */
        vTaskDelay(pdMS_TO_TICKS(4000));
        jpp_sdk_request_close(ctx);
        return;
    }

    /* Fullscreen canvas: the whole UI is hand-drawn, and the firmware's frame
       text would otherwise show through pages 0-1. */
    (void)jpp_sdk_canvas_fullscreen(ctx, true);
    (void)jpp_sdk_canvas_clear(ctx);

    (void)mtp_time_init(ctx);
    if (mtp_config_init(ctx) != MTP_OK) {
        mtp_app_set_error("Startup failed", "Could not prepare server profiles");
        s_screen = SCR_ERROR;
    }
    mtp_client_init(ctx);
    mtp_model_reset();
    mtp_updates_install();

    /*
     * Hold the wakelock for the whole session. A messenger that lets the screen
     * dim mid-conversation, or the device sleep while a socket is open, is worse
     * than useless — the connection would drop and updates would stop.
     */
    (void)jpp_sdk_wakelock_acquire(ctx);

    jpp_sdk_log(ctx, "mtproto_start");

    while (!s_exit && !ctx->close_requested) {
        /* Drain the key queue: it holds only eight events, and a burst from the
           d-pad's auto-repeat would otherwise be dropped. */
        for (;;) {
            jpp_sdk_key_event_t ev = JPP_SDK_KEY_NONE;
            if (jpp_sdk_poll_key(ctx, &ev) != JPP_SDK_STATUS_OK ||
                ev == JPP_SDK_KEY_NONE) {
                break;
            }
            handle_key(ev);
            if (s_exit) {
                break;
            }
        }
        if (s_exit) {
            break;
        }

        /* Service the connection: dispatch updates, keep it alive, notice drops. */
        mtp_client_pump();

        if (mtp_updates_take_dirty()) {
            int peer = mtp_updates_last_message_peer();
            if (peer >= 0) {
                notify(peer);
                scr_chat_on_new_message(peer);
            }
        }

        /*
         * Periodic dialog refresh. Updates cover the common cases, but this
         * client deliberately does not implement updates.getDifference (see
         * mtp_updates.h), so a slow backstop keeps the list honest.
         */
        if (s_screen == SCR_DIALOGS &&
            mtp_client_state() == MTP_CONN_READY &&
            now_ms() - s_last_refresh > DIALOG_REFRESH_MS) {
            s_last_refresh = now_ms();
            scr_dialogs_refresh();
        }

        draw();
        (void)ui_gfx_flush(ctx);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }

    jpp_sdk_log(ctx, "mtproto_stop");

    (void)jpp_sdk_led_off(ctx);
    (void)jpp_sdk_wakelock_release(ctx);
    mtp_client_disconnect();
    jpp_sdk_net_close_all(ctx);
    (void)jpp_sdk_canvas_fullscreen(ctx, false);
    /* After the last draw and after the socket is closed: everything that could
       still touch these buffers has finished. */
    mem_teardown();
    jpp_sdk_request_close(ctx);
}
