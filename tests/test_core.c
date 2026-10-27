#include "gamecom.h"
#include "gamecom_demo.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GcMachine *new_machine(void)
{
    GcMachine *m = (GcMachine *)calloc(1, sizeof(*m));
    assert(m != NULL);
    gc_machine_init(m);
    return m;
}

static void test_reset_and_memory_map(void)
{
    GcMachine *m = new_machine();
    assert(m->cpu.pc == 0x1020u);
    assert(m->bus.boot_enabled);
    assert(m->bus.reg_file[0x19] == 0);

    m->bus.boot_rom[0] = 0xAA;
    assert(gc_bus_read(m, 0x1000) == 0xAA);
    m->bus.system_rom[0x3000] = 0x5A; /* MMU0 bank 1, upper 4 KiB window. */
    gc_bus_write(m, 0x24, 1); /* First MMU0 write exits the boot overlay. */
    assert(!m->bus.boot_enabled);
    assert(gc_bus_read(m, 0x1000) == 0x5A);

    m->bus.system_rom[0x2000] = 0x61; /* MMU1 bank 1, 0x2000 window. */
    gc_bus_write(m, 0x25, 1);
    assert(gc_bus_read(m, 0x2000) == 0x61);
    m->bus.rom[0x40000] = 0xB2; /* Cart bank 0x20 maps its 0x40000 offset. */
    m->bus.cart_present = true;
    /* Writing a cart-range bank before P3 selects a slot latches immediately and
       reads open-bus 0xFF until P3[7:6] activates slot 1 or slot 2. */
    gc_bus_write(m, 0x25, 0x20);
    assert(gc_bus_read(m, 0x2000) == 0xFF);
    gc_bus_write(m, 0x17, 0x40); /* P3 selects cartridge slot 1. */
    assert(m->bus.active_slot == 1u);
    assert(gc_bus_read(m, 0x2000) == 0xB2);
    gc_bus_write(m, 0x17, 0x80); /* Empty slot 2 reads open-bus 0xFF. */
    assert(m->bus.active_slot == 2u);
    assert(!m->bus.cart_active);
    assert(gc_bus_read(m, 0x2000) == 0xFF);

    /* Populating cartridge slot 2 exposes independent Slot 2 ROM on P3=0x80. */
    m->bus.rom2[0x40000] = 0xC3;
    m->bus.cart2_present = true;
    gc_bus_write(m, 0x17, 0x80);
    assert(m->bus.cart_active && m->bus.active_slot == 2u);
    assert(gc_bus_read(m, 0x2000) == 0xC3);
    gc_bus_write(m, 0x17, 0x40);
    assert(m->bus.cart_active && m->bus.active_slot == 1u);
    assert(gc_bus_read(m, 0x2000) == 0xB2);
    gc_bus_write(m, 0x17, 0x00); /* Deselecting both slots returns open-bus 0xFF. */
    assert(!m->bus.cart_active && m->bus.active_slot == 0u);
    assert(gc_bus_read(m, 0x2000) == 0xFF);

    /* P3/P3C serial handshake lines (DTR=P3[0], RTS=P3[1], CTS=P3[2], DSR=P3[3]). */
    gc_bus_write(m, 0x23, 0x82); /* P3[7:6] and P3[1:0] outputs; P3[3:2] inputs. */
    gc_bus_write(m, 0x17, 0x41); /* Slot 1 active, RTS=0 (asserted low), DTR=1. */
    assert(m->bus.cart_active);
    assert(gc_serial_dtr(m) && !gc_serial_rts(m));
    gc_serial_set_inputs(m, false, true); /* CTS=0, DSR=1 */
    assert((gc_bus_read(m, 0x17) & 0x0Cu) == 0x08u);
    gc_serial_set_inputs(m, true, false); /* CTS=1, DSR=0 */
    assert((gc_bus_read(m, 0x17) & 0x0Cu) == 0x04u);

    gc_bus_write(m, 0xA012, 0xE4);
    assert(gc_bus_read(m, 0xA012) == 0x00); /* CPU-visible VRAM reads use the MAME/MiSTer default. */
    assert(gc_bus_debug_read(m, 0xA012) == 0xE4);
    assert(gc_bus_vram_dma_read(m, 0x12) == 0xE4);
    free(m);
}

static void test_register_arithmetic_and_cycles(void)
{
    GcMachine *m = new_machine();
    const uint8_t program[] = {
        0x58, 0x05, 0x02, /* MOV RF[02],#05 */
        0x51, 0x03, 0x02, /* ADD RF[02],#03 */
        0xFF              /* NOP */
    };
    memcpy(m->bus.patch + 0x1020, program, sizeof(program));
    memset(m->bus.patch_valid + 0x1020, 1, sizeof(program));

    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(gc_cpu_reg_read(m, 2) == 5);
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(gc_cpu_reg_read(m, 2) == 8);
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(m->cpu.pc == 0x1027);
    assert(m->cpu.cycles == 14);
    assert(m->cpu.instructions == 3);
    free(m);
}

static void test_flags_and_unknown_dm_fallback(void)
{
    GcMachine *m = new_machine();
    const uint8_t program[] = {
        0x58, 0x7F, 0x02, /* MOV RF[02],#7F */
        0x51, 0x01, 0x02, /* ADD RF[02],#01 => 80, S and V set */
        0x2D,             /* MAME-compatible unknown DM fallback */
        0xFF
    };
    memcpy(m->bus.patch + 0x1020, program, sizeof(program));
    memset(m->bus.patch_valid + 0x1020, 1, sizeof(program));
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(gc_cpu_reg_read(m, 2) == 0x80);
    assert((m->bus.reg_file[0x1F] & (GC_FLAG_S | GC_FLAG_V)) == (GC_FLAG_S | GC_FLAG_V));
    uint64_t before = m->cpu.cycles;
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(m->cpu.cycles - before == 4);
    assert(m->cpu.pc == 0x1027);
    free(m);
}

static void test_cpu_vram_store_instruction(void)
{
    GcMachine *m = new_machine();
    const uint8_t program[] = {
        0x58, 0xA5, 0x00,             /* MOV RF[00],#A5 */
        0x39, 0x80, 0xA0, 0x12,       /* MOV @$A012,RF[00] */
        0xF1                          /* HALT */
    };
    memcpy(m->bus.patch + 0x1020, program, sizeof(program));
    memset(m->bus.patch_valid + 0x1020, 1, sizeof(program));
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(gc_cpu_step(m) == GC_STEP_OK);
    assert(gc_bus_debug_read(m, 0xA012) == 0xA5);
    assert(gc_bus_read(m, 0xA012) == 0x00); /* CPU VRAM read remains a separate behavior. */
    assert(gc_cpu_step(m) == GC_STEP_HALTED);
    assert(m->cpu.cycles == 23);
    char disassembly[128];
    assert(gc_disassemble(m, 0x1023, disassembly, sizeof(disassembly)) == 4);
    assert(strstr(disassembly, "$A012") != NULL);
    free(m);
}

static void test_pio_controller_and_touch_inputs(void)
{
    GcMachine *m = new_machine();
    gc_input_set_button(m, GC_BUTTON_UP, true);
    gc_input_set_button(m, GC_BUTTON_A, true);
    gc_input_set_button(m, GC_BUTTON_B, true);
    gc_bus_write(m, 0x16, 0x7F); /* MAME's first controller mux: 0x7FFF. */
    assert(m->bus.reg_file[0x14] == 0x7Eu);
    assert((m->bus.reg_file[0x15] & 0x03u) == 0x02u);
    gc_input_set_button(m, GC_BUTTON_A, false);
    assert(m->bus.reg_file[0x14] == 0xFEu);
    free(m);

    m = new_machine();
    gc_input_set_button(m, GC_BUTTON_D, true);
    gc_bus_write(m, 0x16, 0xFF); /* MAME's second group: 0xFFFF. */
    assert(m->bus.reg_file[0x14] == 0xFDu);
    assert(m->bus.reg_file[0x15] == 0xFFu);
    free(m);

    m = new_machine();
    assert(!gc_input_set_touch(m, true, 13, 0));
    assert(gc_input_set_touch(m, true, 1, 5));
    assert(!m->bus.touch_strobed);
    gc_bus_write(m, 0x15, 0xFB); /* Select untouched column 0 (mux 0xFFFB). */
    assert(!m->bus.touch_strobed);
    gc_bus_write(m, 0x15, 0xF7); /* Select touched column 1 (mux 0xFFF7). */
    assert(m->bus.touch_strobed);
    assert(m->bus.reg_file[0x14] == 0xDFu); /* Active-low row 5. */
    assert((m->bus.reg_file[0x15] & 0x03u) == 0x03u);
    assert(gc_input_set_touch(m, false, 0, 0));
    assert(!m->bus.touch_strobed);
    assert(m->bus.reg_file[0x14] == 0xFFu);
    assert((m->bus.reg_file[0x15] & 0x03u) == 0x03u);

    /* Unrecognized P1/P2 scan selector preserves previous P0/P1 hold state, and
       all-ones selector (0xFFFF) updates only P0[1:0] while retaining P0[7:2]. */
    gc_input_set_button(m, GC_BUTTON_LEFT, true); /* P0[2] = 0 */
    gc_input_set_button(m, GC_BUTTON_D, true);    /* IN2[1] = 0 */
    gc_bus_write(m, 0x15, 0xFF);
    gc_bus_write(m, 0x16, 0x7F); /* Select main key row (0x7FFF). */
    assert(m->bus.reg_file[0x14] == 0xFBu);
    gc_bus_write(m, 0x16, 0x00); /* Unrecognized transient selector -> holds 0xFB. */
    assert(m->bus.reg_file[0x14] == 0xFBu);
    gc_bus_write(m, 0x16, 0xFF); /* 0xFFFF updates P0[1:0] (D=0) while keeping P0[2]=0 -> 0xF9. */
    assert(m->bus.reg_file[0x14] == 0xF9u);
    free(m);
}

static void test_nvram_and_synthesized_boot_rom(void)
{
    GcMachine *m = new_machine();
    gc_nvram_seed_warm_boot(m);
    assert(gc_bus_read(m, 0xFEE2u) == 0x01u);
    assert(gc_bus_read(m, 0xFEE3u) == 0x09u);
    assert(gc_bus_read(m, 0xFEE4u) == 0x09u);
    assert(gc_bus_read(m, 0xFEE5u) == 0x07u);
    assert(gc_bus_read(m, 0xFEE6u) == 0x00u);
    assert(gc_bus_read(m, 0xFEE7u) == 0x04u);

    gc_bus_write(m, 0xE123u, 0x5Au);
    gc_bus_write(m, 0xFF80u, 0xA5u);
    char err[128] = {0};
    const char *tmp_nvram = "/tmp/tgamecom-test-nvram.sav";
    assert(gc_nvram_save(m, tmp_nvram, err, sizeof(err)));

    GcMachine *m2 = new_machine();
    assert(gc_nvram_load(m2, tmp_nvram, err, sizeof(err)));
    remove(tmp_nvram);
    assert(gc_bus_read(m2, 0xE123u) == 0x5Au);
    assert(gc_bus_read(m2, 0xFF80u) == 0xA5u);
    assert(gc_bus_read(m2, 0xFEE5u) == 0x07u);
    free(m2);

    /* Clean-room internal boot ROM initializes MMU1/2/4, SP, SYS and jumps to 0x2054. */
    gc_machine_init(m);
    gc_synthesize_boot_rom(m);
    m->bus.system_rom[0x4054u] = 0xF1u; /* Bank 2 offset 0x0054 -> 0x2054: HALT */
    GcStepResult res = GC_STEP_OK;
    while (res == GC_STEP_OK && m->cpu.instructions < 32u)
        res = gc_cpu_step(m);
    assert(res == GC_STEP_HALTED);
    assert(m->cpu.pc == 0x2055u);
    assert(m->bus.reg_file[0x19] == 0x46u);
    assert(gc_cpu_sp(m) == 0x03C0u);
    assert(m->bus.mapped_bank[1] == 2u);
    assert(m->bus.mapped_bank[2] == 4u);
    assert(m->bus.mapped_bank[4] == 3u);
    free(m);
}

static void test_homebrew_vram_demo_runs_on_cpu(void)
{
    GcMachine *m = new_machine();
    const size_t capacity = (200u * 40u / 2u) * 8u + 1u;
    uint8_t *program = (uint8_t *)malloc(capacity);
    assert(program != NULL);
    size_t length = gc_build_vram_demo(program, capacity);
    assert(length == capacity);
    assert(length <= GC_PATCH_SIZE - 0x1020u);
    memcpy(m->bus.patch + 0x1020, program, length);
    memset(m->bus.patch_valid + 0x1020, 1, length);
    free(program);

    GcStepResult result = GC_STEP_OK;
    while (result == GC_STEP_OK && m->cpu.instructions < 20000u)
        result = gc_cpu_step(m);
    assert(result == GC_STEP_HALTED);
    assert(!m->cpu.faulted);
    assert(m->cpu.instructions == 8001u);
    assert(m->cpu.cycles == 92002u);
    assert(m->bus.vram[0] == 0x00u);
    assert(m->bus.vram[4u * 40u + 3u] == 0x05u); /* Left border pixels at x=4,y=14..15. */
    bool nonzero = false;
    for (size_t i = 0; i < GC_VRAM_SIZE; ++i) nonzero |= m->bus.vram[i] != 0;
    assert(nonzero);
    free(m);
}

int main(void)
{
    test_reset_and_memory_map();
    test_register_arithmetic_and_cycles();
    test_flags_and_unknown_dm_fallback();
    test_cpu_vram_store_instruction();
    test_pio_controller_and_touch_inputs();
    test_nvram_and_synthesized_boot_rom();
    test_homebrew_vram_demo_runs_on_cpu();
    puts("All core tests passed, including the homebrew VRAM demo.");
    return 0;
}
