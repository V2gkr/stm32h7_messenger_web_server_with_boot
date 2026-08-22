# STM32H750B-DK: bootloader + messenger web server firmware

Monorepo for the STM32H750B-DK project: a bootloader running from internal
flash, and the messenger/web-server application that executes XIP from
external QSPI flash. Both used to be separate repositories/CubeIDE
projects; they now live side by side here, each as its own independent
CMake project, with their original commit histories preserved.

```
bootloader/   internal flash, 0x08000000 - reads a .bin off FATFS (SD/USB)
              and writes it into external QSPI, then jumps to firmware/
firmware/     external QSPI, memory-mapped XIP at 0x90000000 - the
              messenger web server application (LwIP + FatFs + USB)
cmake/        shared toolchain file used by both projects
```

## Building

Each project is a normal, independent CMake project with its own presets.
The root `CMakePresets.json` just `include`s both projects' presets so all
four (`bootloader-debug/release`, `firmware-debug/release`) are visible
from one place; it doesn't merge them into a single build - they stay two
separate images with two separate memory maps, built and flashed
independently. Each build produces both a `.elf` and a raw `.bin`
(`bootloader.bin` / `stm32h750_messenger_web_server.bin`) - the bootloader
needs firmware's `.bin` to flash it into QSPI.

**No standalone `cmake`/`ninja`/`arm-none-eabi-gcc` install is required.**
The STM32Cube VS Code extension already downloads and manages that whole
toolchain under `~/.local/share/stm32cube/bundles` (`$CUBE_BUNDLE_PATH`).
[`scripts/cube-build.sh`](scripts/cube-build.sh) picks up the newest
installed cmake/ninja/gcc from that bundle directory and uses them to
configure+build a project, without touching anything system-wide:

```sh
scripts/cube-build.sh bootloader bootloader-debug
scripts/cube-build.sh firmware   firmware-debug
```

From inside VS Code (with the `stm32h7-messenger-with-boot.code-workspace`
open), the same thing is available as tasks - Terminal → Run Task →
"Build bootloader (Debug)" / "Build firmware (Debug)" / "Build both
(Debug)" (bound to the default build shortcut, Ctrl+Shift+B).

## First-time setup on a new machine

`Drivers/` and `Middlewares/` (the STM32Cube HAL and ST/third-party
middleware) are **not** committed - the vendor package is too large to be
worth versioning. Before building, open each project's `.ioc` file
(`bootloader/qspi_bootloader.ioc`, `firmware/stm32h750_messenger_web_server.ioc`)
in STM32CubeMX and run **Project → Generate Code**. If either folder is
missing, `cmake --preset ...` stops immediately with a reminder instead of
a confusing "no such file" error (see `cmake/check_generated_sources.cmake`).

## Web page content (eMMC card)

The firmware's httpd server doesn't compile HTML into the binary - it serves
files live off the eMMC card via FatFs (`Core/Src/fsdata.c`). The page
source is tracked in [`firmware/web_root/`](firmware/web_root/) for
reference, but getting it onto the board is a manual step: after flashing,
plug the board in as a USB mass-storage device and copy
`firmware/web_root/index.html` and `firmware/web_root/ack.txt` onto the
card's root. Editing the page only requires re-copying the file - no
rebuild/reflash needed.

## Editing both projects at once

Open [`stm32h7-messenger-with-boot.code-workspace`](stm32h7-messenger-with-boot.code-workspace)
in VS Code - it's a multi-root workspace with `bootloader/` and
`firmware/` as separate roots, each keeping its own CMake Tools
configuration (`.vscode/settings.json` per project). Switch the active
CMake project via the CMake Tools status bar / project picker.

## Hardware/firmware lessons learned

[`docs/hardware-lessons-learned.md`](docs/hardware-lessons-learned.md) -
the non-obvious RAM-domain, cache, DMA-concurrency, and CubeMX-generation
bugs this project has hit so far, with root causes and fixes. Worth
skimming before touching SDMMC, USB MSC, or LwIP/httpd code.
