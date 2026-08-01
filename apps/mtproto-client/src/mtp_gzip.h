/*
 * mtp_gzip — DEFLATE decompression for gzip_packed bodies.
 *
 * Telegram wraps any response it can usefully shrink in
 * gzip_packed#3072cfa1 packed_data:bytes, and dialog and history pages are
 * always over that threshold. There is no way to opt out, so a client without an
 * inflate implementation cannot read its own message list.
 *
 * The 32 KB DEFLATE window would not fit this device's budget, and it does not
 * have to: back-references only ever point at bytes already produced, so the
 * output buffer *is* the window. That works precisely because MTP_RX_MAX bounds
 * the uncompressed size — which is also why the client asks the server for small
 * page sizes rather than defaulting to 100 dialogs at a time.
 */
#pragma once

#include "mtp_common.h"

/* gzip_packed#3072cfa1 packed_data:bytes = Object */
#define MTP_ID_GZIP_PACKED 0x3072cfa1u

/*
 * Inflate a gzip stream (RFC 1952 header, RFC 1951 payload) into `out`.
 *
 * Returns MTP_ERR_OVERFLOW if the result does not fit — never a partial write
 * the caller might mistake for success — and MTP_ERR_PROTO on a malformed
 * stream. The CRC32 and length trailer are not verified: the message already
 * passed the msg_key check, so its integrity is established, and re-checking
 * would cost code space for no additional guarantee.
 */
mtp_err_t mtp_gzip_inflate(const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_cap, size_t *out_len);
