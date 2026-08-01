#!/usr/bin/env bash
#
# Verify every symbol the app imports is one the firmware actually exports.
#
# Native apps are separately-built shared ELFs: each external call is resolved at
# load time against jpp_native_symtab.c, and one missing name means the app does
# not start at all — JPP_NATIVE_LOADER_UNRESOLVED_SYM, with nothing on screen to
# say which symbol. Compiling cleanly proves nothing here, because the headers
# declare far more than the table exports (fgets is declared by stdio.h and is
# not in the table).
#
# So: compile every source, list the undefined symbols, and diff that against the
# table. Catching this on a laptop is worth a great deal more than catching it on
# a device with no debugger.
#
#   ./test/check_symbols.sh
#
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JPPDOS_DIR="${JPPDOS_DIR:-$APP_DIR/../../../jppdos}"
JPPDOS_REF="${JPPDOS_REF:-master}"
WORK="${TMPDIR:-/tmp}/mtproto-symcheck"
STUB="${TMPDIR:-/tmp}/mtproto-host-test/stub"

if [ ! -d "$STUB" ]; then
    echo "note: header stubs missing; run ./test/run.sh first" >&2
    exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK"

# The exported names, straight from the loader's table.
git -C "$JPPDOS_DIR" show \
    "$JPPDOS_REF:components/jpp_native_loader_core/src/jpp_native_symtab.c" \
    | grep -oE '\{ *"[A-Za-z_][A-Za-z0-9_]*"' \
    | grep -oE '"[A-Za-z_][A-Za-z0-9_]*"' | tr -d '"' | sort -u > "$WORK/exported.txt"

# Compile everything.
#
# The flags matter: -ffreestanding and -fno-builtin stop the host compiler
# turning a struct assignment into a memcpy call the source never wrote, and
# disabling fortify and the stack protector removes __*_chk and __stack_chk_*,
# which are macOS runtime artifacts the RISC-V build does not emit.
for src in "$APP_DIR"/src/*.c; do
    cc -c -std=c11 -Os -ffreestanding -fno-builtin \
       -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector \
       -I "$APP_DIR/src" -I "$STUB" \
       -o "$WORK/$(basename "$src" .c).o" "$src"
done

# Undefined symbols across all objects, minus anything the app defines itself.
# nm prints a "file.o:" header line per object, which is dropped by requiring a
# leading space (the symbol lines are indented) and a U/T/D/B type column.
nm -g "$WORK"/*.o 2>/dev/null | awk '$1 == "U" {print $2}' \
    | sed 's/^_//' | sort -u > "$WORK/undef.txt"
nm -g "$WORK"/*.o 2>/dev/null | awk 'NF >= 3 && $2 ~ /^[TDBRSC]$/ {print $3}' \
    | sed 's/^_//' | sort -u > "$WORK/defined.txt"
comm -23 "$WORK/undef.txt" "$WORK/defined.txt" > "$WORK/imports.raw"

# Drop host-toolchain artifacts. clang lowers a zeroing memset to bzero on macOS
# regardless of -fno-builtin; the RISC-V GCC in the SDK image emits memset, which
# is exported. Nothing here is written literally in the sources — verified with
# `grep -c bzero src/*.c`.
grep -vxE 'bzero|___bzero' "$WORK/imports.raw" > "$WORK/imports.txt" || true

missing=$(comm -23 "$WORK/imports.txt" "$WORK/exported.txt" || true)

echo "imports: $(wc -l < "$WORK/imports.txt" | tr -d ' ')"
if [ -n "$missing" ]; then
    echo
    echo "NOT EXPORTED BY THE FIRMWARE — the app would fail to load:"
    echo "$missing" | sed 's/^/  /'
    exit 1
fi
echo "all imported symbols are exported by the firmware"
