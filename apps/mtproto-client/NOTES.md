# MTProto client — state of the work

## Where it stands

The protocol and the interface are written and verified as far as they can be
without hardware. **The image lives entirely in the static app pool** — the
mtp_mem block that used to be a heap allocation (see "Pass two" below) is now
just more `.bss`, on the strength of JPPDOS's 80 KB pool guarantee.

Getting there took two passes over the client's memory. The second one changed
where memory comes from, not just how much of it there is, so the reasoning is
worth keeping even though the current code no longer needs the heap at all.

Measured with the real `jppd-app-sdk` toolchain (the estimates below this table,
from before the image existed, were wrong — see "Pass three"). The table is the
state before Pass five; see there for the RX-buffer change and what it did to
the numbers.

| Section | Bytes |
|---|---|
| `.text` | ~46 200 |
| `.rodata` | 8 780 |
| `.data` + `.got` + `.data.rel.ro` | ~700 |
| `.bss` (incl. the mtp_mem block, now 12 512 bytes after Pass three) | ~16 700 |
| Dynamic-linking overhead (`.hash`+`.dynsym`+`.dynstr`+`.rela.dyn`+`.rela.plt`+`.plt`) | ~4 400 |
| RX/RW segment page-alignment gap (linker artefact, not app content) | 4 096 |
| **Resident total (loader's `vaddr_max - vaddr_min`)** | **80 404** |
| Pool available | 81 920 |
| Headroom | 1 516 |

The old table below this one only summed `.text`/`.rodata`/`.data`/`.bss` and
projected ~75.7 KB with ~6.2 KB of headroom. That was never actually measured
against the real toolchain, and it missed two things the loader charges for
that a manual section tally doesn't: the ELF's dynamic-linking metadata and a
linker-inserted alignment gap between the RX and RW `PT_LOAD` segments. Real
resident use came in at 101 764 bytes — over budget by ~20 KB — and that
number is what showed up on hardware as `APP_CRASH app=mtproto-client
reason=NO_MEMORY`. See "Pass three" for the fix.

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

## Pass three: the dynamic-symbol tax (and the crash it caused)

The app shipped, crashed on hardware with `NO_MEMORY`, and the log gave an
exact number: `LOAD /sd/apps/mtproto-client/mtproto.bin: 101764 bytes (pool=81920
bytes)`. `jpp_native_loader_core`'s `load_image()` sizes its pool request as
`vaddr_max - vaddr_min` across every `PT_LOAD` segment — not the sum of
`.text`/`.rodata`/`.data`/`.bss` the table above (as it stood before this pass)
assumed. For an `ET_DYN` PIC binary that span also covers the ELF's own
dynamic-linking metadata: `.hash`, `.dynsym`, `.dynstr`, `.rela.dyn`,
`.rela.plt`, and `.plt`.

Every function and global in this app had default (exported) ELF visibility —
nothing in `build_shared.py`/`jppd-build` passes `-fvisibility=hidden`, and
nothing in the app opted out per-symbol. That put all 261 of the app's own
symbols into `.dynsym`/`.dynstr`/`.hash` alongside the 51 it actually imports
from the firmware, and routed internal calls through PLT stubs meant for
cross-module calls. Fix: `#pragma GCC visibility push(hidden)` in every
`src/*.c` file (placed after each file's own `extern` declarations of
imported SDK/libc symbols, so those stay resolvable — see `mtp_srp.c`'s
`crypto_hash_sha512`), with `jpp_app_entry` in `mtp_entry.c` explicitly
re-exported via `__attribute__((visibility("default")))` since it's the one
symbol the loader actually looks up by name. Verified against the real
toolchain: dynamic symbols 312 → 52, resident total 101 764 → 84 252 (−17.5 KB).

That still left ~2.3 KB over budget. The remainder is a 4 096-byte gap the
linker inserts between the RX and RW `PT_LOAD` segments for page alignment —
dead weight here, since the app pool is one RWX `.bss` block with no real page
protection to align for (see the rationale comment in `jpp_app_pool.h`). It
can't be closed from the app side: `jppd-app.json` only exposes `defines`/
`includes`/`extra_sources`, not link flags, and `LINK_FLAGS` is hardcoded in
`vendor/jppdos/tools/app-sdk/jppd-build`. Closing it would need a firmware-side
change (e.g. `-Wl,-z,max-page-size=4`) that benefits every native app, not just
this one — worth doing upstream, but out of scope for an app-only fix.

Instead the last 2.3 KB came out of `mtp_model.h`'s list caps, continuing Pass
one's trim: `MTP_MAX_PEERS`/`MTP_MAX_DIALOGS`/`MTP_MAX_MESSAGES` went from
24/12/16 to 16/8/10. These are display-cache capacities (how many chats/peers/
messages are held at once on a screen that shows a handful of lines at a
time), not protocol limits — a busier account keeps fewer cached entries
before the oldest ages out, nothing more. Final: 80 404 bytes resident, 1 516
bytes of headroom against the 81 920-byte pool.

**Note on the pool size itself:** the vendored `vendor/jppdos` submodule
pinned in this repo (`8acceff`) still defines `JPP_APP_POOL_BYTES` as 64 KB —
see `AGENTS.md`'s own snapshot. The hardware that produced the crash log is
running firmware past `6f1c8ef` ("Raise the app pool to 80 KB"), i.e. newer
than what's checked out here. All the numbers in this pass were measured
against an 81 920-byte pool to match the actual failing hardware; they will
not fit a 64 KB pool without a great deal more cutting.

## Pass four: the app_id hyphen (why the first boot was a black screen)

With the image finally in the pool, the app launched and did *nothing* — black
screen, no reaction to any key, no crash, and not one line of `app_log` output.
The launcher stayed healthy the whole time, so nothing had faulted.

The cause was the `app_id`, which was `mtproto-client`. **A hyphen is not a
legal character in a JPPDOS app id.** `jpp_str_name_valid()`
(`jpp_string_util.h`) accepts only `[A-Za-z0-9_]` plus non-consecutive interior
dots, and `jpp_sdk_app_id_is_valid()` in `jpp_sdk_bridge.c` gates on it. So
`jpp_sdk_bind_native()` bailed out with `INVALID_ARGUMENT` *before* reaching
`context->bound = true`.

Two things conspired to make that silent rather than obvious:

- **The bind result is discarded.** `main/jpp_app_dispatch.c` calls
  `jpp_sdk_bind_native(&s_sd_ctx, app_id, &caller, &s_native_services);` and
  ignores the return, so a rejected id produces no log and does not abort the
  launch. The app is started with a zeroed context.
- **Every SDK call fails soft.** They all begin with `jpp_sdk_ensure_bound()`,
  which returns `INVALID_STATE` when `bound` is false — an early `return`, not a
  fault. So `set_frame`, `canvas_*`, `poll_key` and `log` all became no-ops, and
  the app ran its main loop perfectly happily, drawing frames into a buffer that
  was never blitted and polling a key queue that never filled.

The manifest validator does **not** catch this: `jpp_manifest_core.c` only
checks `jpp_str_nonempty()` on `app_id` (plus the reserved-id list), never
`jpp_str_name_valid()`. `jppd-build` prints `manifest validated` and the
launcher lists and starts the app — the id is only re-validated deep inside the
SDK bind, where the failure has nowhere to go. Every app that ships in the
firmware tree (`meetapp`, `games`, `demoscene`, `testapp_native`, `testapp_mp`)
happens to use underscores, so nothing upstream had ever exercised the path.

Fixed by renaming the id to `mtproto_client`. That name is also baked into two
absolute paths the app opens with `fopen` rather than the SDK's scoped file API
(`STORE_DIR` in `mtp_store.c`, `CUSTOM_CONF_PATH` in `mtp_config.c`), so those
moved with it — the on-SD directory must be `/sd/apps/mtproto_client/` to match.
The old `/sd/apps/mtproto-client/` directory should be deleted; nothing in it is
worth migrating, since no session was ever established from it.

Diagnosis note for next time: when the SDK appears inert, the useful probe is
`esp_log_write` — it is in `jpp_native_symtab.c`, so an app can call it directly
and get output even when the SDK context is unbound and `jpp_sdk_log` is a
no-op. That is what distinguished "the app is not running" from "the app is
running and the SDK is ignoring it", which no amount of `jpp_sdk_log` could.

**Worth raising upstream** (firmware, not this app): the bind return value
should be checked and the launch aborted with a visible reason, and
`jpp_manifest_core.c` should validate `app_id` with `jpp_str_name_valid()` so a
bad id is rejected at install/validate time instead of silently producing a dead
app. `docs/manifest.md` does not state the character set either.

## Pass five: the dialogs reply, 2FA progress bar, phone placeholder, and logging

A field pass over bugs found once the app had authenticated:

- **No chats / "Reply too large".** `messages.getDialogs` fails with a `Reply too
  large` toast and the list stays empty because the reply does not fit the receive
  buffers. Measured on hardware, a 10-dialog page comes back as a **7 112-byte**
  frame — bigger than the RX buffer, let alone the 6 656-byte inflate arena that
  has to hold the whole thing. Growing the buffers into range is not an option
  (that is several KB against a pool with ~1.5 KB of headroom), so the reply is
  bounded at the request instead: `MTP_DIALOG_PAGE`/`MTP_HISTORY_PAGE` were cut to
  4 (each dialog drags its full user/chat record, ~700 bytes compressed). The RX
  buffer was also restored to 4 KB (its Pass-one value) for margin, which adds
  `ALIGN8(32+4096) − ALIGN8(32+3072) = 1 024` bytes to the `mtp_mem` block —
  `.bss` ~16 700 → ~17 700, resident ~80 404 → ~81 428, headroom ~1 516 → ~492
  against the 81 920-byte pool. Still fits; nothing else should grow if it can be
  helped. The tradeoff is that the dialog list has no load-more, so it now shows
  the four most recent chats rather than ten.
- **2FA hashing progress bar rendered off-screen.** The progress screen draws the
  bar at y=46, which is inside the fullscreen canvas — but the SRP derivation
  runs right after the `jpp_sdk_input()` password prompt, and every modal helper
  drops the canvas back to the 48-row windowed area. On a windowed canvas the
  bar's lower half (rows 48+) is clipped. `scr_connecting_draw()` now re-enables
  fullscreen before drawing, so every progress screen (connect *and* 2FA) renders
  on the full 128×64 display — the same "re-enable fullscreen after a modal" rule
  the SDK documents.
- **Remembered phone number unusable.** The persisted number is shown as the
  input's placeholder, but confirming without typing came back as an empty string
  and was rejected (`< 6 digits`), forcing a retype. An empty submit now means
  "use the remembered number" when one exists.
- **Comprehensive serial logging.** The client only logged three lifecycle events,
  so a working-then-silent connection offered nothing to read in the serial log.
  A single `mtp_log()` helper (implemented by `mtp_client.c`, declared in
  `mtp_common.h`, a no-op until the SDK context is installed) now instruments the
  whole app: connect/handshake/migrate, every transport and session failure
  (including the exact frame size on a `Reply too large`), RPC results/errors/
  timeouts/resends, each login step and 2FA milestone, dialogs/history/send
  results, update constructors, screen transitions and notifications. `jpp_sdk_log`
  takes a single event string with no formatting, so events that carry a value
  snprintf it into a small buffer first; the strings are already enough to follow
  a session in the `app_log` stream. Host-linked modules (`mtp_common`, `mtp_tl`,
  `mtp_pq`, `mtp_config`, `mtp_gzip`, `mtp_skip*`, `mtp_srp`, `mtp_scratch`,
  `mtp_mem`, `ui_*`) are deliberately left alone so the host tests keep linking
  without a firmware symbol table.

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

`./test/run.sh` runs TL round-trips and padding boundaries, PQ factorisation
against known products, gzip inflate cross-checked against zlib at every
compression level, the RSA key fingerprint derived by the real config code
(0xd09d1d85de64fd85, which pins the key bytes and the TL writer at once),
HMAC-SHA512 and PBKDF2 against RFC 4231 and an independent implementation, UTF-8
and text layout, and the TL skipper against objects encoded independently from
the schema — including at every truncation offset.

Text entry (phone number, login code, 2FA password, message compose) now goes
through the App SDK's own `jpp_sdk_input()` instead of the hand-rolled
Cyrillic-capable on-canvas keyboard that used to live in `ui_keyboard.c`. That
trades away Cyrillic typing and caps every field at `JPP_SDK_INPUT_VALUE_MAX`
(64 ASCII characters) — a real regression for message compose, which used to
allow 224 UTF-8 bytes — and the App SDK input has no masked/dots mode, so the
2FA password is now shown in the clear while typed. It also means
`jpp_sdk_input()`'s blocking modal loop runs during phone/code/password entry
and message compose, so `mtp_client_pump()` (keep-alive, incoming updates)
does not run for as long as the user is typing; see `ui_widgets.h` for why
every other screen in this app avoids blocking modals. Both tradeoffs were an
explicit choice to reuse the platform's own keyboard rather than maintain a
custom one.

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
