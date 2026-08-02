/*
 * mtp_model — the client's in-memory picture of chats and messages.
 *
 * Fixed-size tables, no heap. The caps are chosen against the screen rather than
 * against Telegram: four dialog rows are visible at a time and a chat shows three
 * or four messages, so holding a couple of screens' worth in each direction is
 * enough for scrolling to feel instant, and anything beyond that is paid for in
 * pool space that the protocol buffers need more.
 *
 * Peers are interned: a dialog and every message in it refer to a peer by index,
 * so a chat title is stored once rather than per message.
 */
#pragma once

#include "mtp_common.h"
#include "mtp_skip.h"
#include "mtp_tl.h"

#define MTP_MAX_PEERS    16
#define MTP_MAX_DIALOGS  8
#define MTP_MAX_MESSAGES 10

/* Display name length. 21 characters fill the screen at the 5x7 font, so 40
   bytes covers that even when every character is two-byte Cyrillic. */
#define MTP_NAME_MAX    40
#define MTP_PREVIEW_MAX 56
#define MTP_TEXT_MAX    112

typedef enum {
    MTP_PEER_NONE = 0,
    MTP_PEER_USER,
    MTP_PEER_CHAT,      /* basic group */
    MTP_PEER_CHANNEL,   /* channel or supergroup */
} mtp_peer_kind_t;

/*
 * Field order in these three is by alignment, widest first, and the small
 * members are sized to what they hold rather than to `int`. That is not
 * micro-optimisation at this scale: the three tables together are the second
 * largest claim on the pool after the transfer buffers, so an eight-byte hole
 * per peer is a couple of hundred bytes of a 64 KB budget.
 *
 * A peer index is int16_t because -1 has to be representable; it is not a
 * char, so widening the table does not silently truncate.
 */
typedef struct {
    int64_t id;
    int64_t access_hash;
    char    name[MTP_NAME_MAX];
    uint8_t kind;               /* mtp_peer_kind_t */
    bool    is_self;
    bool    online;
    /* A peer we learned about from a message rather than a full user/chat
       record; its name may be a placeholder until we see the real thing. */
    bool    partial;
} mtp_peer_t;

typedef struct {
    int32_t  top_message;
    int32_t  unread_count;
    int32_t  read_inbox_max_id;
    int32_t  read_outbox_max_id;
    uint32_t date;
    char     preview[MTP_PREVIEW_MAX];
    int16_t  peer;              /* index into the peer table, -1 if unresolved */
    bool     preview_out;       /* the last message is ours */
    bool     archived;          /* in an archive/custom folder (folder_id != 0) */
} mtp_dialog_t;

typedef struct {
    int32_t  id;
    uint32_t date;
    char     text[MTP_TEXT_MAX];
    int16_t  peer;              /* sender, index into the peer table */
    bool     out;
    bool     has_media;         /* rendered as a placeholder; see mtp_skip */
    bool     service;           /* joined/left/etc. — no body text */
} mtp_message_t;

/* ---- Peer table ---------------------------------------------------------- */

void mtp_model_reset(void);

/* Claim and drop the three tables. Called only by mtp_mem_init/_release. */
mtp_err_t mtp_model_mem_init(void);
void      mtp_model_mem_clear(void);

/* Find or create a peer. Returns -1 when the table is full. */
int  mtp_peer_intern(mtp_peer_kind_t kind, int64_t id);
int  mtp_peer_find(mtp_peer_kind_t kind, int64_t id);
const mtp_peer_t *mtp_peer_at(int index);
void mtp_peer_set_name(int index, const char *name);
void mtp_peer_set_access(int index, int64_t access_hash);

/* ---- Dialogs ------------------------------------------------------------- */

void mtp_dialogs_clear(void);
int  mtp_dialogs_count(void);
const mtp_dialog_t *mtp_dialog_at(int index);
int  mtp_dialog_find_peer(int peer_index);

/* True when the peer has a dialog and that dialog is in an archived/custom
   folder (folder_id != 0) — a chat whose updates this client ignores. */
bool mtp_dialog_is_archived_peer(int peer_index);

/* ---- Messages ------------------------------------------------------------ */

/* The message list is per-open-chat; switching chats clears it. */
void mtp_messages_clear(int peer_index);
int  mtp_messages_count(void);
int  mtp_messages_peer(void);
const mtp_message_t *mtp_message_at(int index);

/*
 * Insert keeping the list ordered oldest-first and de-duplicated by id, which is
 * what lets history pages and live updates land in the same table without the
 * caller tracking which is which.
 */
int  mtp_message_insert(const mtp_message_t *msg);

/* Lowest id held, for paging further back. 0 when empty. */
int32_t mtp_messages_oldest_id(void);

/* ---- Parsing ------------------------------------------------------------- */

/*
 * Each of these consumes exactly one object from `r` and leaves the reader
 * positioned after it, so vectors can be walked. They return false when the
 * object could not be traversed — see mtp_skip for why that happens and why the
 * caller should stop the batch rather than continue.
 */
bool mtp_parse_user(mtp_r_t *r);
bool mtp_parse_chat(mtp_r_t *r);
bool mtp_parse_dialog(mtp_r_t *r);

/*
 * Parse one Message. Fills `out` with whatever was readable; returns false if the
 * reader could not be advanced past the object, in which case `out->id` is still
 * valid if it was reached — which is what lets the caller resume paging from it.
 */
bool mtp_parse_message(mtp_r_t *r, mtp_message_t *out);

/* Read a Peer and intern it. Returns the peer index, or -1. */
int mtp_parse_peer(mtp_r_t *r);

/*
 * Walk the users and chats vectors that accompany a dialogs or messages
 * response, so that names are known before the dialogs referencing them are
 * displayed. Stops cleanly at the first object it cannot traverse.
 */
void mtp_parse_user_vector(mtp_r_t *r);
void mtp_parse_chat_vector(mtp_r_t *r);
