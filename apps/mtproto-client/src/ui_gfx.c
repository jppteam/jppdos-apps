#include "ui_gfx.h"

#include <string.h>

#include "mtp_mem.h"
#include "ui_font.h"

#pragma GCC visibility push(hidden)

#define ROW_BYTES (UI_W / 8)

_Static_assert(UI_FB_BYTES == UI_H * ROW_BYTES, "framebuffer size mismatch");

/*
 * Row-major, bit 7 of byte 0 leftmost — the convention jpp_sdk_canvas_write
 * expects, so the flush is a straight copy with no bit shuffling.
 *
 * On the heap rather than in the app pool (see mtp_mem.h); the array-of-rows
 * pointer type keeps every s_fb[y][x] below working unchanged.
 */
static uint8_t (*s_fb)[ROW_BYTES];

mtp_err_t ui_gfx_mem_init(void)
{
    s_fb = mtp_mem_take(UI_FB_BYTES);
    return s_fb != NULL ? MTP_OK : MTP_ERR_OVERFLOW;
}

void ui_gfx_mem_clear(void)
{
    s_fb = NULL;
}

void ui_gfx_clear(void)
{
    memset(s_fb, 0, UI_FB_BYTES);
}

void ui_gfx_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) {
        return;
    }
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    if (on) {
        s_fb[y][x >> 3] |= bit;
    } else {
        s_fb[y][x >> 3] &= (uint8_t)~bit;
    }
}

void ui_gfx_hline(int x0, int x1, int y, bool on)
{
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
    }
    for (int x = x0; x <= x1; x++) {
        ui_gfx_pixel(x, y, on);
    }
}

void ui_gfx_vline(int x, int y0, int y1, bool on)
{
    if (y0 > y1) {
        int t = y0; y0 = y1; y1 = t;
    }
    for (int y = y0; y <= y1; y++) {
        ui_gfx_pixel(x, y, on);
    }
}

void ui_gfx_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    ui_gfx_hline(x, x + w - 1, y, on);
    ui_gfx_hline(x, x + w - 1, y + h - 1, on);
    ui_gfx_vline(x, y, y + h - 1, on);
    ui_gfx_vline(x + w - 1, y, y + h - 1, on);
}

void ui_gfx_fill(int x, int y, int w, int h, bool on)
{
    for (int r = 0; r < h; r++) {
        ui_gfx_hline(x, x + w - 1, y + r, on);
    }
}

void ui_gfx_rrect(int x, int y, int w, int h, bool on)
{
    if (w <= 2 || h <= 2) {
        ui_gfx_rect(x, y, w, h, on);
        return;
    }
    /* Edges inset by one at each end, leaving the corners bare. */
    ui_gfx_hline(x + 1, x + w - 2, y, on);
    ui_gfx_hline(x + 1, x + w - 2, y + h - 1, on);
    ui_gfx_vline(x, y + 1, y + h - 2, on);
    ui_gfx_vline(x + w - 1, y + 1, y + h - 2, on);
}

void ui_gfx_rfill(int x, int y, int w, int h, bool on)
{
    if (w <= 2 || h <= 2) {
        ui_gfx_fill(x, y, w, h, on);
        return;
    }
    ui_gfx_hline(x + 1, x + w - 2, y, on);
    ui_gfx_fill(x, y + 1, w, h - 2, on);
    ui_gfx_hline(x + 1, x + w - 2, y + h - 1, on);
}

void ui_gfx_invert(int x, int y, int w, int h)
{
    for (int r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= UI_H) {
            continue;
        }
        for (int c = 0; c < w; c++) {
            int xx = x + c;
            if (xx < 0 || xx >= UI_W) {
                continue;
            }
            s_fb[yy][xx >> 3] ^= (uint8_t)(0x80u >> (xx & 7));
        }
    }
}

void ui_gfx_dither(int x, int y, int w, int h, uint8_t brightness)
{
    static const uint8_t bayer[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 },
    };
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int xx = x + c, yy = y + r;
            uint8_t threshold = (uint8_t)(bayer[yy & 3][xx & 3] * 16u + 8u);
            if (brightness > threshold) {
                ui_gfx_pixel(xx, yy, true);
            }
        }
    }
}

/* ---- Text ---------------------------------------------------------------- */

int ui_gfx_glyph(int x, int y, uint32_t cp, bool on)
{
    const uint8_t *cols = ui_font_glyph(cp);
    for (unsigned c = 0u; c < UI_FONT_W; c++) {
        uint8_t bits = cols[c];
        for (unsigned r = 0u; r < UI_FONT_H; r++) {
            if ((bits & (1u << r)) != 0u) {
                ui_gfx_pixel(x + (int)c, y + (int)r, on);
            }
        }
    }
    return x + (int)UI_FONT_ADV;
}

int ui_gfx_text(int x, int y, const char *s, bool on)
{
    size_t pos = 0u;
    for (;;) {
        uint32_t cp = ui_utf8_next(s, &pos);
        if (cp == 0u) {
            break;
        }
        /* Stop once the pen is off-screen: the remaining glyphs would all clip
           to nothing, and a long string should not cost a loop per character. */
        if (x >= UI_W) {
            break;
        }
        x = ui_gfx_glyph(x, y, cp, on);
    }
    return x;
}

int ui_gfx_text_ellipsis(int x, int y, int max_px, const char *s, bool on)
{
    if (ui_font_width(s) <= max_px) {
        return ui_gfx_text(x, y, s, on);
    }
    /* Reserve room for "..." — three glyphs at the normal advance. */
    int ell_px = 3 * (int)UI_FONT_ADV - 1;
    int room = max_px - ell_px;
    if (room < (int)UI_FONT_W) {
        /* Not even one character plus the ellipsis fits; show the ellipsis alone
           so the slot reads as "there is text here, but no space for it". */
        return ui_gfx_text(x, y, "...", on);
    }
    size_t cut = ui_font_fit(s, room);
    size_t pos = 0u;
    int pen = x;
    while (pos < cut) {
        uint32_t cp = ui_utf8_next(s, &pos);
        if (cp == 0u) {
            break;
        }
        pen = ui_gfx_glyph(pen, y, cp, on);
    }
    return ui_gfx_text(pen, y, "...", on);
}

int ui_gfx_text_center(int x, int y, int w, const char *s, bool on)
{
    int tw = ui_font_width(s);
    int start = x + (w - tw) / 2;
    if (start < x) {
        start = x;
    }
    return ui_gfx_text_ellipsis(start, y, w, s, on);
}

int ui_gfx_text_right(int x_right, int y, const char *s, bool on)
{
    return ui_gfx_text(x_right - ui_font_width(s), y, s, on);
}

/*
 * Shared by the wrap renderer and the height measurer so the two can never
 * disagree — a chat view that measures a bubble at three rows and then draws four
 * corrupts everything below it.
 *
 * Calls `emit` per line when it is non-NULL; otherwise just counts.
 */
static int wrap_walk(int w, const char *s, int max_lines,
                     void (*emit)(void *user, const char *start, size_t len, int row),
                     void *user)
{
    int rows = 0;
    size_t pos = 0u;

    while (s[pos] != '\0' && (max_lines <= 0 || rows < max_lines)) {
        /* Longest prefix of the remainder that fits the width. */
        size_t fit = ui_font_fit(s + pos, w);
        if (fit == 0u) {
            /* Width too narrow for even one glyph — emit it anyway rather than
               looping forever on a zero-length line. */
            fit = 1u;
            while ((s[pos + fit] & 0xC0) == 0x80) {
                fit++;
            }
        }
        size_t take = fit;
        if (s[pos + fit] != '\0') {
            /* Back up to the last space so words stay intact; if the line has no
               space at all, break mid-word rather than dropping the text. */
            size_t sp = 0u;
            for (size_t i = 0u; i < fit; i++) {
                if (s[pos + i] == ' ') {
                    sp = i;
                }
            }
            if (sp > 0u) {
                take = sp;
            }
        }
        /* Honour explicit newlines ahead of the width break. */
        for (size_t i = 0u; i < take; i++) {
            if (s[pos + i] == '\n') {
                take = i;
                break;
            }
        }

        if (emit != NULL) {
            emit(user, s + pos, take, rows);
        }
        rows++;

        pos += take;
        /* Swallow the break character and any run of spaces at the fold. */
        if (s[pos] == '\n') {
            pos++;
        }
        while (s[pos] == ' ') {
            pos++;
        }
    }
    return rows;
}

typedef struct {
    int x;
    int y;
    int line_h;
    bool on;
} wrap_draw_t;

static void wrap_emit(void *user, const char *start, size_t len, int row)
{
    wrap_draw_t *d = user;
    size_t pos = 0u;
    int pen = d->x;
    int y = d->y + row * d->line_h;
    while (pos < len) {
        uint32_t cp = ui_utf8_next(start, &pos);
        if (cp == 0u) {
            break;
        }
        pen = ui_gfx_glyph(pen, y, cp, d->on);
    }
}

int ui_gfx_text_wrap(int x, int y, int w, int line_h, int max_lines,
                     const char *s, bool on)
{
    wrap_draw_t d = { x, y, line_h, on };
    return wrap_walk(w, s, max_lines, wrap_emit, &d);
}

int ui_gfx_wrap_rows(int w, const char *s)
{
    return wrap_walk(w, s, 0, NULL, NULL);
}

typedef struct {
    const char *start;
    size_t      len;
} wrap_last_t;

static void wrap_last_emit(void *user, const char *start, size_t len, int row)
{
    (void)row;
    wrap_last_t *l = user;
    l->start = start;
    l->len = len;
}

int ui_gfx_wrap_last_width(int w, const char *s)
{
    wrap_last_t last = { NULL, 0u };
    (void)wrap_walk(w, s, 0, wrap_last_emit, &last);
    if (last.start == NULL) {
        return 0;
    }
    /* Measure the recorded slice without copying it: the caret needs this, and
       the slice points into the caller's string rather than a NUL-terminated
       buffer of its own. */
    size_t pos = 0u;
    int px = 0;
    while (pos < last.len) {
        if (ui_utf8_next(last.start, &pos) == 0u) {
            break;
        }
        px += (int)UI_FONT_ADV;
    }
    return px > 0 ? px - 1 : 0;
}

/* ---- Bitmaps ------------------------------------------------------------- */

void ui_gfx_bitmap(int x, int y, int w, int h, const uint8_t *bits, bool on)
{
    int stride = (w + 7) / 8;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if ((bits[r * stride + (c >> 3)] & (0x80u >> (c & 7))) != 0u) {
                ui_gfx_pixel(x + c, y + r, on);
            }
        }
    }
}

jpp_sdk_status_t ui_gfx_flush(jpp_sdk_context_t *ctx)
{
    for (uint8_t row = 0u; row < UI_H; row++) {
        jpp_sdk_status_t st = jpp_sdk_canvas_write(ctx, row, s_fb[row]);
        if (st != JPP_SDK_STATUS_OK) {
            return st;
        }
    }
    return JPP_SDK_STATUS_OK;
}

#pragma GCC visibility pop
