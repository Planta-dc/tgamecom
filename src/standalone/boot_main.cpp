#include "gamecom.h"
#include "gamecom_video.h"
#include "sm8521_cpu.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

static bool parse_decimal_u64(const char *text, std::uint64_t& value)
{
    if (!text || !*text) return false;
    std::uint64_t parsed = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned digit = static_cast<unsigned>(*p - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

static bool parse_button_name(const char *text, GcButton& button)
{
    if (!text) return false;
    const std::string name(text);
    if (name == "up") button = GC_BUTTON_UP;
    else if (name == "down") button = GC_BUTTON_DOWN;
    else if (name == "left") button = GC_BUTTON_LEFT;
    else if (name == "right") button = GC_BUTTON_RIGHT;
    else if (name == "menu") button = GC_BUTTON_MENU;
    else if (name == "pause") button = GC_BUTTON_PAUSE;
    else if (name == "sound") button = GC_BUTTON_SOUND;
    else if (name == "a") button = GC_BUTTON_A;
    else if (name == "b") button = GC_BUTTON_B;
    else if (name == "c") button = GC_BUTTON_C;
    else if (name == "d") button = GC_BUTTON_D;
    else if (name == "power") button = GC_BUTTON_POWER;
    else return false;
    return true;
}

static const char *button_name(GcButton button)
{
    switch (button) {
    case GC_BUTTON_UP: return "up";
    case GC_BUTTON_DOWN: return "down";
    case GC_BUTTON_LEFT: return "left";
    case GC_BUTTON_RIGHT: return "right";
    case GC_BUTTON_MENU: return "menu";
    case GC_BUTTON_PAUSE: return "pause";
    case GC_BUTTON_SOUND: return "sound";
    case GC_BUTTON_A: return "a";
    case GC_BUTTON_B: return "b";
    case GC_BUTTON_C: return "c";
    case GC_BUTTON_D: return "d";
    case GC_BUTTON_POWER: return "power";
    default: return "?";
    }
}

struct ButtonEvent {
    std::uint64_t start = 0;
    std::uint64_t duration = 0;
    std::uint64_t end = 0;
    GcButton button = GC_BUTTON_A;
    bool active = false;
};

static std::string join_path(const std::string& directory, const char *filename)
{
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path.push_back('/');
    path += filename;
    return path;
}

static unsigned count_nonzero_vram(const GcMachine& machine)
{
    unsigned count = 0;
    for (std::uint8_t byte : machine.bus.vram)
        count += byte != 0;
    return count;
}

static bool save_frame_ppm(const char *path, const GcMachine& machine)
{
    std::array<std::uint32_t, GC_LCD_WIDTH * GC_LCD_HEIGHT> pixels{};
    if (!gc_lcd_render_argb(&machine, pixels.data(), GC_LCD_WIDTH)) return false;
    FILE *file = std::fopen(path, "wb");
    if (!file) return false;
    bool ok = std::fprintf(file, "P6\n%u %u\n255\n", GC_LCD_WIDTH, GC_LCD_HEIGHT) > 0;
    for (unsigned y = 0; y < GC_LCD_HEIGHT && ok; ++y) {
        for (unsigned x = 0; x < GC_LCD_WIDTH; ++x) {
            const std::uint32_t argb = pixels[y * GC_LCD_WIDTH + x];
            const std::uint8_t rgb[3] = {
                static_cast<std::uint8_t>(argb >> 16),
                static_cast<std::uint8_t>(argb >> 8),
                static_cast<std::uint8_t>(argb)
            };
            if (std::fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) { ok = false; break; }
        }
    }
    if (std::fclose(file) != 0) ok = false;
    return ok;
}

struct TraceEntry {
    std::uint64_t instruction = 0;
    std::uint16_t pc = 0;
    std::uint8_t opcode = 0;
    std::uint16_t next_pc = 0;
    std::uint64_t cycles = 0;
};

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 30) {
        std::cerr << "Usage: gamecom-boot <firmware-dir> [cartridge.bin|-] [step-limit] [frame.ppm] "
                     "[--touch step column row duration] [--button step name duration ...]\n"
                     "Default step limit: 20000000; button names: up/down/left/right/menu/pause/sound/a/b/c/d/power\n"
                     "Example: gamecom-boot firmware Centipede.bin 80000000 frame.ppm "
                     "--touch 10000000 2 3 300000 --button 41000000 a 300000 --button 68000000 a 300000\n";
        return 2;
    }

    std::uint64_t instruction_limit = 20000000;
    if (argc >= 4) {
        if (!parse_decimal_u64(argv[3], instruction_limit) || instruction_limit == 0) {
            std::cerr << "Invalid instruction limit. Use a positive decimal number.\n";
            return 2;
        }
    }

    int optional_arg = 4;
    const char *frame_path = nullptr;
    if (optional_arg < argc && argv[optional_arg][0] != '-')
        frame_path = argv[optional_arg++];
    bool touch_scheduled = false;
    std::uint64_t touch_start = 0, touch_duration = 0, touch_column = 0, touch_row = 0;
    std::uint64_t touch_end = 0;
    std::vector<ButtonEvent> button_events;
    while (optional_arg < argc) {
        const std::string option(argv[optional_arg]);
        if (option == "--touch" && !touch_scheduled && argc - optional_arg >= 5 &&
            parse_decimal_u64(argv[optional_arg + 1], touch_start) &&
            parse_decimal_u64(argv[optional_arg + 2], touch_column) &&
            parse_decimal_u64(argv[optional_arg + 3], touch_row) &&
            parse_decimal_u64(argv[optional_arg + 4], touch_duration) &&
            touch_column < 13 && touch_row < 10 && touch_duration > 0 &&
            touch_start < instruction_limit && touch_duration <= instruction_limit - touch_start) {
            touch_end = touch_start + touch_duration;
            touch_scheduled = true;
            optional_arg += 5;
        } else if (option == "--button" && argc - optional_arg >= 4) {
            ButtonEvent event;
            if (!parse_decimal_u64(argv[optional_arg + 1], event.start) ||
                !parse_button_name(argv[optional_arg + 2], event.button) ||
                !parse_decimal_u64(argv[optional_arg + 3], event.duration) ||
                event.duration == 0 || event.start >= instruction_limit ||
                event.duration > instruction_limit - event.start) {
                std::cerr << "Invalid button event. Use: --button step name duration; "
                             "button names are up/down/left/right/menu/pause/sound/a/b/c/d/power.\n";
                return 2;
            }
            event.end = event.start + event.duration;
            for (const ButtonEvent& existing : button_events) {
                if (existing.button == event.button && event.start < existing.end &&
                    existing.start < event.end) {
                    std::cerr << "Overlapping scheduled presses of the same button are not supported.\n";
                    return 2;
                }
            }
            button_events.push_back(event);
            optional_arg += 4;
        } else {
            std::cerr << "Invalid or duplicate event near '" << argv[optional_arg]
                      << "'. Touch: --touch step column(0-12) row(0-9) duration. "
                         "Button: --button step name duration. Events must fit within the step limit.\n";
            return 2;
        }
    }

    auto machine = std::unique_ptr<GcMachine, decltype(&std::free)>(
        static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine))), &std::free);
    if (!machine) {
        std::cerr << "Out of memory allocating Game.com machine.\n";
        return 1;
    }
    gc_machine_init(machine.get());

    const std::string internal_path = join_path(argv[1], "internal.bin");
    const std::string external_path = join_path(argv[1], "external.bin");
    char error[160] = {};
    if (!gc_load_firmware(machine.get(), internal_path.c_str(), external_path.c_str(),
                          error, sizeof(error))) {
        std::cerr << "Firmware load failed: " << error << '\n';
        return 1;
    }
    if (argc >= 3 && std::string(argv[2]) != "-" &&
        !gc_load_cartridge(machine.get(), argv[2], error, sizeof(error))) {
        std::cerr << "Cartridge load failed: " << error << '\n';
        return 1;
    }

    gc_machine_reset(machine.get());
    standalone::Sm8521Cpu cpu(*machine);
    std::cout << "Game.com firmware boot probe\n"
              << "Reset PC: 0x1020; firmware loaded; cartridge: "
              << (argc >= 3 && std::string(argv[2]) != "-" ? argv[2] : "none") << '\n';
    if (touch_scheduled)
        std::cout << "Scheduled stylus press: step " << touch_start << ", column "
                  << touch_column << ", row " << touch_row << ", duration "
                  << touch_duration << " steps\n";
    for (const ButtonEvent& event : button_events)
        std::cout << "Scheduled button press: " << button_name(event.button)
                  << ", step " << event.start << ", duration " << event.duration << " steps\n";

    constexpr std::uint64_t trace_count = 48;
    std::array<TraceEntry, 32> tail{};
    std::size_t tail_pos = 0;
    std::uint64_t steps = 0;
    bool touch_is_down = false;
    while (steps < instruction_limit) {
        if (touch_scheduled && steps == touch_start) {
            (void)gc_input_set_touch(machine.get(), true,
                                     static_cast<unsigned>(touch_column),
                                     static_cast<unsigned>(touch_row));
            touch_is_down = true;
        }
        if (touch_is_down && steps == touch_end) {
            (void)gc_input_set_touch(machine.get(), false, 0, 0);
            touch_is_down = false;
        }
        for (ButtonEvent& event : button_events) {
            if (!event.active && steps == event.start) {
                gc_input_set_button(machine.get(), event.button, true);
                event.active = true;
            }
            if (event.active && steps == event.end) {
                gc_input_set_button(machine.get(), event.button, false);
                event.active = false;
            }
        }
        const std::uint64_t instructions_before = cpu.instructions();
        const std::uint16_t pc = cpu.pc();
        const std::uint8_t opcode = gc_bus_read(machine.get(), pc);
        const std::uint64_t before = cpu.cycles();
        (void)cpu.step();
        ++steps;
        if (cpu.instructions() == instructions_before) continue; /* HALT idle cycle */
        const TraceEntry entry{cpu.instructions() - 1, pc, opcode, cpu.pc(),
                               cpu.cycles() - before};
        tail[tail_pos++ % tail.size()] = entry;
        if (cpu.instructions() <= trace_count) {
            std::cout << "  " << std::dec << std::setw(5) << entry.instruction
                      << "  PC=" << std::hex << std::setw(4) << std::setfill('0') << entry.pc
                      << " op=" << std::setw(2) << static_cast<unsigned>(entry.opcode)
                      << " -> " << std::setw(4) << entry.next_pc
                      << "  +" << std::dec << entry.cycles << " cyc\n"
                      << std::setfill(' ');
        }
    }
    if (touch_is_down) (void)gc_input_set_touch(machine.get(), false, 0, 0);
    for (ButtonEvent& event : button_events) {
        if (event.active) gc_input_set_button(machine.get(), event.button, false);
    }
    if (cpu.halted()) {
        std::cout << "Recent instructions before HALT:\n";
        const std::size_t stored = static_cast<std::size_t>(cpu.instructions() < tail.size()
                                  ? cpu.instructions() : tail.size());
        const std::size_t begin = tail_pos >= stored ? tail_pos - stored : 0;
        for (std::size_t i = 0; i < stored; ++i) {
            const auto& entry = tail[(begin + i) % tail.size()];
            char disassembly[128] = {};
            (void)gc_disassemble(machine.get(), entry.pc, disassembly, sizeof(disassembly));
            std::cout << "  " << std::dec << std::setw(8) << entry.instruction
                      << "  PC=" << std::hex << std::setw(4) << std::setfill('0') << entry.pc
                      << " op=" << std::setw(2) << static_cast<unsigned>(entry.opcode)
                      << " -> " << std::setw(4) << entry.next_pc
                      << "  " << disassembly << '\n'
                      << std::setfill(' ');
        }
    }

    std::cout << std::dec << "Instructions executed: " << cpu.instructions() << "\n"
              << "CPU steps including HALT idle ticks: " << steps << " / " << instruction_limit << "\n"
              << "Cycles: " << cpu.cycles() << "\n"
              << std::hex << std::setfill('0')
              << "PC: 0x" << std::setw(4) << cpu.pc()
              << "  SYS=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x19])
              << "  PS0=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x1E])
              << "  PS1=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x1F]) << '\n'
              << "IE0/1=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x10])
              << "/0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x11])
              << " IR0/1=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x12])
              << "/0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x13])
              << " LCDC=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x30])
              << " CLKT=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x54])
              << " TM0C/D=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x50])
              << "/0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x51])
              << " TM1C/D=0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x52])
              << "/0x" << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x53]) << '\n'
              << "PIO P0-3: " << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x14])
              << ' ' << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x15])
              << ' ' << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x16])
              << ' ' << std::setw(2) << static_cast<unsigned>(machine->bus.reg_file[0x17])
              << "  cart-active=" << (machine->bus.cart_active ? "yes" : "no") << '\n'
              << "MMU0-4: ";
    for (unsigned i = 0; i < 5; ++i)
        std::cout << "0x" << std::setw(2)
                  << static_cast<unsigned>(machine->bus.reg_file[0x24 + i])
                  << (i == 4 ? '\n' : ' ');
    std::cout << std::dec << "Boot ROM overlay: "
              << (machine->bus.boot_enabled ? "still active" : "disabled") << '\n'
              << "Nonzero VRAM bytes: " << count_nonzero_vram(*machine)
              << "  DMA transfers: " << cpu.dma_transfers()
              << "  VBlank ticks: " << cpu.vblank_ticks()
              << "  clock ticks: " << cpu.clock_ticks() << '\n';

    if (cpu.halted()) {
        std::cout << "CPU reached HALT.\n";
    } else if (steps >= instruction_limit) {
        std::cout << "Step budget reached; the CPU had not entered HALT.\n";
    }
    std::cout << "Caveat: frame-level LCD rendering, basic controller/touch-matrix input, and preliminary"
                 " sound synthesis are implemented; scanline timing/races, physical touch mapping, and"
                 " retail audio fidelity remain unverified. A short scripted playfield frame is not proof of full compatibility.\n";
    if (frame_path) {
        if (save_frame_ppm(frame_path, *machine))
            std::cout << "Wrote LCD frame: " << frame_path << '\n';
        else {
            std::cerr << "Could not write LCD frame: " << frame_path << '\n';
            return 1;
        }
    }
    return 0;
}
