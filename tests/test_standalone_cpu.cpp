#include "gamecom.h"
#include "gamecom_demo.h"
#include "sm8521_cpu.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

static void load_patch(GcMachine *m, uint16_t addr, const std::vector<std::uint8_t>& bytes)
{
    std::memcpy(m->bus.patch + addr, bytes.data(), bytes.size());
    std::memset(m->bus.patch_valid + addr, 1, bytes.size());
}

static void test_rom_to_vram_dma()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine != nullptr);
    gc_machine_init(machine);
    machine->bus.system_rom[0] = 0x40; /* One pixel with shade 1 in the top two bits. */
    machine->bus.reg_file[0x34] = 0x83; /* Start, overwrite, ROM -> VRAM. */
    machine->bus.reg_file[0x3B] = 0xE4; /* Identity 2-bit palette. */
    load_patch(machine, 0x1020, {0xF1, 0xFF}); /* HALT; NOP */

    standalone::Sm8521Cpu cpu(*machine);
    assert(!cpu.step()); /* HALT enters DMA wait path. */
    assert(cpu.dma_transfers() == 0);
    assert((machine->bus.reg_file[0x34] & 0x80u) != 0);
    const std::uint64_t before_dma = cpu.cycles();
    assert(!cpu.step()); /* DMA executes while halted, then asserts its interrupt. */
    assert(cpu.cycles() - before_dma == 6u);
    assert(machine->bus.vram[0] == 0x40);
    assert((machine->bus.reg_file[0x34] & 0x80u) == 0);
    assert(cpu.dma_transfers() == 1);
    assert(cpu.step()); /* Pending DMA request wakes the CPU. */
    assert((machine->bus.reg_file[0x12] & 0x80u) != 0); /* DMA request visible in IR0. */

    /* Full-screen 200x160 aligned ROM->VRAM overwrite accounts for pipelined
     * hardware bus cycles (160 rows * (2 + 50 * 2) = 16,320 phi0 cycles). */
    gc_machine_init(machine);
    machine->bus.reg_file[0x31] = 0x27; /* LCH[5]=1: 50 bytes/row (200 dots). */
    machine->bus.reg_file[0x34] = 0x83; /* Start, overwrite, ROM -> VRAM. */
    machine->bus.reg_file[0x37] = 199u; /* DMDX = 200 - 1 */
    machine->bus.reg_file[0x38] = 159u; /* DMDY = 160 - 1 */
    machine->bus.reg_file[0x3B] = 0xE4;
    load_patch(machine, 0x1020, {0xF1});
    cpu.reset();
    assert(!cpu.step());
    const std::uint64_t fs_before = cpu.cycles();
    assert(!cpu.step());
    assert(cpu.cycles() - fs_before == 16320u);

    /* Slot 2 (P3=0x80) ROM->VRAM DMA blits from rom2 rather than Slot 1 rom. */
    gc_machine_init(machine);
    machine->bus.cart_present = true;
    machine->bus.cart2_present = true;
    machine->bus.rom[0x80000] = 0x11u;
    machine->bus.rom2[0x80000] = 0xCCu; /* 16 KiB bank 0x20 -> 0x80000 */
    gc_bus_write(machine, 0x17, 0x80u); /* Select Slot 2 */
    machine->bus.reg_file[0x34] = 0x83u;
    machine->bus.reg_file[0x3B] = 0xE4u;
    machine->bus.reg_file[0x3C] = 0x20u;
    machine->bus.reg_file[0x37] = 3u;
    machine->bus.reg_file[0x38] = 0u;
    load_patch(machine, 0x1020, {0xF1});
    cpu.reset();
    assert(!cpu.step());
    assert(!cpu.step());
    assert(machine->bus.vram[0] == 0xCCu);

    std::free(machine);
}

static void test_decoupled_vblank_and_clock_hz()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine != nullptr);
    gc_machine_init(machine);

    standalone::Sm8521Cpu cpu(*machine);
    cpu.set_external_vblank(true);
    cpu.set_clock_hz(200000u);
    assert(cpu.external_vblank());
    assert(cpu.clock_hz() == 200000u);

    /* Enable 1-second CLKT via MOV CLKT,#0x80 at 0x1020 followed by loop. */
    load_patch(machine, 0x1020, {0x58, 0x80, 0x54, 0xD8, 0xFE});

    cpu.reset();
    while (cpu.cycles() < 200500u) (void)cpu.step();
    /* With external VBlank enabled, stepping >66,240 cycles must NOT auto-fire VBlank. */
    assert(cpu.vblank_ticks() == 0u);
    assert((machine->bus.reg_file[0x32] & 0x80u) == 0u);
    /* With clock_hz = 200,000, CLKT must tick once after 200,000 cycles. */
    assert(cpu.clock_ticks() == 1u);

    cpu.trigger_vblank();
    assert(cpu.vblank_ticks() == 1u);
    /* LCV[7] reports active VBlank during the 7 * 320 = 2240 phi0 cycle window. */
    assert((machine->bus.reg_file[0x32] & 0x80u) != 0u);
    (void)cpu.step();
    assert((machine->bus.reg_file[0x12] & 0x01u) != 0); /* VBlank pending in IR0. */

    const std::uint64_t after_vb = cpu.cycles();
    while (cpu.cycles() < after_vb + 2400u) (void)cpu.step();
    assert((machine->bus.reg_file[0x32] & 0x80u) == 0u); /* LCV[7] clears after VBlank lines. */

    std::free(machine);
}

static void test_hardware_timers_and_sticky_irqs()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine != nullptr);
    gc_machine_init(machine);

    /* Vector 0x1012 (TIM1) -> 0x1100 ISR:
     *   0x1100: INC R4 (0x08 0x04)
     *   0x1102: IRET   (0xF9)
     * Main at 0x1020 (with I=0 initially!):
     *   MOV SP_L, #0xFF   (0x58 0xFF 0x1D)
     *   MOV IE1, #0x40    (0x58 0x40 0x11)  -- enable TIM1
     *   MOV TM1D, #0x02   (0x58 0x02 0x53)
     *   MOV TM1C, #0x81   (0x58 0x81 0x52)  -- tap 1 = 1024 phi0 ticks -> 2048 ticks total
     *   HALT              (0xF1)            -- wakes when TIM1 fires even with I=0!
     *   INC R5            (0x08 0x05)       -- runs BEFORE vector because I=0
     *   EI                (0xFD)            -- unmasks I -> sticky TIM1 must vector now!
     *   INC R6            (0x08 0x06)       -- runs after IRET (1-instruction deferral check)
     *   HALT              (0xF1)
     */
    load_patch(machine, 0x1012, {0x11, 0x00});
    load_patch(machine, 0x1100, {0x08, 0x04, 0xF9});
    load_patch(machine, 0x1020, {
        0x58, 0xFF, 0x1D,
        0x58, 0x40, 0x11,
        0x58, 0x02, 0x53,
        0x58, 0x81, 0x52,
        0xF1,
        0x08, 0x05,
        0xFD,
        0x08, 0x06,
        0xF1
    });

    standalone::Sm8521Cpu cpu(*machine);
    cpu.set_external_vblank(true);
    for (int i = 0; i < 4; ++i) assert(cpu.step());
    const std::uint64_t start_wait = cpu.cycles();
    assert(!cpu.step()); /* Enters HALT. */

    /* Step while halted until TIM1 wakes HALT; the waking step() clears HALT and
     * immediately executes the following instruction (INC R5) because I=0. */
    while (cpu.halted()) (void)cpu.step();
    const std::uint64_t elapsed = cpu.cycles() - start_wait;
    assert(elapsed >= 2040u && elapsed <= 2060u);
    assert((machine->bus.reg_file[0x13] & 0x40u) != 0u); /* IR1[6] latched while I=0. */
    assert(gc_cpu_reg_read(machine, 4) == 0u); /* Vector not taken yet (I=0). */
    assert(gc_cpu_reg_read(machine, 5) == 1u); /* INC R5 ran immediately on wake. */

    assert(cpu.step()); /* EI sets FLAG_I. */
    assert(gc_cpu_reg_read(machine, 4) == 0u);
    /* Next step takes sticky TIM1 vector -> executes INC R4 at 0x1100! */
    assert(cpu.step());
    assert(gc_cpu_reg_read(machine, 4) == 1u);

    /* Queue another enabled interrupt (LCDC VBlank) right before IRET to verify
     * IRET's 1-instruction deferral lets INC R6 run before re-entering an ISR. */
    load_patch(machine, 0x100E, {0x11, 0x10}); /* LCDC vector -> 0x1110: INC R7; IRET */
    load_patch(machine, 0x1110, {0x08, 0x07, 0xF9});
    machine->bus.reg_file[0x10] = 0x01u; /* Enable LCDC IRQ in IE0. */
    cpu.trigger_vblank();

    assert(cpu.step()); /* IRET restores PC to INC R6 and sets 1-instruction deferral. */
    assert(gc_cpu_reg_read(machine, 6) == 0u);
    assert(gc_cpu_reg_read(machine, 7) == 0u);
    assert(cpu.step()); /* Deferred: INC R6 runs BEFORE LCDC ISR! */
    assert(gc_cpu_reg_read(machine, 6) == 1u);
    assert(gc_cpu_reg_read(machine, 7) == 0u);
    assert(cpu.step()); /* Now LCDC ISR runs INC R7! */
    assert(gc_cpu_reg_read(machine, 7) == 1u);
    assert(cpu.step()); /* IRET from LCDC ISR -> returns to trailing HALT. */
    assert(!cpu.step()); /* Trailing HALT. */

    std::free(machine);
}

static void test_indirect_addressing_and_ckc_stop()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine != nullptr);
    gc_machine_init(machine);

    /* Test fixed MAME bugs:
     * 1. 0x5B indirect MOV immediate in modes 0x00 (@Rn), 0x40 (@Rn+), 0x80 (d8(Rn)), 0xC0 (@-Rn)
     * 2. 0x28 MOV r, -(r) pre-decrement and 8-bit address wrap
     * 3. CKC (0x1A) FCPUEN + STOP (0xF0) committing CPU core prescaler */
    load_patch(machine, 0x1020, {
        0x58, 0x80, 0x01,       /* MOV R1, #0x80 (points into IRAM 0x80) */
        0x58, 0xEE, 0x80,       /* Seed 0x80 with 0xEE (old MAME bug would write to 0xEE!) */
        0x5B, 0x01, 0x11,       /* MOV @R1, #0x11 -> [0x80] = 0x11 */
        0x5B, 0x41, 0x22,       /* MOV @R1+, #0x22 -> [0x80] = 0x22, R1 = 0x81 */
        0x5B, 0x81, 0x02, 0x33, /* MOV 2(R1), #0x33 -> [0x83] = 0x33 */
        0x5B, 0xC1, 0x44,       /* MOV @-R1, #0x44 -> R1 = 0x80, [0x80] = 0x44 */
        0x58, 0x84, 0x02,       /* MOV R2, #0x84 */
        0x28, 0xDA,             /* MOV R3, -(R2) -> R2 = 0x83, R3 = [0x83] = 0x33 */
        0x58, 0x98, 0x1A,       /* MOV CKC, #0x98 (FCPUEN=1, FCPUS=011 -> fCK/4 = 2x) */
        0xF0,                   /* STOP -> commits CKC prescale = 2 */
        0xFF,                   /* NOP -> 2 * 2 = 4 phi0 cycles */
        0xF1                    /* HALT */
    });

    standalone::Sm8521Cpu cpu(*machine);
    cpu.set_external_vblank(true);
    while (!cpu.halted() && cpu.instructions() < 32u) {
        (void)cpu.step();
    }
    assert(cpu.halted());
    assert(gc_bus_read(machine, 0x0080) == 0x44u);
    assert(gc_bus_read(machine, 0x0083) == 0x33u);
    assert(gc_bus_read(machine, 0x00EE) == 0x00u); /* Must NOT clobber 0xEE via double-indirection! */
    assert(gc_cpu_reg_read(machine, 1) == 0x80u);
    assert(gc_cpu_reg_read(machine, 2) == 0x83u);
    assert(gc_cpu_reg_read(machine, 3) == 0x33u);
    assert(cpu.cpu_prescale() == 2u);

    /* Test CKC=0xA3 (FCPUS=100 -> 1x full speed), hardware STOP sleep + Power wake,
       and host RTC BCD mirror seeding at 0x0124..0x0129 on CLKT enable. */
    gc_machine_init(machine);
    load_patch(machine, 0x1020, {
        0x58, 0xA3, 0x1A, /* MOV CKC, #0xA3 (FCPUEN=1, FCPUS=100 -> 1x) */
        0xF0,             /* STOP -> enters STOP sleep until Power button pressed */
        0x58, 0x80, 0x54, /* MOV CLKT, #0x80 -> seeds 0x0124..0x0129 BCD RTC */
        0xF1              /* HALT */
    });
    cpu.reset();
    cpu.set_stop_mode_enabled(true);
    cpu.set_rtc_override(26, 10, 27, 14, 35, 9);
    assert(cpu.step());  /* MOV CKC, #0xA3 */
    assert(!cpu.step()); /* STOP enters stopped state and commits prescale=1 */
    assert(cpu.stopped());
    assert(cpu.cpu_prescale() == 1u);
    assert(!cpu.step()); /* Remains stopped while Power button is released */
    gc_input_set_button(machine, GC_BUTTON_POWER, true);
    assert(cpu.step());  /* Power wakes STOP and executes MOV CLKT, #0x80 */
    gc_input_set_button(machine, GC_BUTTON_POWER, false);
    assert(!cpu.stopped());
    assert(gc_bus_read(machine, 0x0124) == 0x26u);
    assert(gc_bus_read(machine, 0x0125) == 0x10u);
    assert(gc_bus_read(machine, 0x0126) == 0x27u);
    assert(gc_bus_read(machine, 0x0127) == 0x14u);
    assert(gc_bus_read(machine, 0x0128) == 0x35u);
    assert(gc_bus_read(machine, 0x0129) == 0x09u);

    std::free(machine);
}

int main()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine != nullptr);
    gc_machine_init(machine);

    constexpr std::size_t capacity = (200u * 40u / 2u) * 8u + 1u;
    std::vector<std::uint8_t> image(capacity);
    std::size_t length = gc_build_vram_demo(image.data(), image.size());
    assert(length == capacity);
    assert(length <= GC_PATCH_SIZE - 0x1020u);
    std::memcpy(machine->bus.patch + 0x1020u, image.data(), length);
    std::memset(machine->bus.patch_valid + 0x1020u, 1, length);

    standalone::Sm8521Cpu cpu(*machine);
    while (!cpu.halted() && cpu.instructions() < 10000u)
        (void)cpu.step();

    assert(cpu.halted());
    assert(cpu.instructions() == 8001u);
    assert(cpu.cycles() == 92002u);
    assert(machine->bus.vram[0] == 0u);
    assert(machine->bus.vram[4u * 40u + 3u] == 0x05u);
    std::free(machine);

    test_rom_to_vram_dma();
    test_decoupled_vblank_and_clock_hz();
    test_hardware_timers_and_sticky_irqs();
    test_indirect_addressing_and_ckc_stop();
    std::cout << "Standalone C++ SM8521 core passed VRAM, DMA timing, LCV[7], timers, sticky IRQs, indirect ops, and CKC tests.\n";
    return 0;
}
