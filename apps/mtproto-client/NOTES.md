# MTProto client — state of the work

## Where it stands

The protocol and the interface are written and verified as far as they can be
without hardware. **The image lives entirely in the static app pool** — the
mtp_mem block that used to be a heap allocation (see "Pass two" below) is now
just more `.bss`, on the strength of JPPDOS's 80 KB pool guarantee.

Getting there took two passes over the client's memory. The second one changed
where memory comes from, not just how much of it there is, so the reasoning is
worth keeping even though the current code no longer needs the heap at all.

Measured with the SDK's own flags (`riscv32-esp-elf-gcc -Os -fPIC -mno-relax`,
linked `-shared -nostdlib`), stripped:

| Section | Bytes |
|---|---|
| `.text` | ~45 700 |
| `.rodata` | 8 716 |
| `.data` + `.got` + `.data.rel.ro` | 1 650 |
| `.bss` (incl. the 16 128-byte mtp_mem block) | ~19 628 |
| **Resident total** | **~75 700** |
| Pool available | 81 920 |
| Headroom | ~6 200 |

A caveat on the approximate rows. The last real RISC-V build predates both passes;
these figures carry the host-measured deltas onto it. On the host the same
sources went from 30 103 bytes of `.bss` to 3 752, and from 63 427 bytes of
`.text`+`.rodata` to 64 038. The savings are almost entirely fixed-size byte
arrays, identical on both targets, so the derived numbers should be close — but
they have not been through `riscv32-esp-elf-gcc` and must be re-measured when the
image exists. ~6.2 KB of headroom is enough to absorb being wrong about this.

## Pass one: how much

`.bss` started at 40 KB. Sizing it honestly took it to 29.8 KB: transfer buffers
from 2/4/8 KB to 1.5/3/5 KB, model caches from 32/16/24 entries to 24/12/16, and
one 6.5 KB arena (`mtp_scratch.c`) shared by the handshake and the SRP bignums,
which are provably never live together.

Lifetime took it from 29.8 KB to 19.6 KB, without giving up a single feature —
everything it removed was memory held by something that was not using it:

| Change | Bytes |
|---|---|
| The 5 KB inflate buffer became the arena's default tenant | 5 120 |
| `custom.conf`'s 1.4 KB read buffer borrows the arena (it is read once, at startup) | 1 400 |
| Three staging buffers on the request path collapsed into one | 1 152 |
| The TX buffer stopped reserving 1 KB for padding it never emits | 768 |
| Two handshake buffers that were `static` inside a function moved into the arena | 672 |
| The SRP parameters and proof moved to the stack of the one call that uses them | ~900 |
| Peer/dialog structs reordered by alignment, indices narrowed to `int16_t` | 240 |

The arena is the theme. It is sized by SRP at 6 656 bytes and, before this, sat
idle for the entire life of a logged-in session — so four things that are never
simultaneously alive now share it, and the inflate bound went *up* from 5 120 to
6 656 for free while the total went down. `mtp_scratch.h` documents the ordering
rule that keeps that safe, and each tenant carries a `_Static_assert` so growing
one past the arena is a build error rather than a NULL at runtime.

That still left ~75.3 KB resident against a 65.5 KB pool.

## Pass two: whose memory (superseded)

This section originally explained why the buffers moved to a heap allocation
instead of raising `JPP_APP_POOL_BYTES` — kept below for the history, but the
conclusion no longer holds and the code no longer does this.

At the time, the obvious remedy — raise `JPP_APP_POOL_BYTES` — looked wrong,
because the pool is static `.bss`, reserved whether or not an app is running:
raising it to 80 KB would have taken 16 KB from the heap *permanently*,
including while a WebDAV transfer was running with no app loaded, which was
precisely the situation `AGENTS.md` describes as making WiFi allocations fail
and silently wedging the radio. So the buffers (`mtp_mem.c`) went on the heap
instead, taken as one block at startup and freed on exit, and the pool stayed
at 64 KB.

What changed the calculus: JPPDOS moved WebDAV and the LRV server into the app
pool's own workspace instead of the shared heap, and made running a server and
running an app mutually exclusive. That closes the exact hazard this section
was designed around — a server no longer competes with an app for heap, because
they can no longer be up at the same time — and JPPDOS now guarantees 80 KB of
pool as an actual property of the SDK contract, not just an unannounced firmware
size. With that guarantee in place, the heap workaround stopped buying anything
and was removed: `mtp_mem`'s block (the shared arena, TX/RX buffers, peer/
dialog/message caches, and the framebuffer — 16 128 bytes) is a plain static
array now, sized the same way, living in `.bss` like everything else.

The one thing worth carrying forward: **`sdk_min` must reflect whatever SDK
level JPPDOS assigns the 80 KB guarantee to, once that lands.** Until then this
app has the same undeclarable-dependency problem the pool-raise was originally
rejected for — a device whose firmware only promises the old 64 KB pool would
pass the `sdk_min` check and then fail to load this image, with no
`SDK_TOO_OLD` to catch it. Don't ship past `sdk_min: 2` until that's resolved
on the firmware side.

## What is left, and what is not worth doing

`.bss` is now ~3.5 KB, most of it `mtp_config`'s RSA moduli and profile tables and
`mtp_client`'s session record. There is nothing substantial left to move.

`.text` is the dominant weight and is not fat — it is the protocol. The five
largest contributors are the handshake (4.0 KB), the login screens (3.6 KB), the
generated TL skip table (3.5 KB), SRP (3.3 KB), and the model parsers (3.0 KB).

**Code modules are not needed and would not help.** An earlier version of this
note claimed a `login.mod.bin` split would make the app fit; that was wrong for a
reason worth recording. A module does not replace the hub in the pool, it loads
into the *tail* of it, and both are resident together
(`docs/native/modules.md`, "How the pool is split"). The binding constraint is
`hub + largest module ≤ 64 KB`, not `hub ≤ 64 KB`, so total code does not shrink
by being split — only the parts never live at the same time buy anything. The
best case measured out at a 78.8 KB peak against an 84.6 KB unsplit image: 6 KB
for a substantial restructure, and still over. With the image at ~59.6 KB there
is nothing left for it to solve.

## Firmware status

`feature/mtproto-sdk-ext` was merged to master (`7abb2b6`) and shipped as
**v1.1** (`9a0fdd3`), so the whole SDK v2 surface this client needs (TCP,
crypto) is released. Checked against JPPDOS tag `1.2-rc.2`: all 47 symbols the
app imports are exported (down from 49 — dropping the mtp_mem heap block also
dropped `malloc`/`free` as imports; `jpp_sdk_set_frame` is still needed for the
build-time-invariant backstop message).

**This app now requires the 80 KB app pool**, which is present as of `1.2-rc.2`
(`6f1c8ef`, "Raise the app pool to 80 KB") but is currently a firmware-version
fact, not an SDK-level one — there is no manifest field that expresses it, and
`sdk_min: 2` alone does not guarantee it. Per the "Pass two" section above, this
needs the pool guarantee formalized as its own SDK-gated change before this app
can safely declare (and have enforced) a firmware floor above v1.1. Tracking
that with the firmware side; **do not bump `sdk_min` past 2 until it lands**, or
the manifest will claim a compatibility guarantee the firmware doesn't check.

Building still needs `toolchain/build-image.sh` run against master to produce the
`jppd-app-sdk` image, which is not built locally. Everything measured here was
compiled with the same flags by hand in `espressif/idf:v5.5.1`, not through
`jppd-build`.

`vendor/jppdos` in this repo is still pinned at `8acceff`, from before the merge;
worth advancing to `1.2-rc.2` (or later) so the vendored headers match what ships
and the pool size the app now depends on.

## What is verified, and what is not

`./test/run.sh` runs 3 862 checks: TL round-trips and padding boundaries, PQ
factorisation against known products, gzip inflate cross-checked against zlib at
every compression level, the RSA key fingerprint derived by the real config code
(0xd09d1d85de64fd85, which pins the key bytes and the TL writer at once),
HMAC-SHA512 and PBKDF2 against RFC 4231 and an independent implementation, UTF-8
and text layout, the keyboard's editing, and the TL skipper against objects
encoded independently from the schema — including at every truncation offset.

It also checks that every symbol the app imports is one the firmware exports.
That check found `fgets`, which the app used and the loader does not provide; it
would have failed at launch with `UNRESOLVED_SYM` and nothing on screen.

Two bugs came out of the RAM pass rather than the tests, both from asking who
else was using a buffer:

- `migrate()` built its `auth.exportAuthorization` in the same request buffer as
  the call it was redirecting, so after a `PHONE_MIGRATE_n` the reissue on the
  new DC sent migration's leftover bytes. It now stages its own requests and the
  interrupted one is saved across the migration; `migrate()` also refuses to
  nest, which bounds a redirect loop.
- A `msg_container` carrying the awaited `rpc_result` *and* a `gzip_packed`
  update behind it would inflate the update over the result, and the caller would
  parse the update as its answer. `mtp_rpc` now declines the second inflate and
  drops the update, which `updates.getDifference` recovers.

Neither is reachable from the host tests — both need a live server — and neither
was introduced by the RAM changes; the single shared arena is just what made the
aliasing visible.

`mtp_mem_init` is exercised on every test run, same as the real build — it is a
static block now, so there is no allocate-time failure path left to provoke.
The `mem_setup` error frame is unreachable outside of `MTP_MEM_BYTES` itself
being wrong, which would be a build-time miscalculation rather than something
that shows up on device.

**Nothing has talked to a Telegram server.** The handshake, the login flow and
the update loop are unexercised against a live DC. Expect the first hardware
session to find things — the likely candidates are the layer-223 field orders in
`mtp_api.c`, which are read from the schema but never round-tripped, and the
clock re-anchoring in `mtp_time.c`, which cannot be tested without a server that
disagrees with the RTC.

## Values that must be filled in before release

One block at the top of `src/mtp_config.c`: `MTP_TELEGRAM_API_ID` and
`MTP_TELEGRAM_API_HASH` from my.telegram.org, and the whole j++gram profile.
Until then those two modes report "not configured" in the picker instead of
failing at connect time. Telegram's DC addresses and public key are real and
already embedded.
