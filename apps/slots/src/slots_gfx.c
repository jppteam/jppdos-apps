#include "slots_gfx.h"

#include <string.h>

#include "slots_art.h"

#define ROW_BYTES 16

/* Each row is 16 bytes / 128 bits; bit 7 of byte 0 is the leftmost pixel
   (matches jpp_sdk_canvas_write's convention). */
static uint8_t s_fb[SLOTS_GFX_ROWS][ROW_BYTES];

void slots_gfx_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void slots_gfx_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SLOTS_GFX_COLS || y < 0 || y >= SLOTS_GFX_ROWS) {
        return;
    }
    uint8_t bit = (uint8_t)(0x80u >> (x % 8));
    if (on) {
        s_fb[y][x / 8] |= bit;
    } else {
        s_fb[y][x / 8] &= (uint8_t)~bit;
    }
}

void slots_gfx_vline(int x, int y0, int y1, bool on)
{
    for (int y = y0; y <= y1; y++) {
        slots_gfx_set_pixel(x, y, on);
    }
}

void slots_gfx_draw_symbol(int x0, int y0, slots_symbol_t symbol)
{
    if ((unsigned)symbol >= SLOTS_SYMBOL_COUNT) {
        return;
    }
    const uint16_t *rows = slots_art_symbol[symbol];
    for (int row = 0; row < 16; row++) {
        int y = y0 + row;
        if (y < 0 || y >= SLOTS_GFX_ROWS) {
            continue;           /* vertical clip — partial symbols at the window edges */
        }
        uint16_t bits = rows[row];
        for (int col = 0; col < 16; col++) {
            if (bits & (0x8000u >> col)) {
                slots_gfx_set_pixel(x0 + col, y, true);
            }
        }
    }
}

void slots_gfx_draw_sprite5(int x0, int y0, const uint8_t rows[5])
{
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            if (rows[row] & (0x10u >> col)) {
                slots_gfx_set_pixel(x0 + col, y0 + row, true);
            }
        }
    }
}

/* ---- 3x5 font (same glyph data as demoscene and the Games app) ----------- */

static const uint8_t k_font_digits[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,2,2}, {7,5,7,5,7}, {7,5,7,1,7},
};

static const uint8_t k_font_upper[26][5] = {
    {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6}, {7,4,7,4,7},
    {7,4,7,4,4}, {3,4,5,5,3}, {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2},
    {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5}, {6,5,5,5,5}, {7,5,5,5,7},
    {7,5,7,4,4}, {7,5,5,7,1}, {7,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
    {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2},
    {7,1,2,4,7},
};

static const uint8_t *glyph_rows(char c)
{
    static const uint8_t k_space[5] = {0,0,0,0,0};
    static const uint8_t k_bang[5]  = {2,2,2,0,2};
    static const uint8_t k_colon[5] = {0,2,0,2,0};

    if (c >= '0' && c <= '9') { return k_font_digits[c - '0']; }
    if (c >= 'A' && c <= 'Z') { return k_font_upper[c - 'A']; }
    if (c >= 'a' && c <= 'z') { return k_font_upper[c - 'a']; }
    if (c == '!') { return k_bang; }
    if (c == ':') { return k_colon; }
    return k_space;
}

static void stamp(int x, int y, const char *s, int scale, bool on)
{
    for (; *s != '\0'; s++) {
        const uint8_t *rows = glyph_rows(*s);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                if (rows[r] & (4u >> c)) {
                    for (int j = 0; j < scale; j++) {
                        for (int i = 0; i < scale; i++) {
                            slots_gfx_set_pixel(x + c * scale + i, y + r * scale + j, on);
                        }
                    }
                }
            }
        }
        x += 4 * scale;
    }
}

void slots_gfx_text(int x, int y, const char *s, int scale)
{
    stamp(x, y, s, scale, true);
}

int slots_gfx_text_w(const char *s, int scale)
{
    int n = 0;
    while (s[n] != '\0') { n++; }
    return n > 0 ? n * 4 * scale - scale : 0;
}

void slots_gfx_text_halo(int x, int y, const char *s, int scale)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dy != 0) {
                stamp(x + dx, y + dy, s, scale, false);
            }
        }
    }
    stamp(x, y, s, scale, true);
}

static const uint8_t k_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

bool slots_gfx_dither(int x, int y, uint8_t brightness)
{
    uint8_t threshold = (uint8_t)(k_bayer[y & 3][x & 3] * 16u + 8u);
    return brightness > threshold;
}

void slots_gfx_invert(void)
{
    uint8_t *p = &s_fb[0][0];
    for (size_t i = 0; i < sizeof(s_fb); i++) {
        p[i] = (uint8_t)~p[i];
    }
}

void slots_gfx_invert_band(int y0, int y1)
{
    if (y0 < 0) { y0 = 0; }
    if (y1 > SLOTS_GFX_ROWS - 1) { y1 = SLOTS_GFX_ROWS - 1; }
    for (int y = y0; y <= y1; y++) {
        for (int b = 0; b < ROW_BYTES; b++) {
            s_fb[y][b] = (uint8_t)~s_fb[y][b];
        }
    }
}

jpp_sdk_status_t slots_gfx_flush(jpp_sdk_context_t *ctx)
{
    for (uint8_t row = 0; row < SLOTS_GFX_ROWS; row++) {
        jpp_sdk_status_t status = jpp_sdk_canvas_write(ctx, row, s_fb[row]);
        if (status != JPP_SDK_STATUS_OK) {
            return status;
        }
    }
    return JPP_SDK_STATUS_OK;
}
