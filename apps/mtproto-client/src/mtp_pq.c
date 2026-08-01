#include "mtp_pq.h"

uint64_t mtp_pq_mulmod(uint64_t a, uint64_t b, uint64_t m)
{
    /*
     * Shift-and-add rather than a 128-bit intermediate, which RV32 has no type
     * for. Correct for any m < 2^63: both `r + a` and `a << 1` stay under 2^64
     * because r and a are always reduced below m first.
     */
    uint64_t r = 0u;
    a %= m;
    while (b != 0u) {
        if ((b & 1u) != 0u) {
            r = (r + a) % m;
        }
        a = (a << 1) % m;
        b >>= 1;
    }
    return r;
}

static uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b != 0u) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint64_t absdiff(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

/* One Brent/rho attempt with the given increment. Returns a non-trivial factor
   of n, or 0 if this increment did not produce one. */
static uint64_t brent(uint64_t n, uint64_t c)
{
    uint64_t x, y = 2u, ys = 0u, q = 1u, g = 1u;
    uint64_t r = 1u;
    const uint64_t m = 128u;   /* batch size: how many steps between gcd calls */

    do {
        x = y;
        for (uint64_t i = 0u; i < r; i++) {
            y = (mtp_pq_mulmod(y, y, n) + c) % n;
        }
        uint64_t k = 0u;
        while (k < r && g == 1u) {
            ys = y;
            uint64_t batch = (m < r - k) ? m : r - k;
            for (uint64_t i = 0u; i < batch; i++) {
                y = (mtp_pq_mulmod(y, y, n) + c) % n;
                /*
                 * Accumulate the differences and take one gcd per batch instead
                 * of one per step. The gcd is the expensive part, and a product
                 * of differences shares a factor with n exactly when one of them
                 * does.
                 */
                q = mtp_pq_mulmod(q, absdiff(x, y), n);
            }
            g = gcd64(q, n);
            k += batch;
        }
        r *= 2u;
        /* Bound the search so a pathological input cannot spin forever; the
           caller retries with a different increment. */
    } while (g == 1u && r < (1u << 20));

    if (g == n) {
        /*
         * The batched product hit a multiple of n, meaning the batch contained
         * the factor but also overshot. Re-walk that batch one step at a time to
         * recover it.
         */
        g = 1u;
        do {
            ys = (mtp_pq_mulmod(ys, ys, n) + c) % n;
            g = gcd64(absdiff(x, ys), n);
        } while (g == 1u);
    }
    return (g == n) ? 0u : g;
}

bool mtp_pq_factor(uint64_t n, uint32_t *out_p, uint32_t *out_q)
{
    *out_p = 0u;
    *out_q = 0u;
    if (n < 4u) {
        return false;
    }

    uint64_t factor = 0u;
    if ((n & 1u) == 0u) {
        factor = 2u;
    } else {
        /* Different increments explore different cycles; rho can fail for a
           given c, so try several before giving up. */
        for (uint64_t c = 1u; c < 32u && factor == 0u; c++) {
            factor = brent(n, c);
        }
    }
    if (factor == 0u || factor == 1u) {
        return false;
    }

    uint64_t other = n / factor;
    if (factor * other != n) {
        return false;
    }
    uint64_t lo = factor < other ? factor : other;
    uint64_t hi = factor < other ? other : factor;

    /* The protocol sends p and q as 4-byte integers; anything wider means this
       is not the two-prime product resPQ is specified to carry. */
    if (hi > 0xFFFFFFFFu) {
        return false;
    }
    *out_p = (uint32_t)lo;
    *out_q = (uint32_t)hi;
    return true;
}
