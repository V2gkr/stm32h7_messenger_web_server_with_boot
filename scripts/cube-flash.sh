#!/usr/bin/env bash
# Flash/erase the external OSPI NOR flash (MT25TL01G) on the STM32H750B-DISCO,
# via STM32CubeProgrammer's external-loader mechanism -- the CLI equivalent of
# STM32CubeIDE's "External Loader" flashing option. Uses the STM32_Programmer_CLI
# already managed by the STM32Cube VS Code extension under
# ~/.local/share/stm32cube/bundles (see scripts/cube-build.sh) -- no separate
# system-wide STM32CubeProgrammer install required.
#
# mode=UR (connect Under Reset) avoids hanging when the MCU tries to
# boot/execute from external flash that is currently blank/corrupt.
#
# On flash, we use "-g 0x90000000" (go: set SP/PC from the vector table at
# that address and start executing) instead of "-rst". "-rst" issues a real
# system reset, which re-runs the BootROM boot sequence driven by the
# BOOT_ADD0 option byte -- and since that option byte isn't set to point at
# the OSPI flash, the chip would fall back to whatever boot address it's
# actually configured for instead of the image we just wrote. "-g" starts
# execution directly, the same way STM32CubeIDE's Run/Debug does, without
# touching the boot address logic.
#
# Usage:
#   scripts/cube-flash.sh flash <firmware-debug|firmware-release>
#   scripts/cube-flash.sh erase
set -euo pipefail

: "${CUBE_BUNDLE_PATH:=$HOME/.local/share/stm32cube/bundles}"

# Picks the newest installed version of a given bundle, e.g.
# latest_bin programmer -> .../bundles/programmer/2.23.0/bin
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

CLI="$(latest_bin programmer)/STM32_Programmer_CLI"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIRMWARE_DIR="$REPO_ROOT/firmware"
LOADER="$FIRMWARE_DIR/MT25TL01G_STM32H750B-DISCO.stldr"
ELF_NAME="stm32h750_messenger_web_server.elf"

MODE="${1:?usage: $0 <flash|erase> [preset]}"

case "$MODE" in
    flash)
        PRESET="${2:?usage: $0 flash <firmware-debug|firmware-release>}"
        ELF="$FIRMWARE_DIR/build/$PRESET/$ELF_NAME"
        if [ ! -f "$ELF" ]; then
            echo "error: $ELF not found -- build it first (scripts/cube-build.sh firmware $PRESET)" >&2
            exit 1
        fi
        "$CLI" -c port=SWD mode=UR -el "$LOADER" -w "$ELF" -v -g 0x90000000
        ;;
    erase)
        "$CLI" -c port=SWD mode=UR -el "$LOADER" -e all
        ;;
    *)
        echo "usage: $0 <flash|erase> [preset]" >&2
        exit 1
        ;;
esac
