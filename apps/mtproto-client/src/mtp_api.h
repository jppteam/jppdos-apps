/*
 * mtp_api — the Telegram methods this client actually calls.
 *
 * Each function builds a request, invokes it through mtp_client, and folds the
 * response into mtp_model. Callers get a status, not bytes.
 *
 * Page sizes are small on purpose. A phone asks for 100 dialogs at a time; here
 * the uncompressed response has to fit MTP_INFLATE_MAX and the compressed frame
 * has to fit MTP_RX_MAX, and the screen shows four rows. Asking for what fits is
 * cheaper than discovering it does not. These two are the knobs that bound the
 * reply size: a larger page request just comes back bigger, and the transport
 * treats anything that overflows the RX buffer as a fatal "Reply too large".
 */
#pragma once

#include "mtp_client.h"
#include "mtp_model.h"

/* One screen and a bit, in each case. Page sizes are tight on purpose — each
   dialog drags its full user/chat record (measured 1.2 KB+ on a busy account,
   so the reply is bounded at the request), requiring the reply to fit both the
   RX frame buffer (~4 KB) and the inflate arena. See the note above. */
#define MTP_DIALOG_PAGE  3
#define MTP_HISTORY_PAGE 4

/*
 * Refresh the dialog list from scratch. Parses the users and chats vectors first
 * so every row has a name by the time it is drawn.
 */
mtp_err_t mtp_api_get_dialogs(void);

/*
 * Load history for a peer. `offset_id` of 0 starts at the newest message;
 * otherwise it fetches messages older than that id, which is how scroll-back
 * pages.
 *
 * Returns MTP_OK even when the batch stopped early on an untraversable message —
 * *out_stopped_at then holds the id to resume from, and 0 means the page was
 * fully read. See mtp_skip for why that happens.
 */
mtp_err_t mtp_api_get_history(int peer_index, int32_t offset_id,
                              int32_t *out_stopped_at);

/* Send a text message. On success the message is inserted locally right away, so
   the bubble appears before the server's echo arrives. */
mtp_err_t mtp_api_send_message(int peer_index, const char *text);

/* Mark everything up to `max_id` as read, clearing the unread badge. */
mtp_err_t mtp_api_read_history(int peer_index, int32_t max_id);

/* Tell the peer we are typing, or that we stopped. */
mtp_err_t mtp_api_set_typing(int peer_index, bool typing);

/* Fetch the signed-in account, which is what confirms authorization survived a
   reconnect and gives us the "self" peer for Saved Messages. */
mtp_err_t mtp_api_get_self(void);

/*
 * Write an InputPeer for a model peer into a TL writer. Exposed because the login
 * and update paths need it too.
 */
bool mtp_api_write_input_peer(mtp_w_t *w, int peer_index);
