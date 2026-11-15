# SMEDIT debugger bridge

This fork keeps a small, optional debugger ABI for SMEDIT on top of the
libretro Snes9x core. Ordinary libretro execution remains unchanged when no
debug request is active, and clients must probe `smedit_debug_api_version`
before calling any other `smedit_debug_*` symbol.

## Scope

ABI version 1 provides:

- one-instruction stepping;
- bounded run-to-address and execution breakpoints;
- 65c816 CPU registers, flags, widths, timing, and halt state;
- side-effect-free reads from directly mapped memory and supported SRAM.

The bridge deliberately refuses CPU, PPU, and coprocessor reads that could
acknowledge or advance emulated hardware. All debugger calls run synchronously
on the same thread as `retro_run`.

The implementation is isolated in `smedit_debugger.{h,cpp}`, with two inactive
checks in `S9xMainLoop` and a thin exported ABI in `libretro/libretro.cpp`.
Keep unrelated emulator changes out of this branch so upstream rebases remain
reviewable.

## Build and verification

From this repository:

```sh
make -C libretro
```

From the SMEDIT parent repository:

```sh
./gradlew cleanLibretroCore buildLibretroCore
./gradlew :desktopApp:jvmTest \
  --tests 'com.supermetroid.editor.emulator.LibretroCpuDebuggerTest' --rerun
```

SMEDIT treats missing or incompatible debugger symbols as an unsupported
capability; stock libretro cores continue to run games without source-debugger
controls.

## Upstream maintenance

`smedit-debugger` is intentionally pinned to the Snes9x revision recorded by
the SMEDIT submodule. Update it through an explicit upstream merge or rebase,
then rebuild the core and rerun the native debugger tests before advancing the
parent repository's submodule pointer.
