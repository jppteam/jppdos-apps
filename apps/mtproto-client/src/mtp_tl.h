/*
 * mtp_tl — TL (Type Language) serialisation, the wire encoding under MTProto.
 *
 * Both halves are sticky-error: a writer that overflows sets `ovf` and drops
 * everything after, a reader that runs past the end sets `err` and returns
 * zeroes.  That means a caller can emit or consume a whole message and check
 * once at the end, instead of testing every field — which is what makes the
 * schema code in mtp_schema.c readable.  The important consequence is that a
 * truncated or hostile response can never make the reader walk off the buffer.
 */
#pragma once

#include "mtp_common.h"

/* Vector constructor id — the one TL builtin used often enough to name here. */
#define MTP_ID_VECTOR 0x1cb5c415u

/* ---- Writer ------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     ovf;
} mtp_w_t;

void mtp_w_init(mtp_w_t *w, uint8_t *buf, size_t cap);
void mtp_w_u32(mtp_w_t *w, uint32_t v);
void mtp_w_i32(mtp_w_t *w, int32_t v);
void mtp_w_u64(mtp_w_t *w, uint64_t v);
void mtp_w_i64(mtp_w_t *w, int64_t v);
void mtp_w_bool(mtp_w_t *w, bool v);
/* Fixed-width raw copy — int128, int256 and pre-encoded bodies. */
void mtp_w_raw(mtp_w_t *w, const uint8_t *data, size_t len);
/* TL `bytes`/`string`: length prefix, data, zero-padding to a 4-byte boundary. */
void mtp_w_bytes(mtp_w_t *w, const uint8_t *data, size_t len);
void mtp_w_str(mtp_w_t *w, const char *s);
/* Vector header: constructor + element count. Elements follow. */
void mtp_w_vector(mtp_w_t *w, uint32_t count);
/* Zero-fill to the next `align` boundary (MTProto pads plaintext to 16). */
void mtp_w_pad_to(mtp_w_t *w, size_t align);
/* Random-fill to the next `align` boundary. MTProto 2.0 requires 12..1024 bytes
   of random padding on encrypted payloads, not zeroes. */
void mtp_w_pad_random(mtp_w_t *w, size_t align, size_t min_pad);

static inline bool mtp_w_ok(const mtp_w_t *w) { return !w->ovf; }

/* ---- Reader ------------------------------------------------------------- */

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           err;
} mtp_r_t;

void     mtp_r_init(mtp_r_t *r, const uint8_t *buf, size_t len);
uint32_t mtp_r_u32(mtp_r_t *r);
int32_t  mtp_r_i32(mtp_r_t *r);
uint64_t mtp_r_u64(mtp_r_t *r);
int64_t  mtp_r_i64(mtp_r_t *r);
bool     mtp_r_bool(mtp_r_t *r);
/* Copies `len` bytes out. Sets err and leaves `out` untouched if short. */
void     mtp_r_raw(mtp_r_t *r, uint8_t *out, size_t len);
/*
 * TL bytes, returned as a pointer into the reader's own buffer — no copy, so
 * the result is only valid while that buffer lives. Advances past the padding.
 */
const uint8_t *mtp_r_bytes(mtp_r_t *r, size_t *out_len);
/*
 * TL string into a NUL-terminated C buffer, truncated to fit. Truncation is not
 * an error: a chat title longer than the field it is displayed in is normal.
 * UTF-8 safe — never splits a multi-byte sequence.
 */
void mtp_r_str(mtp_r_t *r, char *out, size_t out_cap);
/* Reads a vector header and returns the element count. Sets err if the
   constructor is not vector#1cb5c415. */
uint32_t mtp_r_vector(mtp_r_t *r);
/* Peeks the next constructor id without consuming it. */
uint32_t mtp_r_peek_u32(const mtp_r_t *r);
void     mtp_r_skip(mtp_r_t *r, size_t len);
/* Skips one TL bytes/string field without returning it. */
void     mtp_r_skip_bytes(mtp_r_t *r);

/*
 * Note there is deliberately no generic "skip one object" here. Sizing an
 * arbitrary TL constructor requires the full schema, and guessing would
 * desynchronise the stream — so each vector walk in mtp_schema.c handles the
 * constructors it knows and stops cleanly at anything else.
 */

static inline size_t mtp_r_left(const mtp_r_t *r)
{
    return r->err || r->pos > r->len ? 0u : r->len - r->pos;
}
static inline bool mtp_r_ok(const mtp_r_t *r) { return !r->err; }
