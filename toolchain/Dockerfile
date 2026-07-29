# =============================================================================
# jppd-app-sdk — self-contained build image for J++Device (JPPDOS) apps.
#
# Mount an app source dir at /app and `jppd-build` compiles it against the exact
# toolchain, headers, and include paths this firmware release was built with.
#
# App developers do NOT need this file: pull (or receive) a prebuilt image and
# go straight to `docker run`. Only whoever *publishes* the image builds it, and
# that build needs the firmware sources — the sysroot is distilled from a real
# firmware build (compile_commands.json + generated headers such as sdkconfig.h),
# which cannot be synthesised without one.
#
# Those sources are CLONED AT IMAGE-BUILD TIME, not vendored: this repository has
# no submodule and no firmware checkout. The SDK has to track the firmware's
# master, and a submodule pointer only tracks it as often as somebody remembers
# to move it. Use the wrapper, which resolves the tip for you:
#
#     toolchain/build-image.sh                      # tip of firmware master
#     JPPDOS_REF=1.0-RTM toolchain/build-image.sh   # a specific tag/branch/commit
#
# The image is still VERSION-LOCKED — to whatever revision it was built from,
# recorded in /opt/jppd-sdk/firmware-rev inside the image and in the
# org.jppdevice.firmware-rev label. It is just locked at build time rather than
# in git.
#
# --- Why multi-stage -------------------------------------------------------
# espressif/idf:v5.5.1 is ~12 GB (2.9 GB IDF sources, 3.6 GB of toolchains for
# every ESP target, plus gdb/qemu/cmake). Compiling an app actually needs only a
# RISC-V compiler and ~28 MB of headers. So the builder stage does a full
# firmware build, capture_sysroot.py distills the include closure, and the final
# stage starts from python-slim and copies in just the compiler + those headers.
#
# Build (from the repository ROOT, which is the build context):
#     toolchain/build-image.sh
#
# Use (from an app source directory):
#     docker run --rm -v "$PWD:/app" jppd-app-sdk
#     docker run --rm -v "$PWD:/app" --device /dev/ttyACM0 \
#         jppd-app-sdk --upload /dev/ttyACM0
# =============================================================================

# -----------------------------------------------------------------------------
# Stage 1 — builder: full firmware build, then distill the sysroot.
# Nothing from this stage ships except sysroot.tar, sdk-flags.json, mpy-cross,
# and the RISC-V toolchain.
# -----------------------------------------------------------------------------
FROM espressif/idf:v5.5.1 AS builder

# mpy-cross 1.28.0 (MicroPython app bytecode compiler) — a single small binary.
RUN git clone --depth 1 --branch v1.28.0 \
        https://github.com/micropython/micropython.git /tmp/micropython \
    && make -C /tmp/micropython/mpy-cross \
    && cp /tmp/micropython/mpy-cross/build/mpy-cross /usr/local/bin/mpy-cross \
    && rm -rf /tmp/micropython

# The firmware checkout, cloned rather than vendored (see the header).
#
# JPPDOS_REV is the commit build-image.sh resolved from JPPDOS_REF just before
# invoking docker. Passing it explicitly does two jobs at once: it pins the
# build to one exact revision even though the ref moves, and — because the ARG
# value changes whenever the ref moves — it invalidates the layer cache by
# itself, so "latest" never silently serves a stale clone. Building without it
# takes the tip of JPPDOS_REF and may hit a cached layer.
ARG JPPDOS_REPO=https://github.com/jppteam/jppdos.git
ARG JPPDOS_REF=master
ARG JPPDOS_REV=
# The clone deliberately takes no --branch: a blobless clone of every ref is
# cheap and lets the checkout below accept a branch, a tag, or a raw commit
# SHA uniformly (--branch would reject the last).
RUN git clone --filter=blob:none "$JPPDOS_REPO" /project \
    && git -C /project checkout --detach "${JPPDOS_REV:-$JPPDOS_REF}" \
    && git -C /project submodule update --init --recursive \
    && { echo "repo:    $JPPDOS_REPO"; \
         echo "ref:     $JPPDOS_REF"; \
         echo "rev:     $(git -C /project rev-parse HEAD)"; \
         echo "describe:$(git -C /project describe --tags --always)"; \
         echo "built:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"; } > /firmware-rev \
    && cat /firmware-rev

COPY toolchain/capture_sysroot.py /project/tools/capture_sysroot.py
WORKDIR /project
RUN . "$IDF_PATH/export.sh" \
    && idf.py set-target esp32c6 \
    && idf.py build

# Distill: resolved compiler + full -I list, and the include closure as a tar.
RUN python3 /project/tools/capture_sysroot.py \
        /project/build /project /sysroot-out

# Prune the RISC-V toolchain. Apps link with -nostdlib and resolve every libc /
# libgcc symbol at load time from the firmware's exported symbol table (see
# jpp_native_symtab.c), so the target static libraries are never linked against
# — only their headers are needed. Dropping the multilib .a archives is the
# single biggest saving; C++ and LTO backends are unused too.
RUN TC=$(ls -d /opt/esp/tools/riscv32-esp-elf/*/riscv32-esp-elf) \
    && find "$TC" -name '*.a' -delete \
    && find "$TC" -name 'cc1plus' -delete \
    && find "$TC" -name 'lto1' -delete \
    && find "$TC" -name 'lto-dump*' -delete \
    && rm -f "$TC"/bin/*g++ "$TC"/bin/*c++ \
    && du -sh "$TC"

# -----------------------------------------------------------------------------
# Stage 2 — runtime: slim base + compiler + headers only.
# -----------------------------------------------------------------------------
FROM python:3.12-slim

# pyserial powers --upload (JPPD-SMP over USB-Serial-JTAG).
RUN pip install --no-cache-dir pyserial

# The RISC-V toolchain, at its original path so sdk-flags.json resolves verbatim.
COPY --from=builder /opt/esp/tools/riscv32-esp-elf /opt/esp/tools/riscv32-esp-elf

# The include closure, restored to absolute paths (/opt/esp/idf/..., /project/...).
COPY --from=builder /sysroot-out/sysroot.tar /tmp/sysroot.tar
RUN tar -xf /tmp/sysroot.tar -C / && rm /tmp/sysroot.tar

# The distilled flags — replaces shipping the 6.6 MB compile_commands.json.
COPY --from=builder /sysroot-out/sdk-flags.json /opt/jppd-sdk/sdk-flags.json

# mpy-cross for MicroPython apps.
COPY --from=builder /usr/local/bin/mpy-cross /usr/local/bin/mpy-cross

# The builder script, the firmware's manifest validator, and this repo's own
# deploy tool (jppd-build shells out to it for --upload).
COPY toolchain/jppd-build /usr/local/bin/jppd-build
COPY --from=builder /project/tests/validate_manifests.py /project/tests/validate_manifests.py
COPY deploy.py /opt/jppd-sdk/deploy.py
RUN chmod 0755 /usr/local/bin/jppd-build

# Which firmware revision this image was built from. The submodule used to
# record that in git; now the image records it about itself.
COPY --from=builder /firmware-rev /opt/jppd-sdk/firmware-rev
ARG JPPDOS_REV=
LABEL org.jppdevice.firmware-rev=$JPPDOS_REV

ENV JPPD_SDK_ROOT=/project
ENV JPPD_SDK_FLAGS=/opt/jppd-sdk/sdk-flags.json
ENV JPPD_DEPLOY=/opt/jppd-sdk/deploy.py

WORKDIR /app
ENTRYPOINT ["jppd-build"]
