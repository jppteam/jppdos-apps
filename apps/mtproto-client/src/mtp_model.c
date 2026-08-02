#include "mtp_model.h"

#include <stdio.h>
#include <string.h>

#include "mtp_mem.h"
#include "mtp_schema.h"

#pragma GCC visibility push(hidden)

/* The three tables live on the heap, not in the app pool — see mtp_mem.h. */
static mtp_peer_t   *s_peers;
static int           s_peer_count;

static mtp_dialog_t *s_dialogs;
static int           s_dialog_count;

static mtp_message_t *s_messages;
static int            s_message_count;
static int            s_message_peer = -1;

mtp_err_t mtp_model_mem_init(void)
{
    s_peers    = mtp_mem_take(MTP_MAX_PEERS    * sizeof(*s_peers));
    s_dialogs  = mtp_mem_take(MTP_MAX_DIALOGS  * sizeof(*s_dialogs));
    s_messages = mtp_mem_take(MTP_MAX_MESSAGES * sizeof(*s_messages));
    return (s_peers != NULL && s_dialogs != NULL && s_messages != NULL)
               ? MTP_OK : MTP_ERR_OVERFLOW;
}

void mtp_model_mem_clear(void)
{
    s_peers = NULL;
    s_dialogs = NULL;
    s_messages = NULL;
    s_peer_count = 0;
    s_dialog_count = 0;
    s_message_count = 0;
    s_message_peer = -1;
}

void mtp_model_reset(void)
{
    s_peer_count = 0;
    s_dialog_count = 0;
    s_message_count = 0;
    s_message_peer = -1;
}

/* ---- Peers --------------------------------------------------------------- */

int mtp_peer_find(mtp_peer_kind_t kind, int64_t id)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].kind == kind && s_peers[i].id == id) {
            return i;
        }
    }
    return -1;
}

int mtp_peer_intern(mtp_peer_kind_t kind, int64_t id)
{
    int found = mtp_peer_find(kind, id);
    if (found >= 0) {
        return found;
    }
    if (s_peer_count >= MTP_MAX_PEERS) {
        /*
         * Table full. Refusing rather than evicting: an evicted peer would leave
         * dialogs and messages pointing at a stranger, which is worse than a
         * chat that shows no name.
         */
        return -1;
    }
    mtp_peer_t *p = &s_peers[s_peer_count];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->id = id;
    p->partial = true;
    return s_peer_count++;
}

const mtp_peer_t *mtp_peer_at(int index)
{
    if (index < 0 || index >= s_peer_count) {
        return NULL;
    }
    return &s_peers[index];
}

void mtp_peer_set_name(int index, const char *name)
{
    if (index < 0 || index >= s_peer_count || name == NULL || name[0] == '\0') {
        return;
    }
    snprintf(s_peers[index].name, sizeof(s_peers[index].name), "%s", name);
    s_peers[index].partial = false;
}

void mtp_peer_set_access(int index, int64_t access_hash)
{
    if (index >= 0 && index < s_peer_count) {
        s_peers[index].access_hash = access_hash;
    }
}

/* ---- Dialogs ------------------------------------------------------------- */

void mtp_dialogs_clear(void) { s_dialog_count = 0; }
int  mtp_dialogs_count(void) { return s_dialog_count; }

const mtp_dialog_t *mtp_dialog_at(int index)
{
    if (index < 0 || index >= s_dialog_count) {
        return NULL;
    }
    return &s_dialogs[index];
}

int mtp_dialog_find_peer(int peer_index)
{
    for (int i = 0; i < s_dialog_count; i++) {
        if (s_dialogs[i].peer == peer_index) {
            return i;
        }
    }
    return -1;
}

bool mtp_dialog_is_archived_peer(int peer_index)
{
    int at = mtp_dialog_find_peer(peer_index);
    return at >= 0 && s_dialogs[at].archived;
}

/* ---- Messages ------------------------------------------------------------ */

void mtp_messages_clear(int peer_index)
{
    s_message_count = 0;
    s_message_peer = peer_index;
}

int mtp_messages_count(void) { return s_message_count; }
int mtp_messages_peer(void)  { return s_message_peer; }

const mtp_message_t *mtp_message_at(int index)
{
    if (index < 0 || index >= s_message_count) {
        return NULL;
    }
    return &s_messages[index];
}

int32_t mtp_messages_oldest_id(void)
{
    return s_message_count > 0 ? s_messages[0].id : 0;
}

int mtp_message_insert(const mtp_message_t *msg)
{
    /* Ordered oldest-first. History arrives newest-first and updates arrive one
       at a time, so an insertion sort over a list this short is simpler than
       tracking which direction the caller is filling from. */
    for (int i = 0; i < s_message_count; i++) {
        if (s_messages[i].id == msg->id) {
            s_messages[i] = *msg;      /* an edit, or a duplicate delivery */
            return i;
        }
    }

    int at = s_message_count;
    while (at > 0 && s_messages[at - 1].id > msg->id) {
        at--;
    }

    if (s_message_count >= MTP_MAX_MESSAGES) {
        if (at == 0) {
            return -1;   /* older than everything held, and no room */
        }
        /* Drop the oldest to make room; the user is reading the recent end. */
        memmove(&s_messages[0], &s_messages[1],
                (size_t)(MTP_MAX_MESSAGES - 1) * sizeof(s_messages[0]));
        s_message_count--;
        at--;
    }

    memmove(&s_messages[at + 1], &s_messages[at],
            (size_t)(s_message_count - at) * sizeof(s_messages[0]));
    s_messages[at] = *msg;
    s_message_count++;
    return at;
}

/* ---- Parsing ------------------------------------------------------------- */

/*
 * Reading a value from the visitor: the reader is positioned at the field but
 * must not be advanced, so every accessor works on a copy of the struct. That is
 * cheap (four words) and removes any chance of desynchronising the traversal.
 */
static uint64_t peek_u64(const mtp_r_t *r)
{
    mtp_r_t tmp = *r;
    return mtp_r_u64(&tmp);
}

static uint32_t peek_u32(const mtp_r_t *r)
{
    mtp_r_t tmp = *r;
    return mtp_r_u32(&tmp);
}

static void peek_str(const mtp_r_t *r, char *out, size_t cap)
{
    mtp_r_t tmp = *r;
    mtp_r_str(&tmp, out, cap);
}

/* Reads a nested Peer without consuming it, returning an interned index. */
static int peek_peer(const mtp_r_t *r)
{
    mtp_r_t tmp = *r;
    uint32_t id = mtp_r_u32(&tmp);
    int64_t value = (int64_t)mtp_r_u64(&tmp);
    if (!mtp_r_ok(&tmp)) {
        return -1;
    }
    switch (id) {
    case MTP_ID_PEERUSER:    return mtp_peer_intern(MTP_PEER_USER, value);
    case MTP_ID_PEERCHAT:    return mtp_peer_intern(MTP_PEER_CHAT, value);
    case MTP_ID_PEERCHANNEL: return mtp_peer_intern(MTP_PEER_CHANNEL, value);
    default:                 return -1;
    }
}

/* ---- User ---------------------------------------------------------------- */

typedef struct {
    int  index;
    char first[MTP_NAME_MAX];
    char last[MTP_NAME_MAX];
    char username[MTP_NAME_MAX];
    bool online;
    bool is_self;
} user_acc_t;

static void user_visit(void *user, uint32_t ctor, unsigned field, uint8_t kind,
                       const mtp_r_t *r, uint32_t flags, uint32_t flags2)
{
    (void)kind; (void)flags2;
    user_acc_t *a = user;

    if (ctor == MTP_ID_USEREMPTY) {
        if (field == MTP_F_USEREMPTY_ID) {
            a->index = mtp_peer_intern(MTP_PEER_USER, (int64_t)peek_u64(r));
        }
        return;
    }
    if (ctor != MTP_ID_USER) {
        return;
    }
    switch (field) {
    case MTP_F_USER_ID:
        a->index = mtp_peer_intern(MTP_PEER_USER, (int64_t)peek_u64(r));
        /* flags.10 is `self`; the account's own entry drives the Saved Messages
           row and the "you" label on outgoing bubbles. */
        a->is_self = (flags & (1u << 10)) != 0u;
        break;
    case MTP_F_USER_ACCESS_HASH:
        mtp_peer_set_access(a->index, (int64_t)peek_u64(r));
        break;
    case MTP_F_USER_FIRST_NAME:
        peek_str(r, a->first, sizeof(a->first));
        break;
    case MTP_F_USER_LAST_NAME:
        peek_str(r, a->last, sizeof(a->last));
        break;
    case MTP_F_USER_USERNAME:
        peek_str(r, a->username, sizeof(a->username));
        break;
    case MTP_F_USER_STATUS:
        a->online = peek_u32(r) == MTP_ID_USERSTATUSONLINE;
        break;
    default:
        break;
    }
}

bool mtp_parse_user(mtp_r_t *r)
{
    user_acc_t acc;
    memset(&acc, 0, sizeof(acc));
    acc.index = -1;

    if (mtp_skip_visit(r, MTP_T_USER, user_visit, &acc) != MTP_SKIP_OK) {
        return false;
    }
    if (acc.index < 0) {
        return true;   /* traversed fine, just nothing worth keeping */
    }

    /* Display name, in the order a Telegram client shows it: full name, else
       username, else something that at least identifies the account. */
    char name[MTP_NAME_MAX];
    if (acc.first[0] != '\0' && acc.last[0] != '\0') {
        /* "First Last", truncated at the field width. Assembled by hand because
           the two parts can each fill the buffer on their own. */
        size_t n = strlen(acc.first);
        if (n > sizeof(name) - 2u) {
            n = sizeof(name) - 2u;
        }
        memcpy(name, acc.first, n);
        name[n++] = ' ';
        size_t m = strlen(acc.last);
        if (m > sizeof(name) - 1u - n) {
            m = sizeof(name) - 1u - n;
        }
        memcpy(name + n, acc.last, m);
        name[n + m] = '\0';
    } else if (acc.first[0] != '\0') {
        snprintf(name, sizeof(name), "%s", acc.first);
    } else if (acc.last[0] != '\0') {
        snprintf(name, sizeof(name), "%s", acc.last);
    } else if (acc.username[0] != '\0') {
        name[0] = '@';
        size_t n = strlen(acc.username);
        if (n > sizeof(name) - 2u) {
            n = sizeof(name) - 2u;
        }
        memcpy(name + 1, acc.username, n);
        name[n + 1u] = '\0';
    } else {
        snprintf(name, sizeof(name), "Deleted account");
    }
    mtp_peer_set_name(acc.index, name);

    mtp_peer_t *p = (mtp_peer_t *)mtp_peer_at(acc.index);
    if (p != NULL) {
        p->online = acc.online;
        p->is_self = acc.is_self;
    }
    return true;
}

/* ---- Chat and channel ---------------------------------------------------- */

typedef struct {
    int  index;
    char title[MTP_NAME_MAX];
} chat_acc_t;

static void chat_visit(void *user, uint32_t ctor, unsigned field, uint8_t kind,
                       const mtp_r_t *r, uint32_t flags, uint32_t flags2)
{
    (void)kind; (void)flags; (void)flags2;
    chat_acc_t *a = user;

    switch (ctor) {
    case MTP_ID_CHAT:
    case MTP_ID_CHATFORBIDDEN:
        if (field == MTP_F_CHAT_ID) {
            a->index = mtp_peer_intern(MTP_PEER_CHAT, (int64_t)peek_u64(r));
        } else if (field == MTP_F_CHAT_TITLE) {
            peek_str(r, a->title, sizeof(a->title));
        }
        break;
    case MTP_ID_CHATEMPTY:
        if (field == MTP_F_CHATEMPTY_ID) {
            a->index = mtp_peer_intern(MTP_PEER_CHAT, (int64_t)peek_u64(r));
        }
        break;
    case MTP_ID_CHANNEL:
    case MTP_ID_CHANNELFORBIDDEN:
        if (field == MTP_F_CHANNEL_ID) {
            a->index = mtp_peer_intern(MTP_PEER_CHANNEL, (int64_t)peek_u64(r));
        } else if (field == MTP_F_CHANNEL_ACCESS_HASH) {
            mtp_peer_set_access(a->index, (int64_t)peek_u64(r));
        } else if (field == MTP_F_CHANNEL_TITLE) {
            peek_str(r, a->title, sizeof(a->title));
        }
        break;
    default:
        break;
    }
}

bool mtp_parse_chat(mtp_r_t *r)
{
    chat_acc_t acc;
    memset(&acc, 0, sizeof(acc));
    acc.index = -1;

    if (mtp_skip_visit(r, MTP_T_CHAT, chat_visit, &acc) != MTP_SKIP_OK) {
        return false;
    }
    if (acc.index >= 0 && acc.title[0] != '\0') {
        mtp_peer_set_name(acc.index, acc.title);
    }
    return true;
}

/* ---- Dialog -------------------------------------------------------------- */

static void dialog_visit(void *user, uint32_t ctor, unsigned field, uint8_t kind,
                         const mtp_r_t *r, uint32_t flags, uint32_t flags2)
{
    (void)kind; (void)flags; (void)flags2;
    mtp_dialog_t *d = user;
    if (ctor != MTP_ID_DIALOG) {
        return;
    }
    switch (field) {
    case MTP_F_DIALOG_PEER:               d->peer = peek_peer(r); break;
    case MTP_F_DIALOG_TOP_MESSAGE:        d->top_message = (int32_t)peek_u32(r); break;
    case MTP_F_DIALOG_READ_INBOX_MAX_ID:  d->read_inbox_max_id = (int32_t)peek_u32(r); break;
    case MTP_F_DIALOG_READ_OUTBOX_MAX_ID: d->read_outbox_max_id = (int32_t)peek_u32(r); break;
    case MTP_F_DIALOG_UNREAD_COUNT:       d->unread_count = (int32_t)peek_u32(r); break;
    case MTP_F_DIALOG_FOLDER_ID:          d->archived = peek_u32(r) != 0u; break;
    default: break;
    }
}

bool mtp_parse_dialog(mtp_r_t *r)
{
    mtp_dialog_t d;
    memset(&d, 0, sizeof(d));
    d.peer = -1;

    if (mtp_skip_visit(r, MTP_T_DIALOG, dialog_visit, &d) != MTP_SKIP_OK) {
        return false;
    }
    if (d.peer < 0) {
        return true;
    }
    /* Replace an existing row for the same peer rather than duplicating it —
       getDialogs is re-run on refresh and would otherwise grow the list. */
    int at = mtp_dialog_find_peer(d.peer);
    if (at >= 0) {
        /* Keep the preview, which comes from the messages vector parsed later. */
        char preview[MTP_PREVIEW_MAX];
        memcpy(preview, s_dialogs[at].preview, sizeof(preview));
        bool out = s_dialogs[at].preview_out;
        uint32_t date = s_dialogs[at].date;
        s_dialogs[at] = d;
        memcpy(s_dialogs[at].preview, preview, sizeof(preview));
        s_dialogs[at].preview_out = out;
        s_dialogs[at].date = date;
        return true;
    }
    if (s_dialog_count >= MTP_MAX_DIALOGS) {
        return true;   /* full: the visible list is already longer than the screen */
    }
    s_dialogs[s_dialog_count++] = d;
    return true;
}

/* ---- Message ------------------------------------------------------------- */

typedef struct {
    mtp_message_t *msg;
    int            peer_id;     /* the conversation the message belongs to */
    bool           saw_id;
} msg_acc_t;

static void message_visit(void *user, uint32_t ctor, unsigned field, uint8_t kind,
                          const mtp_r_t *r, uint32_t flags, uint32_t flags2)
{
    (void)kind; (void)flags2;
    msg_acc_t *a = user;

    if (ctor == MTP_ID_MESSAGE) {
        switch (field) {
        case MTP_F_MESSAGE_ID:
            a->msg->id = (int32_t)peek_u32(r);
            a->saw_id = true;
            /* flags.1 is `out` — set on messages we sent. */
            a->msg->out = (flags & (1u << 1)) != 0u;
            break;
        case MTP_F_MESSAGE_FROM_ID:
            a->msg->peer = peek_peer(r);
            break;
        case MTP_F_MESSAGE_PEER_ID:
            a->peer_id = peek_peer(r);
            break;
        case MTP_F_MESSAGE_DATE:
            a->msg->date = peek_u32(r);
            break;
        case MTP_F_MESSAGE_MESSAGE:
            peek_str(r, a->msg->text, sizeof(a->msg->text));
            break;
        case MTP_F_MESSAGE_MEDIA:
            a->msg->has_media = true;
            break;
        default:
            break;
        }
        return;
    }
    if (ctor == MTP_ID_MESSAGESERVICE) {
        a->msg->service = true;
        switch (field) {
        case MTP_F_MESSAGESERVICE_ID:
            a->msg->id = (int32_t)peek_u32(r);
            a->saw_id = true;
            a->msg->out = (flags & (1u << 1)) != 0u;
            break;
        case MTP_F_MESSAGESERVICE_FROM_ID:
            a->msg->peer = peek_peer(r);
            break;
        case MTP_F_MESSAGESERVICE_PEER_ID:
            a->peer_id = peek_peer(r);
            break;
        case MTP_F_MESSAGESERVICE_DATE:
            a->msg->date = peek_u32(r);
            break;
        default:
            break;
        }
    }
}

bool mtp_parse_message(mtp_r_t *r, mtp_message_t *out)
{
    memset(out, 0, sizeof(*out));
    out->peer = -1;

    msg_acc_t acc = { out, -1, false };
    mtp_skip_result_t res = mtp_skip_visit(r, MTP_T_MESSAGE, message_visit, &acc);

    if (out->service && out->text[0] == '\0') {
        /* The action itself is opaque to us; say something rather than showing an
           empty bubble. */
        snprintf(out->text, sizeof(out->text), "(service message)");
    } else if (out->has_media && out->text[0] == '\0') {
        snprintf(out->text, sizeof(out->text), "[media]");
    }
    return res == MTP_SKIP_OK;
}

int mtp_parse_peer(mtp_r_t *r)
{
    uint32_t id = mtp_r_u32(r);
    int64_t value = (int64_t)mtp_r_u64(r);
    if (!mtp_r_ok(r)) {
        return -1;
    }
    switch (id) {
    case MTP_ID_PEERUSER:    return mtp_peer_intern(MTP_PEER_USER, value);
    case MTP_ID_PEERCHAT:    return mtp_peer_intern(MTP_PEER_CHAT, value);
    case MTP_ID_PEERCHANNEL: return mtp_peer_intern(MTP_PEER_CHANNEL, value);
    default:                 return -1;
    }
}

/* ---- Vectors ------------------------------------------------------------- */

void mtp_parse_user_vector(mtp_r_t *r)
{
    uint32_t n = mtp_r_vector(r);
    for (uint32_t i = 0u; i < n && mtp_r_ok(r); i++) {
        if (!mtp_parse_user(r)) {
            /* One unreadable entry means the rest of the buffer is unreachable;
               names already collected stay valid. */
            return;
        }
    }
}

void mtp_parse_chat_vector(mtp_r_t *r)
{
    uint32_t n = mtp_r_vector(r);
    for (uint32_t i = 0u; i < n && mtp_r_ok(r); i++) {
        if (!mtp_parse_chat(r)) {
            return;
        }
    }
}

#pragma GCC visibility pop
