#!/usr/bin/env bash
#
# Run the host tests for the MTProto client.
#
# The protocol code is mostly pure computation, and a mistake in it is invisible
# on-device — a wrong TL padding rule or key fingerprint just yields "connection
# failed" with nothing to inspect. This builds those modules for the host and
# exercises them against known-answer vectors.
#
# SDK headers are taken from a firmware checkout rather than vendored here, so
# the tests compile against the same struct definitions the device build will.
# Point JPPDOS_DIR at it if it is not the default sibling directory, and
# JPPDOS_REF at the ref to check against (SDK v2 is on master as of v1.1).
#
#   ./test/run.sh
#   JPPDOS_DIR=~/src/jppdos JPPDOS_REF=develop ./test/run.sh

set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JPPDOS_DIR="${JPPDOS_DIR:-$APP_DIR/../../../jppdos}"
JPPDOS_REF="${JPPDOS_REF:-master}"
WORK="${TMPDIR:-/tmp}/mtproto-host-test"

if [ ! -d "$JPPDOS_DIR/.git" ]; then
    echo "error: no firmware checkout at $JPPDOS_DIR" >&2
    echo "       set JPPDOS_DIR to a jppdos clone" >&2
    exit 1
fi

STUB="$WORK/stub"
rm -rf "$WORK"
mkdir -p "$STUB/freertos"

# Real SDK headers, so the tests see the same layouts as the firmware.
for header in $(git -C "$JPPDOS_DIR" ls-tree -r --name-only "$JPPDOS_REF" \
                    components/jpp_core/include/ components/jpp_crypto_core/include/); do
    git -C "$JPPDOS_DIR" show "$JPPDOS_REF:$header" > "$STUB/$(basename "$header")"
done

# FreeRTOS is the only thing that has to be faked: the app uses it just for the
# tick counter and vTaskDelay, both trivial to stand in for.
cat > "$STUB/freertos/FreeRTOS.h" <<'EOF'
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 10u
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / portTICK_PERIOD_MS))
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
EOF
printf '#pragma once\n#include "freertos/FreeRTOS.h"\n' > "$STUB/freertos/task.h"
printf '#pragma once\ntypedef void *QueueHandle_t;\n'   > "$STUB/freertos/queue.h"

CFLAGS=(-std=c11 -g -Wall -Wextra -Wno-unused-parameter
        -I "$APP_DIR/test" -I "$APP_DIR/src" -I "$APP_DIR/include" -I "$STUB")

# Step 1: every source must at least compile cleanly, including the ones with
# too much SDK entanglement to unit test yet. This is the cheapest way to catch
# a mistake while the device toolchain is unavailable.
echo "== syntax check =="
status=0
for src in "$APP_DIR"/src/*.c; do
    if out=$(cc -fsyntax-only "${CFLAGS[@]}" "$src" 2>&1) && [ -z "$out" ]; then
        echo "  ok   $(basename "$src")"
    else
        echo "  FAIL $(basename "$src")"
        echo "$out"
        status=1
    fi
done
[ "$status" -eq 0 ] || exit 1

# Step 2: the actual tests. Only modules free of socket and UI dependencies are
# linked in; host_crypto.c supplies the jpp_crypto_* primitives.
echo
echo "== host tests =="
cc "${CFLAGS[@]}" \
   "$APP_DIR/test/host_test.c" "$APP_DIR/test/host_crypto.c" \
   "$APP_DIR/src/mtp_common.c" "$APP_DIR/src/mtp_tl.c" \
   "$APP_DIR/src/mtp_pq.c" "$APP_DIR/src/mtp_config.c" \
   "$APP_DIR/src/mtp_gzip.c" \
   "$APP_DIR/src/ui_font.c" "$APP_DIR/src/ui_font_data.c" "$APP_DIR/src/ui_gfx.c" \
   "$APP_DIR/src/ui_icons_data.c" \
   "$APP_DIR/src/mtp_skip.c" "$APP_DIR/src/mtp_skip_data.c" \
   "$APP_DIR/src/mtp_srp.c" "$APP_DIR/src/mtp_scratch.c" \
   "$APP_DIR/src/mtp_mem.c" -lz \
   -o "$WORK/mtp_test"
"$WORK/mtp_test"

# Step 3: every symbol the app imports must be one the firmware exports. Native
# apps resolve externals against a fixed table at load time, so a name that only
# exists in a header (fgets, for one) means the app never starts.
echo
echo "== symbol check =="
JPPDOS_DIR="$JPPDOS_DIR" JPPDOS_REF="$JPPDOS_REF" "$APP_DIR/test/check_symbols.sh"
