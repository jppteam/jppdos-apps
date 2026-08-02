#include "mtp_common.h"

#pragma GCC visibility push(hidden)

const char *mtp_err_str(mtp_err_t err)
{
    switch (err) {
    case MTP_OK:                 return "OK";
    case MTP_ERR_ARG:            return "Internal error";
    case MTP_ERR_NET:            return "No connection";
    case MTP_ERR_SERVER_ACTIVE:  return "Stop WebDAV first";
    case MTP_ERR_DENIED:         return "Network denied";
    case MTP_ERR_TIMEOUT:        return "Timed out";
    case MTP_ERR_CLOSED:         return "Connection lost";
    case MTP_ERR_PROTO:          return "Bad server reply";
    case MTP_ERR_OVERFLOW:       return "Reply too large";
    case MTP_ERR_CRYPTO:         return "Crypto failed";
    case MTP_ERR_AUTH_KEY:       return "Key exchange failed";
    case MTP_ERR_RPC:            return "Server error";
    case MTP_ERR_MIGRATE:        return "Switching server";
    case MTP_ERR_NO_CONFIG:      return "Profile not configured";
    case MTP_ERR_STORE:          return "SD card error";
    case MTP_ERR_NO_TIME:        return "Clock not set";
    default:                     return "Unknown error";
    }
}

bool mtp_ct_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0u;
    for (size_t i = 0u; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}

void mtp_be_trim(const uint8_t *in, size_t in_len, const uint8_t **out, size_t *out_len)
{
    size_t skip = 0u;
    while (skip < in_len && in[skip] == 0u) {
        skip++;
    }
    /* An all-zero input trims to a single zero byte rather than to nothing:
       a zero-length TL string is not the same thing as the integer zero. */
    if (skip == in_len && in_len > 0u) {
        skip = in_len - 1u;
    }
    *out = in + skip;
    *out_len = in_len - skip;
}

size_t mtp_hex_decode(const char *hex, uint8_t *out, size_t out_cap)
{
    size_t n = 0u;
    uint8_t hi = 0u;
    bool have_hi = false;

    for (const char *p = hex; *p != '\0'; p++) {
        char c = *p;
        /* Tolerate the whitespace and colons people leave in a pasted key. */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':') {
            continue;
        }
        uint8_t nib;
        if (c >= '0' && c <= '9') {
            nib = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nib = (uint8_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nib = (uint8_t)(c - 'A' + 10);
        } else {
            return 0u;
        }
        if (!have_hi) {
            hi = nib;
            have_hi = true;
        } else {
            if (n >= out_cap) {
                return 0u;
            }
            out[n++] = (uint8_t)((hi << 4) | nib);
            have_hi = false;
        }
    }
    /* An odd digit count means the value was truncated somewhere — refuse it
       rather than silently dropping the last nibble. */
    return have_hi ? 0u : n;
}

bool mtp_parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0u;
    bool any = false;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    for (; *s >= '0' && *s <= '9'; s++) {
        uint32_t digit = (uint32_t)(*s - '0');
        if (v > (0xFFFFFFFFu - digit) / 10u) {
            return false;
        }
        v = v * 10u + digit;
        any = true;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (!any || *s != '\0') {
        return false;
    }
    *out = v;
    return true;
}

#pragma GCC visibility pop
