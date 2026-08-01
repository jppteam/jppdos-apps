/*
 * mtp_skip — table-driven traversal of TL objects.
 *
 * Reading element N+1 of a TL vector requires knowing exactly where element N
 * ended, which means understanding every field of it including the optional ones
 * the client does not care about. Rather than hand-write that for `user`, `chat`
 * and `message` — and re-write it at every layer bump — the field layouts are
 * generated from Telegram's own schema into a compact table (mtp_skip_data.c) and
 * interpreted here.
 *
 * Two types are deliberately left out: MessageMedia and MessageAction, whose
 * closures pull in photos, documents, web pages, polls and their attribute
 * vectors — several times more table than the whole rest put together. Reaching
 * one is reported as MTP_SKIP_OPAQUE rather than guessed at, and the caller stops
 * the current batch and resumes from the last id it did read. A chat full of
 * photos therefore costs extra round trips, not missing messages.
 */
#pragma once

#include "mtp_common.h"
#include "mtp_skip_types.h"
#include "mtp_tl.h"

/* Generated tables. */
typedef struct {
    uint8_t kind;   /* see the kind codes in mtp_skip.c */
    uint8_t cond;   /* 0xFF unconditional, else flags_word*32 + bit */
    uint8_t arg;    /* type index for NESTED and VEC */
} mtp_skip_field_t;

typedef struct {
    uint32_t id;
    uint16_t first_field;
    uint8_t  n_fields;
} mtp_skip_ctor_t;

typedef struct {
    uint16_t first_ctor;
    uint8_t  n_ctors;
} mtp_skip_type_t;

extern const mtp_skip_field_t mtp_skip_fields[];
extern const mtp_skip_ctor_t  mtp_skip_ctors[];
extern const mtp_skip_type_t  mtp_skip_types[];
extern const size_t           mtp_skip_type_count;

typedef enum {
    MTP_SKIP_OK = 0,
    MTP_SKIP_OPAQUE,   /* hit a media/action field; position is now unusable */
    MTP_SKIP_ERROR,    /* unknown constructor or the reader ran out */
} mtp_skip_result_t;

/*
 * Advance `r` past one value of type `type_index` (an MTP_T_* constant).
 *
 * On MTP_SKIP_OPAQUE or MTP_SKIP_ERROR the reader is left mid-object, so the only
 * safe move is to abandon the rest of the buffer. That is why callers batch by id
 * and resume rather than trying to continue.
 */
mtp_skip_result_t mtp_skip(mtp_r_t *r, unsigned type_index);

/*
 * Skip the remaining fields of a constructor the caller has already started
 * reading. `consumed` is how many of its fields were read, and `flags`/`flags2`
 * are the flag words the caller saw — the table needs them to know which optional
 * fields are present.
 *
 * This is what lets the message parser read id, date and text itself and then
 * hand the tail back, instead of the table having to surface every value.
 */
mtp_skip_result_t mtp_skip_rest(mtp_r_t *r, unsigned type_index, uint32_t ctor_id,
                                unsigned consumed, uint32_t flags, uint32_t flags2);

/* Look up a constructor's field range; NULL when it is not in the table. */
const mtp_skip_ctor_t *mtp_skip_find(unsigned type_index, uint32_t ctor_id);

/*
 * Traverse like mtp_skip, but call `fn` as each present field is reached.
 *
 * The visitor receives a reader positioned at the start of the field and must not
 * advance it — to read a value, copy the struct (`mtp_r_t tmp = *r;`) and read
 * from the copy. Copying is why no separate peek API is needed.
 *
 * This is how the model extracts an id or a title without the table having to
 * know anything about which fields matter: the traversal stays generated and
 * exact, and the interesting fields are picked out by the index constants in
 * mtp_skip_types.h.
 */
typedef void (*mtp_skip_visit_fn)(void *user, uint32_t ctor_id,
                                  unsigned field_index, uint8_t kind,
                                  const mtp_r_t *r,
                                  uint32_t flags, uint32_t flags2);

mtp_skip_result_t mtp_skip_visit(mtp_r_t *r, unsigned type_index,
                                 mtp_skip_visit_fn fn, void *user);
