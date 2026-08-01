#!/usr/bin/env python3
"""
Generate src/mtp_skip_data.c — a table describing TL field layouts.

Why this exists
---------------
To read the second element of a TL vector you must know where the first one ends,
and to know that you must understand every field of it — including the optional
ones you do not care about. The full transitive closure of the types a Telegram
client touches is around 780 constructors, which will not fit a 64 KB app pool as
generated code.

The way out is that the closure only explodes through a handful of types:
MessageMedia and InputMedia drag in photos, documents, web pages, polls, and
their attribute vectors. Declaring those two opaque cuts the closure to 149
constructors and 634 fields — about 2.5 KB of table, which is affordable.

An opaque field is not a silent failure: mtp_skip reports it, and the caller
stops the current batch and resumes from the last message id it did parse. So a
chat full of photos costs extra round trips, not lost messages.

    ./test/gen_skip.py --stats
    ./test/gen_skip.py > ../src/mtp_skip_data.c
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = os.environ.get('TL_JSON', os.path.join(HERE, 'tl.json'))

# Types whose contents the client never needs and whose closures are unbounded.
OPAQUE = {
    # These three carry the whole photo/document/webpage/poll universe between
    # them. MessageAction is included because a service message's action is
    # unconditional, so tabulating its 64 variants would buy the ability to skip
    # service messages at a cost of ~2.9 KB — not worth it when the fallback is
    # one extra round trip.
    'MessageMedia', 'InputMedia', 'MessageAction',
}

# Everything reachable from these is tabulated.
ROOTS = [
    'Peer', 'UserProfilePhoto', 'UserStatus', 'EmojiStatus', 'Username',
    'RecentStory', 'PeerColor', 'RestrictionReason', 'ChatPhoto',
    'ChatAdminRights', 'ChatBannedRights', 'InputChannel', 'PeerNotifySettings',
    'DraftMessage', 'MessageEntity', 'MessageFwdHeader', 'MessageReplyHeader',
    'MessageReplies', 'MessageReactions', 'ReplyMarkup', 'FactCheck',
    'SuggestedPost', 'auth.SentCodeType', 'PasswordKdfAlgo',
    'SecurePasswordKdfAlgo', 'ForumTopic', 'User', 'Chat', 'Dialog', 'Message',
]

# Kind codes, mirrored in mtp_skip.c.
K = {
    'FLAGS0': 0, 'FLAGS1': 1, 'INT': 2, 'LONG': 3, 'INT128': 4, 'INT256': 5,
    'DOUBLE': 6, 'STRING': 7, 'BOOL': 8, 'TRUE': 9, 'NESTED': 10, 'VEC': 11,
    'VEC_INT': 12, 'VEC_LONG': 13, 'VEC_STRING': 14, 'OPAQUE': 15,
}
UNCOND = 0xFF

BUILTIN_SCALAR = {
    'int': 'INT', 'long': 'LONG', 'int128': 'INT128', 'int256': 'INT256',
    'double': 'DOUBLE', 'string': 'STRING', 'bytes': 'STRING', 'Bool': 'BOOL',
}


CTORS_BY_NAME = {}


def _ensure_schema():
    """Fetch Telegram's schema if it is not already beside this script.

    Deliberately not committed: it is ~500 KB of upstream data that would go
    stale in the tree, and the generated headers it produces are committed
    instead. Regenerating is a two-step: fetch, then run.
    """
    if os.path.exists(SCHEMA):
        return
    import urllib.request
    url = 'https://core.telegram.org/schema/json'
    sys.stderr.write(f"fetching {url} -> {SCHEMA}\n")
    try:
        urllib.request.urlretrieve(url, SCHEMA)
    except Exception as exc:
        sys.exit(f"could not fetch the TL schema: {exc}\n"
                 f"download it manually to {SCHEMA}, or set TL_JSON")


def load():
    _ensure_schema()
    with open(SCHEMA) as f:
        d = json.load(f)
    by_type = {}
    for c in d['constructors']:
        by_type.setdefault(c['type'], []).append(c)
        CTORS_BY_NAME[c['predicate']] = c
    return by_type


def strip_cond(t):
    """Returns (bare_type, cond_code)."""
    m = re.match(r'^flags(\d*)\.(\d+)\?(.*)$', t.strip())
    if not m:
        return t.strip(), UNCOND
    word = 0 if m.group(1) == '' else int(m.group(1)) - 1
    bit = int(m.group(2))
    assert word in (0, 1), f"unexpected flags word in {t!r}"
    assert 0 <= bit < 32, f"unexpected flag bit in {t!r}"
    return m.group(3).strip(), word * 32 + bit


def vec_inner(t):
    m = re.match(r'^[Vv]ector<(.+)>$', t)
    return m.group(1).strip().lstrip('%') if m else None


def closure(by_type):
    seen, frontier = set(), list(ROOTS)
    while frontier:
        t = frontier.pop()
        if t in seen or t in OPAQUE or t in BUILTIN_SCALAR or t in ('true', '#'):
            continue
        if t not in by_type:
            continue
        seen.add(t)
        for c in by_type[t]:
            for p in c['params']:
                bare, _ = strip_cond(p['type'])
                inner = vec_inner(bare)
                cand = inner if inner else bare.lstrip('%')
                if cand and cand not in seen:
                    frontier.append(cand)
    return sorted(seen)


def field_op(ptype, types_index):
    """Encode one field as (kind, cond, arg)."""
    bare, cond = strip_cond(ptype)

    if bare == '#':
        # Two flag words appear in the big constructors; the second is always
        # named flags2 and always follows the first.
        return ('FLAGS0', cond, 0)   # caller fixes up which word
    if bare == 'true':
        return ('TRUE', cond, 0)
    if bare in BUILTIN_SCALAR:
        return (BUILTIN_SCALAR[bare], cond, 0)

    inner = vec_inner(bare)
    if inner:
        if inner in ('int',):
            return ('VEC_INT', cond, 0)
        if inner in ('long',):
            return ('VEC_LONG', cond, 0)
        if inner in ('string', 'bytes'):
            return ('VEC_STRING', cond, 0)
        if inner in OPAQUE or inner not in types_index:
            return ('OPAQUE', cond, 0)
        return ('VEC', cond, types_index[inner])

    bare = bare.lstrip('%')
    if bare in OPAQUE or bare not in types_index:
        return ('OPAQUE', cond, 0)
    return ('NESTED', cond, types_index[bare])


def build():
    by_type = load()
    types = closure(by_type)
    tindex = {t: i for i, t in enumerate(types)}

    fields = []       # flat (kind, cond, arg)
    ctors = []        # (id, first_field, n_fields, name)
    type_rows = []    # (first_ctor, n_ctors, name)

    for t in types:
        first_ctor = len(ctors)
        variants = sorted(by_type[t], key=lambda c: int(c['id']) & 0xFFFFFFFF)
        for c in variants:
            first_field = len(fields)
            seen_flags = 0
            for p in c['params']:
                kind, cond, arg = field_op(p['type'], tindex)
                if kind == 'FLAGS0':
                    # Distinguish the first flags word from the second.
                    kind = 'FLAGS0' if seen_flags == 0 else 'FLAGS1'
                    seen_flags += 1
                fields.append((kind, cond, arg))
            ctors.append((int(c['id']) & 0xFFFFFFFF, first_field,
                          len(fields) - first_field, c['predicate']))
        type_rows.append((first_ctor, len(ctors) - first_ctor, t))

    return types, tindex, fields, ctors, type_rows


def cname(t):
    return 'MTP_T_' + re.sub(r'[^A-Z0-9]', '_', t.upper().replace('.', '_'))


# Fields the client reads by hand while the table does the traversal around them.
# Emitting their indices rather than hand-counting is the whole point: `user` has
# 27 flag-only fields before `id`, and miscounting them is invisible until a name
# renders as garbage.
WANTED_FIELDS = {
    'user': ['id', 'access_hash', 'first_name', 'last_name', 'username', 'status'],
    'userEmpty': ['id'],
    'chat': ['id', 'title'],
    'chatForbidden': ['id', 'title'],
    'chatEmpty': ['id'],
    'channel': ['id', 'access_hash', 'title'],
    'channelForbidden': ['id', 'access_hash', 'title'],
    'dialog': ['peer', 'top_message', 'read_inbox_max_id', 'read_outbox_max_id',
               'unread_count'],
    'message': ['id', 'from_id', 'peer_id', 'date', 'message', 'media'],
    'messageService': ['id', 'from_id', 'peer_id', 'date'],
    'peerUser': ['user_id'],
    'peerChat': ['chat_id'],
    'peerChannel': ['channel_id'],
}


def field_defines(by_type):
    lines = []
    for ctor, names in sorted(WANTED_FIELDS.items()):
        c = CTORS_BY_NAME[ctor]
        index = {p['name']: i for i, p in enumerate(c['params'])}
        for n in names:
            assert n in index, f"{ctor} has no field {n!r}"
            macro = 'MTP_F_' + re.sub(r'[^A-Z0-9]', '_',
                                      (ctor + '_' + n).upper())
            lines.append((macro, index[n]))
    return lines


def emit():
    types, tindex, fields, ctors, type_rows = build()
    out = []
    w = out.append

    w('/*')
    w(' * mtp_skip_data.c — TL field-layout tables.')
    w(' *')
    w(' * GENERATED by test/gen_skip.py from Telegram\'s official schema.')
    w(' * DO NOT HAND-EDIT — regenerate instead.')
    w(' *')
    w(' * See the header of that script for why this exists and why MessageMedia is')
    w(' * deliberately opaque. In short: a full skipper needs ~780 constructors and')
    w(' * will not fit the app pool; excluding the media types brings it to the')
    w(f' * {len(ctors)} below, and an opaque field is reported rather than mis-skipped.')
    w(' */')
    w('#include "mtp_skip.h"')
    w('')
    w(f'/* {len(fields)} fields, {len(ctors)} constructors, {len(types)} types */')
    w('const mtp_skip_field_t mtp_skip_fields[] = {')
    line = '   '
    for kind, cond, arg in fields:
        piece = f' {{{K[kind]:2d},0x{cond:02X},{arg:3d}}},'
        if len(line) + len(piece) > 78:
            w(line)
            line = '   '
        line += piece
    if line.strip():
        w(line)
    w('};')
    w('')
    w('const mtp_skip_ctor_t mtp_skip_ctors[] = {')
    for cid, first, n, name in ctors:
        w(f'    {{ 0x{cid:08x}u, {first:4d}, {n:2d} }},   /* {name} */')
    w('};')
    w('')
    w('const mtp_skip_type_t mtp_skip_types[] = {')
    for first, n, name in type_rows:
        w(f'    {{ {first:4d}, {n:2d} }},   /* {cname(name)} = {name} */')
    w('};')
    w('')
    w('const size_t mtp_skip_type_count = '
      'sizeof(mtp_skip_types) / sizeof(mtp_skip_types[0]);')

    # Header fragment with the type indices, written alongside.
    hdr = []
    hdr.append('/* GENERATED by test/gen_skip.py — type indices for mtp_skip. */')
    hdr.append('#pragma once')
    hdr.append('')
    for i, t in enumerate(types):
        hdr.append(f'#define {cname(t):<44} {i}')
    hdr.append('')
    hdr.append('/* Field indices within a constructor, for mtp_skip_visit. */')
    for macro, idx in field_defines(None):
        hdr.append(f'#define {macro:<44} {idx}')
    return '\n'.join(out) + '\n', '\n'.join(hdr) + '\n'


def stats():
    types, tindex, fields, ctors, type_rows = build()
    opaque = sum(1 for k, _, _ in fields if k == 'OPAQUE')
    print(f"types        : {len(types)}")
    print(f"constructors : {len(ctors)}")
    print(f"fields       : {len(fields)}  ({opaque} opaque)")
    print(f"table bytes  : fields {len(fields) * 3}, "
          f"ctors {len(ctors) * 8}, types {len(type_rows) * 4}"
          f"  => {len(fields) * 3 + len(ctors) * 8 + len(type_rows) * 4}")
    print()
    print("constructors carrying an opaque field (parsing stops at these):")
    for cid, first, n, name in ctors:
        if any(fields[first + i][0] == 'OPAQUE' for i in range(n)):
            print(f"  {name}")


if __name__ == '__main__':
    if '--stats' in sys.argv:
        stats()
    else:
        body, header = emit()
        target = os.path.join(HERE, '..', 'src')
        with open(os.path.join(target, 'mtp_skip_types.h'), 'w') as f:
            f.write(header)
        sys.stdout.write(body)
