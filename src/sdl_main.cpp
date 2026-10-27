#include "gamecom.h"
#include "gamecom_demo.h"
#include "gamecom_sdl.hpp"
#include "gamecom_launcher.hpp"

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct SdlSessionGuard {
    bool active = false;
    SdlSessionGuard()
    {
        active = (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) == 0);
    }
    ~SdlSessionGuard()
    {
        if (active) SDL_Quit();
    }
    SdlSessionGuard(const SdlSessionGuard&) = delete;
    SdlSessionGuard& operator=(const SdlSessionGuard&) = delete;
};

std::string join_path(const std::string& directory, const char *name)
{
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path.push_back('/');
    path += name;
    return path;
}

void print_usage(const char *program)
{
    std::cout << "Usage: " << program
              << " [--launcher] [--firmware DIR] [--rom SLOT1.bin] [--rom2 SLOT2.bin]"
                 " [--nvram FILE] [--warm-boot]"
                 " [--modem] [--phonebook FILE] [--link-listen PORT] [--link-connect HOST:PORT]"
                 " [--lcd-hz HZ] [--vsync on|off] [--lock-hz] [--speed PERCENT] [--pace PERCENT]"
                 " [--touch-mode tap|debounce|hold] [--demo]\n"
                 "With no options, opens the interactive launcher.\n"
                 "Firmware DIR must contain external.bin (or boot.rom); internal.bin is optional.\n"
                 "Timing, input, NVRAM & serial options (also adjustable live in the top menu bar):\n"
                 "  --lcd-hz HZ           Emulated LCD VBlank refresh rate, 10..240 (default: 75.48 HW)\n"
                 "  --vsync on|off        Lock host presentation to monitor VSync (default: off = pace to LCD Hz)\n"
                 "  --lock-hz             Scale SM8521 CPU/timers proportionally when LCD Hz changes\n"
                 "  --speed PERCENT       Emulated SM8521 CPU clock percent of 5.00 MHz, 25..800 (default: 100)\n"
                 "  --pace PERCENT        Overall emulation speed multiplier, 25..400 (default: 100)\n"
                 "  --touch-mode MODE     tap (default, 1-scan pulse), debounce (2-scan), or hold\n"
                 "  --rom2 PATH           Optional second cartridge in Slot 2 (P3=0x80)\n"
                 "  --nvram PATH          8 KiB persistent SRAM file (default: config nvram.sav)\n"
                 "  --warm-boot           Seed warm-boot NVRAM tuple at 0xFEE2..0xFEE7\n"
                 "  --modem               Attach 9600-baud Virtual Hayes Modem + Text Portal\n"
                 "  --phonebook PATH      MidiLink-style number=host:port dial map for --modem\n"
                 "  --link-listen PORT    Host 2-player Compete.com link cable on TCP PORT\n"
                 "  --link-connect H:P    Join 2-player Compete.com link cable at HOST:PORT\n"
                 "Default controls: arrows, A/B/C/D, M=menu, P=pause, S=sound, N=power; mouse=touch, Esc=quit\n"
                 "Right-click or Shift+Left-click always holds/drags stylus. Top menu bar controls all settings.\n";
}

} // namespace

int main(int argc, char **argv)
{
    namespace fs = std::filesystem;
    GcLaunchSelection selection = gc_load_launch_selection();
    std::string cli_firmware;
    std::string cli_rom;
    std::string cli_rom2;
    bool has_cli_firmware = false;
    bool has_cli_rom = false;
    bool has_cli_rom2 = false;
    bool demo = false;
    bool open_launcher = argc == 1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--firmware") == 0 && i + 1 < argc) {
            cli_firmware = argv[++i];
            has_cli_firmware = true;
        } else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            cli_rom = argv[++i];
            has_cli_rom = true;
        } else if (std::strcmp(argv[i], "--rom2") == 0 && i + 1 < argc) {
            cli_rom2 = argv[++i];
            has_cli_rom2 = true;
        } else if (std::strcmp(argv[i], "--nvram") == 0 && i + 1 < argc) {
            selection.nvram_path = argv[++i];
        } else if (std::strcmp(argv[i], "--warm-boot") == 0) {
            selection.warm_boot_nvram = true;
        } else if (std::strcmp(argv[i], "--modem") == 0) {
            selection.serial_mode = GC_SERIAL_MODEM;
        } else if (std::strcmp(argv[i], "--phonebook") == 0 && i + 1 < argc) {
            selection.phonebook_path = argv[++i];
            selection.serial_mode = GC_SERIAL_MODEM;
        } else if (std::strcmp(argv[i], "--link-listen") == 0 && i + 1 < argc) {
            const char *port_str = argv[++i];
            char *end = nullptr;
            const unsigned long p = std::strtoul(port_str, &end, 10);
            if (!end || *end != '\0' || p == 0ul || p > 65535ul) {
                std::cerr << "Invalid --link-listen port (expected 1..65535).\n";
                return 2;
            }
            selection.serial_mode = GC_SERIAL_TCP_HOST;
            selection.serial_endpoint = std::string("0.0.0.0:") + port_str;
        } else if (std::strcmp(argv[i], "--link-connect") == 0 && i + 1 < argc) {
            selection.serial_mode = GC_SERIAL_TCP_CLIENT;
            selection.serial_endpoint = argv[++i];
        } else if (std::strcmp(argv[i], "--lcd-hz") == 0 && i + 1 < argc) {
            char *end = nullptr;
            const double val = std::strtod(argv[++i], &end);
            if (!end || *end != '\0' || !std::isfinite(val) || val < 10.0 || val > 240.0) {
                std::cerr << "Invalid --lcd-hz value (expected 10..240).\n";
                return 2;
            }
            selection.timing.lcd_hz = val;
        } else if (std::strcmp(argv[i], "--vsync") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (std::strcmp(mode, "on") == 0 || std::strcmp(mode, "1") == 0)
                selection.timing.vsync = true;
            else if (std::strcmp(mode, "off") == 0 || std::strcmp(mode, "0") == 0)
                selection.timing.vsync = false;
            else {
                std::cerr << "Invalid --vsync value (expected on or off).\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--lock-hz") == 0) {
            selection.timing.scale_cpu_with_lcd = true;
        } else if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            char *end = nullptr;
            const unsigned long val = std::strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || val < 25ul || val > 800ul) {
                std::cerr << "Invalid --speed percentage (expected 25..800).\n";
                return 2;
            }
            selection.timing.cpu_speed_percent = static_cast<unsigned>(val);
        } else if (std::strcmp(argv[i], "--pace") == 0 && i + 1 < argc) {
            char *end = nullptr;
            const unsigned long val = std::strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || val < 25ul || val > 400ul) {
                std::cerr << "Invalid --pace percentage (expected 25..400).\n";
                return 2;
            }
            selection.timing.game_pace_percent = static_cast<unsigned>(val);
        } else if (std::strcmp(argv[i], "--touch-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (std::strcmp(mode, "tap") == 0)
                selection.timing.touch_mode = GC_TOUCH_SINGLE_TAP;
            else if (std::strcmp(mode, "debounce") == 0)
                selection.timing.touch_mode = GC_TOUCH_DEBOUNCED_TAP;
            else if (std::strcmp(mode, "hold") == 0)
                selection.timing.touch_mode = GC_TOUCH_HOLD_DRAG;
            else {
                std::cerr << "Invalid --touch-mode (expected tap, debounce, or hold).\n";
                return 2;
            }
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (std::strcmp(argv[i], "--launcher") == 0) {
            open_launcher = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (demo && (has_cli_firmware || has_cli_rom || has_cli_rom2)) {
        std::cerr << "--demo is a standalone test; do not combine it with BIOS or cartridge boot.\n";
        return 2;
    }
    if (!open_launcher && !demo && !has_cli_firmware) {
        std::cerr << "Choose --firmware DIR to boot the BIOS directly or run without arguments for the launcher.\n";
        print_usage(argv[0]);
        return 2;
    }

    if (has_cli_firmware) selection.firmware_dir = cli_firmware;
    if (has_cli_rom) selection.cartridge_path = cli_rom;
    else if (!open_launcher) selection.cartridge_path.clear();
    if (has_cli_rom2) selection.cartridge2_path = cli_rom2;
    else if (!open_launcher) selection.cartridge2_path.clear();

    auto machine = std::unique_ptr<GcMachine, decltype(&std::free)>(
        static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine))), &std::free);
    if (!machine) {
        std::cerr << "Out of memory allocating Game.com machine.\n";
        return 1;
    }
    gc_machine_init(machine.get());

    if (!open_launcher) {
        char error[192] = {};
        if (!demo) {
            std::error_code ec;
            const std::string internal_cand = join_path(selection.firmware_dir, "internal.bin");
            const bool has_internal = fs::is_regular_file(internal_cand, ec) && !ec;
            ec.clear();
            std::string external_path = join_path(selection.firmware_dir, "external.bin");
            if (!fs::is_regular_file(external_path, ec) || ec) {
                ec.clear();
                const std::string alias = join_path(selection.firmware_dir, "boot.rom");
                if (fs::is_regular_file(alias, ec) && !ec) external_path = alias;
            }
            if (!gc_load_firmware(machine.get(),
                                  has_internal ? internal_cand.c_str() : nullptr,
                                  external_path.c_str(), error, sizeof(error))) {
                std::cerr << "Firmware load failed: " << error << '\n';
                return 1;
            }
            if (!selection.cartridge_path.empty() &&
                !gc_load_cartridge_slot(machine.get(), 1u, selection.cartridge_path.c_str(),
                                        error, sizeof(error))) {
                std::cerr << "Cartridge load failed: " << error << '\n';
                return 1;
            }
            if (!selection.cartridge2_path.empty() &&
                !gc_load_cartridge_slot(machine.get(), 2u, selection.cartridge2_path.c_str(),
                                        error, sizeof(error))) {
                std::cerr << "Slot 2 cartridge load failed: " << error << '\n';
                return 1;
            }
            const std::string nvram = selection.nvram_path.empty()
                ? gc_default_nvram_path() : selection.nvram_path;
            ec.clear();
            if (!nvram.empty() && fs::is_regular_file(nvram, ec) && !ec)
                (void)gc_nvram_load(machine.get(), nvram.c_str(), error, sizeof(error));
            if (selection.warm_boot_nvram)
                gc_nvram_seed_warm_boot(machine.get());
        } else {
            constexpr std::size_t capacity = (200u * 40u / 2u) * 8u + 1u;
            std::vector<std::uint8_t> program(capacity);
            const std::size_t length = gc_build_vram_demo(program.data(), program.size());
            if (!length || length > GC_PATCH_SIZE - 0x1020u) {
                std::cerr << "Could not build the VRAM demo program.\n";
                return 1;
            }
            std::memcpy(machine->bus.patch + 0x1020u, program.data(), length);
            std::memset(machine->bus.patch_valid + 0x1020u, 1, length);
            machine->bus.reg_file[0x30] = static_cast<std::uint8_t>(
                (machine->bus.reg_file[0x30] & 0x7Fu) | 0x80u);
        }
        gc_machine_reset(machine.get());
    }

    SdlSessionGuard sdl_session;
    if (!sdl_session.active) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    GcInputBindings bindings = gc_load_input_bindings();
    standalone::Sm8521Cpu cpu(*machine);
    return gc_sdl_session_run(*machine, cpu, selection, bindings, open_launcher, demo);
}
