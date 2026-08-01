/*
 * ui_widgets — the reusable pieces of chrome.
 *
 * The SDK's modal helpers (jpp_sdk_dialog, jpp_sdk_list) exist and are genuinely
 * useful, but they take over the screen and the d-pad until the user resolves
 * them. That is fatal here: while a modal is up, nothing is pumping the MTProto
 * connection, so the server drops it and updates pile up. Everything in this file
 * is therefore non-blocking — fold in a key, draw a frame, return.
 */
#pragma once

#include "mtp_common.h"
#include "jpp_sdk_bridge.h"

/* ---- Status bar ---------------------------------------------------------- */

typedef struct {
    const char *title;      /* screen name, or the chat title */
    bool        connected;
    int         unread;     /* total across chats; 0 hides the badge */
    bool        show_clock;
    int         battery_pct;   /* -1 when unknown */
} ui_status_t;

/* Draws the top 9 pixels and the rule under them. */
void ui_status_draw(const ui_status_t *st);

/* ---- Scrolling list ------------------------------------------------------ */

/*
 * Rows are drawn by the caller. The widget owns only selection, scrolling and the
 * scrollbar, because the interesting part of a row — an avatar, a preview line, an
 * unread badge — differs per screen and is not worth abstracting.
 */
typedef void (*ui_list_row_fn)(void *user, int index, int y, int row_h, bool selected);

typedef struct {
    int count;
    int selected;
    int scroll;     /* index of the first visible row */
    int top;        /* y of the first visible row */
    int row_h;
    int height;     /* pixels available for rows */
} ui_list_t;

void ui_list_init(ui_list_t *l, int top, int height, int row_h);

/* Adjusts selection and scroll to stay in range when the backing data changes —
   which happens constantly, since updates arrive while a list is on screen. */
void ui_list_set_count(ui_list_t *l, int count);

/* Returns true if the event moved the selection. */
bool ui_list_key(ui_list_t *l, jpp_sdk_key_event_t ev);

void ui_list_draw(const ui_list_t *l, ui_list_row_fn row_fn, void *user);

/* Rows currently visible — callers use it to decide how much history to fetch. */
int ui_list_visible(const ui_list_t *l);

/* ---- Overlays ------------------------------------------------------------ */

/*
 * A whole-screen message: an icon, a headline and a caption. Used for connecting,
 * errors and empty states, all of which want the same shape.
 */
void ui_screen_message(const char *headline, const char *caption,
                       const void *icon /* const ui_icon_t *, or NULL */);

/*
 * A progress bar with a caption. The handshake and the 2FA key derivation both
 * take seconds, and a still screen during either reads as a crash.
 */
void ui_screen_progress(const char *headline, const char *caption, int percent);

/* Spinner frame for the current time — animates without the caller tracking a
   frame counter. */
const void *ui_spinner_frame(void);

/*
 * A transient banner across the bottom. Set it and it draws for a few seconds,
 * then stops on its own; this is how errors are reported without stealing focus.
 */
void ui_toast(const char *text);
void ui_toast_draw(void);
bool ui_toast_active(void);
