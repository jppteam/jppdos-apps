/*
 * ui_keyboard — on-canvas text entry.
 *
 * The SDK's own jpp_sdk_input() exists but cannot be used for messages: it caps
 * at 64 characters and its character sets are ASCII only, so a Russian user could
 * not type a single word. Since most traffic on this device will be Cyrillic,
 * text entry has to be ours.
 *
 * The layout follows a phone keyboard rather than a uniform grid — rows have
 * different lengths and each row divides the full width among its own keys, so
 * ЙЦУКЕН and QWERTY both come out the shape people expect. Three layers cycle
 * with the Lang key: Russian, Latin, and digits/punctuation.
 *
 * Driven by the caller rather than blocking: ui_kbd_key() folds one key event in
 * and ui_kbd_draw() renders. That keeps the connection pumping while the user
 * types, which a modal helper could not.
 */
#pragma once

#include "mtp_common.h"
#include "jpp_sdk_bridge.h"

typedef enum {
    UI_KBD_LAYER_RU = 0,
    UI_KBD_LAYER_EN,
    UI_KBD_LAYER_NUM,
    UI_KBD_LAYER_COUNT,
} ui_kbd_layer_t;

typedef enum {
    UI_KBD_CONTINUE = 0,   /* still editing */
    UI_KBD_COMMIT,         /* user pressed OK */
    UI_KBD_CANCEL,         /* user backed out */
} ui_kbd_result_t;

typedef struct {
    char       *buf;        /* caller's storage, NUL-terminated */
    size_t      cap;        /* buf size in bytes, including the terminator */
    size_t      len;        /* bytes used */
    size_t      caret;      /* byte offset of the insertion point */
    const char *title;
    /* Render the text as dots. The 2FA password is the reason this exists. */
    bool        password;
    /* Restrict to the digit layer and hide the Lang key — phone and code entry,
       where offering Cyrillic would just be in the way. */
    bool        digits_only;
    ui_kbd_layer_t layer;
    bool        shift;
    int         row;
    int         col;
} ui_kbd_t;

/*
 * Bind the keyboard to a caller-owned buffer. Existing contents are kept and the
 * caret goes to the end, so this doubles as "edit this value".
 */
void ui_kbd_init(ui_kbd_t *k, char *buf, size_t cap, const char *title);

/* Fold in one key event. */
ui_kbd_result_t ui_kbd_key(ui_kbd_t *k, jpp_sdk_key_event_t ev);

/* Render into the framebuffer. Does not flush. */
void ui_kbd_draw(const ui_kbd_t *k);
