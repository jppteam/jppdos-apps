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
