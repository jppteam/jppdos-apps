/*
 * mtp_pq — factor the server's proof-of-work challenge.
 *
 * resPQ carries `pq`, a product of two primes that the client must split before
 * it can answer req_DH_params. The value is under 2^63 and each factor is around
 * 2^31, so trial division is hopeless and Brent's variant of Pollard's rho is
 * the right tool: it finds a factor in roughly n^(1/4) steps, a few tens of
 * thousands of iterations here, which is milliseconds even at 160 MHz.
 *
 * The 64-bit arithmetic is done by hand rather than through __int128, which GCC
 * does not offer on a 32-bit target. __udivdi3 / __umoddi3 / __muldi3 are in the
 * firmware symbol table, so plain uint64_t operators link.
 */
#pragma once

#include "mtp_common.h"

/*
 * Split `n` into two factors with *out_p < *out_q. Returns false if no factor
 * was found or either factor exceeds 32 bits — both of which mean the value is
 * not the shape the protocol promises, so answering would be guesswork.
 */
bool mtp_pq_factor(uint64_t n, uint32_t *out_p, uint32_t *out_q);

/* (a * b) mod m for m < 2^63, by shift-and-add. Exposed for the host tests. */
uint64_t mtp_pq_mulmod(uint64_t a, uint64_t b, uint64_t m);
