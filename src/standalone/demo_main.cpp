#include "gamecom.h"
#include "gamecom_demo.h"
#include "sm8521_cpu.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::uint8_t pixel(const GcMachine& machine, unsigned x, unsigned y)
{
    const std::uint8_t packed = machine.bus.vram[x * 40u + (y >> 2)];
    return static_cast<std::uint8_t>((packed >> (6u - 2u * (y & 3u))) & 3u);
}

int main()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    if (!machine) {
        std::fputs("Out of memory.\n", stderr);
        return 1;
    }
    gc_machine_init(machine);

    constexpr std::size_t program_capacity = (200u * 40u / 2u) * 8u + 1u;
    std::vector<std::uint8_t> program(program_capacity);
    const std::size_t program_size = gc_build_vram_demo(program.data(), program.size());
    if (!program_size || program_size > GC_PATCH_SIZE - 0x1020u) {
        std::fputs("Could not build standalone demo program.\n", stderr);
        std::free(machine);
        return 1;
    }
    std::memcpy(machine->bus.patch + 0x1020u, program.data(), program_size);
    std::memset(machine->bus.patch_valid + 0x1020u, 1, program_size);

    standalone::Sm8521Cpu cpu(*machine);
    while (!cpu.halted() && cpu.instructions() < 20000u)
        (void)cpu.step();
    if (!cpu.halted()) {
        std::fprintf(stderr, "CPU stopped unexpectedly at %04X.\n", cpu.pc());
        std::free(machine);
        return 1;
    }

    std::printf("Independent C++ SM8521 core: %llu instructions, %llu cycles.\n",
                static_cast<unsigned long long>(cpu.instructions()),
                static_cast<unsigned long long>(cpu.cycles()));
    std::puts("Emulated 200x160 VRAM, downsampled to terminal:");
    static constexpr char shades[] = " .o#";
    for (unsigned y = 0; y < 160u; y += 4u) {
        for (unsigned x = 0; x < 200u; x += 2u) {
            const unsigned shade = (pixel(*machine, x, y) + pixel(*machine, x + 1u, y) + 1u) / 2u;
            std::putchar(shades[shade]);
        }
        std::putchar('\n');
    }
    std::free(machine);
    return 0;
}
