#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mtp_time.h"
#include "ui_font.h"
#include "ui_gfx.h"
#include "ui_icons.h"

#pragma GCC visibility push(hidden)

#define TOAST_MS 2500u

static char     s_toast[40];
static uint32_t s_toast_until;

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

/* ---- Status bar ---------------------------------------------------------- */

void ui_status_draw(const ui_status_t *st)
{
    /* Right-hand furniture is laid out first so the title knows how much room is
       left; a long chat title must not overrun the clock. */
    int right = UI_W;

    if (st->battery_pct >= 0) {
        right -= ui_icon_battery.w + 2;
        ui_gfx_bitmap(right, 1, ui_icon_battery.w, ui_icon_battery.h,
                      ui_icon_battery.bits, true);
        /* Fill proportionally inside the outline's 7-pixel interior. */
        int fill = (st->battery_pct * 7) / 100;
        if (fill > 0) {
            ui_gfx_fill(right + 1, 2, fill, 5, true);
        }
    }

    if (st->show_clock) {
        char clock[8];
        mtp_time_fmt_hhmm(mtp_time_now(), clock, sizeof(clock));
        right -= ui_font_width(clock) + 3;
        ui_gfx_text(right, 1, clock, true);
    }

    const ui_icon_t *link = st->connected ? &ui_icon_link_on : &ui_icon_link_off;
    right -= link->w + 3;
    ui_gfx_bitmap(right, 1, link->w, link->h, link->bits, true);

    if (st->unread > 0) {
        char badge[8];
        snprintf(badge, sizeof(badge), "%d", st->unread > 99 ? 99 : st->unread);
        int bw = ui_font_width(badge) + 4;
        right -= bw + 2;
        /* Filled pill with inverted text — the same visual language as the
           unread badge in the dialog list. */
        ui_gfx_rfill(right, 0, bw, 9, true);
        ui_gfx_text(right + 2, 1, badge, false);
    }

    if (st->title != NULL) {
        ui_gfx_text_ellipsis(1, 1, right - 3, st->title, true);
    }
    ui_gfx_hline(0, UI_W - 1, 9, true);
}

/* ---- List ---------------------------------------------------------------- */

void ui_list_init(ui_list_t *l, int top, int height, int row_h)
{
    memset(l, 0, sizeof(*l));
    l->top = top;
    l->height = height;
    l->row_h = row_h > 0 ? row_h : 1;
}

int ui_list_visible(const ui_list_t *l)
{
    int n = l->height / l->row_h;
    return n > 0 ? n : 1;
}

/* Pull selection and scroll back into range and keep the selected row on screen.
   Shared by set_count and the key handler so the two cannot disagree. */
static void list_clamp(ui_list_t *l)
{
    int visible = ui_list_visible(l);

    if (l->count <= 0) {
        l->selected = 0;
        l->scroll = 0;
        return;
    }
    if (l->selected >= l->count) {
        l->selected = l->count - 1;
    }
    if (l->selected < 0) {
        l->selected = 0;
    }
    if (l->selected < l->scroll) {
        l->scroll = l->selected;
    }
    if (l->selected >= l->scroll + visible) {
        l->scroll = l->selected - visible + 1;
    }
    int max_scroll = l->count - visible;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (l->scroll > max_scroll) {
        l->scroll = max_scroll;
    }
    if (l->scroll < 0) {
        l->scroll = 0;
    }
}

void ui_list_set_count(ui_list_t *l, int count)
{
    l->count = count < 0 ? 0 : count;
    list_clamp(l);
}

bool ui_list_key(ui_list_t *l, jpp_sdk_key_event_t ev)
{
    if (l->count <= 0) {
        return false;
    }
    int before = l->selected;
    if (ev == JPP_SDK_KEY_UP) {
        l->selected--;
    } else if (ev == JPP_SDK_KEY_DOWN) {
        l->selected++;
    } else {
        return false;
    }
    /* Clamp rather than wrap: in a list of chats, wrapping from the top to the
       bottom is disorienting, and the d-pad auto-repeats. */
    if (l->selected < 0) {
        l->selected = 0;
    }
    if (l->selected >= l->count) {
        l->selected = l->count - 1;
    }
    list_clamp(l);
    return l->selected != before;
}

void ui_list_draw(const ui_list_t *l, ui_list_row_fn row_fn, void *user)
{
    int visible = ui_list_visible(l);
    bool has_bar = l->count > visible;
    /* The scrollbar eats two columns; rows are told their own height but work out
       their width from UI_W, so reserve by drawing the bar last and letting it
       overwrite. Cheaper than threading a width through every row callback. */

    for (int i = 0; i < visible; i++) {
        int index = l->scroll + i;
        if (index >= l->count) {
            break;
        }
        row_fn(user, index, l->top + i * l->row_h, l->row_h, index == l->selected);
    }

    if (has_bar) {
        int x = UI_W - 2;
        ui_gfx_vline(x, l->top, l->top + l->height - 1, false);
        ui_gfx_vline(x + 1, l->top, l->top + l->height - 1, false);
        /* Thumb length in proportion to the visible fraction, at least 3px so it
           stays visible in a long list. */
        int track = l->height;
        int thumb = track * visible / l->count;
        if (thumb < 3) {
            thumb = 3;
        }
        int max_scroll = l->count - visible;
        int offset = max_scroll > 0 ? (track - thumb) * l->scroll / max_scroll : 0;
        ui_gfx_fill(x, l->top + offset, 2, thumb, true);
    }
}

/* ---- Overlays ------------------------------------------------------------ */

void ui_screen_message(const char *headline, const char *caption, const void *icon)
{
    ui_gfx_clear();
    const ui_icon_t *ic = icon;

    int y = 4;
    if (ic != NULL) {
        ui_gfx_bitmap((UI_W - ic->w) / 2, y, ic->w, ic->h, ic->bits, true);
        y += ic->h + 4;
    }
    if (headline != NULL) {
        (void)ui_gfx_text_center(0, y, UI_W, headline, true);
        y += 10;
    }
    if (caption != NULL) {
        /* Captions are error text and can be long; wrap rather than clip. */
        (void)ui_gfx_text_wrap(4, y, UI_W - 8, 8, (UI_H - y) / 8, caption, true);
    }
}

void ui_screen_progress(const char *headline, const char *caption, int percent)
{
    ui_gfx_clear();

    const ui_icon_t *sp = ui_spinner_frame();
    ui_gfx_bitmap((UI_W - sp->w) / 2, 8, sp->w, sp->h, sp->bits, true);

    if (headline != NULL) {
        (void)ui_gfx_text_center(0, 22, UI_W, headline, true);
    }
    if (caption != NULL) {
        (void)ui_gfx_text_center(0, 32, UI_W, caption, true);
    }

    if (percent >= 0) {
        if (percent > 100) {
            percent = 100;
        }
        int x = 14, w = UI_W - 28, y = 46;
        ui_gfx_rrect(x, y, w, 7, true);
        int fill = (w - 4) * percent / 100;
        if (fill > 0) {
            ui_gfx_fill(x + 2, y + 2, fill, 3, true);
        }
    }
}

const void *ui_spinner_frame(void)
{
    /* 100 ms per frame matches the firmware's own repaint cadence, so every
       redraw advances the animation by exactly one step. */
    uint32_t frame = (now_ms() / 100u) % UI_SPINNER_FRAMES;
    return &ui_icon_spinner[frame];
}

void ui_toast(const char *text)
{
    snprintf(s_toast, sizeof(s_toast), "%s", text != NULL ? text : "");
    s_toast_until = now_ms() + TOAST_MS;
}

bool ui_toast_active(void)
{
    /* Signed comparison so the tick wrap at ~49 days does not strand a toast. */
    return s_toast[0] != '\0' && (int32_t)(s_toast_until - now_ms()) > 0;
}

void ui_toast_draw(void)
{
    if (!ui_toast_active()) {
        return;
    }
    int h = 11;
    int y = UI_H - h;
    /* Clear behind it, then a filled pill with knocked-out text: a banner over
       existing content is unreadable on a 1-bit panel otherwise. */
    ui_gfx_fill(0, y, UI_W, h, false);
    ui_gfx_rfill(0, y, UI_W, h, true);
    (void)ui_gfx_text_center(0, y + 2, UI_W, s_toast, false);
}

#pragma GCC visibility pop
