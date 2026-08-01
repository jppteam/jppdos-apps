/*
 * mtp_client — connection lifecycle above the protocol layers.
 *
 * Owns the sequence that turns a chosen server profile into a usable session:
 * connect, reuse or negotiate an auth_key, follow a DC migration if the account
 * lives elsewhere, and identify the client to the server. Everything above this
 * (the login flow, the dialog list) issues requests through mtp_client_invoke
 * and never touches the transport directly.
 */
#pragma once

#include "mtp_auth.h"
#include "mtp_config.h"
#include "mtp_rpc.h"
#include "mtp_store.h"

typedef enum {
    MTP_CONN_OFFLINE = 0,
    MTP_CONN_CONNECTING,
    MTP_CONN_READY,        /* auth_key established; may or may not be signed in */
} mtp_conn_state_t;

/* Progress reporting during connect, which can take several seconds. */
typedef void (*mtp_client_progress_fn)(void *user, const char *step, int percent);

void mtp_client_init(jpp_sdk_context_t *ctx);

/*
 * Bring up a session for `mode`.
 *
 * Reuses a stored auth_key when there is one, and runs the handshake otherwise.
 * Follows *_MIGRATE_n by reconnecting to the named DC and re-handshaking there —
 * the key is per-DC, so migration always costs a new one.
 */
mtp_err_t mtp_client_connect(mtp_mode_t mode,
                            mtp_client_progress_fn progress, void *user);

void mtp_client_disconnect(void);

mtp_conn_state_t mtp_client_state(void);
mtp_mode_t       mtp_client_mode(void);
bool             mtp_client_is_logged_in(void);

/* The live session record, for the screens that display or update it. */
const mtp_session_data_t *mtp_client_session(void);

/* Persist the current session; call after anything that changes user_id or the
   signed-in flag. */
mtp_err_t mtp_client_save(void);

/* Discard the stored key and sign out locally. */
void mtp_client_forget(void);

/*
 * Scratch space for serialising a request body. Callers build into this rather
 * than declaring their own buffer, so the 12 KB task stack is not carrying a
 * request-sized array at every call depth.
 */
uint8_t *mtp_client_req_buf(size_t *cap);

/*
 * Issue one API request.
 *
 * Wraps the request in invokeWithLayer + initConnection on the first call of a
 * session, which Telegram requires before it will answer anything else. Retries
 * once through a full reconnect if the connection died underneath, so callers do
 * not each need their own recovery path.
 */
mtp_err_t mtp_client_invoke(const uint8_t *body, size_t len,
                            const uint8_t **out_result, size_t *out_len);

/*
 * Service the connection between user actions: dispatch updates, keep the
 * connection alive, and reconnect if it dropped. Call from the main loop; it
 * polls rather than blocks.
 */
void mtp_client_pump(void);

/* Marks the connection as needing initConnection again — used after a migration
   or an explicit reconnect. */
void mtp_client_reset_layer(void);
