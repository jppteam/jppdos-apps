/*
 * mtp_scratch — one working buffer shared by the phases that cannot overlap.
 *
 * The handshake needs ~3.9 KB of nonces, 2048-bit operands and RSA_PAD working
 * space; the two-factor exchange needs ~6.3 KB of bignums; a gzipped response
 * needs up to MTP_INFLATE_MAX bytes to expand into; and reading custom.conf
 * needs the file in memory. All four are large, all four are dead the moment
 * their phase ends, and no two are ever live together — giving each its own
 * static allocation would cost around 14 KB of the app pool to hold things that
 * are never simultaneously alive.
 *
 * There are two ways to use it, and the difference matters:
 *
 *   acquire/release  An exclusive borrow, for a phase that runs to completion
 *                    inside one call: the handshake, the SRP proof, the config
 *                    parse. A second acquire while one is outstanding fails
 *                    rather than silently handing out the same bytes twice.
 *
 *   base()           The unguarded default view, used by mtp_rpc for inflated
 *                    response bodies. An RPC result lives here until the next
 *                    call or the next borrow, whichever comes first.
 *
 * The rule that keeps those two safe together: **an RPC result is dead as soon
 * as an exclusive borrow begins.** Every caller today obeys it by construction —
 * the handshake runs before any RPC exists on the connection, and the one place
 * that borrows between two RPCs (mtp_login_check_password) copies what it needs
 * out of the result before calling mtp_srp_compute. Code that borrows the arena
 * must not hold on to a pointer into an earlier response across the borrow.
 *
 * The reverse direction is the one a bug could plausibly get wrong — inflating
 * into an arena that a handshake is currently using — so mtp_scratch_is_lent()
 * exists for mtp_rpc to check, and it refuses the inflate rather than scribbling
 * over a borrower's bignums.
 */
#pragma once

#include "mtp_common.h"

/*
 * Sized to the largest claimant (the SRP bignums). Raising this is the correct
 * fix if a future user needs more; overlapping a *fifth*, concurrently-live
 * user into it is not.
 */
#define MTP_SCRATCH_BYTES 6656u

/*
 * Claim the arena from the heap block, and drop the pointer again. Called only
 * by mtp_mem_init/_release; nothing else should touch these.
 */
mtp_err_t mtp_scratch_mem_init(void);
void      mtp_scratch_mem_clear(void);

/*
 * Borrow the arena exclusively. Returns NULL if it is already lent out or
 * `bytes` exceeds it — both of which are programming errors, and both of which
 * the caller reports as MTP_ERR_CRYPTO rather than proceeding on a NULL.
 */
void *mtp_scratch_acquire(size_t bytes);

/* Return it, zeroing the contents: what lives here is key material. */
void mtp_scratch_release(void *ptr);

/* The arena's base address, for the unguarded default tenant. Always valid;
   check mtp_scratch_is_lent() first if another phase might be running. */
void *mtp_scratch_base(void);

/* True while an exclusive borrow is outstanding. */
bool mtp_scratch_is_lent(void);
