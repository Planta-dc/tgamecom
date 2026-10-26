#include "gamecom.h"
#include "gamecom_demo.h"
#include "sm8521_cpu.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

static void test_rom_to_vram_dma()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine != nullptr);
    gc_machine_init(machine);
    machine->bus.system_rom[0] = 0x40; /* One pixel with shade 1 in the top two bits. */
    machine->bus.reg_file[0x34] = 0x83; /* Start, overwrite, ROM -> VRAM. */
    machine->bus.reg_file[0x3B] = 0xE4; /* Identity 2-bit palette. */
    const std::uint8_t halt = 0xF1;
    machine->bus.patch[0x1020] = halt;
    machine->bus.patch_valid[0x1020] = 1;

    standalone::Sm8521Cpu cpu(*machine);
    assert(!cpu.step()); /* HALT enters the MAME-style DMA wait path. */
    assert(cpu.dma_transfers() == 0);
    assert((machine->bus.reg_file[0x34] & 0x80u) != 0);
    assert(!cpu.step()); /* DMA executes while halted, then asserts its interrupt. */
    assert(machine->bus.vram[0] == 0x40);
    assert((machine->bus.reg_file[0x34] & 0x80u) == 0);
    assert(cpu.dma_transfers() == 1);
    assert(cpu.step()); /* Pending DMA request wakes the CPU. */
    assert((machine->bus.reg_file[0x12] & 0x80u) != 0); /* DMA request visible in IR0. */
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
    std::cout << "Standalone C++ SM8521 core ran the VRAM demo and ROM-to-VRAM DMA test.\n";
    return 0;
}
