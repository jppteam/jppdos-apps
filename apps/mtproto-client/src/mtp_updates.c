#include "mtp_updates.h"

#include <stdio.h>
#include <string.h>

#include "mtp_model.h"
#include "mtp_rpc.h"
#include "mtp_schema.h"
#include "mtp_skip.h"
#include "mtp_tl.h"

#pragma GCC visibility push(hidden)

static bool s_dirty;
static int  s_last_peer = -1;

bool mtp_updates_take_dirty(void)
{
    bool was = s_dirty;
    s_dirty = false;
    return was;
}

int mtp_updates_last_message_peer(void) { return s_last_peer; }

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

/* Fold a message into the model and the dialog it belongs to. */
static void absorb_message(const mtp_message_t *msg, int conversation_peer)
{
    int peer = conversation_peer >= 0 ? conversation_peer : msg->peer;
    if (peer < 0) {
        return;
    }

    /*
     * Drop updates that belong to chats the UI does not show. getDialogs only
     * returns the main (non-archived) folder, yet archived chats still push
     * updates over the open connection. A message whose chat is not a known
     * dialog — an archived chat, or one not seen yet — must not buzz or churn
     * the list; the periodic dialog refresh will surface it if it is real.
     */
    if (mtp_dialog_find_peer(peer) < 0 || mtp_dialog_is_archived_peer(peer)) {
        return;
    }

    /* Only the open conversation keeps a message list; others just need their
       preview and unread count updating. */
    if (mtp_messages_peer() == peer) {
        (void)mtp_message_insert(msg);
    }

    int at = mtp_dialog_find_peer(peer);
    if (at >= 0) {
        mtp_dialog_t *d = (mtp_dialog_t *)mtp_dialog_at(at);
        if (d != NULL) {
            set_preview(d->preview, sizeof(d->preview), msg->text);
            d->preview_out = msg->out;
            d->date = msg->date;
            d->top_message = msg->id;
            if (!msg->out && mtp_messages_peer() != peer) {
                d->unread_count++;
            }
        }
    }
    if (!msg->out) {
        s_last_peer = peer;
    }
    s_dirty = true;
}

/*
 * updateShortMessage and updateShortChatMessage are the compact forms the server
 * sends for a plain text message with nothing else going on — which is the common
 * case, and conveniently the one with a fixed layout that needs no skip table.
 */
static void handle_short_message(mtp_r_t *r, bool chat)
{
    uint32_t flags = mtp_r_u32(r);
    mtp_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.id = (int32_t)mtp_r_u32(r);

    int conversation;
    if (chat) {
        int64_t chat_id = 0;
        int64_t from_id = (int64_t)mtp_r_u64(r);
        chat_id = (int64_t)mtp_r_u64(r);
        msg.peer = mtp_peer_intern(MTP_PEER_USER, from_id);
        conversation = mtp_peer_intern(MTP_PEER_CHAT, chat_id);
    } else {
        int64_t user_id = (int64_t)mtp_r_u64(r);
        conversation = mtp_peer_intern(MTP_PEER_USER, user_id);
        msg.peer = conversation;
    }
    mtp_r_str(r, msg.text, sizeof(msg.text));
    (void)mtp_r_u32(r);                    /* pts */
    (void)mtp_r_u32(r);                    /* pts_count */
    msg.date = mtp_r_u32(r);
    msg.out = (flags & (1u << 1)) != 0u;

    if (mtp_r_ok(r)) {
        absorb_message(&msg, conversation);
    }
}

/* One Update variant. */
static void handle_update(mtp_r_t *r)
{
    uint32_t id = mtp_r_u32(r);
    switch (id) {
    case MTP_ID_UPDATENEWMESSAGE:
    case MTP_ID_UPDATENEWCHANNELMESSAGE:
    case MTP_ID_UPDATEEDITMESSAGE:
    case MTP_ID_UPDATEEDITCHANNELMESSAGE: {
        mtp_message_t msg;
        /*
         * A media message stops the parse partway, but everything displayed —
         * id, sender, date, and the "[media]" placeholder — has already been
         * read by then, so the update is still usable.
         */
        (void)mtp_parse_message(r, &msg);
        if (msg.id != 0) {
            absorb_message(&msg, -1);
        }
        return;
    }

    case MTP_ID_UPDATEREADHISTORYINBOX: {
        uint32_t flags = mtp_r_u32(r);
        if ((flags & 1u) != 0u) {
            (void)mtp_r_i32(r);            /* folder_id */
        }
        int peer = mtp_parse_peer(r);
        if ((flags & 2u) != 0u) {
            (void)mtp_r_i32(r);            /* top_msg_id */
        }
        (void)mtp_r_i32(r);                /* max_id */
        int32_t still_unread = mtp_r_i32(r);
        int at = mtp_dialog_find_peer(peer);
        if (at >= 0) {
            mtp_dialog_t *d = (mtp_dialog_t *)mtp_dialog_at(at);
            if (d != NULL) {
                /* Another device read the chat; match it rather than keeping a
                   badge the user already cleared elsewhere. */
                d->unread_count = still_unread;
                s_dirty = true;
            }
        }
        return;
    }

    case MTP_ID_UPDATEREADHISTORYOUTBOX: {
        int peer = mtp_parse_peer(r);
        int32_t max_id = mtp_r_i32(r);
        int at = mtp_dialog_find_peer(peer);
        if (at >= 0) {
            mtp_dialog_t *d = (mtp_dialog_t *)mtp_dialog_at(at);
            if (d != NULL && max_id > d->read_outbox_max_id) {
                /* Drives the single-to-double tick change. */
                d->read_outbox_max_id = max_id;
                s_dirty = true;
            }
        }
        return;
    }

    case MTP_ID_UPDATEUSERSTATUS: {
        int64_t user_id = (int64_t)mtp_r_u64(r);
        uint32_t status = mtp_r_u32(r);
        int peer = mtp_peer_find(MTP_PEER_USER, user_id);
        if (peer >= 0) {
            mtp_peer_t *p = (mtp_peer_t *)mtp_peer_at(peer);
            if (p != NULL) {
                p->online = status == MTP_ID_USERSTATUSONLINE;
                s_dirty = true;
            }
        }
        return;
    }

    default:
        /*
         * Everything else — typing indicators in channels, sticker sets, folder
         * changes, and 140-odd others. Ignored, and the reader is abandoned
         * rather than guessed past: this is the last thing read from the buffer
         * in the single-update forms, and in the vector forms the caller stops.
         */
        return;
    }
}

/* The update container that arrived on the wire. */
static void on_update(void *user, const uint8_t *body, size_t len)
{
    (void)user;

    mtp_r_t r;
    mtp_r_init(&r, body, len);
    uint32_t id = mtp_r_peek_u32(&r);
    {
        char ev[48];
        snprintf(ev, sizeof(ev), "upd_0x%x", (unsigned)id);
        mtp_log(ev);
    }

    switch (id) {
    case MTP_ID_UPDATESHORTMESSAGE:
        (void)mtp_r_u32(&r);
        handle_short_message(&r, false);
        return;

    case MTP_ID_UPDATESHORTCHATMESSAGE:
        (void)mtp_r_u32(&r);
        handle_short_message(&r, true);
        return;

    case MTP_ID_UPDATESHORT:
        (void)mtp_r_u32(&r);
        handle_update(&r);
        return;

    case MTP_ID_UPDATES:
    case MTP_ID_UPDATESCOMBINED: {
        (void)mtp_r_u32(&r);
        uint32_t n = mtp_r_vector(&r);
        for (uint32_t i = 0u; i < n && mtp_r_ok(&r); i++) {
            size_t before = r.pos;
            handle_update(&r);
            /*
             * handle_update consumes a variant it knows and nothing otherwise.
             * Without a way to size the rest, the only safe move is to stop —
             * the updates already applied stand, and anything missed is picked up
             * by the next dialog refresh.
             */
            if (r.pos == before || !mtp_r_ok(&r)) {
                break;
            }
        }
        /* users and chats follow, and carry the names for any new sender. */
        if (mtp_r_ok(&r)) {
            mtp_parse_user_vector(&r);
            mtp_parse_chat_vector(&r);
        }
        return;
    }

    case MTP_ID_UPDATESTOOLONG:
        /*
         * Too much was missed to send individually. A full refresh is the
         * client's answer, since it keeps no pts to catch up from.
         */
        s_dirty = true;
        return;

    default:
        return;
    }
}

void mtp_updates_install(void)
{
    s_dirty = false;
    s_last_peer = -1;
    mtp_rpc_set_update_handler(on_update, NULL);
}

#pragma GCC visibility pop
