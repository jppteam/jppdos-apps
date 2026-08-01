/*
 * ui_screens — per-screen draw and key handling.
 *
 * Each screen is a draw function plus a key handler, both non-blocking. The state
 * machine in mtp_app.c decides which pair is live; screens change screens by
 * calling mtp_app_goto rather than returning a next-state, which keeps the
 * transitions readable at the call site where the reason for them is.
 */
#pragma once

#include "mtp_app.h"
#include "mtp_login.h"

/* Welcome and server-mode selection. */
void scr_welcome_draw(void);
void scr_welcome_key(jpp_sdk_key_event_t ev);

void scr_mode_enter(void);
void scr_mode_draw(void);
void scr_mode_key(jpp_sdk_key_event_t ev);

/* Login: phone, code, two-factor password. */
void scr_phone_enter(void);
void scr_phone_draw(void);
void scr_phone_key(jpp_sdk_key_event_t ev);

void scr_code_enter(const mtp_code_info_t *info);
void scr_code_draw(void);
void scr_code_key(jpp_sdk_key_event_t ev);

void scr_password_enter(void);
void scr_password_draw(void);
void scr_password_key(jpp_sdk_key_event_t ev);

/* Chat list and conversation. */
void scr_dialogs_enter(void);
void scr_dialogs_draw(void);
void scr_dialogs_key(jpp_sdk_key_event_t ev);
void scr_dialogs_refresh(void);

void scr_chat_enter(int peer_index);
void scr_chat_draw(void);
void scr_chat_key(jpp_sdk_key_event_t ev);
/* Called when an update lands, so an open chat scrolls to a new message. */
void scr_chat_on_new_message(int peer_index);

void scr_compose_enter(void);
void scr_compose_draw(void);
void scr_compose_key(jpp_sdk_key_event_t ev);

void scr_settings_enter(void);
void scr_settings_draw(void);
void scr_settings_key(jpp_sdk_key_event_t ev);

/* Shared: the status bar every in-session screen shows. */
void ui_screens_status(const char *title, bool clock);
