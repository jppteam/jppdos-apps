/*
 * mtp_mem — the block of buffers too large to duplicate per-module, carved
 * out of the app pool itself rather than the heap.
 *
 * This used to be a heap allocation, taken at startup, specifically to avoid
 * needing more than the 64 KB app pool that v1.1 firmware provided — raising
 * the pool was firmware policy the app had no way to declare a dependency on
 * (`sdk_min` gates SDK level, not pool size). JPPDOS now guarantees an 80 KB
 * pool as an actual SDK-gated property, so that workaround is gone: the block
 * is static and lives in `.bss` like everything else in the image.
 *
 * `mtp_mem_take` is still a bump pointer over one fixed-size region — there is
 * no fragmentation and no free list, and a caller asking for more than
 * MTP_MEM_BYTES accounts for is a build-time mistake, not a runtime one.
 */
#pragma once

#include "mtp_common.h"

/*
 * Zero the block and hand it to the modules that need it. Call once, before
 * anything else in the client.
 */
mtp_err_t mtp_mem_init(void);

/* Wipe it. Safe to call when init was never called. */
void mtp_mem_release(void);

/*
 * Carve `bytes` off the block, zeroed and 8-byte aligned. Returns NULL when the
 * block is exhausted, which means MTP_MEM_BYTES below is out of step with what
 * the modules ask for — a build-time mistake, caught at startup.
 *
 * There is no matching free: the whole block goes back at once.
 */
void *mtp_mem_take(size_t bytes);

/* Total block size, for the startup log. */
size_t mtp_mem_bytes(void);
