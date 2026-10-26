#include "gamecom.h"
#include "gamecom_demo.h"
#include "gamecom_sdl.hpp"
#include "gamecom_launcher.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static std::string join_path(const std::string& directory, const char *name)
{
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path.push_back('/');
    path += name;
    return path;
}

static void print_usage(const char *program)
{
    std::cout << "Usage: " << program << " [--launcher] [--firmware DIR] [--rom RAW-CARTRIDGE.bin] [--demo]\n"
                 "With no options, opens the interactive launcher.\n"
                 "Firmware DIR must contain internal.bin and external.bin.\n"
                 "Controls: arrows, A/B/C/D, M=menu, P=pause, S=sound, N=reset, mouse=touch, Esc=quit\n";
}

int main(int argc, char **argv)
{
    std::string firmware_dir;
    std::string cartridge_path;
    bool demo = false;
    bool open_launcher = argc == 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--firmware") == 0 && i + 1 < argc) firmware_dir = argv[++i];
        else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) cartridge_path = argv[++i];
        else if (std::strcmp(argv[i], "--demo") == 0) demo = true;
        else if (std::strcmp(argv[i], "--launcher") == 0) open_launcher = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }
    if (open_launcher) {
        GcLaunchSelection selection;
        if (!gc_sdl_launcher_run(selection)) return 0;
        firmware_dir = std::move(selection.firmware_dir);
        cartridge_path = std::move(selection.cartridge_path);
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
              << (demo ? ", VRAM demo enabled" : "") << '\n';
    return gc_sdl_frontend_run(*machine, cpu);
}
