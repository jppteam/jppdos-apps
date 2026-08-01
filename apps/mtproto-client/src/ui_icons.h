/*
 * ui_icons — the small bitmaps the UI needs.
 *
 * Deliberately a short list. Every icon costs pool space, and at 128x64 most
 * affordances read better as text or as a shape drawn with ui_gfx primitives than
 * as a bitmap. These are the ones where a glyph genuinely says it better: the
 * read ticks, the lock on the password step, the spinner during the handshake,
 * and the paper plane on the welcome screen.
 *
 * Art and packing live in test/gen_icons.py; ui_icons_data.c is generated.
 */
#pragma once

#include "mtp_common.h"

typedef struct {
    int            w;
    int            h;
    const uint8_t *bits;   /* row-major, MSB leftmost, rows byte-padded */
} ui_icon_t;

#define UI_SPINNER_FRAMES 8

extern const ui_icon_t ui_icon_logo;      /* paper plane, welcome screen */
extern const ui_icon_t ui_icon_check;     /* delivered */
extern const ui_icon_t ui_icon_dcheck;    /* read */
extern const ui_icon_t ui_icon_lock;      /* two-factor password step */
extern const ui_icon_t ui_icon_back;
extern const ui_icon_t ui_icon_send;
extern const ui_icon_t ui_icon_battery;
extern const ui_icon_t ui_icon_link_on;   /* connected */
extern const ui_icon_t ui_icon_link_off;  /* disconnected */
extern const ui_icon_t ui_icon_person;    /* avatar placeholder */
extern const ui_icon_t ui_icon_spinner[UI_SPINNER_FRAMES];
