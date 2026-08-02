#include "mtp_api.h"

#include <stdio.h>
#include <string.h>

#include "mtp_schema.h"
#include "mtp_time.h"

#pragma GCC visibility push(hidden)

bool mtp_api_write_input_peer(mtp_w_t *w, int peer_index)
{
    const mtp_peer_t *p = mtp_peer_at(peer_index);
    if (p == NULL) {
        return false;
    }
    switch (p->kind) {
    case MTP_PEER_USER:
        if (p->is_self) {
            /* inputPeerSelf is accepted where a user peer is expected and needs
               no access_hash, which we may not have for our own account. */
            mtp_w_u32(w, MTP_ID_INPUTPEERSELF);
            return true;
        }
        mtp_w_u32(w, MTP_ID_INPUTPEERUSER);
        mtp_w_i64(w, p->id);
        mtp_w_i64(w, p->access_hash);
        return true;
    case MTP_PEER_CHAT:
        /* Basic groups are addressed by id alone. */
        mtp_w_u32(w, MTP_ID_INPUTPEERCHAT);
        mtp_w_i64(w, p->id);
        return true;
    case MTP_PEER_CHANNEL:
        mtp_w_u32(w, MTP_ID_INPUTPEERCHANNEL);
        mtp_w_i64(w, p->id);
        mtp_w_i64(w, p->access_hash);
        return true;
    default:
        return false;
    }
}

/* Copy a message body into a dialog preview, which is deliberately much shorter.
   Done explicitly rather than by snprintf truncation so the intent is visible and
   the compiler has nothing to warn about. */
static void set_preview(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1u;
        /* Never cut a UTF-8 sequence in half. */
        while (n > 0u && ((unsigned char)src[n] & 0xC0u) == 0x80u) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- Dialogs ------------------------------------------------------------- */

/*
 * The messages vector that accompanies a dialogs page supplies each row's
 * preview. Walked before the dialogs themselves are displayed, and matched to
 * them by the conversation peer.
 *
 * Returns false when the vector could not be fully traversed — a top message
 * that is a media object reaches mtp_skip's opaque field, which leaves the
 * reader stranded mid-object. The previews already collected stand, but the
 * caller must not continue past this point (the chats/users vectors would be
 * read from the wrong offset), so the return value gates exactly that.
 */
static bool absorb_dialog_messages(mtp_r_t *r)
{
    uint32_t n = mtp_r_vector(r);
    for (uint32_t i = 0u; i < n && mtp_r_ok(r); i++) {
        mtp_message_t msg;
        /*
         * A message we cannot traverse ends the walk — but the previews already
         * collected stand, and every dialog row still has its title and unread
         * count from the dialogs vector. A missing preview is a cosmetic loss.
         */
        bool ok = mtp_parse_message(r, &msg);

        /*
         * mtp_parse_message reports the *sender*; the row this belongs to is the
         * conversation. For a private chat those differ on incoming messages, so
         * the dialog is found by matching either.
         */
        int at = -1;
        if (msg.peer >= 0) {
            at = mtp_dialog_find_peer(msg.peer);
        }
        if (at >= 0) {
            mtp_dialog_t *d = (mtp_dialog_t *)mtp_dialog_at(at);
            if (d != NULL && d->top_message == msg.id) {
                set_preview(d->preview, sizeof(d->preview), msg.text);
                d->preview_out = msg.out;
                d->date = msg.date;
            }
        }
        if (!ok) {
            return false;   /* reader is mid-object; nothing further is reachable */
        }
    }
    return true;
}

mtp_err_t mtp_api_get_dialogs(void)
{
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_MESSAGES_GETDIALOGS);
    mtp_w_u32(&w, 0u);                       /* flags: no folder, include pinned */
    mtp_w_i32(&w, 0);                        /* offset_date */
    mtp_w_i32(&w, 0);                        /* offset_id */
    mtp_w_u32(&w, MTP_ID_INPUTPEEREMPTY);    /* offset_peer */
    mtp_w_i32(&w, MTP_DIALOG_PAGE);
    mtp_w_i64(&w, 0);                        /* hash: 0 disables the not-modified
                                                short-circuit, which we want since
                                                nothing is cached across launches */
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err != MTP_OK) {
        char ev[40];
        snprintf(ev, sizeof(ev), "get_dialogs_fail_%d", (int)err);
        mtp_log(ev);
        return err;
    }

    mtp_r_t r;
    mtp_r_init(&r, res, res_len);
    uint32_t id = mtp_r_u32(&r);
    if (id == MTP_ID_MESSAGES_DIALOGSNOTMODIFIED) {
        return MTP_OK;
    }
    if (id == MTP_ID_MESSAGES_DIALOGSSLICE) {
        (void)mtp_r_i32(&r);                 /* total count, unused */
    } else if (id != MTP_ID_MESSAGES_DIALOGS) {
        return MTP_ERR_PROTO;
    }

    /*
     * Order on the wire is dialogs, messages, chats, users — but names live in
     * the last two, so the dialog rows are built first and the peer table is
     * filled in behind them. Interning by id means the rows pick up the names
     * without a second pass.
     */
    mtp_dialogs_clear();
    uint32_t n = mtp_r_vector(&r);
    for (uint32_t i = 0u; i < n && mtp_r_ok(&r); i++) {
        if (!mtp_parse_dialog(&r)) {
            return MTP_OK;   /* partial list is still worth showing */
        }
    }
    bool absorbed = absorb_dialog_messages(&r);
    if (!mtp_r_ok(&r)) {
        return MTP_OK;
    }
    /*
     * Only when the messages vector was fully walked is the reader positioned
     * at the start of the chats vector. If a media top message stranded it, the
     * chats and users (carrying the names) can no longer be reached — reading
     * them from the wrong offset would only corrupt the peer table, so stop.
     */
    if (absorbed) {
        mtp_parse_chat_vector(&r);
        mtp_parse_user_vector(&r);
    }
    {
        char ev[40];
        snprintf(ev, sizeof(ev), "get_dialogs_n_%d", mtp_dialogs_count());
        mtp_log(ev);
    }
    return MTP_OK;
}

/* ---- History ------------------------------------------------------------- */

mtp_err_t mtp_api_get_history(int peer_index, int32_t offset_id,
                              int32_t *out_stopped_at)
{
    if (out_stopped_at != NULL) {
        *out_stopped_at = 0;
    }

    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_MESSAGES_GETHISTORY);
    if (!mtp_api_write_input_peer(&w, peer_index)) {
        return MTP_ERR_ARG;
    }
    mtp_w_i32(&w, offset_id);
    mtp_w_i32(&w, 0);                    /* offset_date */
    mtp_w_i32(&w, 0);                    /* add_offset */
    mtp_w_i32(&w, MTP_HISTORY_PAGE);
    mtp_w_i32(&w, 0);                    /* max_id */
    mtp_w_i32(&w, 0);                    /* min_id */
    mtp_w_i64(&w, 0);                    /* hash */
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err != MTP_OK) {
        char ev[48];
        snprintf(ev, sizeof(ev), "get_history_fail_%d", (int)err);
        mtp_log(ev);
        return err;
    }

    mtp_r_t r;
    mtp_r_init(&r, res, res_len);
    uint32_t id = mtp_r_u32(&r);
    if (id == MTP_ID_MESSAGES_MESSAGESNOTMODIFIED) {
        return MTP_OK;
    }
    if (id == MTP_ID_MESSAGES_MESSAGESSLICE) {
        (void)mtp_r_u32(&r);             /* flags */
        (void)mtp_r_i32(&r);             /* count */
        /* next_rate / offset_id_offset / search_flood are flag-gated and this
           client never sets the flags that would produce them. */
    } else if (id == MTP_ID_MESSAGES_CHANNELMESSAGES) {
        (void)mtp_r_u32(&r);             /* flags */
        (void)mtp_r_i32(&r);             /* pts */
        (void)mtp_r_i32(&r);             /* count */
    } else if (id != MTP_ID_MESSAGES_MESSAGES) {
        return MTP_ERR_PROTO;
    }

    uint32_t n = mtp_r_vector(&r);
    for (uint32_t i = 0u; i < n && mtp_r_ok(&r); i++) {
        mtp_message_t msg;
        bool ok = mtp_parse_message(&r, &msg);
        if (msg.id != 0) {
            (void)mtp_message_insert(&msg);
        }
        if (!ok) {
            /*
             * Stopped on a media or service message. Its id is known, so the
             * caller can ask for the next page starting just past it — the batch
             * is short, not lost.
             */
            if (out_stopped_at != NULL) {
                *out_stopped_at = msg.id;
            }
            return MTP_OK;
        }
    }
    if (!mtp_r_ok(&r)) {
        return MTP_OK;
    }
    /* Trailing topics, chats and users. Names matter for group senders. */
    mtp_r_t save = r;
    if (mtp_r_peek_u32(&r) == MTP_ID_VECTOR) {
        uint32_t topics = mtp_r_vector(&r);
        /* ForumTopic is in the skip table, so these can be stepped over. */
        for (uint32_t i = 0u; i < topics && mtp_r_ok(&r); i++) {
            if (mtp_skip(&r, MTP_T_FORUMTOPIC) != MTP_SKIP_OK) {
                return MTP_OK;
            }
        }
    } else {
        r = save;
    }
    mtp_parse_chat_vector(&r);
    mtp_parse_user_vector(&r);
    return MTP_OK;
}

/* ---- Sending ------------------------------------------------------------- */

mtp_err_t mtp_api_send_message(int peer_index, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return MTP_ERR_ARG;
    }

    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    uint64_t random_id;
    randombytes_buf(&random_id, sizeof(random_id));

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_MESSAGES_SENDMESSAGE);
    mtp_w_u32(&w, 0u);                   /* flags: plain text, no reply markup */
    if (!mtp_api_write_input_peer(&w, peer_index)) {
        return MTP_ERR_ARG;
    }
    mtp_w_str(&w, text);
    mtp_w_u64(&w, random_id);            /* deduplicates a resend after a retry */
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err != MTP_OK) {
        char ev[48];
        snprintf(ev, sizeof(ev), "send_message_fail_%d", (int)err);
        mtp_log(ev);
        return err;
    }

    /*
     * The reply is an Updates carrying the real message. Rather than parse it —
     * Updates has 150-odd variants — the local copy is synthesised here so the
     * bubble appears immediately, and the server's own copy arrives through the
     * update stream and replaces it by id.
     */
    mtp_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.id = 0;
    msg.peer = peer_index;
    msg.date = mtp_time_now();
    msg.out = true;
    snprintf(msg.text, sizeof(msg.text), "%s", text);

    /* A local-only id above anything the server has issued, so it sorts last and
       is replaced rather than duplicated when the echo lands. */
    static int32_t s_local_id = 0x7F000000;
    msg.id = ++s_local_id;
    (void)mtp_message_insert(&msg);
    return MTP_OK;
}

mtp_err_t mtp_api_read_history(int peer_index, int32_t max_id)
{
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_MESSAGES_READHISTORY);
    if (!mtp_api_write_input_peer(&w, peer_index)) {
        return MTP_ERR_ARG;
    }
    mtp_w_i32(&w, max_id);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    const uint8_t *res;
    size_t res_len;
    return mtp_client_invoke(buf, w.len, &res, &res_len);
}

mtp_err_t mtp_api_set_typing(int peer_index, bool typing)
{
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_MESSAGES_SETTYPING);
    mtp_w_u32(&w, 0u);                   /* flags: no top_msg_id */
    if (!mtp_api_write_input_peer(&w, peer_index)) {
        return MTP_ERR_ARG;
    }
    mtp_w_u32(&w, typing ? MTP_ID_SENDMESSAGETYPINGACTION
                         : MTP_ID_SENDMESSAGECANCELACTION);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    const uint8_t *res;
    size_t res_len;
    return mtp_client_invoke(buf, w.len, &res, &res_len);
}

mtp_err_t mtp_api_get_self(void)
{
    size_t cap;
    uint8_t *buf = mtp_client_req_buf(&cap);

    mtp_w_t w;
    mtp_w_init(&w, buf, cap);
    mtp_w_u32(&w, MTP_ID_USERS_GETUSERS);
    mtp_w_vector(&w, 1u);
    mtp_w_u32(&w, MTP_ID_INPUTUSERSELF);
    if (!mtp_w_ok(&w)) {
        return MTP_ERR_OVERFLOW;
    }

    const uint8_t *res;
    size_t res_len;
    mtp_err_t err = mtp_client_invoke(buf, w.len, &res, &res_len);
    if (err != MTP_OK) {
        return err;
    }

    mtp_r_t r;
    mtp_r_init(&r, res, res_len);
    mtp_parse_user_vector(&r);
    return MTP_OK;
}

#pragma GCC visibility pop
