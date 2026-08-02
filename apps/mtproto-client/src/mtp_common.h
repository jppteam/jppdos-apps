/*
 * mtp_common.h — types, budgets and small helpers shared across the client.
 *
 * Everything here is sized for the 64 KB app pool: the big transfer buffers are
 * declared once in mtp_transport.c and lent out, rather than each layer keeping
 * its own.  Nothing in this app allocates from the heap — the ESP32-C6 shares
 * one SRAM with the WiFi/lwIP buffers, so a static footprint that the loader can
 * reject up front is preferable to a malloc that wedges the radio at runtime.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Budgets ------------------------------------------------------------ */

/*
 * The three transfer buffers, and the reasoning behind their sizes — together
 * they are the largest single claim on the 64 KB app pool, so the numbers matter.
 *
 *   TX      Outgoing requests are small; the largest is set_client_DH_params at
 *           roughly 380 bytes, and a full 224-character message at about 400.
 *           The plaintext envelope adds 32 bytes and the MTProto 2.0 padding a
 *           further 12..27 (mtp_w_pad_random rounds to a 16-byte boundary, so it
 *           never approaches the spec's 1024-byte ceiling).
 *   RX      Holds one encrypted frame as it arrives. Responses of any size worth
 *           having are gzipped, so this bounds the *compressed* form. Sized at
 *           4 KB rather than the 3 KB this was trimmed to: a messages.getDialogs
 *           reply for even a modest account can compress to more than 3 KB, and
 *           the transport treats an oversized frame as fatal ("Reply too large").
 *   INFLATE Holds a gzip_packed body once expanded, which is what the TL parser
 *           actually reads. Necessarily the largest of the three, and it doubles
 *           as the DEFLATE window (see mtp_gzip.c).
 *
 * Only TX and RX are statically allocated. The inflate buffer is the default
 * tenant of the shared arena in mtp_scratch.c, which is sized by SRP and would
 * otherwise sit idle for the entire life of a logged-in session — so this bound
 * costs nothing, and is set to the whole arena rather than to the smallest
 * figure that would do. It is what limits the page sizes the client asks for.
 */
#define MTP_TX_MAX      768u
#define MTP_RX_MAX      4096u
#define MTP_INFLATE_MAX 6656u   /* == MTP_SCRATCH_BYTES; see mtp_scratch.h */

/* 2048-bit operands: RSA modulus, DH prime, g_a/g_b, SRP. */
#define MTP_KEY_BYTES      256u
#define MTP_AUTH_KEY_BYTES 256u

/* ---- Errors ------------------------------------------------------------- */

/*
 * One error space for the whole client so a failure can be reported to the user
 * without every layer inventing its own mapping.  mtp_err_str() gives the short
 * text the UI shows; MTP_ERR_SERVER_ACTIVE exists as its own code because it is
 * the one failure a user can actually fix (stop the WebDAV server) and it must
 * not read as a generic network error.
 */
typedef enum {
    MTP_OK = 0,
    MTP_ERR_ARG,           /* bad argument from our own code — a bug */
    MTP_ERR_NET,           /* connect/send/recv failed */
    MTP_ERR_SERVER_ACTIVE, /* net_connect refused: WebDAV or LRV is running */
    MTP_ERR_DENIED,        /* user denied the network.connect capability */
    MTP_ERR_TIMEOUT,
    MTP_ERR_CLOSED,        /* peer hung up */
    MTP_ERR_PROTO,         /* malformed / unexpected wire data */
    MTP_ERR_OVERFLOW,      /* would not fit a fixed buffer */
    MTP_ERR_CRYPTO,        /* an SDK crypto primitive failed */
    MTP_ERR_AUTH_KEY,      /* handshake produced an unusable auth_key */
    MTP_ERR_RPC,           /* server returned rpc_error; see mtp_rpc last error */
    MTP_ERR_MIGRATE,       /* server wants a different DC */
    MTP_ERR_NO_CONFIG,     /* profile has unfilled placeholder credentials */
    MTP_ERR_STORE,         /* SD read/write failed */
    MTP_ERR_NO_TIME,       /* RTC unavailable and no server time latched yet */
} mtp_err_t;

const char *mtp_err_str(mtp_err_t err);

/* ---- Byte helpers ------------------------------------------------------- */

/*
 * TL is little-endian on the wire; the crypto primitives take big-endian
 * operands.  Both directions are needed constantly, so they live here rather
 * than being open-coded at each call site.
 */
static inline uint32_t mtp_rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t mtp_rd_u64le(const uint8_t *p)
{
    return (uint64_t)mtp_rd_u32le(p) | ((uint64_t)mtp_rd_u32le(p + 4) << 32);
}

static inline void mtp_wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void mtp_wr_u64le(uint8_t *p, uint64_t v)
{
    mtp_wr_u32le(p, (uint32_t)v);
    mtp_wr_u32le(p + 4, (uint32_t)(v >> 32));
}

static inline uint64_t mtp_rd_u64be(const uint8_t *p)
{
    uint64_t v = 0u;
    for (size_t i = 0u; i < 8u; i++) {
        v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

static inline void mtp_wr_u64be(uint8_t *p, uint64_t v)
{
    for (size_t i = 0u; i < 8u; i++) {
        p[7u - i] = (uint8_t)(v >> (i * 8u));
    }
}

/*
 * Constant-time equality.  Used on msg_key and SRP proofs, where an early-exit
 * memcmp would leak how many leading bytes matched.
 */
bool mtp_ct_eq(const uint8_t *a, const uint8_t *b, size_t len);

/*
 * Strip leading zero bytes from a big-endian integer.  MTProto sends DH
 * operands as TL strings whose length is the natural byte length, so a value
 * that came back from modexp (always padded to modulus_len) has to be trimmed
 * before it goes back out, or the server rejects the length.
 */
void mtp_be_trim(const uint8_t *in, size_t in_len, const uint8_t **out, size_t *out_len);

/* Hex parsing for custom.conf — the symbol table has no strtol. Returns the
   number of bytes written, or 0 on any invalid character. */
size_t mtp_hex_decode(const char *hex, uint8_t *out, size_t out_cap);

/* Decimal parsing for custom.conf. Returns false on overflow or bad input. */
bool mtp_parse_u32(const char *s, uint32_t *out);

extern void randombytes_buf(void *buf, size_t size);

/*
 * Serial logging (device console, tag `app_log`). Emits a named event through
 * the App SDK; a no-op until the app is running with an SDK context installed.
 * jpp_sdk_log takes a single event string and no formatting, so callers that
 * want to carry a value snprintf it into a small buffer and pass that.
 *
 * Declared here rather than in a module header so every layer can log without
 * chasing includes. Implemented by the network client (mtp_client.c).
 */
void mtp_log(const char *event_name);
