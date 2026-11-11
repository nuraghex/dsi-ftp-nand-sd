#!/bin/bash
# Builds DSi FTP with BlocksDS (Wonderful toolchain), e.g. inside WSL:
#   wsl -- bash build.sh
# Set BLOCKSDS to use another BlocksDS tree.
set -e
source /opt/wonderful/bin/wf-env
export BLOCKSDS="${BLOCKSDS:-/opt/wonderful/thirdparty/blocksds/core}"
cd "$(dirname "$0")"
make "$@"
cp -f dsiftp.nds dsiftp.dsi
ls -l dsiftp.dsi
