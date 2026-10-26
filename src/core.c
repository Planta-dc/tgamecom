#include "gamecom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RF_PS0 0x1Eu
#define RF_PS1 0x1Fu
#define RF_SYS 0x19u
#define RF_MMU0 0x24u
#define SYS_SP16 0x40u

static uint8_t *reg_ptr(GcMachine *m, uint16_t address)
{
    if (address < 0x80u) return &m->bus.reg_file[address];
    if (address < 0x400u) return &m->bus.ram[address - 0x80u];
    return NULL;
}

static uint8_t read_mapped_bank(const GcMachine *m, unsigned window, unsigned offset)
{
    const size_t physical = (size_t)m->bus.mapped_bank[window] * 0x2000u + offset;
    if (m->bus.mapped_is_cart[window]) return m->bus.rom[physical];
    return m->bus.system_rom[physical];
}

static void update_mmu_mapping(GcMachine *m, unsigned window, uint8_t bank)
{
    if (window >= 5u) return;
    if (bank < 0x20u) {
        m->bus.mapped_bank[window] = bank;
        m->bus.mapped_is_cart[window] = false;
    } else if (m->bus.cart_active) {
        m->bus.mapped_bank[window] = bank;
        m->bus.mapped_is_cart[window] = true;
    }
}

static void update_pio_input_matrix(GcMachine *m)
{
    static const uint16_t touch_mux[13] = {
        0xFFFB, 0xFFF7, 0xFFEF, 0xFFDF, 0xFFBF, 0xFF7F,
        0xFEFF, 0xFDFF, 0xFBFF, 0xF7FF, 0xEFFF, 0xDFFF, 0xBFFF
    };
    const uint16_t mux = (uint16_t)(m->bus.pio_output_latch[1] |
                                    ((uint16_t)m->bus.pio_output_latch[2] << 8));
    if (mux == 0x7FFFu) { /* Buttons: arrows/menu/pause/sound/A, then B/C. */
        m->bus.reg_file[0x14] = m->bus.input_buttons[0];
        m->bus.reg_file[0x15] = (uint8_t)((m->bus.reg_file[0x15] & 0xFCu) |
                                          (m->bus.input_buttons[1] & 0x03u));
        return;
    }
    if (mux == 0xFFFFu) { /* Power/reset and button D. */
        m->bus.reg_file[0x14] = (uint8_t)((m->bus.reg_file[0x14] & 0xFCu) |
                                          (m->bus.input_buttons[2] & 0x03u));
        m->bus.reg_file[0x15] = 0xFF;
        return;
    }
    for (unsigned column = 0; column < 13u; ++column) {
        if (mux != touch_mux[column]) continue;
        uint16_t touched_rows = 0;
        if (m->bus.touch_active && m->bus.touch_column == column && m->bus.touch_row < 10u)
            touched_rows = (uint16_t)(1u << m->bus.touch_row);
        const uint16_t stylus_y = (uint16_t)(touched_rows ^ 0x03FFu);
        m->bus.reg_file[0x14] = (uint8_t)stylus_y;
        m->bus.reg_file[0x15] = (uint8_t)((m->bus.reg_file[0x15] & 0xFCu) |
                                          ((stylus_y >> 8) & 0x03u));
        return;
    }
}

void gc_input_set_button(GcMachine *machine, GcButton button, bool down)
{
    unsigned group;
    uint8_t mask;
    switch (button) {
    case GC_BUTTON_UP: group = 0; mask = 0x01; break;
    case GC_BUTTON_DOWN: group = 0; mask = 0x02; break;
    case GC_BUTTON_LEFT: group = 0; mask = 0x04; break;
    case GC_BUTTON_RIGHT: group = 0; mask = 0x08; break;
    case GC_BUTTON_MENU: group = 0; mask = 0x10; break;
    case GC_BUTTON_PAUSE: group = 0; mask = 0x20; break;
    case GC_BUTTON_SOUND: group = 0; mask = 0x40; break;
    case GC_BUTTON_A: group = 0; mask = 0x80; break;
    case GC_BUTTON_B: group = 1; mask = 0x01; break;
    case GC_BUTTON_C: group = 1; mask = 0x02; break;
    case GC_BUTTON_POWER: group = 2; mask = 0x01; break;
    case GC_BUTTON_D: group = 2; mask = 0x02; break;
    default: return;
    }
    if (down) machine->bus.input_buttons[group] &= (uint8_t)~mask;
    else machine->bus.input_buttons[group] |= mask;
    update_pio_input_matrix(machine);
}

bool gc_input_set_touch(GcMachine *machine, bool down, unsigned column, unsigned row)
{
    if (down && (column >= 13u || row >= 10u)) return false;
    machine->bus.touch_active = down;
    if (down) {
        machine->bus.touch_column = (uint8_t)column;
        machine->bus.touch_row = (uint8_t)row;
    }
    update_pio_input_matrix(machine);
    return true;
}

static uint8_t raw_map_read(GcMachine *m, uint16_t address, bool debugger_view)
{
    if (m->bus.patch_valid[address]) return m->bus.patch[address];
    if (address < 0x400u) {
        uint8_t *p = reg_ptr(m, address);
        return p ? *p : 0;
    }
    if (address < 0x1000u) return 0;
    if (address < 0x2000u) {
        if (m->bus.boot_enabled)
            return m->bus.boot_rom[address - 0x1000u];
        return read_mapped_bank(m, 0, (unsigned)address & 0x1FFFu);
    }
    if (address < 0xA000u) {
        const unsigned window = (unsigned)address >> 13;
        return read_mapped_bank(m, window, (unsigned)address & 0x1FFFu);
    }
    if (address < 0xE000u) {
        if (debugger_view) return m->bus.vram[address - 0xA000u];
        return 0; /* Best-supported CPU-visible behavior: VRAM reads return 00h. */
    }
    if (address < 0xFF00u) return m->bus.ext_ram[address - 0xE000u];
    return m->bus.ext_io[address & 0xFFu];
}

void gc_machine_init(GcMachine *machine)
{
    memset(machine, 0, sizeof(*machine));
    memset(machine->bus.rom, 0xFF, sizeof(machine->bus.rom));
    memset(machine->bus.system_rom, 0xFF, sizeof(machine->bus.system_rom));
    memset(machine->bus.boot_rom, 0xFF, sizeof(machine->bus.boot_rom));
    gc_machine_reset(machine);
}

void gc_machine_reset(GcMachine *machine)
{
    memset(&machine->cpu, 0, sizeof(machine->cpu));
    memset(machine->bus.reg_file, 0, sizeof(machine->bus.reg_file));
    machine->bus.reg_file[0x14] = 0xFF; /* P0/P1 reset values from MAME reference. */
    machine->bus.reg_file[0x15] = 0xFF;
    machine->bus.reg_file[0x16] = 0xFF;
    machine->bus.reg_file[0x17] = 0x00;
    machine->bus.pio_output_latch[0] = 0xFF;
    machine->bus.pio_output_latch[1] = 0xFF;
    machine->bus.pio_output_latch[2] = 0xFF;
    machine->bus.pio_output_latch[3] = 0x00;
    memset(machine->bus.input_buttons, 0xFF, sizeof(machine->bus.input_buttons));
    machine->bus.touch_active = false;
    machine->bus.touch_column = 0;
    machine->bus.touch_row = 0;
    machine->bus.reg_file[0x30] = 0xB0; /* LCDC reset defaults from MAME reference. */
    machine->bus.reg_file[0x31] = 0x07; /* LCH: default 200-dot timing. */
    machine->bus.reg_file[0x32] = 0x27; /* LCV: default visible/blank timing. */
    machine->bus.reg_file[RF_PS0] = 0;
    machine->bus.reg_file[RF_PS1] = 0; /* I=0; other flags are undocumented at reset. */
    machine->bus.reg_file[RF_SYS] = 0;
    machine->bus.reg_file[0x2B] = 0xFF; /* UART timer/reset defaults from MAME. */
    machine->bus.reg_file[0x2D] = 0x42;
    machine->bus.reg_file[0x5F] = 0x38;
    machine->bus.boot_enabled = true;
    machine->bus.cart_active = false;
    memset(machine->bus.mapped_bank, 0, sizeof(machine->bus.mapped_bank));
    memset(machine->bus.mapped_is_cart, 0, sizeof(machine->bus.mapped_is_cart));
    machine->bus.watch_hit = false;
    machine->cpu.pc = 0x1020u;
}

uint8_t gc_bus_read(GcMachine *machine, uint16_t address)
{
    return raw_map_read(machine, address, false);
}

uint8_t gc_bus_debug_read(GcMachine *machine, uint16_t address)
{
    return raw_map_read(machine, address, true);
}

void gc_bus_write(GcMachine *machine, uint16_t address, uint8_t value)
{
    uint8_t old_value = raw_map_read(machine, address, true);
    uint8_t *p = reg_ptr(machine, address);
    if (p) {
        *p = value;
        if (address == RF_MMU0) {
            machine->bus.boot_enabled = false;
            update_mmu_mapping(machine, 0, value);
        } else if (address >= RF_MMU0 + 1u && address <= RF_MMU0 + 4u) {
            update_mmu_mapping(machine, address - RF_MMU0, value);
        } else if (address >= 0x14u && address <= 0x17u) {
            machine->bus.pio_output_latch[address - 0x14u] = value;
            if (address == 0x15u || address == 0x16u)
                update_pio_input_matrix(machine);
            else if (address == 0x17u)
                machine->bus.cart_active = machine->bus.cart_present &&
                    (((value & 0xC0u) == 0x40u) || ((value & 0xC0u) == 0x80u));
        }
    } else if (address >= 0xA000u && address < 0xE000u) {
        machine->bus.vram[address - 0xA000u] = value;
    } else if (address >= 0xE000u && address < 0xFF00u) {
        machine->bus.ext_ram[address - 0xE000u] = value;
    } else if (address >= 0xFF00u) {
        machine->bus.ext_io[address & 0xFFu] = value;
    }
    if (machine->bus.watch_enabled && machine->bus.watch_address == address) {
        machine->bus.watch_hit = true;
        machine->bus.watch_old_value = old_value;
        machine->bus.watch_new_value = value;
    }
}

uint8_t gc_bus_vram_dma_read(const GcMachine *machine, uint16_t address)
{
    return machine->bus.vram[address & (GC_VRAM_SIZE - 1u)];
}

uint8_t gc_cpu_reg_read(GcMachine *machine, unsigned index)
{
    uint16_t base = (uint16_t)(machine->bus.reg_file[RF_PS0] & 0xF8u);
    return gc_bus_read(machine, (uint16_t)(base + (index & 0x0Fu)));
}

void gc_cpu_reg_write(GcMachine *machine, unsigned index, uint8_t value)
{
    uint16_t base = (uint16_t)(machine->bus.reg_file[RF_PS0] & 0xF8u);
    gc_bus_write(machine, (uint16_t)(base + (index & 0x0Fu)), value);
}

uint16_t gc_cpu_sp(const GcMachine *machine)
{
    return (uint16_t)(((uint16_t)machine->bus.reg_file[0x1Cu] << 8) |
                      machine->bus.reg_file[0x1Du]);
}

void gc_cpu_set_sp(GcMachine *machine, uint16_t value)
{
    machine->bus.reg_file[0x1Cu] = (uint8_t)(value >> 8);
    machine->bus.reg_file[0x1Du] = (uint8_t)value;
    if ((machine->bus.reg_file[RF_SYS] & SYS_SP16) == 0)
        machine->bus.reg_file[0x1Cu] = 0;
}

static uint8_t fetch8(GcMachine *m)
{
    uint8_t value = gc_bus_read(m, m->cpu.pc);
    m->cpu.pc++;
    return value;
}

static uint16_t fetch16le(GcMachine *m)
{
    uint8_t lo = fetch8(m);
    uint8_t hi = fetch8(m);
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static uint16_t fetch16be(GcMachine *m)
{
    uint8_t hi = fetch8(m);
    uint8_t lo = fetch8(m);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static const uint8_t pair_base[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };

static uint16_t read_pair(GcMachine *m, unsigned pair)
{
    unsigned base = pair_base[pair & 7u];
    return (uint16_t)(((uint16_t)gc_cpu_reg_read(m, base) << 8) |
                      gc_cpu_reg_read(m, base + 1u));
}

static void write_pair(GcMachine *m, unsigned pair, uint16_t value)
{
    unsigned base = pair_base[pair & 7u];
    gc_cpu_reg_write(m, base, (uint8_t)(value >> 8));
    gc_cpu_reg_write(m, base + 1u, (uint8_t)value);
}

static void push8(GcMachine *m, uint8_t value)
{
    uint16_t sp = (uint16_t)(gc_cpu_sp(m) - 1u);
    if ((m->bus.reg_file[RF_SYS] & SYS_SP16) == 0) sp &= 0x00FFu;
    gc_cpu_set_sp(m, sp);
    gc_bus_write(m, sp, value);
}

static uint8_t pop8(GcMachine *m)
{
    uint16_t sp = gc_cpu_sp(m);
    uint8_t value = gc_bus_read(m, sp);
    sp++;
    if ((m->bus.reg_file[RF_SYS] & SYS_SP16) == 0) sp &= 0x00FFu;
    gc_cpu_set_sp(m, sp);
    return value;
}

static uint8_t ps1(const GcMachine *m) { return m->bus.reg_file[RF_PS1]; }
static void set_ps1(GcMachine *m, uint8_t v) { m->bus.reg_file[RF_PS1] = v; }

static void flags_add8(GcMachine *m, uint8_t a, uint8_t b, uint16_t result)
{
    uint8_t r = (uint8_t)result;
    uint8_t f = (uint8_t)(ps1(m) & (GC_FLAG_B | GC_FLAG_I));
    if (result > 0xFFu) f |= GC_FLAG_C;
    if (r == 0) f |= GC_FLAG_Z;
    if (r & 0x80u) f |= GC_FLAG_S;
    if (((uint8_t)(~(a ^ b)) & (uint8_t)(a ^ r) & 0x80u) != 0) f |= GC_FLAG_V;
    if (((a ^ b ^ r) & 0x10u) != 0) f |= GC_FLAG_H;
    set_ps1(m, f);
}

static void flags_sub8(GcMachine *m, uint8_t a, uint8_t b, uint16_t result, bool compare)
{
    uint8_t r = (uint8_t)result;
    uint8_t preserve = compare ? (GC_FLAG_B | GC_FLAG_I | GC_FLAG_H | GC_FLAG_D)
                              : (GC_FLAG_B | GC_FLAG_I);
    uint8_t f = (uint8_t)(ps1(m) & preserve);
    if (result > 0xFFu) f |= GC_FLAG_C;
    if (r == 0) f |= GC_FLAG_Z;
    if (r & 0x80u) f |= GC_FLAG_S;
    if ((((a ^ b) & (a ^ r)) & 0x80u) != 0) f |= GC_FLAG_V;
    if (!compare) {
        f |= GC_FLAG_D;
        if (((a ^ b ^ r) & 0x10u) != 0) f |= GC_FLAG_H;
    }
    set_ps1(m, f);
}

static void flags_incdec8(GcMachine *m, uint8_t before, uint8_t after, bool decrement)
{
    uint8_t f = (uint8_t)(ps1(m) & (GC_FLAG_C | GC_FLAG_D | GC_FLAG_H | GC_FLAG_B | GC_FLAG_I));
    if (after == 0) f |= GC_FLAG_Z;
    if (after & 0x80u) f |= GC_FLAG_S;
    if (!decrement && before == 0x7Fu) f |= GC_FLAG_V;
    if (decrement && before == 0x80u) f |= GC_FLAG_V;
    set_ps1(m, f);
}

static GcStepResult unsupported(GcMachine *m, uint16_t pc, uint8_t opcode)
{
    m->cpu.faulted = true;
    m->cpu.fault_pc = pc;
    m->cpu.fault_opcode = opcode;
    (void)snprintf(m->cpu.fault_message, sizeof(m->cpu.fault_message),
                   "opcode %02X is not implemented in this prototype", opcode);
    return GC_STEP_UNSUPPORTED;
}

GcStepResult gc_cpu_step(GcMachine *machine)
{
    if (machine->cpu.faulted) return GC_STEP_FAULT;
    if (machine->cpu.halted) return GC_STEP_HALTED;
    if (machine->cpu.stopped) return GC_STEP_STOPPED;

    uint16_t instruction_pc = machine->cpu.pc;
    uint8_t op = fetch8(machine);
    uint8_t a, b, old, result8;
    uint16_t result16, target;
    uint64_t spent = 0;

    switch (op) {
    case 0x00: /* CLR register-file byte */
        a = fetch8(machine);
        gc_bus_write(machine, a, 0);
        spent = 4;
        break;
    case 0x08: /* INC register-file byte */
        a = fetch8(machine); old = gc_bus_read(machine, a); result8 = (uint8_t)(old + 1u);
        flags_incdec8(machine, old, result8, false); gc_bus_write(machine, a, result8); spent = 4;
        break;
    case 0x09: /* DEC register-file byte */
        a = fetch8(machine); old = gc_bus_read(machine, a); result8 = (uint8_t)(old - 1u);
        flags_incdec8(machine, old, result8, true); gc_bus_write(machine, a, result8); spent = 4;
        break;
    case 0x2D: /* MAME fallback for undocumented DM opcode. */
    case 0x3D:
        spent = 4;
        break;
    case 0x3B: { /* MOVW @address,RR ; word store to VRAM or another memory area */
        uint8_t mode = fetch8(machine);
        unsigned address_pair = mode & 7u;
        unsigned source_pair = (mode >> 3) & 7u;
        uint16_t address;
        if ((mode & 0xC0u) == 0x80u) {
            address = fetch16be(machine);
            if (address_pair != 0u) address = (uint16_t)(address + read_pair(machine, address_pair));
            spent = address_pair != 0u ? 18u : 14u;
        } else if ((mode & 0xC0u) == 0x00u) {
            address = read_pair(machine, address_pair);
            spent = 11;
        } else if ((mode & 0xC0u) == 0x40u) {
            address = read_pair(machine, address_pair);
            write_pair(machine, address_pair, (uint16_t)(address + 2u));
            spent = 16;
        } else {
            address = (uint16_t)(read_pair(machine, address_pair) - 2u);
            write_pair(machine, address_pair, address);
            spent = 16;
        }
        uint16_t value = read_pair(machine, source_pair);
        gc_bus_write(machine, address, (uint8_t)(value >> 8));
        gc_bus_write(machine, (uint16_t)(address + 1u), (uint8_t)value);
        break;
    }
    case 0x39: { /* MOV @RR,R ; direct-absolute and pair-indirect forms */
        uint8_t mode = fetch8(machine);
        unsigned address_pair = mode & 7u;
        unsigned source_pair = (mode >> 3) & 7u;
        uint16_t address;
        if ((mode & 0xC0u) == 0x80u) {
            address = fetch16be(machine);
            if (address_pair != 0u) address = (uint16_t)(address + read_pair(machine, address_pair));
            spent = address_pair != 0u ? 11u : 15u;
        } else if ((mode & 0xC0u) == 0x00u) {
            address = read_pair(machine, address_pair);
            spent = 8;
        } else if ((mode & 0xC0u) == 0x40u) {
            address = read_pair(machine, address_pair);
            write_pair(machine, address_pair, (uint16_t)(address + 1u));
            spent = 13;
        } else {
            address = (uint16_t)(read_pair(machine, address_pair) - 1u);
            write_pair(machine, address_pair, address);
            spent = 13;
        }
        gc_bus_write(machine, address, gc_cpu_reg_read(machine, pair_base[source_pair]));
        break;
    }
    case 0x2E: /* MOV PS0,#imm */
        machine->bus.reg_file[RF_PS0] = fetch8(machine); spent = 4;
        break;
    case 0x40: /* CMP register-file, register-file; destination is second byte */
        b = fetch8(machine); a = fetch8(machine);
        old = gc_bus_read(machine, a); result8 = gc_bus_read(machine, b);
        result16 = (uint16_t)old - result8; flags_sub8(machine, old, result8, result16, true); spent = 6;
        break;
    case 0x41: /* ADD register-file, register-file */
        b = fetch8(machine); a = fetch8(machine);
        old = gc_bus_read(machine, a); result8 = gc_bus_read(machine, b);
        result16 = (uint16_t)old + result8; flags_add8(machine, old, result8, result16);
        gc_bus_write(machine, a, (uint8_t)result16); spent = 6;
        break;
    case 0x42: /* SUB register-file, register-file */
        b = fetch8(machine); a = fetch8(machine);
        old = gc_bus_read(machine, a); result8 = gc_bus_read(machine, b);
        result16 = (uint16_t)old - result8; flags_sub8(machine, old, result8, result16, false);
        gc_bus_write(machine, a, (uint8_t)result16); spent = 6;
        break;
    case 0x48: /* MOV register-file, register-file */
        b = fetch8(machine); a = fetch8(machine);
        gc_bus_write(machine, a, gc_bus_read(machine, b)); spent = 6;
        break;
    case 0x4B: /* MOVW RR,#word ; pair register index is the first operand byte */
        a = fetch8(machine);
        target = fetch16be(machine);
        gc_cpu_reg_write(machine, a, (uint8_t)(target >> 8));
        gc_cpu_reg_write(machine, (unsigned)a + 1u, (uint8_t)target);
        spent = 9;
        break;
    case 0x49: /* CALL absolute */
        target = fetch16le(machine);
        push8(machine, (uint8_t)machine->cpu.pc);
        push8(machine, (uint8_t)(machine->cpu.pc >> 8));
        machine->cpu.pc = target; spent = ((machine->bus.reg_file[RF_SYS] & SYS_SP16) != 0) ? 12u : 10u;
        break;
    case 0x50: /* CMP register-file,#imm */
        b = fetch8(machine); a = fetch8(machine);
        old = gc_bus_read(machine, a); result16 = (uint16_t)old - b;
        flags_sub8(machine, old, b, result16, true); spent = 6;
        break;
    case 0x51: /* ADD register-file,#imm */
        b = fetch8(machine); a = fetch8(machine);
        old = gc_bus_read(machine, a); result16 = (uint16_t)old + b;
        flags_add8(machine, old, b, result16); gc_bus_write(machine, a, (uint8_t)result16); spent = 6;
        break;
    case 0x52: /* SUB register-file,#imm */
        b = fetch8(machine); a = fetch8(machine);
        old = gc_bus_read(machine, a); result16 = (uint16_t)old - b;
        flags_sub8(machine, old, b, result16, false); gc_bus_write(machine, a, (uint8_t)result16); spent = 6;
        break;
    case 0x58: /* MOV register-file,#imm */
        b = fetch8(machine); a = fetch8(machine); gc_bus_write(machine, a, b); spent = 6;
        break;
    case 0xF0: machine->cpu.stopped = true; spent = 2; break;
    case 0xF1: machine->cpu.halted = true; spent = 2; break;
    case 0xF8: { /* RET; stack byte order follows the MAME reference core. */
        uint8_t hi = pop8(machine); uint8_t lo = pop8(machine);
        machine->cpu.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
        spent = ((machine->bus.reg_file[RF_SYS] & SYS_SP16) != 0) ? 10u : 8u;
        break;
    }
    case 0xF9: { /* IRET */
        machine->bus.reg_file[RF_PS1] = pop8(machine);
        uint8_t hi = pop8(machine); uint8_t lo = pop8(machine);
        machine->cpu.pc = (uint16_t)(((uint16_t)hi << 8) | lo);
        spent = ((machine->bus.reg_file[RF_SYS] & SYS_SP16) != 0) ? 12u : 10u;
        break;
    }
    case 0xFA: set_ps1(machine, (uint8_t)(ps1(machine) & (uint8_t)~GC_FLAG_C)); spent = 2; break;
    case 0xFB: set_ps1(machine, (uint8_t)(ps1(machine) ^ GC_FLAG_C)); spent = 2; break;
    case 0xFC: set_ps1(machine, (uint8_t)(ps1(machine) | GC_FLAG_C)); spent = 2; break;
    case 0xFD: set_ps1(machine, (uint8_t)(ps1(machine) | GC_FLAG_I)); spent = 2; break;
    case 0xFE: set_ps1(machine, (uint8_t)(ps1(machine) & (uint8_t)~GC_FLAG_I)); spent = 2; break;
    case 0xFF: spent = 2; break; /* NOP */
    default: return unsupported(machine, instruction_pc, op);
    }

    machine->cpu.cycles += spent;
    machine->cpu.instructions++;
    return machine->cpu.halted ? GC_STEP_HALTED :
           machine->cpu.stopped ? GC_STEP_STOPPED : GC_STEP_OK;
}

void gc_cpu_clear_stop(GcMachine *machine)
{
    machine->cpu.halted = false;
    machine->cpu.stopped = false;
    machine->cpu.faulted = false;
    machine->cpu.fault_message[0] = '\0';
}

static bool parse_hex_byte(const char *p, uint8_t *value)
{
    unsigned v = 0;
    for (int i = 0; i < 2; ++i) {
        char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *value = (uint8_t)v;
    return true;
}

static void set_error(char *error, size_t size, const char *message)
{
    if (error && size) (void)snprintf(error, size, "%s", message);
}

bool gc_load_intel_hex(GcMachine *machine, const char *path, char *error, size_t error_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { set_error(error, error_size, "could not open Intel HEX file"); return false; }
    char line[1024];
    uint32_t upper = 0;
    bool eof = false;
    unsigned line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;
        if (line[0] != ':' || n < 11) { fclose(f); set_error(error, error_size, "malformed Intel HEX record"); return false; }
        uint8_t count, ah, al, type, check;
        if (!parse_hex_byte(line + 1, &count) || !parse_hex_byte(line + 3, &ah) ||
            !parse_hex_byte(line + 5, &al) || !parse_hex_byte(line + 7, &type)) {
            fclose(f); set_error(error, error_size, "invalid hex digit in Intel HEX record"); return false;
        }
        if (n < (size_t)(11u + 2u * count)) { fclose(f); set_error(error, error_size, "truncated Intel HEX record"); return false; }
        uint8_t sum = (uint8_t)(count + ah + al + type);
        uint32_t address = upper + (((uint32_t)ah << 8) | al);
        for (unsigned i = 0; i < count; ++i) {
            uint8_t byte;
            if (!parse_hex_byte(line + 9 + 2 * i, &byte)) { fclose(f); set_error(error, error_size, "invalid record data"); return false; }
            sum = (uint8_t)(sum + byte);
            if (type == 0x00) {
                if (address <= 0xFFFFu) {
                    machine->bus.patch[address] = byte;
                    machine->bus.patch_valid[address] = 1;
                } else if (address < GC_ROM_SIZE) {
                    machine->bus.rom[address] = byte;
                } else { fclose(f); set_error(error, error_size, "Intel HEX address exceeds supported memory"); return false; }
            }
            address++;
        }
        if (!parse_hex_byte(line + 9 + 2 * count, &check) || (uint8_t)(sum + check) != 0) {
            fclose(f); set_error(error, error_size, "Intel HEX checksum mismatch"); return false;
        }
        if (type == 0x01) { eof = true; break; }
        if (type == 0x02 || type == 0x04) {
            uint8_t hi, lo;
            if (count != 2 || !parse_hex_byte(line + 9, &hi) || !parse_hex_byte(line + 11, &lo)) {
                fclose(f); set_error(error, error_size, "invalid extended-address record"); return false;
            }
            uint32_t val = ((uint32_t)hi << 8) | lo;
            upper = (type == 0x02) ? (val << 4) : (val << 16);
        } else if (type != 0x00 && type != 0x03 && type != 0x05) {
            fclose(f); set_error(error, error_size, "unsupported Intel HEX record type"); return false;
        }
    }
    fclose(f);
    if (!eof) { set_error(error, error_size, "Intel HEX file has no EOF record"); return false; }
    if (error && error_size) error[0] = '\0';
    return true;
}

bool gc_load_binary_patch(GcMachine *machine, const char *path, uint16_t address,
                          char *error, size_t error_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { set_error(error, error_size, "could not open binary file"); return false; }
    size_t n = fread(machine->bus.patch + address, 1, GC_PATCH_SIZE - address, f);
    int extra = fgetc(f);
    if (ferror(f)) { fclose(f); set_error(error, error_size, "failed reading binary file"); return false; }
    if (extra != EOF) { fclose(f); set_error(error, error_size, "binary does not fit in logical 64 KiB image"); return false; }
    fclose(f);
    memset(machine->bus.patch_valid + address, 1, n);
    if (error && error_size) error[0] = '\0';
    return true;
}

static bool load_into_array(const char *path, uint8_t *dst, size_t capacity,
                            bool exact_size, char *error, size_t error_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { set_error(error, error_size, "could not open binary image"); return false; }
    size_t n = fread(dst, 1, capacity, f);
    int extra = fgetc(f);
    bool bad = ferror(f) || extra != EOF || (exact_size && n != capacity);
    fclose(f);
    if (bad) { set_error(error, error_size, exact_size ? "image has incorrect size" : "image exceeds supported size"); return false; }
    if (error && error_size) error[0] = '\0';
    return true;
}

bool gc_load_cartridge(GcMachine *machine, const char *path, char *error, size_t error_size)
{
    static const size_t supported_sizes[] = {
        0x008000u, 0x040000u, 0x080000u, 0x100000u, 0x1C0000u, 0x200000u
    };
    memset(machine->bus.rom, 0xFF, sizeof(machine->bus.rom));
    machine->bus.cart_present = false;
    machine->bus.cart_active = false;
    FILE *f = fopen(path, "rb");
    if (!f) { set_error(error, error_size, "could not open raw cartridge image"); return false; }
    size_t n = fread(machine->bus.rom, 1, sizeof(machine->bus.rom), f);
    int extra = fgetc(f);
    const bool io_error = ferror(f) != 0;
    fclose(f);
    if (io_error || extra != EOF) {
        set_error(error, error_size, "cartridge image exceeds 2 MiB or could not be read");
        return false;
    }
    bool supported = false;
    for (size_t i = 0; i < sizeof(supported_sizes) / sizeof(supported_sizes[0]); ++i)
        supported |= (n == supported_sizes[i]);
    if (!supported) {
        set_error(error, error_size,
                  "unsupported raw cartridge size; expected 32 KiB, 256/512 KiB, 1/1.75/2 MiB (not a ZIP)");
        return false;
    }

    if (n == 0x1C0000u) {
        memmove(machine->bus.rom + 0x40000u, machine->bus.rom, n);
    } else {
        if (n < 0x010000u) memcpy(machine->bus.rom + 0x008000u, machine->bus.rom, 0x008000u);
        if (n < 0x020000u) memcpy(machine->bus.rom + 0x010000u, machine->bus.rom, 0x010000u);
        if (n < 0x040000u) memcpy(machine->bus.rom + 0x020000u, machine->bus.rom, 0x020000u);
        if (n < 0x080000u) memcpy(machine->bus.rom + 0x040000u, machine->bus.rom, 0x040000u);
        if (n < 0x100000u) memcpy(machine->bus.rom + 0x080000u, machine->bus.rom, 0x080000u);
        if (n < 0x1C0000u) memcpy(machine->bus.rom + 0x100000u, machine->bus.rom, 0x100000u);
    }
    machine->bus.cart_present = true;
    if (error && error_size) error[0] = '\0';
    return true;
}

bool gc_load_system_rom(GcMachine *machine, const char *path, char *error, size_t error_size)
{
    return load_into_array(path, machine->bus.system_rom, sizeof(machine->bus.system_rom), true, error, error_size);
}

bool gc_load_boot_rom(GcMachine *machine, const char *path, char *error, size_t error_size)
{
    return load_into_array(path, machine->bus.boot_rom, sizeof(machine->bus.boot_rom), true, error, error_size);
}

bool gc_load_firmware(GcMachine *machine, const char *internal_path,
                      const char *external_path, char *error, size_t error_size)
{
    if (!gc_load_boot_rom(machine, internal_path, error, error_size)) return false;
    return gc_load_system_rom(machine, external_path, error, error_size);
}
