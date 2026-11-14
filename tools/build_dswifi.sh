#!/bin/bash
# Builds the DSWiFi that DSi FTP links against: the one of BlocksDS with
# patches/dswifi-<version>-dsiftp.patch applied (see patches/README.md), and
# the ARM7 cores that contain it.
#
# The result is a BlocksDS tree made of links to the installed one, except for
# libs/dswifi and sys/arm7/main_core. The installed BlocksDS is not modified.
# Progress goes to stderr; the only line on stdout is the path of that tree,
# to be used as BLOCKSDS. Nothing is rebuilt if the patch has not changed.
#
#   BLOCKSDS      installed BlocksDS (default: the Wonderful toolchain one)
#   DSIFTP_CACHE  where the source and the tree are kept (default: ~/.cache/dsiftp)
set -e
cd "$(dirname "$0")/.."

SDK_VER=1.25.0
PATCH="$PWD/patches/dswifi-$SDK_VER-dsiftp.patch"
BASE="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"
CACHE="${DSIFTP_CACHE:-$HOME/.cache/dsiftp}"
SRC="$CACHE/sdk-$SDK_VER"
OUT="$CACHE/blocksds-$SDK_VER-dsiftp"

if ! grep -q "v$SDK_VER" "$BASE/version.txt" 2>/dev/null; then
    echo "build_dswifi: $BASE is not BlocksDS $SDK_VER (the patch is for that version)" >&2
    exit 1
fi

SUM="$(sha256sum "$PATCH" | cut -d' ' -f1) $BASE"
if [ "$(cat "$OUT/.dsiftp-patch" 2>/dev/null)" = "$SUM" ]; then
    echo "$OUT"
    exit 0
fi

{
    source /opt/wonderful/bin/wf-env

    echo "build_dswifi: getting the BlocksDS $SDK_VER source"
    mkdir -p "$CACHE"
    if [ ! -d "$SRC/.git" ]; then
        git -c advice.detachedHead=false clone -q --branch "v$SDK_VER" --depth 1 \
            https://codeberg.org/blocksds/sdk.git "$SRC"
    fi
    git -C "$SRC" submodule update -q --init --depth 1 libs/dswifi
    git -C "$SRC/libs/dswifi" submodule update -q --init --depth 1
    git -C "$SRC/libs/dswifi" checkout -q -- source
    git -C "$SRC/libs/dswifi" apply "$PATCH"

    echo "build_dswifi: preparing $OUT"
    rm -rf "$OUT"
    mkdir -p "$OUT/libs" "$OUT/sys/arm7/main_core"
    for e in "$BASE"/*; do
        n="$(basename "$e")"
        [ "$n" = libs ] || [ "$n" = sys ] || ln -s "$e" "$OUT/$n"
    done
    for e in "$BASE"/libs/*; do
        [ "$(basename "$e")" = dswifi ] || ln -s "$e" "$OUT/libs/"
    done
    for e in "$BASE"/sys/*; do
        [ "$(basename "$e")" = arm7 ] || ln -s "$e" "$OUT/sys/"
    done
    for e in "$BASE"/sys/arm7/*; do
        [ "$(basename "$e")" = main_core ] || ln -s "$e" "$OUT/sys/arm7/"
    done

    echo "build_dswifi: building DSWiFi"
    make -C "$SRC/libs/dswifi" clean
    BLOCKSDS="$BASE" make -C "$SRC/libs/dswifi" -j"$(nproc)"
    BLOCKSDS="$BASE" make -C "$SRC/libs/dswifi" install INSTALLDIR="$OUT/libs/dswifi"

    echo "build_dswifi: building the ARM7 cores"
    cd "$SRC/sys/arm7/main_core"
    rm -rf build ./*.elf
    BLOCKSDS="$OUT" make dswifi -j"$(nproc)"
    cp arm7_dswifi.elf arm7_dswifi_debug.elf "$OUT/sys/arm7/main_core/"

    echo "$SUM" > "$OUT/.dsiftp-patch"
} >&2

echo "$OUT"
