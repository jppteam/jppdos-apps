#include "mtp_gzip.h"

#include <string.h>

#pragma GCC visibility push(hidden)

/* DEFLATE's alphabets: 288 literal/length symbols, 30 distance symbols, and 19
   code-length symbols for the dynamic-header encoding. */
#define SYM_MAX 288

typedef struct {
    const uint8_t *in;
    size_t         in_len;
    size_t         in_pos;
    uint32_t       bit_buf;
    uint32_t       bit_cnt;

    uint8_t *out;
    size_t   out_cap;
    size_t   out_pos;

    bool err;
} inf_t;

/* A canonical Huffman table in the compact form RFC 1951 implies: how many codes
   exist of each bit length, plus the symbols ordered by code value. */
typedef struct {
    uint16_t counts[16];
    uint16_t symbols[SYM_MAX];
} huff_t;

/* ---- Bit reader ---------------------------------------------------------- */

static uint32_t get_bits(inf_t *d, uint32_t n)
{
    while (d->bit_cnt < n) {
        if (d->in_pos >= d->in_len) {
            /* Running out of input mid-symbol is a malformed stream. Feeding
               zeroes keeps the decoder from looping, and err stops the caller
               from trusting the output. */
            d->err = true;
            return 0u;
        }
        d->bit_buf |= (uint32_t)d->in[d->in_pos++] << d->bit_cnt;
        d->bit_cnt += 8u;
    }
    uint32_t v = d->bit_buf & ((1u << n) - 1u);
    d->bit_buf >>= n;
    d->bit_cnt -= n;
    return v;
}

/* ---- Huffman ------------------------------------------------------------- */

static void huff_build(huff_t *h, const uint8_t *lengths, size_t count)
{
    memset(h->counts, 0, sizeof(h->counts));
    for (size_t i = 0u; i < count; i++) {
        h->counts[lengths[i]]++;
    }
    /* Length 0 means "symbol unused"; it must not occupy a code. */
    h->counts[0] = 0u;

    uint16_t offsets[16];
    offsets[0] = 0u;
    for (size_t i = 1u; i < 16u; i++) {
        offsets[i] = (uint16_t)(offsets[i - 1u] + h->counts[i - 1u]);
    }
    for (size_t i = 0u; i < count; i++) {
        if (lengths[i] != 0u) {
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
        }
    }
}

/*
 * Walk the tree one bit at a time. Slower than a lookup table but a fraction of
 * the code and none of the memory, which is the right trade at this scale.
 */
static int huff_decode(inf_t *d, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)get_bits(d, 1u);
        if (d->err) {
            return -1;
        }
        int count = h->counts[len];
        if (code - first < count) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    d->err = true;
    return -1;
}

/* ---- Blocks -------------------------------------------------------------- */

static const uint16_t LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static bool out_byte(inf_t *d, uint8_t b)
{
    if (d->out_pos >= d->out_cap) {
        d->err = true;
        return false;
    }
    d->out[d->out_pos++] = b;
    return true;
}

static void inflate_block(inf_t *d, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym = huff_decode(d, lit);
        if (d->err) {
            return;
        }
        if (sym < 256) {
            if (!out_byte(d, (uint8_t)sym)) {
                return;
            }
            continue;
        }
        if (sym == 256) {
            return;          /* end of block */
        }
        sym -= 257;
        if (sym >= 29) {
            d->err = true;
            return;
        }
        size_t length = (size_t)LEN_BASE[sym] + get_bits(d, LEN_EXTRA[sym]);

        int dsym = huff_decode(d, dist);
        if (d->err || dsym < 0 || dsym >= 30) {
            d->err = true;
            return;
        }
        size_t distance = (size_t)DIST_BASE[dsym] + get_bits(d, DIST_EXTRA[dsym]);
        if (d->err) {
            return;
        }
        /*
         * The reference must land inside what we have already produced. This is
         * the check that makes using the output buffer as the window safe: a
         * corrupt distance would otherwise read before the start of the buffer.
         */
        if (distance == 0u || distance > d->out_pos) {
            d->err = true;
            return;
        }
        /* Copied one byte at a time on purpose — overlapping copies are how
           DEFLATE encodes runs, so memcpy would be wrong here. */
        size_t from = d->out_pos - distance;
        for (size_t i = 0u; i < length; i++) {
            if (!out_byte(d, d->out[from + i])) {
                return;
            }
        }
    }
}

static void inflate_fixed(inf_t *d)
{
    /* The fixed tables from RFC 1951 §3.2.6, built rather than stored. */
    uint8_t lengths[SYM_MAX];
    for (int i = 0; i < 144; i++)  lengths[i] = 8u;
    for (int i = 144; i < 256; i++) lengths[i] = 9u;
    for (int i = 256; i < 280; i++) lengths[i] = 7u;
    for (int i = 280; i < 288; i++) lengths[i] = 8u;

    huff_t lit, dist;
    huff_build(&lit, lengths, 288u);
    for (int i = 0; i < 30; i++) {
        lengths[i] = 5u;
    }
    huff_build(&dist, lengths, 30u);
    inflate_block(d, &lit, &dist);
}

static void inflate_dynamic(inf_t *d)
{
    /* The order code lengths are transmitted in — deliberately arranged so the
       common lengths come first and trailing entries can be omitted. */
    static const uint8_t ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    uint32_t hlit  = get_bits(d, 5u) + 257u;
    uint32_t hdist = get_bits(d, 5u) + 1u;
    uint32_t hclen = get_bits(d, 4u) + 4u;
    if (d->err || hlit > 286u || hdist > 30u) {
        d->err = true;
        return;
    }

    uint8_t cl_lengths[19] = { 0 };
    for (uint32_t i = 0u; i < hclen; i++) {
        cl_lengths[ORDER[i]] = (uint8_t)get_bits(d, 3u);
    }
    if (d->err) {
        return;
    }
    huff_t cl;
    huff_build(&cl, cl_lengths, 19u);

    /* Literal/length and distance lengths share one run-length-coded stream. */
    uint8_t lengths[SYM_MAX + 30];
    memset(lengths, 0, sizeof(lengths));
    uint32_t total = hlit + hdist;
    uint32_t n = 0u;
    while (n < total) {
        int sym = huff_decode(d, &cl);
        if (d->err || sym < 0) {
            d->err = true;
            return;
        }
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            /* Repeat the previous length 3..6 times. */
            if (n == 0u) {
                d->err = true;
                return;
            }
            uint8_t prev = lengths[n - 1u];
            uint32_t rep = 3u + get_bits(d, 2u);
            while (rep-- > 0u && n < total) {
                lengths[n++] = prev;
            }
        } else if (sym == 17) {
            uint32_t rep = 3u + get_bits(d, 3u);
            while (rep-- > 0u && n < total) {
                lengths[n++] = 0u;
            }
        } else {
            uint32_t rep = 11u + get_bits(d, 7u);
            while (rep-- > 0u && n < total) {
                lengths[n++] = 0u;
            }
        }
        if (d->err) {
            return;
        }
    }

    huff_t lit, dist;
    huff_build(&lit, lengths, hlit);
    huff_build(&dist, lengths + hlit, hdist);
    inflate_block(d, &lit, &dist);
}

static void inflate_stored(inf_t *d)
{
    /* Stored blocks are byte-aligned, so any partial byte is discarded first. */
    d->bit_buf = 0u;
    d->bit_cnt = 0u;
    if (d->in_pos + 4u > d->in_len) {
        d->err = true;
        return;
    }
    uint32_t len  = (uint32_t)d->in[d->in_pos] | ((uint32_t)d->in[d->in_pos + 1u] << 8);
    uint32_t nlen = (uint32_t)d->in[d->in_pos + 2u] | ((uint32_t)d->in[d->in_pos + 3u] << 8);
    d->in_pos += 4u;
    /* The header carries the length and its complement; a mismatch means the
       stream is damaged. */
    if ((len ^ 0xFFFFu) != nlen) {
        d->err = true;
        return;
    }
    if (d->in_pos + len > d->in_len || d->out_pos + len > d->out_cap) {
        d->err = true;
        return;
    }
    memcpy(d->out + d->out_pos, d->in + d->in_pos, len);
    d->in_pos += len;
    d->out_pos += len;
}

/* ---- Entry point --------------------------------------------------------- */

mtp_err_t mtp_gzip_inflate(const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    *out_len = 0u;

    /* RFC 1952 header: magic 1f 8b, method 8 (deflate), flags, mtime, xfl, os. */
    if (in_len < 18u || in[0] != 0x1Fu || in[1] != 0x8Bu || in[2] != 0x08u) {
        return MTP_ERR_PROTO;
    }
    uint8_t flags = in[3];
    size_t pos = 10u;

    if ((flags & 0x04u) != 0u) {            /* FEXTRA */
        if (pos + 2u > in_len) {
            return MTP_ERR_PROTO;
        }
        size_t xlen = (size_t)in[pos] | ((size_t)in[pos + 1u] << 8);
        pos += 2u + xlen;
    }
    if ((flags & 0x08u) != 0u) {            /* FNAME, NUL-terminated */
        while (pos < in_len && in[pos] != 0u) {
            pos++;
        }
        pos++;
    }
    if ((flags & 0x10u) != 0u) {            /* FCOMMENT */
        while (pos < in_len && in[pos] != 0u) {
            pos++;
        }
        pos++;
    }
    if ((flags & 0x02u) != 0u) {            /* FHCRC */
        pos += 2u;
    }
    if (pos >= in_len) {
        return MTP_ERR_PROTO;
    }

    inf_t d;
    memset(&d, 0, sizeof(d));
    d.in = in;
    d.in_len = in_len;
    d.in_pos = pos;
    d.out = out;
    d.out_cap = out_cap;

    bool final = false;
    while (!final && !d.err) {
        final = get_bits(&d, 1u) != 0u;
        uint32_t type = get_bits(&d, 2u);
        switch (type) {
        case 0u: inflate_stored(&d);  break;
        case 1u: inflate_fixed(&d);   break;
        case 2u: inflate_dynamic(&d); break;
        default: d.err = true;        break;   /* type 3 is reserved */
        }
    }
    if (d.err) {
        /*
         * Report overflow distinctly from corruption: hitting the cap means the
         * response was simply bigger than this device asks for, which is a
         * different problem from a damaged stream.
         */
        return (d.out_pos >= d.out_cap) ? MTP_ERR_OVERFLOW : MTP_ERR_PROTO;
    }
    *out_len = d.out_pos;
    return MTP_OK;
}

#pragma GCC visibility pop
