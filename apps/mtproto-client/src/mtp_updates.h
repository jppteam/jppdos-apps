/*
 * mtp_updates — live updates from the open connection.
 *
 * Telegram pushes updates on the same socket rather than being polled, so this
 * is what makes a message appear without the user asking. The full Updates type
 * has over 150 variants; only the handful that change something visible on a
 * 128x64 screen are handled, and the rest are ignored by constructor id.
 *
 * Deliberately not implemented: updates.getDifference. Recovering missed state
 * needs a pts/qts/date sequence tracked across reconnects, and the payoff on a
 * device that reloads the dialog list on every launch is small. Instead a
 * reconnect re-fetches, which reaches the same place with less state to get
 * wrong.
 */
#pragma once

#include "mtp_common.h"

/* Register the handler with mtp_rpc. Call once after connecting. */
void mtp_updates_install(void);

/*
 * Set when an update changed something the UI should react to — a new message in
 * the open chat, or a dialog list that needs redrawing. Reading clears it.
 */
bool mtp_updates_take_dirty(void);

/* Peer of the most recent incoming message, or -1. Used to decide whether to
   notify and whether the open chat should scroll. */
int mtp_updates_last_message_peer(void);
