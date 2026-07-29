![Banner](banner.webp)

# JPPDOS Apps

This repository contains apps for  **J++Device Operating System**, plus the toolchain that builds and deploys them.

## Apps in this repo:

1. **Slots** by evaqum
   Native, ID: `slots`
   
   A three-reel fruit machine that takes the theatre seriously. Press CENTER to
   spin; any three matching symbols pay, and three sevens are the jackpot. The
   reels ease to a stop one at a time on a fullscreen 128×64 canvas, and when
   the first two land on a pair the third slows to a crawl to draw the moment
   out. Wins and jackpots set off particle bursts, showers of coins and flashes
   of inverted screen, backed by the buzzer and by the onboard RGB LED, which
   breathes amber while idle, pulses with each reel tick, beats red through a
   near miss and cycles colour on a jackpot. Your jackpot count is saved
   between sessions. Any key skips a celebration, CENTER during one re-spins
   straight away, and a long CENTER press exits.

2. **MTProto (skeleton)** by jppdos
   Native, ID: `mtproto`

   A partial Telegram MTProto client — a skeleton that measures the envelope
   rather than a usable messenger. It opens an abridged TCP transport, completes
   the PQ/Diffie-Hellman auth-key handshake, and frames MTProto 2.0 encrypted
   messages, all on the SDK's own crypto primitives so none of the AES or bignum
   code has to fit in the app pool. Handshake parsing is deliberately reduced to
   the fields the flow actually consumes. Its point is the footprint: about 11 KB
   of the 64 KB pool, which is what establishes that a real minimal client would
   fit. Requires firmware with `sdk_min: 2`.

-----

# Getting the apps

## Building from sources

Apps are built with the `jppd-app-sdk` Docker image, which bakes a full firmware
build as an SDK sysroot: the exact compiler, headers, and include paths of a
firmware release. **You do not need the firmware sources** — obtain a prebuilt
image and mount your app directory on it.

```bash
# from an app source directory (the one holding manifest.json)
cd apps/slots
docker run --rm -v "$PWD:/app" jppd-app-sdk
```

Native C and MicroPython apps are auto-detected from the manifest's `app_type`.
Output lands in `./dist/<app_id>/` — the `.bin` or `.mpy` plus `manifest.json`,
which is exactly what goes to `/sd/apps/<app_id>/` on the device. The manifest is
validated as part of the build, against the firmware's own validator.

To build every app in this repo into one `dist/` root at the repository root:

```bash
for app in apps/*/; do
    docker run --rm -v "$PWD/$app:/app" -v "$PWD/dist:/out" jppd-app-sdk --out /out
done
```

Per-app build tuning (extra sources, include dirs, defines) goes in an optional
`jppd-app.json` next to the manifest. Building the image itself, the full flag
reference, and running `jppd-build` without Docker are all covered in
[`toolchain/README.md`](toolchain/README.md).

## Deploying to a J++Device

`deploy.py` sends built apps to a device over **JPPD-SMP**, the firmware's binary
serial protocol, on the device's native USB-Serial-JTAG port. It finds the port
itself, lets you tick off which apps to send, and uploads the whole batch inside
a single session — so the device asks for consent **once**.

```bash
pip install pyserial
./deploy.py
```

```
Select apps to deploy  (dist/)
↑/↓ move · SPACE toggle · a all · ENTER confirm · q cancel

[x] slots              Slots            native       v1.0.0    2 files, 44.1 KB
[ ] mtproto            MTProto (skele…  native       v0.1.0    2 files, 11.8 KB
```

Then press **Allow** on the device when the consent dialog appears. Files land in
`/sd/apps/<app_id>/` and the app shows up in the launcher.

**Port autodiscovery.** Ports presenting the Espressif USB-Serial-JTAG ID
(`303a:1001`) are recognised as J++Devices; if exactly one is attached it is used
without asking. Otherwise every serial port is offered in a picker, likely
devices first. On macOS the redundant `/dev/tty.*` twin of each port is hidden.

### Non-interactive use

Everything the TUI does is also reachable from flags, for scripts and CI:

```bash
./deploy.py slots                             # one app, auto-discovered port
./deploy.py slots mtproto                     # several
./deploy.py --all --port /dev/ttyACM0         # everything under dist/
./deploy.py --dist build/apps --all           # a different build output root
./deploy.py --list                            # show deployable apps + ports, exit
```

| Flag | Meaning |
|---|---|
| `--dist DIR` | Build output root holding `<app_id>/` dirs (default `dist`) |
| `--port PORT` | Serial port (default: auto-discover, ask if ambiguous) |
| `--all` | Deploy every app found under `--dist` |
| `--list` | List deployable apps and serial ports, then exit |

Set `JPPD_SMP_DEBUG=1` to echo the device's interleaved `ESP_LOG` output to
stderr while a command runs — useful when an upload fails on the device side.

The build image can also upload directly, one app at a time, if you pass the
port through to the container:

```bash
docker run --rm -v "$PWD:/app" --device /dev/ttyACM0 \
    jppd-app-sdk --upload /dev/ttyACM0
```

### Deploying by hand

`deploy.py` is a convenience, not a requirement. A `dist/<app_id>/` directory is
a verbatim copy of what belongs at `/sd/apps/<app_id>/`, so pulling the SD card
and copying the directory across works just as well — as does the device's
built-in WebDAV server (`Settings → WebDAV server`).

---

# Making new apps

## Examples and boilerplates

**The reference corpus lives in the firmware repo, not here.** Example apps sit
under `apps/` in the [firmware repo][fw-repo], because they are rebuilt on every
firmware build and so cannot fall behind the SDK surface they demonstrate. Start
by copying whichever matches what you want to write:

| App | ID | Type | What it is |
|---|---|---|---|
| SDK Test (C) | `testapp_native` | native | Menu-driven exercise of every App SDK capability — storage, KV, UI helpers, canvas, buzzer, LED, HTTP, BLE, ESP-NOW, background tasks. The reference for what a native app can do, and the closest thing to a native boilerplate. |
| SDK Test (MP) | `testapp_mp` | micropython | The same coverage from MicroPython, through the `jppsdk` module. |
| Games | `games` | native | A hub that pages game modules into the app pool on demand — the reference for code modules. |
| DemoScene | `demoscene` | native | Fullscreen canvas effects, integer-only maths, async buzzer music. |
| MeetApp | `meetapp` | native | BLE contact exchange with Ed25519 identity and multi-party signing. |

The two SDK test apps declare the full capability set, so launching them
exercises the permission-prompt flow as well. That is deliberate for a test app
and a bad default for a real one — declare only what you actually call (see
below).

The apps *in this repo* (`slots`, `mtproto`) are finished standalone apps rather
than SDK demonstrations, and both are native — for a MicroPython starting point
use `testapp_mp` from the firmware repo. Copying one of these works too; they
are laid out exactly as this repo expects a new app to be.

## App SDK documentation

Published at **<https://jppdevice.by.m4l3vi.ch/sdk-docs/>** — searchable, and
rebuilt from the firmware repo's `docs/` tree on every change to it.

| Doc | |
|---|---|
| Platform overview — sandbox model, capability tiers, app lifecycle | [Read][docs-index] |
| Every `jpp_sdk_*` call | [SDK reference][docs-sdk] |
| `manifest.json` fields and capability names | [Manifest schema][docs-manifest] |
| Writing a native C app | [Native C guide][docs-native] |
| Paging a second ELF in at runtime | [Code modules][docs-modules] |
| Writing a MicroPython app | [MicroPython guide][docs-mp] |
| What `deploy.py` speaks | [Serial protocol][docs-serial] |

The site tracks the firmware's `master`, which is also what the SDK image is
built from — so the two match. For docs pinned to an older image, read the
Markdown under `docs/` in a [firmware repo][fw-repo] checkout at the revision
that image records (`docker run --rm --entrypoint cat jppd-app-sdk
/opt/jppd-sdk/firmware-rev`).

## Adding a new app

**Put it in `apps/<app_id>/`**, one directory per app, with the directory name
matching the manifest's `app_id`. That is the only placement rule — the build
takes one app directory at a time, so nothing else needs registering, and there
is no top-level list of apps to update.

A native app:

```
apps/my_app/
├── manifest.json        # app_id "my_app", app_type "native", entry "my_app.bin"
├── jppd-app.json        # optional: extra sources / includes / defines
├── include/             # optional: your own headers
└── src/
    └── my_app.c         # exports jpp_app_entry(...) — all src/**/*.c are compiled
```

A MicroPython app:

```
apps/my_app/
├── manifest.json        # app_id "my_app", app_type "micropython", entry "main.mpy"
└── main.py
```

Then, from that directory:

```bash
docker run --rm -v "$PWD:/app" jppd-app-sdk
```

A few things worth getting right before you open a PR:

- **`app_id` must match the directory name**, and must not be one of the ids the
  firmware reserves for its own screens — `launcher`, `settings`, `webdav`,
  `webdav_passconfig`, `shell`, `dialog`, `app_crash`, `sd_ejected`. It is also
  the name of your storage roots on the SD card, `/sd/apps/<app_id>/` and
  `/sd/shared/<app_id>/`.
- **Declare only the capabilities you call.** Tier-1 caps (`http.request`,
  `https.request`, `ble.scan`, `ble.advertise`, `esp_now`, `background.register`)
  prompt once and persist; tier-2 (`files.full`, `network.bind`,
  `network.connect`, `ble.connect`, `ble.host`) prompt on first use every launch.
  Two of them prompt a second time, scoped to the resource rather than the
  capability: `files.full` asks per path, and `https.request` asks per origin
  (that one persists, so a given host is approved once and never again).
  Undeclared caps are refused outright, and a large chunk of the SDK — storage,
  KV, IPC, UI, canvas, buzzer, LED — needs no declaration at all. The full tier
  table is in [`docs/manifest.md`][docs-manifest].
- **The manifest is validated on every build** by the firmware's own validator,
  so a schema mistake fails the build rather than the device.
- **Test on hardware, not just on a build.** Build, `./deploy.py`, and confirm
  the app appears in the launcher and exits cleanly back to it.

Shared app-side helpers (for example `jpp_ble_msg`, which chunks BLE payloads
over the 512-byte GATT limit) live in the firmware's `apps/common/` and are
already on the include path; pull one in via `extra_sources` in `jppd-app.json`.

### Building the SDK image

Only needed when the firmware's SDK surface changes and the image has to be
rebuilt against it — app authors never do this. It requires the firmware
sources, but **this repository does not vendor them**: the Dockerfile clones
them itself, so there is nothing to check out or keep in step.

```bash
toolchain/build-image.sh
```

That resolves the current tip of the firmware's `master` and builds against it —
there is no pinned revision in git to move first. `JPPDOS_REF` builds against a
specific branch, tag, or commit instead; the resolved revision is baked into the
image at `/opt/jppd-sdk/firmware-rev`. The SDK surface changes between firmware
revisions, so rerun it whenever you want the image to catch up. Details,
including why the image is ~340 MB rather than ~12 GB, are in
[`toolchain/README.md`](toolchain/README.md).


<!-- The docs site is published from the firmware repo's docs/ tree by its
     .github/workflows/docs.yml, which syncs the MkDocs build to the sdk-docs/
     prefix of the docs bucket. Being a plain hosted URL, it is the same link
     whichever git mirror the reader came from — unlike a forge link, which
     could only ever be correct for one of them. [fw-repo] is GitHub because
     it has to pick one; the internal mirror is
     https://git.nova.tokyo/jppdevice/jppdos. -->

[fw-repo]:       https://github.com/jppteam/jppdos
[docs-index]:    https://jppdevice.by.m4l3vi.ch/sdk-docs/
[docs-sdk]:      https://jppdevice.by.m4l3vi.ch/sdk-docs/sdk-reference/
[docs-manifest]: https://jppdevice.by.m4l3vi.ch/sdk-docs/manifest/
[docs-native]:   https://jppdevice.by.m4l3vi.ch/sdk-docs/native/getting-started/
[docs-modules]:  https://jppdevice.by.m4l3vi.ch/sdk-docs/native/modules/
[docs-mp]:       https://jppdevice.by.m4l3vi.ch/sdk-docs/micropython/getting-started/
[docs-serial]:   https://jppdevice.by.m4l3vi.ch/sdk-docs/serial-protocol/
