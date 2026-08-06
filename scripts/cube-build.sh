#!/usr/bin/env bash
# Configure + build one of this repo's STM32 projects using the toolchain
# bundle already managed by the STM32Cube VS Code extension (cmake, ninja,
# arm-none-eabi-gcc) - no separate system-wide install required.
#
# Usage: scripts/cube-build.sh <bootloader|firmware> <preset-name>
# Example: scripts/cube-build.sh bootloader bootloader-debug
set -euo pipefail

PROJECT="${1:?usage: $0 <bootloader|firmware> <preset>}"
PRESET="${2:?usage: $0 <bootloader|firmware> <preset>}"

: "${CUBE_BUNDLE_PATH:=$HOME/.local/share/stm32cube/bundles}"

# Picks the newest installed version of a given bundle, e.g.
# latest_bin cmake -> .../bundles/cmake/4.2.3+st.1/bin
latest_bin() {
    local dir
    dir=$(find "$CUBE_BUNDLE_PATH/$1" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)
    if [ -z "$dir" ]; then
        echo "error: bundle '$1' not found under $CUBE_BUNDLE_PATH" >&2
        echo "       install/update it via the STM32Cube VS Code extension (Bundles Manager)." >&2
        exit 1
    fi
    echo "$dir/bin"
}

export PATH="$(latest_bin cmake):$(latest_bin ninja):$(latest_bin gnu-tools-for-stm32):$PATH"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT/$PROJECT"

cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
