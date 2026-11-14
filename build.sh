#!/bin/bash
# Builds DSi FTP with BlocksDS (Wonderful toolchain), e.g. inside WSL:
#   wsl -- bash build.sh
# Set BLOCKSDS to use another BlocksDS tree.
#
# DSi FTP links against a patched DSWiFi (see patches/README.md), which
# tools/build_dswifi.sh builds once; the first build needs git and a network
# connection. STOCK_DSWIFI=1 uses the library of BlocksDS as it is instead
# (uploads are then about half as fast).
set -e
source /opt/wonderful/bin/wf-env
export BLOCKSDS="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"
cd "$(dirname "$0")"
if [ "$STOCK_DSWIFI" != 1 ]; then
    BLOCKSDS="$(bash tools/build_dswifi.sh)"
    export BLOCKSDS
fi
# The library is not a dependency of the ROM in the makefile: link again.
rm -f build/dsiftp.elf dsiftp.nds
make "$@"
cp -f dsiftp.nds dsiftp.dsi
ls -l dsiftp.dsi
