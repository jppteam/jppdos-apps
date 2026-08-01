#include "mtp_tl.h"

#include <string.h>

/* TL booleans are constructors, not a bit. */
#define TL_BOOL_TRUE  0x997275b5u
#define TL_BOOL_FALSE 0xbc799737u

/* ---- Writer ------------------------------------------------------------- */

void mtp_w_init(mtp_w_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0u;
    w->ovf = false;
}

/* Single choke point for capacity: every writer below goes through this, so
   there is one place where overflow is detected and made sticky. */
static uint8_t *w_take(mtp_w_t *w, size_t n)
{
    if (w->ovf || w->len + n > w->cap) {
        w->ovf = true;
        return NULL;
    }
    uint8_t *p = w->buf + w->len;
    w->len += n;
    return p;
}

void mtp_w_u32(mtp_w_t *w, uint32_t v)
{
    uint8_t *p = w_take(w, 4u);
    if (p != NULL) {
        mtp_wr_u32le(p, v);
    }
}

void mtp_w_i32(mtp_w_t *w, int32_t v) { mtp_w_u32(w, (uint32_t)v); }

void mtp_w_u64(mtp_w_t *w, uint64_t v)
{
    uint8_t *p = w_take(w, 8u);
    if (p != NULL) {
        mtp_wr_u64le(p, v);
    }
}

void mtp_w_i64(mtp_w_t *w, int64_t v) { mtp_w_u64(w, (uint64_t)v); }

void mtp_w_bool(mtp_w_t *w, bool v)
{
    mtp_w_u32(w, v ? TL_BOOL_TRUE : TL_BOOL_FALSE);
}

void mtp_w_raw(mtp_w_t *w, const uint8_t *data, size_t len)
{
    uint8_t *p = w_take(w, len);
    if (p != NULL) {
        memcpy(p, data, len);
    }
}

void mtp_w_bytes(mtp_w_t *w, const uint8_t *data, size_t len)
{
    /*
     * Two length encodings: a single byte for < 254, otherwise 0xFE plus a
     * 24-bit length. Either way the field is zero-padded so the *total* is a
     * multiple of 4 — which is why the padding is computed from the header size
     * as well as the data size.
     */
    size_t header;
    if (len < 254u) {
        uint8_t *p = w_take(w, 1u);
        if (p != NULL) {
            p[0] = (uint8_t)len;
        }
        header = 1u;
    } else {
        uint8_t *p = w_take(w, 4u);
        if (p != NULL) {
            p[0] = 0xFEu;
            p[1] = (uint8_t)len;
            p[2] = (uint8_t)(len >> 8);
            p[3] = (uint8_t)(len >> 16);
        }
        header = 4u;
    }
    mtp_w_raw(w, data, len);

    size_t pad = (4u - ((header + len) & 3u)) & 3u;
    uint8_t *p = w_take(w, pad);
    if (p != NULL) {
        memset(p, 0, pad);
    }
}

void mtp_w_str(mtp_w_t *w, const char *s)
{
    mtp_w_bytes(w, (const uint8_t *)s, strlen(s));
}

void mtp_w_vector(mtp_w_t *w, uint32_t count)
{
    mtp_w_u32(w, MTP_ID_VECTOR);
    mtp_w_u32(w, count);
}

void mtp_w_pad_to(mtp_w_t *w, size_t align)
{
    size_t pad = (align - (w->len % align)) % align;
    uint8_t *p = w_take(w, pad);
    if (p != NULL) {
        memset(p, 0, pad);
    }
}

void mtp_w_pad_random(mtp_w_t *w, size_t align, size_t min_pad)
{
    /*
     * MTProto 2.0 wants at least 12 bytes of random padding, rounded up so the
     * plaintext is a multiple of 16. Random rather than zero because the padding
     * is inside the AES-IGE envelope and feeds the msg_key — predictable filler
     * would hand an attacker known plaintext.
     */
    size_t pad = min_pad;
    size_t rem = (w->len + pad) % align;
    if (rem != 0u) {
        pad += align - rem;
    }
    uint8_t *p = w_take(w, pad);
    if (p != NULL) {
        randombytes_buf(p, pad);
    }
}

/* ---- Reader ------------------------------------------------------------- */

void mtp_r_init(mtp_r_t *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0u;
    r->err = false;
}

/* The reader's single bounds check, mirroring w_take. */
static const uint8_t *r_take(mtp_r_t *r, size_t n)
{
    if (r->err || r->pos + n > r->len) {
        r->err = true;
        return NULL;
    }
    const uint8_t *p = r->buf + r->pos;
    r->pos += n;
    return p;
}

uint32_t mtp_r_u32(mtp_r_t *r)
{
    const uint8_t *p = r_take(r, 4u);
    return p != NULL ? mtp_rd_u32le(p) : 0u;
}

int32_t mtp_r_i32(mtp_r_t *r) { return (int32_t)mtp_r_u32(r); }

uint64_t mtp_r_u64(mtp_r_t *r)
{
    const uint8_t *p = r_take(r, 8u);
    return p != NULL ? mtp_rd_u64le(p) : 0u;
}

int64_t mtp_r_i64(mtp_r_t *r) { return (int64_t)mtp_r_u64(r); }

bool mtp_r_bool(mtp_r_t *r)
{
    uint32_t id = mtp_r_u32(r);
    if (id == TL_BOOL_TRUE) {
        return true;
    }
    if (id != TL_BOOL_FALSE) {
        r->err = true;
    }
    return false;
}

void mtp_r_raw(mtp_r_t *r, uint8_t *out, size_t len)
{
    const uint8_t *p = r_take(r, len);
    if (p != NULL) {
        memcpy(out, p, len);
    }
}

void mtp_r_skip(mtp_r_t *r, size_t len) { (void)r_take(r, len); }

const uint8_t *mtp_r_bytes(mtp_r_t *r, size_t *out_len)
{
    *out_len = 0u;

    const uint8_t *hdr = r_take(r, 1u);
    if (hdr == NULL) {
        return NULL;
    }

    size_t len;
    size_t header;
    if (hdr[0] == 0xFEu) {
        const uint8_t *p = r_take(r, 3u);
        if (p == NULL) {
            return NULL;
        }
        len = (size_t)p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16);
        header = 4u;
    } else if (hdr[0] == 0xFFu) {
        /* 0xFF introduces TL's 64-bit length form, which this client never has
           a legitimate reason to see — a 4 GB field cannot fit MTP_RX_MAX. */
        r->err = true;
        return NULL;
    } else {
        len = hdr[0];
        header = 1u;
    }

    const uint8_t *data = r_take(r, len);
    if (data == NULL) {
        return NULL;
    }
    (void)r_take(r, (4u - ((header + len) & 3u)) & 3u);
    if (r->err) {
        return NULL;
    }
    *out_len = len;
    return data;
}

void mtp_r_skip_bytes(mtp_r_t *r)
{
    size_t len;
    (void)mtp_r_bytes(r, &len);
}

void mtp_r_str(mtp_r_t *r, char *out, size_t out_cap)
{
    size_t len = 0u;
    const uint8_t *data = mtp_r_bytes(r, &len);

    if (out_cap == 0u) {
        return;
    }
    if (data == NULL) {
        out[0] = '\0';
        return;
    }
    size_t n = len < out_cap - 1u ? len : out_cap - 1u;
    /*
     * Back off a truncation point that landed inside a UTF-8 sequence. Cyrillic
     * is two bytes per character here, so cutting a title at an arbitrary byte
     * would otherwise leave a stray continuation byte and render as garbage.
     */
    if (n < len) {
        while (n > 0u && (data[n] & 0xC0u) == 0x80u) {
            n--;
        }
    }
    memcpy(out, data, n);
    out[n] = '\0';
}

uint32_t mtp_r_vector(mtp_r_t *r)
{
    if (mtp_r_u32(r) != MTP_ID_VECTOR) {
        r->err = true;
        return 0u;
    }
    uint32_t count = mtp_r_u32(r);
    /*
     * A count is only plausible if that many 4-byte minimum elements could still
     * fit. This rejects a corrupt or hostile length before it becomes a long
     * parse loop, without needing to know the element type.
     */
    if (count > mtp_r_left(r) / 4u) {
        r->err = true;
        return 0u;
    }
    return count;
}

uint32_t mtp_r_peek_u32(const mtp_r_t *r)
{
    if (r->err || r->pos + 4u > r->len) {
        return 0u;
    }
    return mtp_rd_u32le(r->buf + r->pos);
}
