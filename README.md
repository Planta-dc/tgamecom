# Independent Tiger Game.com emulator — development workspace

## Goal

Build a clean-room, hardware-accurate standalone Tiger Game.com emulator with its own portable Sharp SM8521 CPU, bus/MMU, firmware and dual-slot cartridge loaders, peripherals, SDL2 display/input frontend, audio synthesizer, and online serial/modem link. The emulator contains no MAME code and does not link to, launch, or require MAME.

## Current status

The project provides a clean-room C++ Sharp SM8521 CPU (`src/standalone/sm8521_cpu.cpp`, `src/standalone/sm8521_ops.inc`), bus/MMU model (`src/core.c`), reset/boot-ROM overlay (with a clean-room built-in handoff so `internal.bin` is optional when `external.bin` / `boot.rom` is present), external firmware and dual-slot cartridge bank mapping (`Slot 1` on `P3=0x40`, `Slot 2` on `P3=0x80`) with hardware address-mask mirroring (supporting `4 KiB..2 MiB` raw `.bin`/`.tgc` images as well as `768 KiB` and `1.75 MiB` split retail dumps), 8 KiB persistent system NVRAM (`0xE000..0xFFFF`, compatible with `.sav` / `boot1.vhd` images) with optional warm-boot seeding, host local-time BCD RTC synchronization (`0x0124..0x0129`), ROM-to-VRAM DMA, hardware `{1, 1024..65536}` timer prescalers, sticky interrupt latching, hardware-calibrated 5-shade STN LCD palettes (`Natural`, `Greenish`, `Grayish`, `High-Contrast`), and a cycle-timed SM8521 UART + `P3` serial subsystem (`SerialLink`) supporting both 2-player `Compete.com` TCP link play and a 9600-baud Virtual Hayes Modem + built-in text portal / TCP dialer for the Internet cartridge.

The supplied firmware reaches the built-in OS/PDA menu after a 20-million-step run. With Centipede loaded, a scripted touch launches the cartridge; after its title and main menu, scheduled A/D-pad inputs advance to the active playfield. In a short repeatable run, left/right input moves the shooter across the screen and A produces a visible shot (`demos/centipede-gameplay-left.png`, `demos/centipede-gameplay-right.png`, `demos/centipede-gameplay-fire.png`). This is initial scripted gameplay/control verification, not proof of a long playable session or hardware accuracy. The 200×160 LCD renderer applies VRAM page selection, LCDC enable, display inversion (`LCC[0]`), 40/50-byte line stride (`LCH[5]`), four gradation palettes, live `LCV[7]` VBlank status, and Game.com LCD colors. Active-low controller groups, `P3` serial handshake lines (`DTR`/`RTS`/`CTS`/`DSR`), and an approximate 13×10 stylus grid are implemented. The SM8521 audio engine synthesizes signed 4-bit `SG0`/`SG1` wave RAM (`12-bit period + 1` at `5.000 MHz`), 15-bit LFSR `SG2` noise, and exclusive `SGDA` 8-bit PCM with sub-sample anti-aliasing, reconstruction low-pass filtering, and DC blocking, sending PCM to a bundled miniaudio backend in the SDL frontend. Scanline fetch races and analog LCD response remain incomplete.

The C++ CPU and SDL frontend are independent of MAME. The SM8521 core runs at the hardware `5.000 MHz` `phi0` clock (`10.000 MHz / 2`) with hardware `{1, 1024..65536}` timer prescalers, sticky `IR0`/`IR1` vs edge-latched interrupt request tracking (`HALT` wake without `FLAG_I` and 1-instruction `IRET` deferral), `CKC` core clock division on `STOP`, `64x` Timer 0 UART baud division, open-bus `0xFF` unpopulated cartridge reads, and pipelined `phi0` DMA bus cycle accounting during `HALT`. `demos/bios-tiger-logo.png` shows the firmware splash; `demos/bios-os-menu.png` shows the built-in OS menu; `demos/bios-centipede-title.png` shows the cartridge title; `demos/centipede-main-menu.png` and `demos/centipede-gameplay-final.png` show the game menu and active playfield.

User testing has confirmed that the Batman and Robin cartridge starts and runs. Host window presentation (VSync), emulated LCD VBlank rate (`--lcd-hz`, default `75.48 Hz` native SM8521 `320×207` scan), SM8521 CPU clock scaling (`--speed`, default `100%` = `5.00 MHz`), and overall emulation pace (`--pace`, default `100%`) are decoupled and controllable live from a top menu bar (`System`, `LCD`, `CPU`, `Input`, `Audio/View`) with presets, fine `+/-` buttons, mouse-wheel scrolling, and exact numeric entry. Stylus clicks default to a column-strobe-aware single-tap pulse so holding a mouse click does not repeatedly trigger game menus while still registering reliably across multi-frame BIOS touch scans (right-click, `Shift`+click, or `Input -> Hold / Drag Stylus` enables continuous dragging).

## Build and run

No MAME installation is needed for these commands:

```sh
make
make test
./gamecom-standalone-demo
./gamecom-debug
./gamecom-boot firmware - 20000000 boot-frame.ppm
make sdl                         # optional; needs SDL2 dev files
./gamecom-sdl                    # interactive launcher: choose firmware, ROM, and clock rates
./gamecom-sdl --firmware firmware --lcd-hz 60 --speed 120 # direct boot (optional --rom PATH)
# or: ./gamecom-sdl --demo       # standalone VRAM/LCD test
```

The Linux command-line executables are fully statically linked by default with GNU Make/GCC; `ldd` reports them as not dynamically linked. This avoids shared glibc and `GLIBCXX` runtime requirements for those CLI tools. The SDL frontend statically links libstdc++/libgcc and links SDL2 without bundling GTK3 (on Linux, **Browse** launches the host system dialog asynchronously via `zenity`/`qarma`/`kdialog` when available, with an instant in-app file browser fallback and drag-and-drop support; optional in-process GTK3 support remains available via `make sdl USE_GTK3=1` or `-DGAMECOM_USE_GTK3=ON`). The manual GitHub workflow packages `gamecom-sdl` as a self-contained AppImage bundling SDL2; glibc cannot safely be bundled, so the AppImage needs a host with glibc 2.35 or newer. Static libc/C++ development archives are needed to build the CLI tools. If those archives are unavailable, build dynamically with `make PORTABLE_LDFLAGS=`; CMake has the equivalent `-DGAMECOM_STATIC_LINUX_EXECUTABLES=OFF` option. `gamecom-test-standalone` is a test binary, not required to run the emulator.

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

Do not pass the BIOS ZIP as a cartridge; the loader rejects ZIP/unsupported sizes. Both Game.com cartridge slots are modeled (`--rom` for Slot 1 on `P3=0x40`, `--rom2` for Slot 2 on `P3=0x80`). System NVRAM (`0xE000..0xFFFF`, 8 KiB) persists automatically in the SDL frontend (`nvram.sav`) or via `--nvram PATH`. The local `firmware/` directory is present in this workspace but its ROM files are not included in source archives.

### GitHub Actions Linux AppImage build

The repository includes `.github/workflows/manual-build.yml`, triggered automatically on pushes to `main` as well as manually via `workflow_dispatch`. It builds on Ubuntu 22.04, compiles the CLI and GUI/audio frontend, runs all tests, then creates a portable `gamecom-linux-x86_64.AppImage` that bundles SDL2 and required runtime libraries without bundling host-conflicting GTK3 libraries. Download the workflow artifact, make the AppImage executable (`chmod +x gamecom-linux-x86_64.AppImage`), and run it. With no arguments, the app opens a launcher; **Browse** opens an asynchronous system folder/file dialog (or the built-in browser; hold `Shift` while clicking **Browse** to jump straight to the built-in picker, or drag-and-drop a folder/ROM onto the window). The firmware folder must contain `external.bin` (or `boot.rom`); `internal.bin` is used automatically when present and optional otherwise. Selected paths, LCD Hz, VSync, palette, CPU speed, NVRAM options, and serial mode save in `$XDG_CONFIG_HOME/gamecom-emulator/launcher.ini` (or `~/.config/gamecom-emulator/launcher.ini`), 8 KiB system NVRAM saves in `nvram.sav`, and keyboard bindings save in `controls.ini`. AppImage does not bundle glibc: the host must provide glibc 2.35 or newer. Some Linux systems also need FUSE for AppImages; when FUSE is unavailable, try `APPIMAGE_EXTRACT_AND_RUN=1 ./gamecom-linux-x86_64.AppImage`. The raw `gamecom-sdl` binary is also included and requires SDL2 on the host. No BIOS or retail-game ROMs are included in the artifact.

## Online Multiplayer (`Compete.com`) & Virtual Hayes Modem

Both serial features use the hardware SM8521 UART (`0x2B..0x2E`, clocked at `Timer 0 / 64`) and active-low RS-232 handshake lines on `P3[3:0]` (`DSR`, `CTS`, `RTS`, `DTR`), and can be toggled live from **`System -> Serial`** in the top menu bar or from the command line:

### 1. 2-Player `Compete.com` Link Cable (TCP over LAN / Internet / Localhost)

1. Both players load the same 2-player-compatible Game.com ROM in Slot 1 and boot to the game's **`Compete` / `2 Player`** menu.
2. **Player 1 (Host)**:
   - GUI: open **`System -> (*) SERIAL: 2P HOST (TCP)`** (listens on TCP port `1997` by default), or
   - CLI: `./gamecom-sdl --firmware firmware --rom Game.bin --link-listen 1997`
   - Note: if playing over the internet, forward TCP port `1997` (or use Tailscale / ZeroTier / Radmin VPN so no port forwarding is needed).
3. **Player 2 (Client)**:
   - GUI: open **`System -> ( ) SERIAL: 2P JOIN (TCP)...`**, enter `<host-ip>:1997` (for two windows on the same PC, use `127.0.0.1:1997`), and press `Enter`, or
   - CLI: `./gamecom-sdl --firmware firmware --rom Game.bin --link-connect 127.0.0.1:1997`
4. Start the 2-player session inside both games; `DTR->DSR` and `RTS->CTS` cross-connect automatically once the TCP socket connects.

### 2. Internet Cartridge / Web / Email (9600-Baud Virtual Hayes Modem)

1. Load the ** Tiger Web Link / Internet ** cartridge in Slot 1 (or any terminal/modem cartridge).
2. Enable the virtual modem:
   - GUI: open **`System -> (*) SERIAL: MODEM (9600 AT)`**, or
   - CLI: `./gamecom-sdl --firmware firmware --rom Internet.bin --modem`
3. In the cartridge's ISP / dial-up setup, enter **any phone number** (e.g., `555-1997` or `0`) and dial:
   - By default, dialing any unmapped number answers `CONNECT 9600` into the built-in interactive **`GAMECOM.NET`** text portal (`HELP`, `NEWS`, `GAMES`, `TIME`, `QUIT`, and `+++` escape back to `OK` command mode).
   - To dial out to real Telnet BBSes or custom TCP text gateways, pass `--phonebook phonebook.ini` (with `number=host:port` lines such as `5551234=telehack.com:23` or `*=127.0.0.1:8080`) or dial an inline `ATDT127.0.0.1:2323` endpoint.

CMake build and tests:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/gamecom-standalone-demo
./build/gamecom-boot firmware - 20000000 boot-frame.ppm
```

`gamecom-sdl` is optional and requires SDL2 development files at build time. SDL handles the launcher, top menu bar, window, display, and input; bundled miniaudio provides audio output without a separate audio SDK. Running `./gamecom-sdl` with no arguments opens the launcher; direct boot is also available with `./gamecom-sdl --firmware firmware` or `./gamecom-sdl --firmware firmware --rom /path/to/raw-cartridge.bin --lcd-hz 60 --speed 120 --pace 100`. Both the launcher and active emulation feature a top menu bar (`System`, `LCD`, `CPU`, `Input`, `Audio/View`) for loading ROMs/firmware, resetting/pausing, switching back to the launcher, adjusting LCD Hz and CPU/game speed (presets, `+/-` steppers, scroll wheel, or typing an exact value), switching stylus touch mode (`Single Tap`, `2-Scan Debounce`, or `Hold / Drag`), remapping controls, muting/adjusting volume, integer pixel scaling, and fullscreen (`F11`). Left-click defaults to a single-tap pulse that waits for the CPU to scan the touched column once and then auto-releases so held clicks never repeat; right-click or `Shift`+left-click always holds/drags the stylus, and focus loss releases all inputs. If no host audio device is available, the frontend reports that and continues silently. Sound synthesis consumes CPU-cycle-timestamped writes for SG0/SG1 wave RAM and SGDA (direct DAC), keeping audio pitch invariant when CPU clock scaling is adjusted. SG2/noise synthesis and real-game sound fidelity remain unverified; MAME also marks this hardware imperfect. LCD scanline fetch races also remain.

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
