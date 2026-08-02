#include "ui_font.h"

#pragma GCC visibility push(hidden)

uint32_t ui_utf8_next(const char *s, size_t *pos)
{
    const uint8_t *p = (const uint8_t *)s + *pos;
    uint8_t c = p[0];
    if (c == 0u) {
        return 0u;
    }

    size_t need;
    uint32_t cp;
    if (c < 0x80u) {
        *pos += 1u;
        return c;
    } else if ((c & 0xE0u) == 0xC0u) {
        need = 1u; cp = (uint32_t)(c & 0x1Fu);
    } else if ((c & 0xF0u) == 0xE0u) {
        need = 2u; cp = (uint32_t)(c & 0x0Fu);
    } else if ((c & 0xF8u) == 0xF0u) {
        need = 3u; cp = (uint32_t)(c & 0x07u);
    } else {
        /* A continuation or invalid lead byte. Advance one and report the
           replacement character: skipping further could swallow real text. */
        *pos += 1u;
        return 0xFFFDu;
    }

    for (size_t i = 1u; i <= need; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            *pos += 1u;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    *pos += need + 1u;
    return cp;
}

size_t ui_utf8_len(const char *s)
{
    size_t pos = 0u, n = 0u;
    while (ui_utf8_next(s, &pos) != 0u) {
        n++;
    }
    return n;
}

size_t ui_utf8_offset(const char *s, size_t n)
{
    size_t pos = 0u;
    for (size_t i = 0u; i < n; i++) {
        if (ui_utf8_next(s, &pos) == 0u) {
            break;
        }
    }
    return pos;
}

const uint8_t *ui_font_glyph(uint32_t cp)
{
    /* Binary search: the table is sorted, and the two covered blocks (ASCII and
       Cyrillic) are far enough apart that a direct index would be mostly holes. */
    if (cp > 0xFFFFu) {
        return ui_font_notdef;
    }
    size_t lo = 0u, hi = ui_font_glyph_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        uint16_t at = ui_font_glyphs[mid].cp;
        if (at == (uint16_t)cp) {
            return ui_font_glyphs[mid].cols;
        }
        if (at < (uint16_t)cp) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return ui_font_notdef;
}

int ui_font_width(const char *s)
{
    size_t pos = 0u;
    int w = 0;
    while (ui_utf8_next(s, &pos) != 0u) {
        w += (int)UI_FONT_ADV;
    }
    /* Drop the trailing inter-character gap so centring is symmetric. */
    return w > 0 ? w - 1 : 0;
}

size_t ui_font_fit(const char *s, int max_px)
{
    size_t pos = 0u, last = 0u;
    int w = 0;
    for (;;) {
        size_t next = pos;
        if (ui_utf8_next(s, &next) == 0u) {
            return last;   /* end of string; pos and last are in step */
        }
        int advance = (w == 0) ? (int)UI_FONT_W : (int)UI_FONT_ADV;
        if (w + advance > max_px) {
            return last;
        }
        w += advance;
        last = next;
        pos = next;
    }
}

#pragma GCC visibility pop
