#!/usr/bin/env sh
# =============================================================================
# build-image.sh — build the jppd-app-sdk image against the current firmware.
#
# Replaces the old `git submodule update --remote && docker build` dance. There
# is no firmware checkout in this repository: this script resolves the tip of
# the firmware ref to a commit and hands it to the Dockerfile, which clones it
# in the builder stage.
#
# Resolving the commit here rather than letting the Dockerfile clone a moving
# ref is what makes "always latest" actually work: the resolved SHA is passed in
# as a build arg, so Docker's layer cache invalidates the moment the ref moves,
# and the image is still pinned to one exact revision.
#
# Run from the repository ROOT (the whole repo is the build context):
#
#     toolchain/build-image.sh                       # tip of firmware master
#     JPPDOS_REF=1.0-RTM toolchain/build-image.sh    # a tag, branch, or commit
#     IMAGE_TAG=jppd-app-sdk:test toolchain/build-image.sh
#     toolchain/build-image.sh --no-cache            # extra args go to docker build
#
# Env:
#   JPPDOS_REPO  firmware git URL   (default: the public GitHub mirror)
#   JPPDOS_REF   branch/tag/commit  (default: master)
#   IMAGE_TAG    image tag          (default: jppd-app-sdk)
# =============================================================================
set -eu

REPO="${JPPDOS_REPO:-https://github.com/jppteam/jppdos.git}"
REF="${JPPDOS_REF:-master}"
IMAGE_TAG="${IMAGE_TAG:-jppd-app-sdk}"

if [ ! -f toolchain/Dockerfile ]; then
    echo "error: run this from the repository root" >&2
    exit 1
fi

# Resolve the ref to a commit. An annotated tag lists twice — the tag object and
# the peeled `^{}` commit — and it is the commit we want, so prefer that line. A
# ref that is already a raw SHA matches nothing and is used verbatim.
LS=$(git ls-remote "$REPO" "$REF" "refs/tags/$REF^{}" 2>/dev/null || true)
REV=$(printf '%s\n' "$LS" | grep '\^{}$' | head -1 | cut -f1)
[ -n "$REV" ] || REV=$(printf '%s\n' "$LS" | head -1 | cut -f1)
if [ -z "$REV" ]; then
    case "$REF" in
        [0-9a-f]*) REV="$REF" ;;
        *) echo "error: cannot resolve '$REF' in $REPO" >&2; exit 1 ;;
    esac
fi

echo "firmware repo: $REPO"
echo "firmware ref:  $REF -> $REV"
echo "image tag:     $IMAGE_TAG"
echo

exec docker build \
    -f toolchain/Dockerfile \
    --build-arg "JPPDOS_REPO=$REPO" \
    --build-arg "JPPDOS_REF=$REF" \
    --build-arg "JPPDOS_REV=$REV" \
    -t "$IMAGE_TAG" \
    "$@" \
    .
