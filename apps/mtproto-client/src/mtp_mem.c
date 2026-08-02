#include "mtp_mem.h"

#include <string.h>

/*
 * Headers only, for the sizes. This file deliberately calls nothing in those
 * modules — it is a leaf, so a host test can link it without dragging in the
 * transport and the SDK. Who takes what is decided by the caller of
 * mtp_mem_take; see mem_setup() in mtp_app.c.
 */
#include "mtp_model.h"
#include "mtp_scratch.h"
#include "mtp_session.h"
#include "ui_gfx.h"

#pragma GCC visibility push(hidden)

/*
 * The block's size, itemised. Every term is a compile-time constant from the
 * header that owns the buffer, so this cannot drift out of step with the
 * buffers themselves — and if a caller asks for more than is listed here,
 * mtp_mem_take returns NULL at startup rather than corrupting anything.
 *
 * Per-take alignment padding is covered by rounding each term up to 8.
 */
#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

#define MTP_MEM_BYTES ( \
    ALIGN8(MTP_SCRATCH_BYTES)                                    /* shared arena  */ \
  + ALIGN8(MTP_SESS_TX_BYTES) + ALIGN8(MTP_SESS_RX_BYTES)        /* transfer bufs */ \
  + ALIGN8(MTP_MAX_PEERS    * sizeof(mtp_peer_t))                                    \
  + ALIGN8(MTP_MAX_DIALOGS  * sizeof(mtp_dialog_t))                                  \
  + ALIGN8(MTP_MAX_MESSAGES * sizeof(mtp_message_t))             /* model caches  */ \
  + ALIGN8(UI_FB_BYTES))                                         /* framebuffer   */

static uint8_t s_block[MTP_MEM_BYTES] __attribute__((aligned(8)));
static size_t  s_used;

mtp_err_t mtp_mem_init(void)
{
    memset(s_block, 0, sizeof(s_block));
    s_used = 0u;
    return MTP_OK;
}

void mtp_mem_release(void)
{
    /* Wipe on the way out: the arena holds the auth_key and, briefly,
       password-derived material. */
    memset(s_block, 0, sizeof(s_block));
    s_used = 0u;
}

void *mtp_mem_take(size_t bytes)
{
    size_t need = ALIGN8(bytes);
    if (need > MTP_MEM_BYTES - s_used) {
        return NULL;
    }
    void *p = s_block + s_used;
    s_used += need;
    memset(p, 0, need);
    return p;
}

size_t mtp_mem_bytes(void) { return MTP_MEM_BYTES; }

#pragma GCC visibility pop
