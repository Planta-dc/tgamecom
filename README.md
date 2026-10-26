# Independent Tiger Game.com emulator — development workspace

## Goal

Build a standalone Game.com emulator with its own portable CPU, bus/MMU, firmware and cartridge loaders, peripherals, SDL2-capable display/input, and audio. The finished emulator must not link to, launch, or require MAME. MAME is a coding reference only; the standalone SM8521 CPU operation table in this project is adapted from MAME's BSD-3-Clause source and retains its attribution/license notice.

## Current status

The project has a standalone C++ SM8521 interpreter, a separate bus model, reset/boot-ROM overlay, external firmware and cartridge bank mapping, MMU switching, ROM-to-VRAM DMA, basic timer/clock handling, and periodic interrupt inputs. The CPU uses the adapted MAME operation table but does not call or link to MAME.

The supplied firmware reaches the built-in OS/PDA menu after a 20-million-step run. With Centipede loaded, a scripted touch launches the cartridge; after its title and main menu, scheduled A/D-pad inputs advance to the active playfield. In a short repeatable run, left/right input moves the shooter across the screen and A produces a visible shot (`demos/centipede-gameplay-left.png`, `demos/centipede-gameplay-right.png`, `demos/centipede-gameplay-fire.png`). This is initial scripted gameplay/control verification, not proof of a long playable session or hardware accuracy. The 200×160 LCD renderer applies MAME-reference VRAM page selection, LCDC enable, four gradation palettes, and Game.com LCD colors. Active-low controller groups and an approximate 13×10 stylus grid are implemented; physical touch-coordinate mapping is not hardware-confirmed. An initial sound path synthesizes the two 4-bit waveform channels and direct DAC output, then sends PCM to a bundled miniaudio backend in the SDL frontend. Sound timing is sampled at frame boundaries; retail audio fidelity and SG2 remain unverified. Scanline timing/races and analog LCD response are also incomplete.

The C++ CPU and SDL frontend are independent of MAME. DMA progresses during HALT wait cycles, matching the reference CPU callback order. The SDL frontend runs the standalone C++ CPU, supports BIOS/cartridge boot, and is frame-paced at the reference LCD cadence. `demos/bios-tiger-logo.png` shows the firmware splash; `demos/bios-os-menu.png` shows the built-in OS menu; `demos/bios-centipede-title.png` shows the cartridge title; `demos/centipede-main-menu.png` and `demos/centipede-gameplay-final.png` show the game menu and active playfield.

## Build and run

No MAME installation is needed for these commands:

```sh
make
make test
./gamecom-standalone-demo
./gamecom-debug
./gamecom-boot firmware - 20000000 boot-frame.ppm
make sdl                         # optional; needs SDL2 development files
./gamecom-sdl --firmware firmware # interactive BIOS/LCD window
# or: ./gamecom-sdl --demo       # standalone VRAM/LCD test
```

Linux executables are fully statically linked by default with GNU Make/GCC; `ldd` reports them as not dynamically linked. This avoids the `GLIBCXX_3.4.32` runtime dependency and also avoids shared glibc version requirements. Static libc/C++ development archives are needed to build them. If those archives are unavailable, build dynamically with `make PORTABLE_LDFLAGS=`; CMake has the equivalent `-DGAMECOM_STATIC_LINUX_EXECUTABLES=OFF` option. `gamecom-test-standalone` is a test binary, not required to run the emulator.

`gamecom-boot` takes a firmware directory, an optional raw cartridge `.bin` path (use `-` for none), an emulation-step budget (default 20,000,000), and an optional PPM path. Scripted inputs use `--touch step column row duration` and repeatable `--button step name duration` events; button names are `up`, `down`, `left`, `right`, `menu`, `pause`, `sound`, `a`, `b`, `c`, `d`, and `power`. The touch grid is approximate and event times are CPU steps. A menu/title probe is:

```sh
./gamecom-boot firmware /path/to/Centipede.bin 20000000 centipede-title.ppm \
  --touch 10000000 2 3 300000
```

This scripted run proceeds to Centipede gameplay and exercises left/right movement and A fire:

```sh
./gamecom-boot firmware /path/to/Centipede.bin 70000000 gameplay.ppm \
  --touch 10000000 2 3 300000 \
  --button 41000000 a 300000 \
  --button 60000000 left 1000000 \
  --button 61000000 a 300000 \
  --button 67000000 right 2000000 \
  --button 69000000 a 1000000
```

Do not pass the BIOS ZIP as a cartridge; the loader rejects ZIP/unsupported sizes. A single loaded image is exposed when BIOS selects either P3 cartridge port; two simultaneous carts are not modeled. The local `firmware/` directory is present in this workspace but its ROM files are not included in source archives.

### Manual GitHub Actions build

The repository includes `.github/workflows/manual-build.yml`, triggered only by `workflow_dispatch`. After pushing the repo, open **Actions → Manual Linux build → Run workflow**. It installs SDL2 build files, compiles the CLI and GUI/audio frontend, runs all tests, and uploads the Linux executables as a workflow artifact. The SDL frontend uses the system SDL2 runtime, so a downloaded GUI binary needs that runtime installed (the `libsdl2-dev` build package also installs it). The artifact contains no BIOS or retail-game ROMs.

CMake build and tests:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/gamecom-standalone-demo
./build/gamecom-boot firmware - 20000000 boot-frame.ppm
```

`gamecom-sdl` is optional and requires SDL2 development files at build time. SDL handles the window, display, and input; bundled miniaudio provides audio output without a separate audio SDK. It uses the independent C++ SM8521 core; run `./gamecom-sdl --firmware firmware` for the BIOS or `./gamecom-sdl --firmware firmware --rom /path/to/raw-cartridge.bin` for a cartridge. `./gamecom-sdl --demo` launches a homebrew LCD test. Inputs: arrows, A/B/C/D, M/P/S, N=reset/power; mouse presses map to the nearest cell in the MAME-reference 13×10 touch matrix; Escape exits. The screen-to-matrix mapping is a practical approximation, not confirmed physical coordinates. If no host audio device is available, the frontend reports that and continues silently. Sound currently models SG0/SG1 wave RAM and the direct DAC using frame-boundary register snapshots; sub-frame timing, SG2, and real-game audio have not been validated. LCD scanline fetch races also remain.

`gamecom-standalone-demo` runs a homebrew diagnostic that writes VRAM; it is not a commercial game. The Centipede boot probe now reaches the game's active playfield: scripted inputs change the shooter position and A produces a visible shot. Long play sessions, collisions/scoring, timing fidelity, and real-time SDL gameplay remain to be validated.

## Local firmware and test images

The authorized system firmware is kept locally in `firmware/internal.bin` (4 KiB) and `firmware/external.bin` (256 KiB); it is ignored by source-control/source-package rules. Retail-named 2 MiB game `.bin` images and historical `MAIN.HEX`, `MAIN0*.HEX`, and `BITMAP2.BIN` kernel assets are available elsewhere in the workspace. No copyrighted firmware or retail games are included in source archives.

## Next standalone milestones

1. Extend Centipede gameplay validation across longer runs, including enemy movement/collisions, scoring, and real-time input in the SDL frontend.
2. Refine LCD scanline timing/fetch behavior and validate the touch-grid coordinate mapping.
3. Compare Centipede traces/frames against the reference and fix compatibility gaps without requiring MAME at runtime.
4. Refine timers, interrupts, LCD/DMA corner cases, persistent memory, and audio timing/SG2 support.

Hardware-uncertain behavior should remain labeled and be checked against the supplied documentation, MAME reference code, and tests rather than guessed silently.
