# jppd-app-sdk — Docker build toolchain for JPPDOS apps

Build a J++Device app (native C **or** MicroPython) with a single
`docker run`, without checking out or building the firmware yourself. The image
bakes a full firmware build as an SDK "sysroot" — the exact toolchain, headers,
and ~145 include paths that firmware release compiles `jpp_core` with.

> **Naming:** the tool is `jppd-build` / `jppd-app-sdk` (JPPD = J++Device /
> JPPDOS). This is unrelated to *J++*, the separate community.

## Do you need to build the image?

**No, if you are writing an app.** Pull (or otherwise obtain) a prebuilt
`jppd-app-sdk` image and skip to [Build an app](#build-an-app). Nothing in this
section applies to you, and you never need the firmware sources.

**Yes, if you are publishing the image** for a new firmware release. That build
needs the firmware sources: `capture_sysroot.py` distills the sysroot out of a
*real* firmware build — `build/compile_commands.json` for the include closure,
plus generated headers such as `sdkconfig.h` — none of which can be synthesised
without compiling the firmware.

This repository does **not** vendor those sources — there is no submodule and no
firmware checkout. The Dockerfile clones them in its builder stage, and
`build-image.sh` decides which revision to clone. Run it from the **repository
root** (the whole repo is the build context):

```bash
toolchain/build-image.sh
```

That builds against the current tip of the firmware's `master`. To build against
something else, set `JPPDOS_REF` to any branch, tag, or commit:

```bash
JPPDOS_REF=1.0-RTM toolchain/build-image.sh
IMAGE_TAG=jppd-app-sdk:rtm JPPDOS_REF=1.0-RTM toolchain/build-image.sh
```

Extra arguments are forwarded to `docker build` (`--no-cache`, `--platform`, …),
and `JPPDOS_REPO` overrides the firmware git URL if you are building from the
internal mirror rather than the public one.

The image is still **version-locked** to whatever revision it was built from —
headers and the SDK surface change between revisions — but locked at *build*
time rather than in git. Every image records its own provenance, so you can
always recover which firmware an SDK image matches:

```bash
docker run --rm --entrypoint cat jppd-app-sdk /opt/jppd-sdk/firmware-rev
docker inspect -f '{{ index .Config.Labels "org.jppdevice.firmware-rev" }}' jppd-app-sdk
```

### Why a wrapper script and not just `docker build`

`build-image.sh` resolves `JPPDOS_REF` to a concrete commit with `git ls-remote`
*before* invoking docker, and passes it in as the `JPPDOS_REV` build arg. That
one indirection buys both halves of what you want:

- **Always latest.** The arg value changes the moment the ref moves, so the
  `RUN git clone` layer is invalidated automatically. Cloning a moving ref
  directly would keep serving a layer cached from whenever you last built, and
  "latest" would silently rot — the same failure mode the submodule had, just
  hidden in the Docker cache instead of in `git status`.
- **Still reproducible.** The build is pinned to one exact commit, and that
  commit is recorded in the image.

`docker build -f toolchain/Dockerfile -t jppd-app-sdk .` still works and takes
the tip of `master`, but without the cache guarantee. Prefer the script.

The build is **multi-stage**: the builder stage runs a full firmware build on
`espressif/idf:v5.5.1`, then the final stage starts from `python:3.12-slim` and
copies in only what compiling an app actually needs. Expect a few minutes for
the builder stage; the resulting image is small.

### Why it is not 12 GB

`espressif/idf:v5.5.1` is ~12 GB, but almost none of it is needed to build an
app. Measured on the RTM firmware revision:

| Piece | In the IDF image | Shipped | Why |
|---|---|---|---|
| IDF sources | 2.9 GB | **27 MB** | only the 135 include dirs on the compile line |
| RISC-V toolchain | 2.0 GB | **180 MB** | see the prune below |
| xtensa toolchain | 1.1 GB | — | ESP32-C6 is RISC-V |
| gdb / qemu / cmake / openocd | ~550 MB | — | `jppd-build` invokes `gcc` directly |
| firmware `build/` | 266 MB | ~2 MB | only generated headers (`sdkconfig.h`, 8 micropython headers) |
| `compile_commands.json` | 6.6 MB | ~30 KB | distilled to `sdk-flags.json`; 1 of 1368 entries matters |

`capture_sysroot.py` performs the distillation in the builder stage: it resolves
the compiler and the full ordered `-I` list into `sdk-flags.json`, and packages
exactly those include directories — at their **absolute paths**, minus build
artefacts — into `sysroot.tar`. Preserving absolute paths means the recorded
flags resolve verbatim in the final image, with no rewriting to drift.

**The toolchain prune (2.0 GB → 180 MB)** deletes the multilib `*.a` archives,
`cc1plus`, `lto1`, and the C++ drivers. This is safe *by construction*: apps are
linked `-nostdlib -Wl,--allow-shlib-undefined`, and every libc/libgcc symbol
(`snprintf`, `__udivdi3`, …) is left undefined and resolved at load time from the
firmware's exported symbol table (`jpp_native_symtab.c`). Only the toolchain's
*headers* are needed, never its target libraries. Verified by compiling and
linking a PIC app object against the pruned toolchain.

## Build an app

From an app source directory (containing `manifest.json`):

```bash
# native C or MicroPython — auto-detected from manifest "app_type"
docker run --rm -v "$PWD:/app" jppd-app-sdk
```

Output lands in `./dist/<app_id>/` — the `.bin`/`.mpy` plus `manifest.json`,
ready to copy verbatim to `/sd/apps/<app_id>/` on the device SD card.

### Upload straight to a device

Pass the port through to the container with `--device`, then tell `jppd-build`
which port to use. The device shows a JPPD-SMP consent dialog — press **Allow**.

```bash
docker run --rm -v "$PWD:/app" --device /dev/ttyACM0 \
    jppd-app-sdk --upload /dev/ttyACM0
```

This shells out to the repo's [`deploy.py`](../deploy.py) in single-app mode.
For picking several apps at once, with port auto-discovery, run `deploy.py`
directly on the host instead — see the [root README](../README.md#deploying).

## App source layout

Minimum for a **native** app:

```
myapp/
├── manifest.json        # app_id, app_type "native", entry "myapp.bin", caps…
└── src/
    └── myapp.c          # exports jpp_app_entry(...) — all src/**/*.c are compiled
```

Minimum for a **MicroPython** app:

```
myapp/
├── manifest.json        # app_type "micropython", entry "main.mpy"
└── main.py
```

### Optional `jppd-app.json`

For apps that need shared helpers (e.g. the firmware's `apps/common/jpp_ble_msg.c`),
extra include dirs, or extra defines, drop a `jppd-app.json` next to
`manifest.json`:

```json
{
  "extra_sources": ["apps/common/jpp_ble_msg.c"],
  "includes":      ["vendor/include"],
  "defines":       ["MY_FLAG=1"]
}
```

`extra_sources` are resolved against both the app dir and the SDK root, so a
path like `apps/common/jpp_ble_msg.c` picks up the firmware's shared helper.
Every jpp component's public headers and the firmware's `apps/common/` are
already on the include path, so you never need to name which component a
`jpp_*` symbol lives in.

## CLI reference

```
jppd-build [--app-dir DIR] [--out DIR] [--upload PORT] [--no-validate] [--dry-run]
```

| Flag            | Meaning                                                        |
|-----------------|----------------------------------------------------------------|
| `--app-dir DIR` | App source dir (default `.` / the mounted `/app`)              |
| `--out DIR`     | Output root (default `<app-dir>/dist`)                         |
| `--upload PORT` | Upload to a device after building (needs `--device` on `run`) |
| `--no-validate` | Skip manifest validation                                       |
| `--dry-run`     | Print the compile/link plan without invoking the toolchain     |

## Running without Docker

`jppd-build` is a plain script. Point it at a local firmware build (one that has
already run `idf.py build`) and it works on the host, provided the riscv
toolchain / `mpy-cross` are on `PATH`:

```bash
JPPD_SDK_ROOT=../jppdos JPPD_SDK_BUILD=../jppdos/build \
    toolchain/jppd-build --app-dir apps/slots --dry-run
```

| Env var          | Default                    | Meaning                          |
|------------------|----------------------------|----------------------------------|
| `JPPD_SDK_ROOT`  | `/project`                 | Firmware repo root               |
| `JPPD_SDK_BUILD` | `$JPPD_SDK_ROOT/build`     | `idf.py build` output dir        |
| `JPPD_SDK_FLAGS` | `/opt/jppd-sdk/sdk-flags.json` | Distilled flags (image only) |
| `JPPD_DEPLOY`    | `/opt/jppd-sdk/deploy.py`  | Deploy tool used by `--upload`; falls back to this repo's `deploy.py` |

## Publishing to the App Hub

`.github/workflows/app-hub.yml` builds every app in `apps/` on a push to
`master`, signs each package, and uploads it to the `jppdos-apps/` prefix of the
OTA bucket together with the index a device reads to discover apps. App authors
never run any of this; it is here for whoever maintains the bucket.

The bucket is read over **plain HTTP** — the hardware is too weak for TLS — so
trust lives in the payload: every object `X` has a detached `X.sig` beside it,
verified against a public key compiled into the firmware.

**The wire format is not owned here.** Bucket layout, signature encoding and the
index *grammar* are one contract shared with the firmware's own OTA publisher,
specified in [`docs/ota-registry.md`][ota-registry] in the firmware repo and
read by firmware that ships on its own schedule; change it there first. What
this repository owns is the set of keys the App Hub index puts inside that
grammar, described under [The index](#the-index) below. `hub_publish.py` is the
App Hub half of that repo's `scripts/ota_publish.py` and deliberately duplicates
its key and index helpers; `tests/test_hub_index.py` pins the parts this side
owns.

| | |
|---|---|
| Signature | ECDSA P-256 (secp256r1) over SHA-256 |
| `.sig` contents | 64 raw bytes, `r ‖ s` big-endian — **not DER** |
| Public key in firmware | 64 raw bytes, `X ‖ Y` (uncompressed point, `0x04` stripped) |
| Index | `jppdos-apps/index`, line-oriented ASCII, one `[app_id]` section per app |

### The index

`jppdos-apps/index` is what a device fetches to find out what apps exist and
whether it already has the current version of each. It is not JSON: the parser
on the device is a fixed-size line buffer and a `strcmp`, and nothing here needs
nesting, escaping, or a tokeniser. The firmware's own `jppdos/latest` uses the
same grammar with one section per release channel; this one has a section per
app.

```text
jppdos-index 1                         ← magic and format version
generated 2026-09-04T09:23:55Z
commit 7a7266b273baf67727a577a889b2733768a40cfa
apps 2                                 ← number of sections that follow

[slots]                                ← the section name IS the app id
schema 2
version 2.0.0
name Slots
author evaqum
type native
sdk_min 1
sdk_max 1
commit 82b5bcb6007809f5e0f68934a66c3ffd2c3d8f96
date 2026-07-27T19:34:46Z
entry slots.bin
file manifest.json 212 116cb1e3b9394f8401d41287512a72771d127fff9074114cafc8638624dd2363
file slots.bin 39512 0a147510b9977f0bcbf83c2d238da4bf244f78107b2ac04dd154802cd37e1d7a

[mtproto]                              ← abridged; same keys as above
version 0.1.0
cap http.request                       ← one line per declared capability
cap network.connect
file mtproto.bin 12000 bb2b7baefbbb5ad0ec96e0230f4297ca703dcd9f61994ac5455e8994575b6a90
```

Header keys:

| Key | Meaning |
|---|---|
| `jppdos-index` | Format version, currently `1`. A device that does not recognise the number must stop, not guess. |
| `generated` | When the index was written, ISO 8601 UTC. Changes on every publish, since every push rebuilds every app. |
| `commit` | Full 40-character SHA of the `jppdos-apps` commit the build came from. |
| `apps` | Number of app sections in the file. |

App section keys — the section name is the `app_id`, which is also the directory
the app's files live in under the prefix (`jppdos-apps/<app_id>/`):

| Key | Meaning |
|---|---|
| `schema` | Manifest `schema_version`. |
| `version` | The app's version string — what a device compares against what it has installed. |
| `name`, `author` | For display. Folded to ASCII and capped at 96 characters; `name` falls back to the app id if nothing survives the fold. |
| `type` | `native` or `micropython`. |
| `sdk_min`, `sdk_max` | SDK API levels the app supports. A device filters on these before offering it. |
| `cap` | One line per declared capability, in manifest order. Absent when the app declares none. Repeated rather than comma-joined because the full set would run past the 128-byte line limit, and a silently truncated capability list is not something to hand a device. |
| `commit` | Full 40-character SHA of the last commit that touched `apps/<app_id>/`. Not the build: an app nobody has changed keeps its original commit across republishes. |
| `date` | That commit's timestamp, ISO 8601 UTC. |
| `entry` | The file the launcher loads, from the manifest. |
| `file` | One line per file in the package: **name, size in bytes, lowercase SHA-256**, space-separated. This is the one value with internal structure — an app package holds a variable number of files, which fixed per-file keys cannot express. |

Parsing rules, in full (the shared half of the contract — the spec is
[`docs/ota-registry.md`][ota-registry]):

- Lines are LF-terminated ASCII, under 128 bytes. `hub_publish.py` refuses to
  emit a line that breaks either rule rather than letting it fail on hardware.
- A blank line, or one whose first character is `#`, is skipped.
- `[name]` opens a section; everything after it belongs to that section until
  the next one. Lines before the first section are the header.
- Any other line is a **key**, one space, and the **rest of the line** as the
  value. Values are never quoted and never continue onto a second line.
- **Unknown keys and unknown sections must be ignored, not rejected.** This is
  the format's only forward-compatibility mechanism: it is how a newer publisher
  adds a field without bricking update checks on older firmware.
- Sections are sorted by app id and `file` lines by name, so two runs can be
  diffed. Only `generated` changes when nothing else has.
- Every `file` listed has a `<name>.sig` beside it in the bucket, and the index
  itself has `index.sig`. Signatures are never listed — the convention is that
  every object has one.

The digests are a corruption check for the download, not a security control: the
signature is what establishes trust. A device should check both.

**There is no `keygen` here.** One key signs both prefixes of the bucket; it is
generated once, in the firmware repo, with `scripts/ota_publish.py keygen`, and
it cannot be rotated without reflashing every fielded device over USB. This side
only ever consumes it, from the `OTA_SIGNING_KEY` secret.

Everything the workflow does is runnable by hand against a `dist/` tree, which
is how to debug a publish without pushing:

```bash
toolchain/hub_publish.py index-build --dist dist --repo . --out dist/index
toolchain/hub_publish.py sign   --key ota_seckey.pem $(find dist -type f ! -name '*.sig')
toolchain/hub_publish.py verify --pubkey ota_pubkey.pem $(find dist -type f ! -name '*.sig')

# confirm the bucket's key is the one the firmware trusts
toolchain/hub_publish.py pubkey-c --pubkey ota_pubkey.pem
```

Needs `cryptography` (`pip install cryptography`); it is not baked into the
build image, because signing happens in CI and not in an app build.

[ota-registry]: https://github.com/jppteam/jppdos/blob/master/docs/ota-registry.md
