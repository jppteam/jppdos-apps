#include "ui_keyboard.h"

#include <string.h>

#include "ui_font.h"
#include "ui_gfx.h"
#include "ui_icons.h"

/*
 * Letter rows, as UTF-8 strings. Rows differ in length on purpose: each divides
 * the full 128 pixels among its own keys, which is what makes ЙЦУКЕН and QWERTY
 * come out looking like themselves instead of like a spreadsheet.
 *
 * The Russian layout is the standard ЙЦУКЕН arrangement, so muscle memory from a
 * phone transfers.
 */
static const char *const LAYER_RU[3] = {
    "йцукенгшщзх",
    "фывапролджэ",
    "ячсмитьбюёъ",
};

static const char *const LAYER_EN[3] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
};

static const char *const LAYER_NUM[3] = {
    "1234567890",
    "@#$%&*-+()",
    "!?,.:;/'\"_",
};

/* The function row. Space is given extra width because it is the key people hit
   most and the hardest to land on with a d-pad. */
typedef enum {
    FN_SHIFT = 0,
    FN_LANG,
    FN_SPACE,
    FN_BKSP,
    FN_OK,
    FN_COUNT,
} fn_key_t;

/* Relative widths within the function row. */
static const int FN_WEIGHT[FN_COUNT] = { 3, 3, 8, 3, 3 };

#define KBD_ROWS 4          /* three letter rows plus the function row */
#define FN_ROW   3

/* Geometry. The keyboard occupies the bottom half; the text being edited gets
   the top, which is the split a phone uses. */
#define KEY_H     8
#define KBD_Y     (UI_H - KBD_ROWS * KEY_H)   /* 64 - 32 = 32 */
#define TEXT_Y    11
#define TEXT_ROWS 2

static const char *const *layer_rows(const ui_kbd_t *k)
{
    if (k->digits_only) {
        return LAYER_NUM;
    }
    switch (k->layer) {
    case UI_KBD_LAYER_EN:  return LAYER_EN;
    case UI_KBD_LAYER_NUM: return LAYER_NUM;
    default:               return LAYER_RU;
    }
}

/* Keys in a row: the letter count, or the fixed function-key count. */
static int row_len(const ui_kbd_t *k, int row)
{
    if (row == FN_ROW) {
        return FN_COUNT;
    }
    if (k->digits_only && row >= 1) {
        /* Only the digit row is offered; the punctuation rows would be noise on a
           phone-number field. */
        return 0;
    }
    return (int)ui_utf8_len(layer_rows(k)[row]);
}

/* Pixel span of key `col` in a letter row. Computed by proportion rather than a
   fixed cell width so a row of 9 and a row of 11 both fill the screen. */
static void key_span(const ui_kbd_t *k, int row, int col, int *x, int *w)
{
    int n = row_len(k, row);
    if (n <= 0) {
        *x = 0; *w = 0;
        return;
    }
    if (row == FN_ROW) {
        int total = 0;
        for (int i = 0; i < FN_COUNT; i++) {
            total += FN_WEIGHT[i];
        }
        int acc = 0;
        for (int i = 0; i < col; i++) {
            acc += FN_WEIGHT[i];
        }
        *x = acc * UI_W / total;
        *w = (acc + FN_WEIGHT[col]) * UI_W / total - *x;
        return;
    }
    *x = col * UI_W / n;
    *w = (col + 1) * UI_W / n - *x;
}

/* Uppercase a Cyrillic or Latin codepoint. Only the two ranges the layouts use;
   there is no general case-mapping table to consult here. */
static uint32_t upper_cp(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z') {
        return cp - 'a' + 'A';
    }
    if (cp >= 0x430u && cp <= 0x44Fu) {      /* а-я  → А-Я */
        return cp - 0x20u;
    }
    if (cp == 0x451u) {                       /* ё → Ё */
        return 0x401u;
    }
    return cp;
}

/* Encode a codepoint as UTF-8. Returns bytes written. */
static size_t encode_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    out[0] = (char)(0xE0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3u;
}

void ui_kbd_init(ui_kbd_t *k, char *buf, size_t cap, const char *title)
{
    memset(k, 0, sizeof(*k));
    k->buf = buf;
    k->cap = cap;
    k->title = title;
    k->layer = UI_KBD_LAYER_RU;
    /* Keep whatever the caller pre-filled and put the caret after it, so this
       also serves as "edit the existing value". */
    k->len = strnlen(buf, cap > 0u ? cap - 1u : 0u);
    k->buf[k->len] = '\0';
    k->caret = k->len;
}

/* ---- Editing ------------------------------------------------------------- */

static void insert_cp(ui_kbd_t *k, uint32_t cp)
{
    char enc[4];
    size_t n = encode_utf8(cp, enc);
    if (k->len + n + 1u > k->cap) {
        return;
    }
    memmove(k->buf + k->caret + n, k->buf + k->caret, k->len - k->caret + 1u);
    memcpy(k->buf + k->caret, enc, n);
    k->len += n;
    k->caret += n;
    k->buf[k->len] = '\0';
}

static void backspace(ui_kbd_t *k)
{
    if (k->caret == 0u) {
        return;
    }
    /* Step back over a whole UTF-8 sequence, not one byte — deleting half a
       Cyrillic letter would leave an invalid string. */
    size_t start = k->caret - 1u;
    while (start > 0u && ((uint8_t)k->buf[start] & 0xC0u) == 0x80u) {
        start--;
    }
    size_t n = k->caret - start;
    memmove(k->buf + start, k->buf + k->caret, k->len - k->caret + 1u);
    k->len -= n;
    k->caret = start;
    k->buf[k->len] = '\0';
}

/* The codepoint a letter key produces, honouring shift. */
static uint32_t key_cp(const ui_kbd_t *k, int row, int col)
{
    const char *s = layer_rows(k)[row];
    size_t off = ui_utf8_offset(s, (size_t)col);
    uint32_t cp = ui_utf8_next(s, &off);
    return k->shift ? upper_cp(cp) : cp;
}

/* Move to a valid key after a layer change may have shortened the row. */
static void clamp_focus(ui_kbd_t *k)
{
    if (k->row < 0) {
        k->row = 0;
    }
    if (k->row >= KBD_ROWS) {
        k->row = KBD_ROWS - 1;
    }
    /* digits_only hides rows 1 and 2; skip focus past them. */
    while (k->row != FN_ROW && row_len(k, k->row) == 0) {
        k->row = FN_ROW;
    }
    int n = row_len(k, k->row);
    if (n <= 0) {
        k->col = 0;
        return;
    }
    if (k->col >= n) {
        k->col = n - 1;
    }
    if (k->col < 0) {
        k->col = 0;
    }
}

ui_kbd_result_t ui_kbd_key(ui_kbd_t *k, jpp_sdk_key_event_t ev)
{
    switch (ev) {
    case JPP_SDK_KEY_UP:
        /* Wrap top-to-bottom: with four rows it is never more than two presses
           to anything, which matters when every press is a d-pad click. */
        k->row = (k->row + KBD_ROWS - 1) % KBD_ROWS;
        clamp_focus(k);
        return UI_KBD_CONTINUE;

    case JPP_SDK_KEY_DOWN:
        k->row = (k->row + 1) % KBD_ROWS;
        clamp_focus(k);
        return UI_KBD_CONTINUE;

    case JPP_SDK_KEY_LEFT: {
        int n = row_len(k, k->row);
        if (n > 0) {
            k->col = (k->col + n - 1) % n;
        }
        return UI_KBD_CONTINUE;
    }

    case JPP_SDK_KEY_RIGHT: {
        int n = row_len(k, k->row);
        if (n > 0) {
            k->col = (k->col + 1) % n;
        }
        return UI_KBD_CONTINUE;
    }

    case JPP_SDK_KEY_CENTER:
        if (k->row != FN_ROW) {
            insert_cp(k, key_cp(k, k->row, k->col));
            /* One-shot shift, as a phone does. */
            k->shift = false;
            return UI_KBD_CONTINUE;
        }
        switch ((fn_key_t)k->col) {
        case FN_SHIFT:
            k->shift = !k->shift;
            break;
        case FN_LANG:
            if (!k->digits_only) {
                k->layer = (ui_kbd_layer_t)((k->layer + 1) % UI_KBD_LAYER_COUNT);
                clamp_focus(k);
            }
            break;
        case FN_SPACE:
            insert_cp(k, ' ');
            break;
        case FN_BKSP:
            backspace(k);
            break;
        case FN_OK:
            return UI_KBD_COMMIT;
        default:
            break;
        }
        return UI_KBD_CONTINUE;

    case JPP_SDK_KEY_BACK:
        return UI_KBD_CANCEL;

    default:
        return UI_KBD_CONTINUE;
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static const char *fn_label(const ui_kbd_t *k, fn_key_t key)
{
    switch (key) {
    case FN_SHIFT: return k->shift ? "SHF" : "shf";
    case FN_LANG:
        if (k->digits_only) {
            return "-";
        }
        switch (k->layer) {
        case UI_KBD_LAYER_RU:  return "РУС";
        case UI_KBD_LAYER_EN:  return "ENG";
        default:               return "123";
        }
    case FN_SPACE: return "space";
    case FN_BKSP:  return "del";
    case FN_OK:    return "OK";
    default:       return "";
    }
}

/*
 * Render the text being edited, scrolled so the end stays visible.
 *
 * The whole string is handed to ui_gfx_text_wrap at a negative offset and the
 * off-screen lines clip away, rather than this re-implementing the wrap to pick
 * out a window. Sharing the one wrap implementation is the point: a second copy
 * of the line-breaking rules would eventually disagree with the first.
 *
 * The caret is drawn, not blinked — the firmware repaints on its own 100 ms
 * cadence, and a blink at that rate reads as a glitch rather than a cursor.
 */
static void draw_text_area(const ui_kbd_t *k)
{
    const int w = UI_W - 2;
    char shown[257];

    if (k->password) {
        /* One asterisk per character, not per byte, or a Cyrillic password would
           appear twice as long as it is. */
        size_t chars = ui_utf8_len(k->buf);
        size_t n = chars < sizeof(shown) - 1u ? chars : sizeof(shown) - 1u;
        memset(shown, '*', n);
        shown[n] = '\0';
    } else {
        size_t n = k->len < sizeof(shown) - 1u ? k->len : sizeof(shown) - 1u;
        memcpy(shown, k->buf, n);
        shown[n] = '\0';
    }

    int rows = ui_gfx_wrap_rows(w, shown);
    int skip = rows > TEXT_ROWS ? rows - TEXT_ROWS : 0;
    int y = TEXT_Y - skip * 9;

    if (rows > 0) {
        (void)ui_gfx_text_wrap(1, y, w, 9, rows, shown, true);
    }

    /* Caret after the last glyph of the last line. */
    int caret_row = rows > 0 ? rows - 1 : 0;
    int caret_x = 1 + (rows > 0 ? ui_gfx_wrap_last_width(w, shown) : 0);
    int caret_y = TEXT_Y + (caret_row - skip) * 9;
    ui_gfx_vline(caret_x, caret_y - 1, caret_y + 7, true);
}

void ui_kbd_draw(const ui_kbd_t *k)
{
    ui_gfx_clear();

    /*
     * Text first, then the header painted over it. draw_text_area deliberately
     * lets scrolled-off lines run above TEXT_Y, and this is what hides them —
     * cheaper than giving ui_gfx a scissor rectangle for one caller.
     */
    draw_text_area(k);
    ui_gfx_fill(0, 0, UI_W, 9, false);
    if (k->title != NULL) {
        ui_gfx_text(1, 1, k->title, true);
    }
    ui_gfx_hline(0, UI_W - 1, 9, true);

    /* Separator above the keyboard. */
    ui_gfx_hline(0, UI_W - 1, KBD_Y - 1, true);

    for (int row = 0; row < KBD_ROWS; row++) {
        int n = row_len(k, row);
        for (int col = 0; col < n; col++) {
            int x, w;
            key_span(k, row, col, &x, &w);
            int y = KBD_Y + row * KEY_H;
            bool focused = (row == k->row && col == k->col);

            if (row == FN_ROW) {
                const char *label = fn_label(k, (fn_key_t)col);
                ui_gfx_text_center(x, y + 1, w, label, true);
            } else {
                uint32_t cp = key_cp(k, row, col);
                int cx = x + (w - (int)UI_FONT_W) / 2;
                ui_gfx_glyph(cx, y + 1, cp, true);
            }
            if (focused) {
                /* Invert rather than outline: at 8 pixels tall a box around the
                   key would collide with the glyph. */
                ui_gfx_invert(x, y, w, KEY_H);
            }
        }
    }
}
