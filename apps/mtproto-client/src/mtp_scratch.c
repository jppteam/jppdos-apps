#include "mtp_scratch.h"

#include <string.h>

#include "mtp_mem.h"

/* Lives on the heap, not in the app pool — see mtp_mem.h. mtp_mem_take returns
   8-byte-aligned memory, so callers can still place structs containing uint64_t
   here. */
static uint8_t *s_arena;
static bool     s_lent;

mtp_err_t mtp_scratch_mem_init(void)
{
    s_arena = mtp_mem_take(MTP_SCRATCH_BYTES);
    return s_arena != NULL ? MTP_OK : MTP_ERR_OVERFLOW;
}

void mtp_scratch_mem_clear(void)
{
    s_arena = NULL;
    s_lent = false;
}

void *mtp_scratch_acquire(size_t bytes)
{
    if (s_arena == NULL || s_lent || bytes > MTP_SCRATCH_BYTES) {
        /*
         * Either two phases are live at once — which the design says cannot
         * happen, so it is a bug worth failing loudly on — or someone grew a
         * structure past MTP_SCRATCH_BYTES without raising it.
         */
        return NULL;
    }
    s_lent = true;
    memset(s_arena, 0, bytes);
    return s_arena;
}

void mtp_scratch_release(void *ptr)
{
    if (ptr == NULL || ptr != (void *)s_arena) {
        return;
    }
    /* Wipe on the way out: the handshake leaves the auth_key here and SRP leaves
       password-derived material. */
    memset(s_arena, 0, MTP_SCRATCH_BYTES);
    s_lent = false;
}

void *mtp_scratch_base(void) { return s_arena; }
bool  mtp_scratch_is_lent(void) { return s_lent; }
