#include "gamecom.h"
#include "gamecom_video.h"
#include "sm8521_cpu.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
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

static bool parse_hex_u16(const char *text, std::uint16_t& value)
{
    if (!text || !*text) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (!*text) return false;
    std::uint32_t parsed = 0;
    unsigned digits = 0;
    for (const char *p = text; *p; ++p) {
        unsigned digit;
        if (*p >= '0' && *p <= '9') digit = static_cast<unsigned>(*p - '0');
        else if (*p >= 'a' && *p <= 'f') digit = static_cast<unsigned>(*p - 'a' + 10);
        else if (*p >= 'A' && *p <= 'F') digit = static_cast<unsigned>(*p - 'A' + 10);
        else return false;
        if (++digits > 4u) return false;
        parsed = (parsed << 4) | digit;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

static bool is_cartridge_fetch(const GcMachine& machine, std::uint16_t pc,
                               unsigned& window, std::uint8_t& bank)
{
    if (pc < 0x1000u || pc >= 0xA000u) return false;
    if (pc < 0x2000u && machine.bus.boot_enabled) return false;
    window = static_cast<unsigned>(pc >> 13);
    bank = machine.bus.mapped_bank[window];
    return machine.bus.cart_active && bank >= 0x20u;
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

struct TouchEvent {
    std::uint64_t start = 0;
    std::uint64_t duration = 0;
    std::uint64_t end = 0;
    unsigned column = 0;
    unsigned row = 0;
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
    std::array<char, 128> disassembly{};
};

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 128) {
        std::cerr << "Usage: gamecom-boot <firmware-dir> [cartridge.bin|-] [step-limit] [frame.ppm] [options]\n"
                     "Events: --touch step column row duration; --button step name duration (repeatable)\n"
                     "Debug: --break PC (repeatable), --break-cart, --watch ADDRESS, --trace-boot, --dump ADDRESS LENGTH\n"
                     "Addresses are hexadecimal (optional 0x); dump length is decimal. Steps are CPU steps, not cycles.\n"
                     "Example: gamecom-boot firmware Centipede.bin 80000000 --trace-boot "
                     "--break-cart --dump E000 128 --touch 10000000 2 3 300000\n";
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
    const char *rom2_path = nullptr;
    const char *nvram_path = nullptr;
    bool warm_boot = false;
    if (optional_arg < argc && argv[optional_arg][0] != '-')
        frame_path = argv[optional_arg++];
    std::vector<TouchEvent> touch_events;
    std::vector<ButtonEvent> button_events;
    std::vector<std::uint16_t> breakpoints;
    bool break_on_cart = false;
    bool trace_boot = false;
    bool watch_set = false;
    std::uint16_t watch_address = 0;
    bool dump_set = false;
    std::uint16_t dump_address = 0;
    std::uint32_t dump_length = 0;
    while (optional_arg < argc) {
        const std::string option(argv[optional_arg]);
        if (option == "--rom2" && argc - optional_arg >= 2) {
            rom2_path = argv[optional_arg + 1];
            optional_arg += 2;
        } else if (option == "--nvram" && argc - optional_arg >= 2) {
            nvram_path = argv[optional_arg + 1];
            optional_arg += 2;
        } else if (option == "--warm-boot") {
            warm_boot = true;
            ++optional_arg;
        } else if (option == "--break" && argc - optional_arg >= 2) {
            std::uint16_t address = 0;
            if (!parse_hex_u16(argv[optional_arg + 1], address) ||
                breakpoints.size() >= GC_MAX_BREAKPOINTS) {
                std::cerr << "Invalid breakpoint address or too many breakpoints (max "
                          << GC_MAX_BREAKPOINTS << ").\n";
                return 2;
            }
            breakpoints.push_back(address);
            optional_arg += 2;
        } else if (option == "--break-cart") {
            break_on_cart = true;
            ++optional_arg;
        } else if (option == "--trace-boot") {
            trace_boot = true;
            ++optional_arg;
        } else if (option == "--watch" && argc - optional_arg >= 2) {
            if (watch_set || !parse_hex_u16(argv[optional_arg + 1], watch_address)) {
                std::cerr << "Use one --watch address (hexadecimal).\n";
                return 2;
            }
            watch_set = true;
            optional_arg += 2;
        } else if (option == "--dump" && argc - optional_arg >= 3) {
            std::uint64_t length = 0;
            if (dump_set || !parse_hex_u16(argv[optional_arg + 1], dump_address) ||
                !parse_decimal_u64(argv[optional_arg + 2], length) || length == 0 ||
                length > 0x10000u || length > 0x10000u - dump_address) {
                std::cerr << "Use one --dump HEX_ADDRESS DECIMAL_LENGTH wholly within 64 KiB.\n";
                return 2;
            }
            dump_set = true;
            dump_length = static_cast<std::uint32_t>(length);
            optional_arg += 3;
        } else if (option == "--touch" && argc - optional_arg >= 5) {
            TouchEvent event;
            std::uint64_t column = 0, row = 0;
            if (!parse_decimal_u64(argv[optional_arg + 1], event.start) ||
                !parse_decimal_u64(argv[optional_arg + 2], column) ||
                !parse_decimal_u64(argv[optional_arg + 3], row) ||
                !parse_decimal_u64(argv[optional_arg + 4], event.duration) ||
                column >= 13 || row >= 10 || event.duration == 0 ||
                event.start >= instruction_limit ||
                event.duration > instruction_limit - event.start) {
                std::cerr << "Invalid touch event. Use: --touch step column(0-12) row(0-9) duration.\n";
                return 2;
            }
            event.column = static_cast<unsigned>(column);
            event.row = static_cast<unsigned>(row);
            event.end = event.start + event.duration;
            for (const TouchEvent& existing : touch_events) {
                if (event.start < existing.end && existing.start < event.end) {
                    std::cerr << "Overlapping stylus presses are not supported.\n";
                    return 2;
                }
            }
            touch_events.push_back(event);
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
            std::cerr << "Invalid event near '" << argv[optional_arg]
                      << "'. Repeat --touch step column(0-12) row(0-9) duration and/or "
                         "--button step name duration. Events must fit within the step limit.\n";
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

    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string internal_candidate = join_path(argv[1], "internal.bin");
    const bool has_internal = fs::is_regular_file(internal_candidate, ec) && !ec;
    std::string external_path = join_path(argv[1], "external.bin");
    if (!fs::is_regular_file(external_path, ec) || ec) {
        const std::string boot_alias = join_path(argv[1], "boot.rom");
        if (fs::is_regular_file(boot_alias, ec) && !ec) external_path = boot_alias;
    }
    char error[160] = {};
    if (!gc_load_firmware(machine.get(),
                          has_internal ? internal_candidate.c_str() : nullptr,
                          external_path.c_str(), error, sizeof(error))) {
        std::cerr << "Firmware load failed: " << error << '\n';
        return 1;
    }
    if (argc >= 3 && std::string(argv[2]) != "-" &&
        !gc_load_cartridge_slot(machine.get(), 1u, argv[2], error, sizeof(error))) {
        std::cerr << "Cartridge load failed: " << error << '\n';
        return 1;
    }
    if (rom2_path && std::string(rom2_path) != "-" &&
        !gc_load_cartridge_slot(machine.get(), 2u, rom2_path, error, sizeof(error))) {
        std::cerr << "Slot 2 cartridge load failed: " << error << '\n';
        return 1;
    }
    if (nvram_path && fs::is_regular_file(nvram_path, ec) && !ec) {
        if (!gc_nvram_load(machine.get(), nvram_path, error, sizeof(error))) {
            std::cerr << "NVRAM load failed: " << error << '\n';
            return 1;
        }
    }
    if (warm_boot) gc_nvram_seed_warm_boot(machine.get());

    gc_machine_reset(machine.get());
    if (watch_set) {
        machine->bus.watch_enabled = true;
        machine->bus.watch_hit = false;
        machine->bus.watch_address = watch_address;
    }
    standalone::Sm8521Cpu cpu(*machine);
    std::cout << "Game.com firmware boot probe\n"
              << "Reset PC: 0x1020; firmware loaded; cartridge: "
              << (argc >= 3 && std::string(argv[2]) != "-" ? argv[2] : "none") << '\n';
    for (std::uint16_t address : breakpoints)
        std::cout << "Execution breakpoint: 0x" << std::hex << std::setw(4)
                  << std::setfill('0') << address << std::dec << '\n';
    if (break_on_cart) std::cout << "Execution breakpoint: first cartridge instruction\n";
    if (watch_set)
        std::cout << "Write watchpoint: 0x" << std::hex << std::setw(4)
                  << std::setfill('0') << watch_address << std::dec << '\n';
    if (trace_boot) std::cout << "Boot handoff tracing enabled\n";
    for (const TouchEvent& event : touch_events)
        std::cout << "Scheduled stylus press: step " << event.start << ", column "
                  << event.column << ", row " << event.row << ", duration "
                  << event.duration << " steps\n";
    for (const ButtonEvent& event : button_events)
        std::cout << "Scheduled button press: " << button_name(event.button)
                  << ", step " << event.start << ", duration " << event.duration << " steps\n";

    constexpr std::uint64_t trace_count = 48;
    std::array<TraceEntry, 32> tail{};
    std::size_t tail_pos = 0;
    std::uint64_t steps = 0;
    bool debugger_stop = false;
    std::string debug_stop_reason;
    bool previous_fetch_cart = false;
    std::uint8_t previous_fetch_bank = 0;
    std::uint8_t previous_fetch_slot = 0;
    while (steps < instruction_limit) {
        for (TouchEvent& event : touch_events) {
            if (event.active && steps == event.end) {
                (void)gc_input_set_touch(machine.get(), false, 0, 0);
                event.active = false;
            }
            if (!event.active && steps == event.start) {
                (void)gc_input_set_touch(machine.get(), true, event.column, event.row);
                event.active = true;
            }
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

        const std::uint16_t pc = cpu.pc();
        unsigned fetch_window = 0;
        std::uint8_t fetch_bank = 0;
        const bool fetch_cart = is_cartridge_fetch(*machine, pc, fetch_window, fetch_bank);
        if ((trace_boot || break_on_cart) && fetch_cart &&
            (!previous_fetch_cart || previous_fetch_bank != fetch_bank ||
             previous_fetch_slot != machine->bus.active_slot)) {
            std::cout << "CART FETCH at step " << steps << ", instruction "
                      << cpu.instructions() << ", cycle " << cpu.cycles()
                      << ": PC=0x" << std::hex << std::setw(4) << std::setfill('0') << pc
                      << " window=MMU" << std::dec << fetch_window
                      << " bank=0x" << std::hex << std::setw(2)
                      << static_cast<unsigned>(fetch_bank)
                      << " slot=" << std::dec << static_cast<unsigned>(machine->bus.active_slot)
                      << " ROM-offset=0x" << std::hex << std::setw(6)
                      << ((static_cast<unsigned>(fetch_bank) << 13) | (pc & 0x1FFFu))
                      << std::setfill(' ') << '\n';
        }
        previous_fetch_cart = fetch_cart;
        if (fetch_cart) {
            previous_fetch_bank = fetch_bank;
            previous_fetch_slot = machine->bus.active_slot;
        }

        bool hit_breakpoint = false;
        for (std::uint16_t address : breakpoints) {
            if (pc == address) {
                std::ostringstream reason;
                reason << "PC breakpoint at 0x" << std::hex << std::setw(4)
                       << std::setfill('0') << pc;
                debug_stop_reason = reason.str();
                hit_breakpoint = true;
                break;
            }
        }
        if (!hit_breakpoint && break_on_cart && fetch_cart) {
            std::ostringstream reason;
            reason << "first cartridge instruction at PC 0x" << std::hex
                   << std::setw(4) << std::setfill('0') << pc;
            debug_stop_reason = reason.str();
            hit_breakpoint = true;
        }
        if (hit_breakpoint) {
            debugger_stop = true;
            break;
        }

        const std::array<std::uint8_t, 5> old_mmu = {
            machine->bus.mapped_bank[0], machine->bus.mapped_bank[1],
            machine->bus.mapped_bank[2], machine->bus.mapped_bank[3],
            machine->bus.mapped_bank[4]
        };
        const std::uint8_t old_p3 = machine->bus.pio_output_latch[3];
        const bool old_boot_enabled = machine->bus.boot_enabled;
        const bool old_cart_active = machine->bus.cart_active;
        const std::uint8_t old_slot = machine->bus.active_slot;
        machine->bus.watch_hit = false;
        const std::uint64_t instructions_before = cpu.instructions();
        const std::uint8_t opcode = gc_bus_read(machine.get(), pc);
        std::array<char, 128> captured_disassembly{};
        if (trace_boot || break_on_cart || watch_set || !breakpoints.empty())
            (void)gc_disassemble(machine.get(), pc, captured_disassembly.data(),
                                 captured_disassembly.size());
        const std::uint64_t before = cpu.cycles();
        (void)cpu.step();
        ++steps;

        if (trace_boot) {
            bool changed = false;
            for (unsigned i = 0; i < old_mmu.size(); ++i) {
                if (old_mmu[i] == machine->bus.mapped_bank[i]) continue;
                if (!changed) {
                    std::cout << "BOOT STATE at step " << steps << ", cycle " << cpu.cycles()
                              << ", PC 0x" << std::hex << std::setw(4) << std::setfill('0')
                              << pc << " -> 0x" << std::setw(4) << cpu.pc() << std::dec << ":\n";
                    changed = true;
                }
                std::cout << "  MMU" << i << " bank 0x" << std::hex << std::setw(2)
                          << static_cast<unsigned>(old_mmu[i]) << " -> 0x" << std::setw(2)
                          << static_cast<unsigned>(machine->bus.mapped_bank[i]) << std::dec << '\n';
            }
            if (old_p3 != machine->bus.pio_output_latch[3]) {
                if (!changed) {
                    std::cout << "BOOT STATE at step " << steps << ", cycle " << cpu.cycles()
                              << ", PC 0x" << std::hex << std::setw(4) << std::setfill('0')
                              << pc << " -> 0x" << std::setw(4) << cpu.pc() << std::dec << ":\n";
                    changed = true;
                }
                std::cout << "  P3 latch 0x" << std::hex << std::setw(2)
                          << static_cast<unsigned>(old_p3) << " -> 0x" << std::setw(2)
                          << static_cast<unsigned>(machine->bus.pio_output_latch[3])
                          << std::dec << '\n';
            }
            if (old_boot_enabled != machine->bus.boot_enabled) {
                if (!changed) {
                    std::cout << "BOOT STATE at step " << steps << ", cycle " << cpu.cycles()
                              << ", PC 0x" << std::hex << std::setw(4) << std::setfill('0')
                              << pc << " -> 0x" << std::setw(4) << cpu.pc() << std::dec << ":\n";
                    changed = true;
                }
                std::cout << "  internal boot overlay "
                          << (old_boot_enabled ? "enabled" : "disabled") << " -> "
                          << (machine->bus.boot_enabled ? "enabled" : "disabled") << '\n';
            }
            if (old_cart_active != machine->bus.cart_active || old_slot != machine->bus.active_slot) {
                if (!changed) {
                    std::cout << "BOOT STATE at step " << steps << ", cycle " << cpu.cycles()
                              << ", PC 0x" << std::hex << std::setw(4) << std::setfill('0')
                              << pc << " -> 0x" << std::setw(4) << cpu.pc() << std::dec << ":\n";
                    changed = true;
                }
                std::cout << "  cartridge select slot " << static_cast<unsigned>(old_slot)
                          << " (" << (old_cart_active ? "active" : "inactive") << ") -> slot "
                          << static_cast<unsigned>(machine->bus.active_slot) << " ("
                          << (machine->bus.cart_active ? "active" : "inactive") << ")\n";
            }
            if (changed) std::cout << std::setfill(' ');
        }

        if (machine->bus.watch_hit) {
            std::ostringstream reason;
            reason << "write watchpoint at 0x" << std::hex << std::setw(4)
                   << std::setfill('0') << machine->bus.watch_address;
            debug_stop_reason = reason.str();
            std::cout << "WATCH WRITE at step " << steps << ", instruction "
                      << instructions_before << ", PC=0x" << std::hex << std::setw(4)
                      << std::setfill('0') << pc << ": [0x" << std::setw(4)
                      << machine->bus.watch_address << "] 0x" << std::setw(2)
                      << static_cast<unsigned>(machine->bus.watch_old_value) << " -> 0x"
                      << std::setw(2) << static_cast<unsigned>(machine->bus.watch_new_value)
                      << std::dec << std::setfill(' ') << '\n';
            debugger_stop = true;
        }
        if (cpu.instructions() == instructions_before) { /* HALT idle cycle */
            if (debugger_stop) break;
            continue;
        }
        TraceEntry entry{};
        entry.instruction = cpu.instructions() - 1;
        entry.pc = pc;
        entry.opcode = opcode;
        entry.next_pc = cpu.pc();
        entry.cycles = cpu.cycles() - before;
        entry.disassembly = captured_disassembly;
        tail[tail_pos++ % tail.size()] = entry;
        if (cpu.instructions() <= trace_count) {
            std::cout << "  " << std::dec << std::setw(5) << entry.instruction
                      << "  PC=" << std::hex << std::setw(4) << std::setfill('0') << entry.pc
                      << " op=" << std::setw(2) << static_cast<unsigned>(entry.opcode)
                      << " -> " << std::setw(4) << entry.next_pc
                      << "  +" << std::dec << entry.cycles << " cyc\n"
                      << std::setfill(' ');
        }
        if (debugger_stop) break;
    }
    for (TouchEvent& event : touch_events) {
        if (event.active) {
            (void)gc_input_set_touch(machine.get(), false, 0, 0);
            event.active = false;
        }
    }
    for (ButtonEvent& event : button_events) {
        if (event.active) gc_input_set_button(machine.get(), event.button, false);
    }
    if (debugger_stop) {
        char disassembly[128] = {};
        (void)gc_disassemble(machine.get(), cpu.pc(), disassembly, sizeof(disassembly));
        std::cout << "Debugger stopped: " << debug_stop_reason << "\n"
                  << "Next PC 0x" << std::hex << std::setw(4) << std::setfill('0')
                  << cpu.pc() << "  " << disassembly << std::dec << std::setfill(' ') << '\n';
    }
    if (cpu.halted() || debugger_stop) {
        std::cout << (debugger_stop ? "Recent instructions before debugger stop:\n"
                                   : "Recent instructions before HALT:\n");
        const std::size_t stored = static_cast<std::size_t>(cpu.instructions() < tail.size()
                                  ? cpu.instructions() : tail.size());
        const std::size_t begin = tail_pos >= stored ? tail_pos - stored : 0;
        for (std::size_t i = 0; i < stored; ++i) {
            const auto& entry = tail[(begin + i) % tail.size()];
            char disassembly[128] = {};
            if (entry.disassembly[0] != '\0')
                std::snprintf(disassembly, sizeof(disassembly), "%s", entry.disassembly.data());
            else
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
              << "  P3-latch=0x" << std::setw(2)
              << static_cast<unsigned>(machine->bus.pio_output_latch[3])
              << "  slot=" << std::dec << static_cast<unsigned>(machine->bus.active_slot)
              << "  cart-active=" << (machine->bus.cart_active ? "yes" : "no") << '\n'
              << "MMU0-4: " << std::hex;
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
    if (debugger_stop)
        std::cout << "Stopped by debugger: " << debug_stop_reason << '\n';
    if (dump_set) {
        std::cout << "Memory dump at 0x" << std::hex << std::setw(4)
                  << std::setfill('0') << dump_address << std::dec << ":\n";
        for (std::uint32_t offset = 0; offset < dump_length; offset += 16u) {
            const std::uint32_t row_length = (dump_length - offset < 16u)
                ? dump_length - offset : 16u;
            std::cout << std::hex << std::setw(4) << std::setfill('0')
                      << (static_cast<std::uint32_t>(dump_address) + offset) << ":";
            for (std::uint32_t i = 0; i < row_length; ++i) {
                const auto address = static_cast<std::uint16_t>(dump_address + offset + i);
                std::cout << ' ' << std::setw(2)
                          << static_cast<unsigned>(gc_bus_debug_read(machine.get(), address));
            }
            std::cout << std::dec << std::setfill(' ') << '\n';
        }
    }

    if (cpu.halted()) {
        std::cout << "CPU reached HALT.\n";
    } else if (steps >= instruction_limit) {
        std::cout << "Step budget reached; the CPU had not entered HALT.\n";
    }
    std::cout << "Caveat: frame-level LCD rendering, basic controller/touch-matrix input, and preliminary"
                 " sound synthesis are implemented; scanline timing/races, physical touch mapping, and"
                 " retail audio fidelity remain unverified. A short scripted playfield frame is not proof of full compatibility.\n";
    if (nvram_path && !gc_nvram_save(machine.get(), nvram_path, error, sizeof(error))) {
        std::cerr << "Could not save NVRAM image: " << error << '\n';
        return 1;
    }
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
