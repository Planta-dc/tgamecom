#include "gamecom.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t breakpoints[GC_MAX_BREAKPOINTS];
static size_t breakpoint_count;

static bool parse_hex(const char *text, unsigned long *value)
{
    if (!text || !*text) return false;
    char *end = NULL;
    int base = 16;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) base = 0;
    unsigned long v = strtoul(text, &end, base);
    if (!end || *end != '\0') return false;
    *value = v;
    return true;
}

static bool at_breakpoint(uint16_t pc)
{
    for (size_t i = 0; i < breakpoint_count; ++i)
        if (breakpoints[i] == pc) return true;
    return false;
}

static void show_regs(GcMachine *m)
{
    printf("PC=%04X  SP=%04X  PS0=%02X  PS1=%02X  SYS=%02X  cycles=%llu  insns=%llu\n",
           m->cpu.pc, gc_cpu_sp(m), m->bus.reg_file[0x1E], m->bus.reg_file[0x1F],
           m->bus.reg_file[0x19], (unsigned long long)m->cpu.cycles,
           (unsigned long long)m->cpu.instructions);
    for (unsigned row = 0; row < 4; ++row) {
        printf("R%02u..R%02u:", row * 4, row * 4 + 3);
        for (unsigned col = 0; col < 4; ++col)
            printf(" %02X", gc_cpu_reg_read(m, row * 4 + col));
        putchar('\n');
    }
}

static void show_memory(GcMachine *m, uint16_t start, unsigned count)
{
    if (count > 0x1000u) count = 0x1000u;
    for (unsigned i = 0; i < count; i += 16) {
        uint16_t a = (uint16_t)(start + i);
        printf("%04X:", a);
        for (unsigned j = 0; j < 16 && i + j < count; ++j)
            printf(" %02X", gc_bus_debug_read(m, (uint16_t)(a + j)));
        putchar('\n');
    }
}

static void show_disassembly(GcMachine *m, uint16_t start, unsigned count)
{
    uint16_t pc = start;
    if (count > 256u) count = 256u;
    for (unsigned i = 0; i < count; ++i) {
        char text[128];
        size_t len = gc_disassemble(m, pc, text, sizeof(text));
        printf("%04X  %02X", pc, gc_bus_debug_read(m, pc));
        if (len > 1) printf(" %02X", gc_bus_debug_read(m, (uint16_t)(pc + 1)));
        if (len > 2) printf(" %02X", gc_bus_debug_read(m, (uint16_t)(pc + 2)));
        printf("  %s\n", text);
        pc = (uint16_t)(pc + (len ? len : 1));
    }
}

static bool do_step(GcMachine *m, bool print_instruction)
{
    char text[128];
    uint16_t pc = m->cpu.pc;
    if (print_instruction) {
        (void)gc_disassemble(m, pc, text, sizeof(text));
        printf("%04X  %-44s", pc, text);
    }
    m->bus.watch_hit = false;
    GcStepResult result = gc_cpu_step(m);
    if (print_instruction) {
        if (result == GC_STEP_UNSUPPORTED || result == GC_STEP_FAULT)
            printf("  !! %s", m->cpu.fault_message);
        if (m->bus.watch_hit)
            printf("  [watch %04X: %02X -> %02X]", m->bus.watch_address,
                   m->bus.watch_old_value, m->bus.watch_new_value);
        putchar('\n');
    }
    return result == GC_STEP_OK;
}

static void print_help(void)
{
    puts("Commands:");
    puts("  help                         Show this help");
    puts("  regs                         Show PC, flags, SP and R0-R15");
    puts("  step [count]                 Execute instruction(s), showing each");
    puts("  run [count]                  Run up to count instructions (default 10000)");
    puts("  disasm [address] [count]     Disassemble (default current PC, 16)");
    puts("  mem <address> [count]        Inspect memory (default 64 bytes)");
    puts("  setpc <address>              Set PC (hexadecimal)");
    puts("  break <address> | breaks     Add/list execution breakpoints");
    puts("  watch <address> | watch off  Watch writes to one logical address");
    puts("  load <file.hex>              Load Intel HEX into a logical test overlay");
    puts("  loadbin <file> <address>     Load a raw test image at a logical address");
    puts("  rom <file.bin>               Load a cartridge image (up to 2 MiB)");
    puts("  boot <file.bin>              Load a 4 KiB boot ROM image");
    puts("  reset                        Reset CPU/registers, preserve loaded images");
    puts("  resume                       Clear HALT/STOP/unsupported-opcode stop");
    puts("  quit                         Exit");
    puts("Addresses are hexadecimal; prefix 0x is optional.");
}

static void report_load(bool ok, const char *error)
{
    if (ok) puts("Loaded.");
    else printf("Load failed: %s\n", error[0] ? error : "unknown error");
}

static void monitor(GcMachine *m)
{
    char line[512];
    print_help();
    for (;;) {
        printf("gc> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;
        for (char *p = cmd; *p; ++p) *p = (char)tolower((unsigned char)*p);
        char *arg1 = strtok(NULL, " \t\r\n");
        char *arg2 = strtok(NULL, " \t\r\n");
        char *arg3 = strtok(NULL, " \t\r\n");
        unsigned long x = 0, y = 0;

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) break;
        else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "h") == 0) print_help();
        else if (strcmp(cmd, "regs") == 0 || strcmp(cmd, "r") == 0) show_regs(m);
        else if (strcmp(cmd, "reset") == 0) { gc_machine_reset(m); puts("Reset to PC=1020h."); }
        else if (strcmp(cmd, "resume") == 0) { gc_cpu_clear_stop(m); puts("Execution stop cleared."); }
        else if (strcmp(cmd, "setpc") == 0 && parse_hex(arg1, &x) && x <= 0xFFFFu) {
            m->cpu.pc = (uint16_t)x; gc_cpu_clear_stop(m); printf("PC=%04X\n", m->cpu.pc);
        }
        else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            unsigned long n = 1;
            if (arg1 && (!parse_hex(arg1, &n) || n == 0)) { puts("Bad count."); continue; }
            if (n > 100000u) n = 100000u;
            for (unsigned long i = 0; i < n; ++i) if (!do_step(m, true)) break;
            show_regs(m);
        }
        else if (strcmp(cmd, "run") == 0 || strcmp(cmd, "g") == 0) {
            unsigned long limit = 10000;
            if (arg1 && (!parse_hex(arg1, &limit) || limit == 0)) { puts("Bad count."); continue; }
            if (limit > 10000000u) limit = 10000000u;
            gc_cpu_clear_stop(m);
            unsigned long done = 0;
            while (done < limit) {
                if (at_breakpoint(m->cpu.pc)) { printf("Breakpoint at %04X\n", m->cpu.pc); break; }
                bool keep_running = do_step(m, false);
                done++;
                if (!keep_running) break;
                if (m->bus.watch_hit) {
                    printf("Watchpoint %04X: %02X -> %02X\n", m->bus.watch_address,
                           m->bus.watch_old_value, m->bus.watch_new_value);
                    break;
                }
            }
            printf("Stopped after %lu instruction(s).\n", done);
            if (m->cpu.faulted) printf("CPU: %s at %04X\n", m->cpu.fault_message, m->cpu.fault_pc);
            show_regs(m);
        }
        else if (strcmp(cmd, "disasm") == 0 || strcmp(cmd, "d") == 0) {
            unsigned long address = m->cpu.pc, count = 16;
            if (arg1 && !parse_hex(arg1, &address)) { puts("Bad address."); continue; }
            if (arg2 && !parse_hex(arg2, &count)) { puts("Bad count."); continue; }
            if (address > 0xFFFFu) { puts("Address out of range."); continue; }
            show_disassembly(m, (uint16_t)address, (unsigned)count);
        }
        else if (strcmp(cmd, "mem") == 0 || strcmp(cmd, "m") == 0) {
            if (!parse_hex(arg1, &x) || x > 0xFFFFu) { puts("Usage: mem <hex-address> [count]"); continue; }
            y = 64;
            if (arg2 && !parse_hex(arg2, &y)) { puts("Bad count."); continue; }
            show_memory(m, (uint16_t)x, (unsigned)y);
        }
        else if (strcmp(cmd, "breaks") == 0) {
            for (size_t i = 0; i < breakpoint_count; ++i) printf("%02zu: %04X\n", i, breakpoints[i]);
        }
        else if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0) {
            if (!parse_hex(arg1, &x) || x > 0xFFFFu) { puts("Usage: break <hex-address>"); continue; }
            if (breakpoint_count == GC_MAX_BREAKPOINTS) { puts("Breakpoint table full."); continue; }
            breakpoints[breakpoint_count++] = (uint16_t)x; printf("Breakpoint set at %04lX\n", x);
        }
        else if (strcmp(cmd, "watch") == 0) {
            if (arg1 && strcmp(arg1, "off") == 0) { m->bus.watch_enabled = false; puts("Watchpoint disabled."); }
            else if (parse_hex(arg1, &x) && x <= 0xFFFFu) {
                m->bus.watch_address = (uint16_t)x; m->bus.watch_enabled = true;
                printf("Watching writes to %04lX\n", x);
            } else puts("Usage: watch <hex-address> | watch off");
        }
        else if (strcmp(cmd, "load") == 0 && arg1) {
            char err[128]; report_load(gc_load_intel_hex(m, arg1, err, sizeof(err)), err);
        }
        else if (strcmp(cmd, "loadbin") == 0 && arg1 && parse_hex(arg2, &x) && x <= 0xFFFFu) {
            char err[128]; report_load(gc_load_binary_patch(m, arg1, (uint16_t)x, err, sizeof(err)), err);
        }
        else if (strcmp(cmd, "rom") == 0 && arg1) {
            char err[128]; report_load(gc_load_cartridge(m, arg1, err, sizeof(err)), err);
        }
        else if (strcmp(cmd, "boot") == 0 && arg1) {
            char err[128]; report_load(gc_load_boot_rom(m, arg1, err, sizeof(err)), err);
        }
        else if (strcmp(cmd, "help") != 0) puts("Unknown command; type help.");
        (void)arg3;
    }
}

static void usage(const char *argv0)
{
    printf("Usage: %s [--hex file.hex] [--bin file addr] [--rom file.bin] [--boot file.bin] [--pc addr]\n", argv0);
}

int main(int argc, char **argv)
{
    GcMachine *m = (GcMachine *)calloc(1, sizeof(*m));
    if (!m) { fputs("Out of memory.\n", stderr); return 1; }
    gc_machine_init(m);
    char error[160] = { 0 };
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); free(m); return 0; }
        else if (strcmp(argv[i], "--hex") == 0 && i + 1 < argc) {
            if (!gc_load_intel_hex(m, argv[++i], error, sizeof(error))) { fprintf(stderr, "%s\n", error); free(m); return 1; }
        }
        else if (strcmp(argv[i], "--bin") == 0 && i + 2 < argc) {
            const char *path = argv[++i]; unsigned long a;
            if (!parse_hex(argv[++i], &a) || a > 0xFFFFu || !gc_load_binary_patch(m, path, (uint16_t)a, error, sizeof(error))) {
                fprintf(stderr, "--bin: %s\n", error[0] ? error : "bad address"); free(m); return 1;
            }
        }
        else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            if (!gc_load_cartridge(m, argv[++i], error, sizeof(error))) { fprintf(stderr, "%s\n", error); free(m); return 1; }
        }
        else if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            if (!gc_load_boot_rom(m, argv[++i], error, sizeof(error))) { fprintf(stderr, "%s\n", error); free(m); return 1; }
        }
        else if (strcmp(argv[i], "--pc") == 0 && i + 1 < argc) {
            unsigned long a;
            if (!parse_hex(argv[++i], &a) || a > 0xFFFFu) { usage(argv[0]); free(m); return 1; }
            m->cpu.pc = (uint16_t)a;
        }
        else { usage(argv[0]); free(m); return 1; }
    }
    printf("Tiger Game.com / SM8521 prototype debugger (C11)\n");
    puts("Prototype scope: CPU subset + bus/debug monitor; not yet a game-compatible emulator.");
    monitor(m);
    free(m);
    return 0;
}
