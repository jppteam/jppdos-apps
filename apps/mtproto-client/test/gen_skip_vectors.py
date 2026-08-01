#!/usr/bin/env python3
"""
Generate test/skip_vectors.h — encoded TL objects for the skipper tests.

The encoder here is written straight off the schema JSON and shares no code with
the C table generator, so agreement between the two is real evidence rather than
a tautology. Each vector records the exact byte length the skipper must consume;
landing anywhere else means a field was mis-sized.

The cases deliberately include the awkward shapes: absent and present optionals,
both flag words, nested objects, vectors, and objects carrying an opaque media
field that the skipper is expected to refuse rather than guess at.

    ./test/gen_skip_vectors.py > test/skip_vectors.h
"""
import json
import os
import random
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = os.environ.get('TL_JSON', os.path.join(HERE, 'tl.json'))

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


_ensure_schema()
with open(SCHEMA) as f:
    _d = json.load(f)
CTORS = {c['predicate']: c for c in _d['constructors']}
BY_TYPE = {}
for _c in _d['constructors']:
    BY_TYPE.setdefault(_c['type'], []).append(_c)

OPAQUE_TYPES = {'MessageMedia', 'InputMedia', 'MessageAction'}


def cid(name):
    return int(CTORS[name]['id']) & 0xFFFFFFFF


def u32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def u64(v):
    return struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF)


def tl_bytes(b):
    if isinstance(b, str):
        b = b.encode()
    if len(b) < 254:
        out = bytes([len(b)]) + b
    else:
        out = b'\xfe' + len(b).to_bytes(3, 'little') + b
    out += b'\x00' * ((-len(out)) % 4)
    return out


def strip_cond(t):
    m = re.match(r'^flags(\d*)\.(\d+)\?(.*)$', t.strip())
    if not m:
        return t.strip(), None, None
    word = 0 if m.group(1) == '' else int(m.group(1)) - 1
    return m.group(3).strip(), word, int(m.group(2))


def vec_inner(t):
    m = re.match(r'^[Vv]ector<(.+)>$', t)
    return m.group(1).strip().lstrip('%') if m else None


class Enc:
    """Encodes a constructor, choosing which optional fields to include."""

    def __init__(self, rng, present):
        self.rng = rng
        # present: set of "flagsN.B" strings the caller wants switched on.
        self.present = present

    def value(self, typ, depth=0):
        inner = vec_inner(typ)
        if inner:
            n = self.rng.randint(0, 2) if depth < 2 else 0
            out = u32(0x1cb5c415) + u32(n)
            for _ in range(n):
                out += self.scalar_or_obj(inner, depth + 1)
            return out
        return self.scalar_or_obj(typ, depth)

    def scalar_or_obj(self, typ, depth):
        if typ == 'int':
            return u32(self.rng.randrange(1 << 31))
        if typ == 'long':
            return u64(self.rng.randrange(1 << 63))
        if typ == 'double':
            return u64(self.rng.randrange(1 << 63))
        if typ == 'int128':
            return bytes(self.rng.randrange(256) for _ in range(16))
        if typ == 'int256':
            return bytes(self.rng.randrange(256) for _ in range(32))
        if typ in ('string', 'bytes'):
            n = self.rng.choice([0, 1, 5, 13, 254, 300])
            return tl_bytes(bytes(self.rng.randrange(65, 90) for _ in range(n)))
        if typ == 'Bool':
            return u32(cid('boolTrue') if self.rng.random() < 0.5 else cid('boolFalse'))
        # An object: pick the simplest variant so the encoding stays shallow.
        variants = BY_TYPE.get(typ)
        if not variants:
            raise KeyError(typ)
        variants = sorted(variants, key=lambda c: len(c['params']))
        return self.ctor(variants[0]['predicate'], depth + 1)

    def ctor(self, name, depth=0):
        c = CTORS[name]
        out = u32(cid(name))
        # First pass: work out the flag words.
        words = [0, 0]
        for p in c['params']:
            bare, word, bit = strip_cond(p['type'])
            if word is None:
                continue
            key = f"flags{'' if word == 0 else '2'}.{bit}"
            if key in self.present:
                words[word] |= 1 << bit
        seen_flags = 0
        for p in c['params']:
            bare, word, bit = strip_cond(p['type'])
            if bare == '#':
                out += u32(words[seen_flags])
                seen_flags += 1
                continue
            if word is not None:
                if not (words[word] >> bit) & 1:
                    continue
            if bare == 'true':
                continue
            out += self.value(bare, depth)
        return out


def emit_case(name, ctor, type_macro, present, seed, expect_opaque=False):
    rng = random.Random(seed)
    data = Enc(rng, set(present)).ctor(ctor)
    body = ', '.join(f'0x{b:02X}' for b in data)
    lines = []
    lines.append(f'/* {ctor}, optionals: {sorted(present) or "none"} */')
    lines.append(f'static const uint8_t {name}_data[] = {{')
    line = '   '
    for b in data:
        piece = f' 0x{b:02X},'
        if len(line) + len(piece) > 78:
            lines.append(line)
            line = '   '
        line += piece
    if line.strip():
        lines.append(line)
    lines.append('};')
    lines.append(f'static const skip_case_t {name} = {{')
    lines.append(f'    "{ctor}{"" if not present else " +" + "+".join(sorted(present))}",')
    lines.append(f'    {type_macro}, {name}_data, sizeof({name}_data), '
                 f'{"true" if expect_opaque else "false"} }};')
    return '\n'.join(lines)


CASES = [
    # (c-name, constructor, type macro, optional flags to switch on, seed, opaque?)
    ('user_min',   'user',    'MTP_T_USER',    [], 1),
    ('user_named', 'user',    'MTP_T_USER',    ['flags.0', 'flags.1', 'flags.2', 'flags.3'], 2),
    ('user_full',  'user',    'MTP_T_USER',
     ['flags.0', 'flags.1', 'flags.2', 'flags.3', 'flags.4', 'flags.5',
      'flags.6', 'flags.14', 'flags.18', 'flags.22', 'flags2.0', 'flags2.5'], 3),
    ('user_empty', 'userEmpty', 'MTP_T_USER',  [], 4),

    ('chat_min',   'chat',    'MTP_T_CHAT',    [], 5),
    ('chat_opts',  'chat',    'MTP_T_CHAT',    ['flags.6', 'flags.14', 'flags.18'], 6),
    ('channel_min', 'channel', 'MTP_T_CHAT',   [], 7),
    ('channel_opts', 'channel', 'MTP_T_CHAT',
     ['flags.13', 'flags.6', 'flags.9', 'flags.17', 'flags2.0'], 8),

    ('dialog_min', 'dialog',  'MTP_T_DIALOG',  [], 9),
    ('dialog_opts', 'dialog', 'MTP_T_DIALOG',  ['flags.0', 'flags.4', 'flags.5'], 10),

    ('peer_user',  'peerUser', 'MTP_T_PEER',   [], 11),
    ('peer_chan',  'peerChannel', 'MTP_T_PEER', [], 12),

    ('msg_plain',  'message', 'MTP_T_MESSAGE', ['flags.8'], 13),
    ('msg_reply',  'message', 'MTP_T_MESSAGE', ['flags.8', 'flags.3'], 14),
    ('msg_fwd',    'message', 'MTP_T_MESSAGE', ['flags.8', 'flags.2'], 15),
    ('msg_ents',   'message', 'MTP_T_MESSAGE', ['flags.8', 'flags.7'], 16),
    ('msg_edited', 'message', 'MTP_T_MESSAGE', ['flags.8', 'flags.15', 'flags.16'], 17),
    # flags.9 is `media`, which the table deliberately does not model.
    ('msg_media',  'message', 'MTP_T_MESSAGE', ['flags.8', 'flags.9'], 18, True),
]


def main():
    out = []
    w = out.append
    w('/*')
    w(' * skip_vectors.h — GENERATED by test/gen_skip_vectors.py.')
    w(' *')
    w(' * Encoded TL objects with their exact byte lengths. The encoder is written')
    w(' * independently of the C skip table, so agreement between them is evidence')
    w(' * rather than a tautology.')
    w(' */')
    w('#pragma once')
    w('')
    w('#include "mtp_skip.h"')
    w('')
    w('typedef struct {')
    w('    const char    *name;')
    w('    unsigned       type_index;')
    w('    const uint8_t *data;')
    w('    size_t         len;')
    w('    bool           expect_opaque;')
    w('} skip_case_t;')
    w('')
    names = []
    for case in CASES:
        name, ctor, macro, present, seed = case[:5]
        opaque = len(case) > 5 and case[5]
        w(emit_case(name, ctor, macro, present, seed, opaque))
        w('')
        names.append(name)
    w('static const skip_case_t *const skip_cases[] = {')
    for n in names:
        w(f'    &{n},')
    w('};')
    w('static const size_t skip_case_count = '
      'sizeof(skip_cases) / sizeof(skip_cases[0]);')
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
