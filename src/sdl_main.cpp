#include "gamecom.h"
#include "gamecom_demo.h"
#include "gamecom_sdl.hpp"
#include "gamecom_launcher.hpp"

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
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
              << " [--launcher] [--firmware DIR] [--rom RAW-CARTRIDGE.bin]"
                 " [--lcd-hz HZ] [--speed PERCENT] [--demo]\n"
                 "With no options, opens the interactive launcher.\n"
                 "Firmware DIR must contain internal.bin and external.bin.\n"
                 "Timing options (decoupled from monitor VSync):\n"
                 "  --lcd-hz HZ      Emulated LCD VBlank refresh rate, 10..240 (default: 60)\n"
                 "  --speed PERCENT  Emulated SM8521 CPU clock percent, 25..800 (default: 100)\n"
                 "Default controls: arrows, A/B/C/D, M=menu, P=pause, S=sound, N=power; mouse=touch, Esc=quit\n"
                 "In-game hotkeys: F5/F6=CPU speed, F7=LCD Hz, hold F8=4x fast-forward\n"
                 "Keyboard bindings and timing defaults can be changed in the launcher.\n";
}

} // namespace

int main(int argc, char **argv)
{
    GcLaunchSelection saved = gc_load_launch_selection();
    std::string firmware_dir;
    std::string cartridge_path;
    GcTimingOptions timing = saved.timing;
    bool cli_lcd_hz = false;
    bool cli_speed = false;
    bool demo = false;
    bool open_launcher = argc == 1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--firmware") == 0 && i + 1 < argc) {
            firmware_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            cartridge_path = argv[++i];
        } else if (std::strcmp(argv[i], "--lcd-hz") == 0 && i + 1 < argc) {
            char *end = nullptr;
            const double val = std::strtod(argv[++i], &end);
            if (!end || *end != '\0' || !std::isfinite(val) || val < 10.0 || val > 240.0) {
                std::cerr << "Invalid --lcd-hz value (expected 10..240).\n";
                return 2;
            }
            timing.lcd_hz = val;
            cli_lcd_hz = true;
        } else if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            char *end = nullptr;
            const unsigned long val = std::strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || val < 25ul || val > 800ul) {
                std::cerr << "Invalid --speed percentage (expected 25..800).\n";
                return 2;
            }
            timing.cpu_speed_percent = static_cast<unsigned>(val);
            cli_speed = true;
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

    SdlSessionGuard sdl_session;
    if (!sdl_session.active) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    GcInputBindings bindings = gc_load_input_bindings();
    if (open_launcher) {
        GcLaunchSelection selection = saved;
        if (!firmware_dir.empty()) selection.firmware_dir = firmware_dir;
        if (!cartridge_path.empty()) selection.cartridge_path = cartridge_path;
        if (cli_lcd_hz) selection.timing.lcd_hz = timing.lcd_hz;
        if (cli_speed) selection.timing.cpu_speed_percent = timing.cpu_speed_percent;
        if (!gc_sdl_launcher_run(selection, bindings)) return 0;
        firmware_dir = std::move(selection.firmware_dir);
        cartridge_path = std::move(selection.cartridge_path);
        timing = selection.timing;
        demo = false;
    }
    if (firmware_dir.empty() && !demo) {
        std::cerr << "Choose --firmware DIR to boot the BIOS or open the launcher.\n";
        print_usage(argv[0]);
        return 2;
    }
    if (demo && (!firmware_dir.empty() || !cartridge_path.empty())) {
        std::cerr << "--demo is a standalone test; do not combine it with BIOS or cartridge boot.\n";
        return 2;
    }

    auto machine = std::unique_ptr<GcMachine, decltype(&std::free)>(
        static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine))), &std::free);
    if (!machine) {
        std::cerr << "Out of memory allocating Game.com machine.\n";
        return 1;
    }
    gc_machine_init(machine.get());
    char error[192] = {};
    if (!firmware_dir.empty()) {
        const std::string internal_path = join_path(firmware_dir, "internal.bin");
        const std::string external_path = join_path(firmware_dir, "external.bin");
        if (!gc_load_firmware(machine.get(), internal_path.c_str(), external_path.c_str(),
                              error, sizeof(error))) {
            std::cerr << "Firmware load failed: " << error << '\n';
            return 1;
        }
    }
    if (!cartridge_path.empty() &&
        !gc_load_cartridge(machine.get(), cartridge_path.c_str(), error, sizeof(error))) {
        std::cerr << "Cartridge load failed: " << error << '\n';
        return 1;
    }
    if (demo) {
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
    standalone::Sm8521Cpu cpu(*machine);
    std::cout << "Starting standalone Game.com SDL LCD; BIOS: "
              << (firmware_dir.empty() ? "not loaded" : firmware_dir)
              << ", cartridge: " << (cartridge_path.empty() ? "none" : cartridge_path)
              << ", LCD: " << timing.lcd_hz << " Hz"
              << ", CPU: " << timing.cpu_speed_percent << "%"
              << (demo ? ", VRAM demo enabled" : "") << '\n';
    return gc_sdl_frontend_run(*machine, cpu, bindings, timing);
}
