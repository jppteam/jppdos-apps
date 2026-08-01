#include "mtp_skip.h"

/* Kind codes. Mirrors the K table in test/gen_skip.py — change both together. */
enum {
    K_FLAGS0 = 0, K_FLAGS1, K_INT, K_LONG, K_INT128, K_INT256, K_DOUBLE,
    K_STRING, K_BOOL, K_TRUE, K_NESTED, K_VEC, K_VEC_INT, K_VEC_LONG,
    K_VEC_STRING, K_OPAQUE,
};

#define COND_UNCOND 0xFFu

/*
 * Nesting bound. The real schema nests about six deep at worst (a message's
 * reply header's forward header's peer), so 12 leaves plenty of room while making
 * it impossible for a crafted response to recurse until the 12 KB task stack runs
 * out.
 */
#define MAX_DEPTH 12

const mtp_skip_ctor_t *mtp_skip_find(unsigned type_index, uint32_t ctor_id)
{
    if (type_index >= mtp_skip_type_count) {
        return NULL;
    }
    const mtp_skip_type_t *t = &mtp_skip_types[type_index];
    /* Constructors are emitted sorted by id, so this is a binary search. */
    unsigned lo = 0u, hi = t->n_ctors;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        const mtp_skip_ctor_t *c = &mtp_skip_ctors[t->first_ctor + mid];
        if (c->id == ctor_id) {
            return c;
        }
        if (c->id < ctor_id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

typedef struct {
    mtp_skip_visit_fn fn;
    void             *user;
    /* Only the outermost object's fields are reported. A nested Peer's `user_id`
       is at index 0 too, and reporting it would collide with the parent's field
       numbering. */
    int               depth_limit;
} visit_ctx_t;

static mtp_skip_result_t skip_value(mtp_r_t *r, unsigned type_index, int depth,
                                    visit_ctx_t *vis);

/*
 * Run a constructor's field list from `start` onwards. Split out so that both
 * mtp_skip (which reads the constructor id itself) and mtp_skip_rest (whose
 * caller already consumed some fields) share one implementation.
 */
static mtp_skip_result_t run_fields(mtp_r_t *r, const mtp_skip_ctor_t *c,
                                    unsigned start, uint32_t flags, uint32_t flags2,
                                    int depth, visit_ctx_t *vis)
{
    for (unsigned i = start; i < c->n_fields; i++) {
        const mtp_skip_field_t *f = &mtp_skip_fields[c->first_field + i];

        if (f->cond != COND_UNCOND) {
            uint32_t word = (f->cond < 32u) ? flags : flags2;
            unsigned bit = f->cond & 31u;
            if ((word & (1u << bit)) == 0u) {
                continue;   /* field absent */
            }
        }

        if (vis != NULL && vis->fn != NULL && depth == vis->depth_limit) {
            vis->fn(vis->user, c->id, i, f->kind, r, flags, flags2);
        }

        switch (f->kind) {
        case K_FLAGS0:
            flags = mtp_r_u32(r);
            break;
        case K_FLAGS1:
            flags2 = mtp_r_u32(r);
            break;
        case K_INT:
            (void)mtp_r_u32(r);
            break;
        case K_LONG:
        case K_DOUBLE:
            (void)mtp_r_u64(r);
            break;
        case K_INT128:
            mtp_r_skip(r, 16u);
            break;
        case K_INT256:
            mtp_r_skip(r, 32u);
            break;
        case K_STRING:
            mtp_r_skip_bytes(r);
            break;
        case K_BOOL:
            (void)mtp_r_u32(r);   /* a Bool is a bare constructor id */
            break;
        case K_TRUE:
            /* Flag-only: presence is the value, nothing on the wire. */
            break;
        case K_NESTED: {
            mtp_skip_result_t res = skip_value(r, f->arg, depth + 1, vis);
            if (res != MTP_SKIP_OK) {
                return res;
            }
            break;
        }
        case K_VEC:
        case K_VEC_INT:
        case K_VEC_LONG:
        case K_VEC_STRING: {
            uint32_t n = mtp_r_vector(r);
            if (!mtp_r_ok(r)) {
                return MTP_SKIP_ERROR;
            }
            for (uint32_t k = 0u; k < n; k++) {
                if (f->kind == K_VEC_INT) {
                    (void)mtp_r_u32(r);
                } else if (f->kind == K_VEC_LONG) {
                    (void)mtp_r_u64(r);
                } else if (f->kind == K_VEC_STRING) {
                    mtp_r_skip_bytes(r);
                } else {
                    mtp_skip_result_t res = skip_value(r, f->arg, depth + 1, vis);
                    if (res != MTP_SKIP_OK) {
                        return res;
                    }
                }
                if (!mtp_r_ok(r)) {
                    return MTP_SKIP_ERROR;
                }
            }
            break;
        }
        case K_OPAQUE:
        default:
            /* A media or action field. The reader is now stranded mid-object;
               saying so is the only honest answer. */
            return MTP_SKIP_OPAQUE;
        }

        if (!mtp_r_ok(r)) {
            return MTP_SKIP_ERROR;
        }
    }
    return MTP_SKIP_OK;
}

static mtp_skip_result_t skip_value(mtp_r_t *r, unsigned type_index, int depth,
                                    visit_ctx_t *vis)
{
    if (depth > MAX_DEPTH) {
        return MTP_SKIP_ERROR;
    }
    uint32_t id = mtp_r_u32(r);
    if (!mtp_r_ok(r)) {
        return MTP_SKIP_ERROR;
    }
    const mtp_skip_ctor_t *c = mtp_skip_find(type_index, id);
    if (c == NULL) {
        /*
         * A constructor the table does not know — almost always because the
         * server is running a newer layer than this build was generated against.
         * Treated as opaque rather than fatal: the caller falls back to resuming
         * by id, which degrades gracefully instead of breaking outright.
         */
        return MTP_SKIP_OPAQUE;
    }
    return run_fields(r, c, 0u, 0u, 0u, depth, vis);
}

mtp_skip_result_t mtp_skip(mtp_r_t *r, unsigned type_index)
{
    return skip_value(r, type_index, 0, NULL);
}

mtp_skip_result_t mtp_skip_visit(mtp_r_t *r, unsigned type_index,
                                 mtp_skip_visit_fn fn, void *user)
{
    visit_ctx_t vis = { fn, user, 0 };
    return skip_value(r, type_index, 0, &vis);
}

mtp_skip_result_t mtp_skip_rest(mtp_r_t *r, unsigned type_index, uint32_t ctor_id,
                                unsigned consumed, uint32_t flags, uint32_t flags2)
{
    const mtp_skip_ctor_t *c = mtp_skip_find(type_index, ctor_id);
    if (c == NULL) {
        return MTP_SKIP_OPAQUE;
    }
    if (consumed > c->n_fields) {
        return MTP_SKIP_ERROR;
    }
    return run_fields(r, c, consumed, flags, flags2, 0, NULL);
}
