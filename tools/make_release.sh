#!/bin/bash
# Builds the release archive release/dsiftp-<version>.zip:
# the ROM, the documentation and every licence text the binary needs.
# Usage: bash tools/make_release.sh <version>
set -e
cd "$(dirname "$0")/.."
VER="$1"
[ -n "$VER" ] || { echo "usage: $0 <version>"; exit 1; }
grep -q "APP_VERSION \"$VER\"" source/app.h || { echo "APP_VERSION in source/app.h is not $VER"; exit 1; }
source /opt/wonderful/bin/wf-env
export BLOCKSDS="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"

python3 -I tools/check_lang.py source
rm -f dsiftp.nds
bash build.sh > /dev/null

NAME="dsi-ftp-nand-sd-$VER"
OUT="release/$NAME"
rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT/licenses/blocksds"
cp dsiftp.dsi README.md LICENSE THIRD_PARTY_NOTICES.md "$OUT/"
cp -r docs "$OUT/"
cp -r licenses/fusion-pixel-font licenses/picolibc "$OUT/licenses/"
cp -r "$BLOCKSDS/licenses/." "$OUT/licenses/blocksds/"
cp "$BLOCKSDS"/libs/dswifi/COPYING* "$OUT/licenses/blocksds/"
(cd release && python3 -m zipfile -c "$NAME.zip" "$NAME")
ls -l "$OUT.zip"
find "$OUT" -type f | sort
