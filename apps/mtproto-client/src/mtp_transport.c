#include "mtp_transport.h"

#include <stdio.h>
#include <string.h>

#pragma GCC visibility push(hidden)

/* Selects the intermediate transport; sent once, immediately after connect. */
static const uint8_t TP_MAGIC[4] = { 0xEE, 0xEE, 0xEE, 0xEE };

/* How long to wait for the rest of a frame once its first bytes have landed.
   Generous on purpose: the alternative to waiting is a desynchronised stream. */
#define TP_BODY_TIMEOUT_MS 15000u

/* Telegram's own cap on a single message. A larger length prefix is a protocol
   error, not something to try to allocate for. */
#define TP_FRAME_MAX (1024u * 1024u)

static int s_sock = -1;

static mtp_err_t map_connect_error(const jpp_broker_result_t *result)
{
    const char *code = result->code;
    if (code == NULL) {
        return MTP_ERR_NET;
    }
    if (strcmp(code, "SERVER_ACTIVE") == 0) {
        return MTP_ERR_SERVER_ACTIVE;
    }
    return MTP_ERR_NET;
}

mtp_err_t mtp_tp_connect(jpp_sdk_context_t *ctx, const char *host, uint16_t port)
{
    if (s_sock >= 0) {
        mtp_tp_close(ctx);
    }

    jpp_broker_result_t result;
    memset(&result, 0, sizeof(result));

    jpp_sdk_status_t st = jpp_sdk_net_connect(ctx, host, port, 10000u, &s_sock, &result);
    if (st == JPP_SDK_STATUS_ACCESS_DENIED) {
        s_sock = -1;
        return MTP_ERR_DENIED;
    }
    if (st != JPP_SDK_STATUS_OK || !result.ok || s_sock < 0) {
        s_sock = -1;
        return map_connect_error(&result);
    }

    memset(&result, 0, sizeof(result));
    if (jpp_sdk_net_send(ctx, s_sock, TP_MAGIC, sizeof(TP_MAGIC), &result) != JPP_SDK_STATUS_OK ||
        !result.ok) {
        mtp_tp_close(ctx);
        return MTP_ERR_NET;
    }
    mtp_log("tp_connected");
    return MTP_OK;
}

void mtp_tp_close(jpp_sdk_context_t *ctx)
{
    if (s_sock < 0) {
        return;
    }
    mtp_log("tp_closed");
    jpp_broker_result_t result;
    memset(&result, 0, sizeof(result));
    (void)jpp_sdk_net_close(ctx, s_sock, &result);
    s_sock = -1;
}

bool mtp_tp_is_open(void) { return s_sock >= 0; }

mtp_err_t mtp_tp_send(jpp_sdk_context_t *ctx, const uint8_t *payload, size_t len)
{
    if (s_sock < 0) {
        return MTP_ERR_NET;
    }
    if ((len & 3u) != 0u || len == 0u) {
        return MTP_ERR_ARG;
    }

    uint8_t header[4];
    mtp_wr_u32le(header, (uint32_t)len);

    jpp_broker_result_t result;
    memset(&result, 0, sizeof(result));
    if (jpp_sdk_net_send(ctx, s_sock, header, sizeof(header), &result) != JPP_SDK_STATUS_OK ||
        !result.ok) {
        return MTP_ERR_NET;
    }
    memset(&result, 0, sizeof(result));
    if (jpp_sdk_net_send(ctx, s_sock, payload, len, &result) != JPP_SDK_STATUS_OK ||
        !result.ok) {
        return MTP_ERR_NET;
    }
    return MTP_OK;
}

/*
 * Fill exactly `need` bytes. The first read uses `first_timeout` so a caller
 * polling for updates is not blocked; every read after that uses the body
 * timeout, because by then we are committed to this frame.
 */
static mtp_err_t recv_exact(jpp_sdk_context_t *ctx, uint8_t *buf, size_t need,
                            uint32_t first_timeout)
{
    size_t got = 0u;
    while (got < need) {
        jpp_broker_result_t result;
        memset(&result, 0, sizeof(result));
        size_t n = 0u;
        uint32_t timeout = (got == 0u) ? first_timeout : TP_BODY_TIMEOUT_MS;

        if (jpp_sdk_net_recv(ctx, s_sock, buf + got, need - got, &n, timeout, &result) !=
                JPP_SDK_STATUS_OK || !result.ok) {
            return MTP_ERR_NET;
        }
        if (jpp_broker_result_get(&result, "closed") != NULL) {
            return MTP_ERR_CLOSED;
        }
        if (n == 0u) {
            /* Timed out. Only benign before the first byte; mid-frame it means
               the peer stalled and the stream can no longer be trusted. */
            return (got == 0u) ? MTP_ERR_TIMEOUT : MTP_ERR_CLOSED;
        }
        got += n;
    }
    return MTP_OK;
}

mtp_err_t mtp_tp_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t cap,
                      size_t *out_len, uint32_t timeout_ms)
{
    *out_len = 0u;
    if (s_sock < 0) {
        return MTP_ERR_NET;
    }

    uint8_t header[4];
    mtp_err_t err = recv_exact(ctx, header, sizeof(header), timeout_ms);
    if (err != MTP_OK) {
        return err;
    }

    uint32_t len = mtp_rd_u32le(header);
    if (len == 0u || len > TP_FRAME_MAX || (len & 3u) != 0u) {
        /* Not a length we could have produced or should honour — the stream is
           out of step, and reading on would compound the error. */
        mtp_tp_close(ctx);
        return MTP_ERR_PROTO;
    }
    if (len > cap) {
        char ev[48];
        snprintf(ev, sizeof(ev), "tp_recv_overflow_%lu", (unsigned long)len);
        mtp_log(ev);
        mtp_tp_close(ctx);
        return MTP_ERR_OVERFLOW;
    }

    err = recv_exact(ctx, buf, len, TP_BODY_TIMEOUT_MS);
    if (err != MTP_OK) {
        return err;
    }
    *out_len = len;
    return MTP_OK;
}

bool mtp_tp_frame_is_error(const uint8_t *buf, size_t len, int32_t *out_code)
{
    if (len != 4u) {
        return false;
    }
    int32_t code = (int32_t)mtp_rd_u32le(buf);
    /* Only negative values are transport errors; a 4-byte positive frame would
       be a (malformed) message, not an error report. */
    if (code >= 0) {
        return false;
    }
    if (out_code != NULL) {
        *out_code = code;
    }
    return true;
}

#pragma GCC visibility pop
