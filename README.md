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

Each project is a normal, independent CMake project with its own presets:

```sh
cmake --preset bootloader-debug && cmake --build --preset bootloader-debug
cmake --preset firmware-debug   && cmake --build --preset firmware-debug
```

The root `CMakePresets.json` just `include`s both projects' presets so all
four (`bootloader-debug/release`, `firmware-debug/release`) are visible
from one place; it doesn't merge them into a single build - they stay two
separate images with two separate memory maps, built and flashed
independently. Each build produces both a `.elf` and a raw `.bin`
(`bootloader.bin` / `stm32h750_messenger_web_server.bin`) - the bootloader
needs firmware's `.bin` to flash it into QSPI.

## First-time setup on a new machine

`Drivers/` and `Middlewares/` (the STM32Cube HAL and ST/third-party
middleware) are **not** committed - the vendor package is too large to be
worth versioning. Before building, open each project's `.ioc` file
(`bootloader/qspi_bootloader.ioc`, `firmware/stm32h750_messenger_web_server.ioc`)
in STM32CubeMX and run **Project → Generate Code**. If either folder is
missing, `cmake --preset ...` stops immediately with a reminder instead of
a confusing "no such file" error (see `cmake/check_generated_sources.cmake`).

## Editing both projects at once

Open [`stm32h7-messenger-with-boot.code-workspace`](stm32h7-messenger-with-boot.code-workspace)
in VS Code - it's a multi-root workspace with `bootloader/` and
`firmware/` as separate roots, each keeping its own CMake Tools
configuration (`.vscode/settings.json` per project). Switch the active
CMake project via the CMake Tools status bar / project picker.
