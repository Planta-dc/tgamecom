# Independent Tiger Game.com emulator — development workspace

## Goal

Build a standalone Game.com emulator with its own portable CPU, bus/MMU, firmware and cartridge loaders, peripherals, SDL2-capable display/input, and audio. The finished emulator must not link to, launch, or require MAME. MAME is a coding reference only; the standalone SM8521 CPU operation table in this project is adapted from MAME's BSD-3-Clause source and retains its attribution/license notice.

## Current status

The project has a standalone C++ SM8521 interpreter, a separate bus model, reset/boot-ROM overlay, external firmware and cartridge bank mapping, MMU switching, ROM-to-VRAM DMA, basic timer/clock handling, periodic interrupt inputs, and a cycle-timed SM8521 UART register model. UART byte RX/TX, timer-derived frame timing, status flags, overrun, and interrupt requests have regression tests. A virtual Hayes modem and live-web gateway are not connected yet; online browsing is still future work. The CPU uses the adapted MAME operation table but does not call or link to MAME.

The supplied firmware reaches the built-in OS/PDA menu after a 20-million-step run. With Centipede loaded, a scripted touch launches the cartridge; after its title and main menu, scheduled A/D-pad inputs advance to the active playfield. In a short repeatable run, left/right input moves the shooter across the screen and A produces a visible shot (`demos/centipede-gameplay-left.png`, `demos/centipede-gameplay-right.png`, `demos/centipede-gameplay-fire.png`). This is initial scripted gameplay/control verification, not proof of a long playable session or hardware accuracy. The 200×160 LCD renderer applies MAME-reference VRAM page selection, LCDC enable, four gradation palettes, and Game.com LCD colors. Active-low controller groups and an approximate 13×10 stylus grid are implemented; physical touch-coordinate mapping is not hardware-confirmed. An initial sound path synthesizes the two 4-bit waveform channels and direct DAC output, then sends PCM to a bundled miniaudio backend in the SDL frontend. Audio-register writes are now timestamped against emulated CPU cycles for SG0/SG1 and the direct DAC; SG2 and retail audio fidelity remain unverified. Scanline timing/races and analog LCD response are also incomplete.

The C++ CPU and SDL frontend are independent of MAME. DMA progresses during HALT wait cycles, matching the reference CPU callback order. The SDL frontend runs the standalone C++ CPU, supports BIOS/cartridge boot, and targets the reference LCD cadence independently of monitor refresh. `demos/bios-tiger-logo.png` shows the firmware splash; `demos/bios-os-menu.png` shows the built-in OS menu; `demos/bios-centipede-title.png` shows the cartridge title; `demos/centipede-main-menu.png` and `demos/centipede-gameplay-final.png` show the game menu and active playfield.

User testing has confirmed that the Batman and Robin cartridge starts and runs. The same test reports that overall emulation speed is too high and audio quality is poor; running a retail cartridge is confirmed, but timing and sound fidelity are not. SDL frame pacing now targets a fixed 59.732 Hz rather than following monitor refresh and needs retesting on the user's hardware. Audio synthesis remains approximate and needs further work.

## Build and run

No MAME installation is needed for these commands:

```sh
make
make test
./gamecom-standalone-demo
./gamecom-debug
./gamecom-boot firmware - 20000000 boot-frame.ppm
make sdl                         # optional; needs SDL2 dev files; GTK3 dev enables native file dialogs
./gamecom-sdl                    # interactive launcher: choose firmware and optional game ROM
./gamecom-sdl --firmware firmware # boot BIOS directly (optional --rom PATH)
# or: ./gamecom-sdl --demo       # standalone VRAM/LCD test
```

The Linux command-line executables are fully statically linked by default with GNU Make/GCC; `ldd` reports them as not dynamically linked. This avoids shared glibc and `GLIBCXX` runtime requirements for those CLI tools. The SDL frontend statically links libstdc++/libgcc but still uses the host glibc plus SDL2/GTK3 when run directly with native dialogs enabled. The manual GitHub workflow also packages it as an AppImage, bundling SDL2, GTK3, and other non-core runtime libraries; glibc cannot safely be bundled, so the AppImage needs a host with glibc 2.35 or newer. Static libc/C++ development archives are needed to build the CLI tools. If those archives are unavailable, build dynamically with `make PORTABLE_LDFLAGS=`; CMake has the equivalent `-DGAMECOM_STATIC_LINUX_EXECUTABLES=OFF` option. `gamecom-test-standalone` is a test binary, not required to run the emulator.

`gamecom-boot` takes a firmware directory, an optional raw cartridge `.bin` path (use `-` for none), an emulation-step budget (default 20,000,000), and an optional PPM path. Scripted inputs use repeatable `--touch step column row duration` and `--button step name duration` events; touch presses may not overlap. Button names are `up`, `down`, `left`, `right`, `menu`, `pause`, `sound`, `a`, `b`, `c`, `d`, and `power`. The touch grid is approximate and event times are CPU steps. A menu/title probe is:

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

The repository includes `.github/workflows/manual-build.yml`, triggered only by `workflow_dispatch`. After pushing the repo, open **Actions → Manual Linux build → Run workflow**. It builds on Ubuntu 22.04, compiles the CLI and GUI/audio frontend, runs all tests, then creates a portable `gamecom-linux-x86_64.AppImage` that bundles SDL2, GTK3, and other non-core runtime libraries. Download the workflow artifact, make the AppImage executable (`chmod +x gamecom-linux-x86_64.AppImage`), and run it. With no arguments, the app opens a launcher; **Browse** opens a normal folder/file dialog for firmware and an optional raw `.bin` cartridge. The firmware folder must contain `internal.bin` and `external.bin`. The launcher also has a **Controls** screen for remapping keyboard buttons; bindings are saved in `$XDG_CONFIG_HOME/gamecom-emulator/controls.ini` or `~/.config/gamecom-emulator/controls.ini`. AppImage does not bundle glibc: the host must provide glibc 2.35 or newer. Some Linux systems also need FUSE for AppImages; when FUSE is unavailable, try `APPIMAGE_EXTRACT_AND_RUN=1 ./gamecom-linux-x86_64.AppImage`. The raw `gamecom-sdl` binary is also included and still requires SDL2/GTK3 runtime libraries on the host. No BIOS or retail-game ROMs are included in the artifact.

CMake build and tests:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/gamecom-standalone-demo
./build/gamecom-boot firmware - 20000000 boot-frame.ppm
```

`gamecom-sdl` is optional and requires SDL2 development files at build time; GTK3 development files enable the normal system file chooser (without GTK3, the built-in browser remains available). SDL handles the launcher, window, display, and input; bundled miniaudio provides audio output without a separate audio SDK. Running `./gamecom-sdl` with no arguments opens the launcher to choose firmware and an optional raw cartridge; direct boot is also available with `./gamecom-sdl --firmware firmware` or `./gamecom-sdl --firmware firmware --rom /path/to/raw-cartridge.bin`. Use **Controls** in the launcher to rebind arrows, A/B/C/D, Menu, Pause, Sound, and Power; Escape remains reserved to exit, and mouse touch remains fixed. Bindings persist in the user's XDG config directory. `./gamecom-sdl --demo` launches a homebrew LCD test. Mouse presses map to the nearest cell in the MAME-reference 13×10 touch matrix; the screen-to-matrix mapping is a practical approximation, not confirmed physical coordinates. If no host audio device is available, the frontend reports that and continues silently. Sound synthesis now consumes CPU-cycle-timestamped writes for SG0/SG1 wave RAM and SGDA (direct DAC), preserving rapid DAC updates within a video frame. SG2/noise synthesis and real-game sound fidelity remain unverified; MAME also marks this hardware imperfect. LCD scanline fetch races also remain.

`gamecom-standalone-demo` runs a homebrew diagnostic that writes VRAM; it is not a commercial game. The Centipede boot probe now reaches the game's active playfield: scripted inputs change the shooter position and A produces a visible shot. Long play sessions, collisions/scoring, timing fidelity, and real-time SDL gameplay remain to be validated.

## Local firmware and test images

The authorized system firmware is kept locally in `firmware/internal.bin` (4 KiB) and `firmware/external.bin` (256 KiB); it is ignored by source-control/source-package rules. Retail-named 2 MiB game `.bin` images and historical `MAIN.HEX`, `MAIN0*.HEX`, and `BITMAP2.BIN` kernel assets are available elsewhere in the workspace. No copyrighted firmware or retail games are included in source archives.

## Next standalone milestones

1. Extend Centipede gameplay validation across longer runs, including enemy movement/collisions, scoring, and real-time input in the SDL frontend.
2. Refine LCD scanline timing/fetch behavior and validate the touch-grid coordinate mapping.
3. Compare Centipede traces/frames against the reference and fix compatibility gaps without requiring MAME at runtime.
4. Connect the documented UART to a virtual Hayes modem, then implement and validate a safe text-only web gateway for the Internet cartridge.
5. Refine timers, interrupts, LCD/DMA corner cases, persistent memory, and audio timing/SG2 support.

Hardware-uncertain behavior should remain labeled and be checked against the supplied documentation, MAME reference code, and tests rather than guessed silently.
