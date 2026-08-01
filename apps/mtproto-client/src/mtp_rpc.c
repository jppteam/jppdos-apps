#include "mtp_rpc.h"

#include <stdio.h>
#include <string.h>

#include "mtp_gzip.h"
#include "mtp_scratch.h"
#include "mtp_time.h"
#include "mtp_tl.h"

_Static_assert(MTP_INFLATE_MAX == MTP_SCRATCH_BYTES,
               "the inflate buffer is the scratch arena; keep the bounds equal");

/* ---- Service-message constructor ids ------------------------------------ */

#define ID_MSG_CONTAINER        0x73f1f8dcu
#define ID_RPC_RESULT           0xf35c6d01u
#define ID_RPC_ERROR            0x2144ca19u
#define ID_BAD_MSG_NOTIFICATION 0xa7eff811u
#define ID_BAD_SERVER_SALT      0xedab447bu
#define ID_NEW_SESSION_CREATED  0x9ec20908u
#define ID_MSGS_ACK             0x62d6b459u
#define ID_PONG                 0x347773c5u
#define ID_PING_DELAY_DISCONNECT 0xf3427b8cu
#define ID_MSG_DETAILED_INFO    0x276d3ec6u
#define ID_MSG_NEW_DETAILED_INFO 0x809db6dfu
#define ID_RPC_ANSWER_DROPPED   0xa43ad8b7u
#define ID_GZIP_PACKED          MTP_ID_GZIP_PACKED

/* Nested containers are not legal, but a depth cap costs nothing and removes any
   possibility of a crafted response recursing without bound. */
#define MAX_DEPTH 4

/* Acks are batched: sending one per message would double the traffic. */
#define ACK_MAX 16

/* How many times a single call may be resent after a correction. Each retry is
   a legitimate protocol step, but a server stuck in a correction loop must not
   trap the client forever. */
#define MAX_RETRIES 4

static mtp_rpc_update_fn s_update_fn;
static void             *s_update_user;

static uint64_t s_ack_ids[ACK_MAX];
static size_t   s_ack_count;

/* The in-flight call. */
static uint64_t       s_pending_msg_id;
static bool           s_got_result;
static const uint8_t *s_result;
static size_t         s_result_len;

static int  s_error_code;
static char s_error_text[64];
static int  s_migrate_dc;
static int  s_flood_wait;

/*
 * A correction arrived and the pending call has to be reissued. Distinct from an
 * error: the request was well-formed, the envelope was not.
 */
static bool s_needs_resend;

/* The outstanding request, so a correction can reissue it. Not a copy: the body
   lives in the caller's staging buffer (mtp_client_req_buf), which mtp_sess_send
   does not touch, so it is still intact when a resend is needed. */
static const uint8_t *s_call_body;
static size_t         s_call_len;

/*
 * Uncompressed gzip_packed bodies land in the shared arena — see mtp_scratch.h
 * for why that is safe and what the ordering rule is. Everything below reaches
 * it through this one accessor so there is a single place the guards live.
 *
 * Two things can make an inflate unsafe, and both end in a dropped frame rather
 * than a corrupted one:
 *
 *  - The arena is lent out. That would mean a response arrived mid-handshake,
 *    which cannot happen on a correct connection; inflating over a live auth_key
 *    would be far worse than losing the frame.
 *
 *  - A result already decoded into this buffer during the same pass. Containers
 *    make that reachable: one can carry the rpc_result we are waiting for *and*
 *    a gzipped update behind it, and inflating the update would hand the caller
 *    the update's bytes as its result. Dropping the update instead is cheap —
 *    the server resends anything unacknowledged, and updates.getDifference
 *    exists precisely to recover missed ones.
 */
static bool result_in_arena(void)
{
    const uint8_t *base = (const uint8_t *)mtp_scratch_base();
    return s_result != NULL && s_result >= base && s_result < base + MTP_INFLATE_MAX;
}

static uint8_t *inflate_buf(void)
{
    if (mtp_scratch_is_lent() || result_in_arena()) {
        return NULL;
    }
    return (uint8_t *)mtp_scratch_base();
}

void mtp_rpc_set_update_handler(mtp_rpc_update_fn fn, void *user)
{
    s_update_fn = fn;
    s_update_user = user;
}

void mtp_rpc_reset(void)
{
    s_ack_count = 0u;
    s_pending_msg_id = 0u;
    s_got_result = false;
    s_result = NULL;
    s_result_len = 0u;
    s_needs_resend = false;
    s_call_body = NULL;
    s_call_len = 0u;
}

int         mtp_rpc_error_code(void) { return s_error_code; }
const char *mtp_rpc_error_text(void) { return s_error_text; }
int         mtp_rpc_migrate_dc(void) { return s_migrate_dc; }
int         mtp_rpc_flood_wait(void) { return s_flood_wait; }

/* ---- Acknowledgements ---------------------------------------------------- */

static void ack_queue(uint64_t msg_id)
{
    if (s_ack_count >= ACK_MAX) {
        /* Full. Dropping the oldest is safe: the server resends anything it has
           not seen acknowledged, so a lost ack costs a retransmit, not data. */
        memmove(s_ack_ids, s_ack_ids + 1, (ACK_MAX - 1u) * sizeof(s_ack_ids[0]));
        s_ack_count = ACK_MAX - 1u;
    }
    s_ack_ids[s_ack_count++] = msg_id;
}

mtp_err_t mtp_rpc_flush_acks(jpp_sdk_context_t *ctx)
{
    if (s_ack_count == 0u) {
        return MTP_OK;
    }
    uint8_t body[8 + ACK_MAX * 8];
    mtp_w_t w;
    mtp_w_init(&w, body, sizeof(body));
    mtp_w_u32(&w, ID_MSGS_ACK);
    mtp_w_vector(&w, (uint32_t)s_ack_count);
    for (size_t i = 0u; i < s_ack_count; i++) {
        mtp_w_u64(&w, s_ack_ids[i]);
    }
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }
    /* Acks are not content-related — they must not advance seq_no. */
    mtp_err_t err = mtp_sess_send(ctx, body, w.len, false, NULL);
    if (err == MTP_OK) {
        s_ack_count = 0u;
    }
    return err;
}

/* ---- rpc_error ----------------------------------------------------------- */

/*
 * Pull the trailing integer out of errors shaped like PHONE_MIGRATE_4 or
 * FLOOD_WAIT_30. The suffix is the actionable part: which DC to move to, or how
 * long to wait before trying again.
 */
static int trailing_int(const char *text)
{
    const char *p = text + strlen(text);
    while (p > text && p[-1] >= '0' && p[-1] <= '9') {
        p--;
    }
    if (*p == '\0') {
        return -1;
    }
    int v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (*p - '0');
    }
    return v;
}

static void handle_rpc_error(mtp_r_t *r)
{
    s_error_code = mtp_r_i32(r);
    mtp_r_str(r, s_error_text, sizeof(s_error_text));
    s_migrate_dc = -1;
    s_flood_wait = -1;

    /*
     * The MIGRATE family is a redirect, not a failure: the account lives on
     * another DC and the whole handshake has to be repeated there.
     */
    if (strstr(s_error_text, "_MIGRATE_") != NULL) {
        s_migrate_dc = trailing_int(s_error_text);
    } else if (strncmp(s_error_text, "FLOOD_WAIT_", 11) == 0) {
        s_flood_wait = trailing_int(s_error_text);
    }
}

/* ---- Dispatch ------------------------------------------------------------ */

static void dispatch(jpp_sdk_context_t *ctx, uint64_t msg_id, uint32_t seq_no,
                     const uint8_t *body, size_t len, int depth);

/* Unpack msg_container. Note the element vector is *bare* — a count with no
   0x1cb5c415 header — which is why this cannot use mtp_r_vector. */
static void dispatch_container(jpp_sdk_context_t *ctx, mtp_r_t *r, int depth)
{
    uint32_t count = mtp_r_u32(r);
    if (!mtp_r_ok(r) || count > 64u) {
        return;
    }
    for (uint32_t i = 0u; i < count; i++) {
        uint64_t inner_id  = mtp_r_u64(r);
        uint32_t inner_seq = mtp_r_u32(r);
        uint32_t inner_len = mtp_r_u32(r);
        if (!mtp_r_ok(r) || inner_len > mtp_r_left(r)) {
            return;
        }
        const uint8_t *inner = r->buf + r->pos;
        mtp_r_skip(r, inner_len);
        if (!mtp_r_ok(r)) {
            return;
        }
        dispatch(ctx, inner_id, inner_seq, inner, inner_len, depth + 1);
    }
}

static void dispatch(jpp_sdk_context_t *ctx, uint64_t msg_id, uint32_t seq_no,
                     const uint8_t *body, size_t len, int depth)
{
    if (depth > MAX_DEPTH) {
        return;
    }

    mtp_r_t r;
    mtp_r_init(&r, body, len);
    uint32_t id = mtp_r_u32(&r);
    if (!mtp_r_ok(&r)) {
        return;
    }

    switch (id) {
    case ID_MSG_CONTAINER:
        dispatch_container(ctx, &r, depth);
        return;

    case ID_GZIP_PACKED: {
        size_t packed_len = 0u;
        const uint8_t *packed = mtp_r_bytes(&r, &packed_len);
        if (packed == NULL) {
            return;
        }
        uint8_t *out = inflate_buf();
        size_t plain_len = 0u;
        if (out == NULL ||
            mtp_gzip_inflate(packed, packed_len, out, MTP_INFLATE_MAX,
                             &plain_len) != MTP_OK) {
            return;
        }
        dispatch(ctx, msg_id, seq_no, out, plain_len, depth + 1);
        return;
    }

    case ID_RPC_RESULT: {
        uint64_t req_id = mtp_r_u64(&r);
        if (!mtp_r_ok(&r)) {
            return;
        }
        ack_queue(msg_id);
        if (req_id != s_pending_msg_id) {
            /* A reply to something we have already given up on. */
            return;
        }
        const uint8_t *inner = r.buf + r.pos;
        size_t inner_len = mtp_r_left(&r);

        /* The result itself may be gzipped, or may be an error. */
        mtp_r_t ir;
        mtp_r_init(&ir, inner, inner_len);
        uint32_t inner_id = mtp_r_peek_u32(&ir);

        if (inner_id == ID_RPC_ERROR) {
            (void)mtp_r_u32(&ir);
            handle_rpc_error(&ir);
            s_got_result = true;
            s_result = NULL;
            s_result_len = 0u;
            return;
        }
        if (inner_id == ID_GZIP_PACKED) {
            (void)mtp_r_u32(&ir);
            size_t packed_len = 0u;
            const uint8_t *packed = mtp_r_bytes(&ir, &packed_len);
            if (packed == NULL) {
                return;
            }
            uint8_t *out = inflate_buf();
            size_t plain_len = 0u;
            if (out == NULL ||
                mtp_gzip_inflate(packed, packed_len, out, MTP_INFLATE_MAX,
                                 &plain_len) != MTP_OK) {
                return;
            }
            /* An error can be hiding inside the gzip too. */
            mtp_r_t gr;
            mtp_r_init(&gr, out, plain_len);
            if (mtp_r_peek_u32(&gr) == ID_RPC_ERROR) {
                (void)mtp_r_u32(&gr);
                handle_rpc_error(&gr);
                s_got_result = true;
                s_result = NULL;
                s_result_len = 0u;
                return;
            }
            s_result = out;
            s_result_len = plain_len;
            s_got_result = true;
            return;
        }
        s_result = inner;
        s_result_len = inner_len;
        s_got_result = true;
        return;
    }

    case ID_BAD_SERVER_SALT: {
        (void)mtp_r_u64(&r);                  /* bad_msg_id  */
        (void)mtp_r_u32(&r);                  /* bad_msg_seqno */
        (void)mtp_r_i32(&r);                  /* error_code  */
        uint64_t new_salt = mtp_r_u64(&r);
        if (!mtp_r_ok(&r)) {
            return;
        }
        /* Expected during normal operation — salts rotate roughly hourly. Adopt
           it and resend; the request was fine. */
        mtp_sess_set_salt(new_salt);
        s_needs_resend = true;
        return;
    }

    case ID_BAD_MSG_NOTIFICATION: {
        (void)mtp_r_u64(&r);                  /* bad_msg_id */
        (void)mtp_r_u32(&r);                  /* bad_msg_seqno */
        int32_t code = mtp_r_i32(&r);
        if (!mtp_r_ok(&r)) {
            return;
        }
        /*
         * 16, 17 and 20 all mean our msg_id was outside the server's accepted
         * window — the expected consequence of seeding the clock from a
         * minute-resolution RTC in an unknown timezone. mtp_sess_recv has
         * already latched the correct time from this very message's msg_id, so
         * resending is enough. 32/33 are seq_no faults, which mean the session
         * is confused; a fresh session is the documented remedy.
         */
        if (code == 16 || code == 17 || code == 20) {
            s_needs_resend = true;
        } else if (code == 32 || code == 33) {
            mtp_sess_new_session();
            s_needs_resend = true;
        }
        return;
    }

    case ID_NEW_SESSION_CREATED: {
        (void)mtp_r_u64(&r);                  /* first_msg_id */
        (void)mtp_r_u64(&r);                  /* unique_id    */
        uint64_t salt = mtp_r_u64(&r);
        if (mtp_r_ok(&r)) {
            mtp_sess_set_salt(salt);
        }
        ack_queue(msg_id);
        /*
         * The server discarded our session, so anything in flight was lost with
         * it. Resend rather than wait for a reply that will never come.
         */
        if (s_pending_msg_id != 0u && !s_got_result) {
            s_needs_resend = true;
        }
        return;
    }

    case ID_PONG:
    case ID_MSGS_ACK:
    case ID_MSG_DETAILED_INFO:
    case ID_MSG_NEW_DETAILED_INFO:
    case ID_RPC_ANSWER_DROPPED:
        /* Informational. Nothing to do, and nothing to acknowledge — acking an
           ack is how ack storms start. */
        return;

    default:
        /*
         * Anything else is an update (or a container element we do not model).
         * Updates are content-related, so they do need acknowledging.
         */
        ack_queue(msg_id);
        if (s_update_fn != NULL) {
            s_update_fn(s_update_user, body, len);
        }
        return;
    }
}

/* Read one frame and dispatch it. */
static mtp_err_t pump(jpp_sdk_context_t *ctx, uint32_t timeout_ms)
{
    mtp_msg_t msg;
    mtp_err_t err = mtp_sess_recv(ctx, &msg, timeout_ms);
    if (err != MTP_OK) {
        return err;
    }
    dispatch(ctx, msg.msg_id, msg.seq_no, msg.body, msg.len, 0);
    return MTP_OK;
}

/* ---- Public calls -------------------------------------------------------- */

mtp_err_t mtp_rpc_poll(jpp_sdk_context_t *ctx, uint32_t timeout_ms)
{
    /*
     * Whatever the last call returned is dead by the time anyone polls again —
     * callers parse a result before returning to the main loop. Saying so
     * explicitly is what lets updates inflate into the arena again; leaving the
     * pointer set would make inflate_buf refuse every one of them.
     */
    s_got_result = false;
    s_result = NULL;
    s_result_len = 0u;

    mtp_err_t err = pump(ctx, timeout_ms);
    if (err == MTP_OK) {
        /* Drain anything else already buffered before returning, so a burst of
           updates is handled in one pass rather than one per main-loop tick. */
        while (pump(ctx, 0u) == MTP_OK) {
            /* keep going */
        }
        (void)mtp_rpc_flush_acks(ctx);
    }
    return err;
}

mtp_err_t mtp_rpc_call(jpp_sdk_context_t *ctx,
                       const uint8_t *body, size_t len,
                       const uint8_t **out_result, size_t *out_len,
                       uint32_t timeout_ms)
{
    *out_result = NULL;
    *out_len = 0u;
    s_error_code = 0;
    s_error_text[0] = '\0';
    s_migrate_dc = -1;
    s_flood_wait = -1;

    /*
     * Remember where the request is, so a correction can reissue it. `body` must
     * therefore survive mtp_sess_send, which frames in place inside the TX
     * buffer — so it may not point there. mtp_client stages every request in its
     * own buffer for exactly this reason; see mtp_client_req_buf.
     */
    s_call_body = body;
    s_call_len = len;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        s_got_result = false;
        s_needs_resend = false;
        s_result = NULL;
        s_result_len = 0u;

        /* Piggyback any outstanding acks ahead of the request. */
        (void)mtp_rpc_flush_acks(ctx);

        mtp_err_t err = mtp_sess_send(ctx, s_call_body, s_call_len, true,
                                      &s_pending_msg_id);
        if (err != MTP_OK) {
            return err;
        }

        /*
         * Read until the result arrives, the deadline passes, or a correction
         * tells us to start over. Service messages and updates are handled
         * inside dispatch as they come.
         */
        uint32_t waited = 0u;
        const uint32_t slice = 250u;
        while (!s_got_result && !s_needs_resend && waited < timeout_ms) {
            err = pump(ctx, slice);
            if (err == MTP_ERR_TIMEOUT) {
                waited += slice;
                continue;
            }
            if (err != MTP_OK) {
                return err;
            }
        }

        if (s_got_result) {
            s_pending_msg_id = 0u;
            (void)mtp_rpc_flush_acks(ctx);
            if (s_error_code != 0) {
                return (s_migrate_dc > 0) ? MTP_ERR_MIGRATE : MTP_ERR_RPC;
            }
            *out_result = s_result;
            *out_len = s_result_len;
            return MTP_OK;
        }
        if (!s_needs_resend) {
            s_pending_msg_id = 0u;
            return MTP_ERR_TIMEOUT;
        }
        /* Loop and resend under the corrected salt / clock / session. */
    }

    s_pending_msg_id = 0u;
    return MTP_ERR_TIMEOUT;
}

mtp_err_t mtp_rpc_ping(jpp_sdk_context_t *ctx, uint32_t disconnect_delay_s)
{
    uint8_t body[20];
    uint64_t ping_id;
    randombytes_buf(&ping_id, sizeof(ping_id));

    mtp_w_t w;
    mtp_w_init(&w, body, sizeof(body));
    mtp_w_u32(&w, ID_PING_DELAY_DISCONNECT);
    mtp_w_u64(&w, ping_id);
    mtp_w_i32(&w, (int32_t)disconnect_delay_s);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }
    /* Pings are not content-related. */
    return mtp_sess_send(ctx, body, w.len, false, NULL);
}
