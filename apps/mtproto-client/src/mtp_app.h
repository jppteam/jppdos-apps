/*
 * mtp_app — screen state machine and the main loop.
 *
 * The app owns its thread: jpp_app_entry is called once and blocks until it
 * returns, and the firmware blits ctx->canvas on its own 100 ms cadence. So the
 * loop here is read a key, advance a screen, redraw, service the connection,
 * sleep — never wait on anything without pumping MTProto, or the server drops us
 * and updates stop arriving.
 *
 * Network calls are synchronous and do block the loop, which is why the slow ones
 * (the handshake, the 2FA key derivation) take a progress callback that repaints
 * from inside them. That keeps a ten-second operation showing motion without
 * needing a second task competing for the same 12 KB stack.
 */
#pragma once

#include "jpp_sdk_bridge.h"

typedef enum {
    SCR_WELCOME = 0,
    SCR_MODE_PICK,
    SCR_CONNECTING,
    SCR_DIALOGS,
    SCR_CHAT,
    SCR_SETTINGS,
    SCR_ERROR,
} mtp_screen_t;

void mtp_app_run(jpp_sdk_context_t *ctx);

/* ---- Shared with the screen implementations ------------------------------ */

/* The screen the loop is currently showing. */
mtp_screen_t mtp_app_screen(void);
void         mtp_app_goto(mtp_screen_t screen);

/* Repaint and push to the canvas right now. Used by progress callbacks inside
   long synchronous operations, where the main loop is not running. */
void mtp_app_render_now(void);

/* Set the message shown by SCR_ERROR before switching to it. */
void mtp_app_set_error(const char *headline, const char *detail);

jpp_sdk_context_t *mtp_app_ctx(void);

/* Peer whose chat is open, or -1. */
int  mtp_app_open_chat(void);
void mtp_app_set_open_chat(int peer_index);
