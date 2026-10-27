#include "sm8521_cpu.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>

namespace standalone {
struct Sm8521CpuTestAccess {
    static uint8_t read(Sm8521Cpu& cpu, uint32_t address)
    {
        return cpu.mem_readbyte(address);
    }

    static void write(Sm8521Cpu& cpu, uint32_t address, uint8_t value)
    {
        cpu.mem_writebyte(address, value);
    }

    static void advance(Sm8521Cpu& cpu, uint64_t cycles)
    {
        cpu.m_cycles += cycles;
        cpu.advance_devices(static_cast<unsigned>(cycles));
    }
};
} // namespace standalone

int main()
{
    auto *machine = static_cast<GcMachine *>(std::calloc(1, sizeof(GcMachine)));
    assert(machine);
    gc_machine_init(machine);
    standalone::Sm8521Cpu cpu(*machine);
    using Access = standalone::Sm8521CpuTestAccess;

    assert((Access::read(cpu, 0x2D) & 0x02u) != 0); // TDRE set after reset.
    assert(Access::read(cpu, 0x2E) == 0);
    assert(!cpu.queue_uart_receive('X')); // Receiver is disabled.

    // Timer 0 input divided by the configured counter and then by the UART's
    // fixed factor of 32. With prescale 1 and TM0D=18 this is 9600 baud at
    // the emulator's 5.5296 MHz CPU clock.
    Access::write(cpu, 0x50, 0x80);
    Access::write(cpu, 0x51, 18);
    Access::write(cpu, 0x2E, 0x1C); // TE + RE + no parity, one stop bit.
    Access::write(cpu, 0x2B, 'A');
    assert((Access::read(cpu, 0x2D) & 0x02u) == 0); // TDRE clears on write.
    assert(cpu.uart_transmits().empty());

    constexpr uint64_t frame_cycles = 18u * 32u * 10u;
    Access::advance(cpu, frame_cycles - 1);
    assert(cpu.uart_transmits().empty());
    Access::advance(cpu, 1);
    assert(cpu.uart_transmits().size() == 1);
    assert(cpu.uart_transmits()[0].value == 'A');
    assert(cpu.uart_transmits()[0].cycle == frame_cycles);
    assert((Access::read(cpu, 0x2D) & 0x02u) != 0); // TDRE set after frame completion.

    assert(cpu.queue_uart_receive('Z'));
    Access::advance(cpu, 0); // Start the incoming serial frame at the current cycle.
    assert((Access::read(cpu, 0x2D) & 0x20u) != 0); // RBSY
    Access::advance(cpu, frame_cycles - 1);
    assert((Access::read(cpu, 0x2D) & 0x20u) != 0);
    Access::advance(cpu, 1);
    assert((Access::read(cpu, 0x2D) & 0x01u) != 0); // RDRF
    assert((Access::read(cpu, 0x2D) & 0x20u) == 0);
    assert(Access::read(cpu, 0x2C) == 'Z');
    assert((Access::read(cpu, 0x2D) & 0x01u) == 0); // Reading URTR clears RDRF.

    // An unread byte causes an overrun; the original receive byte is retained.
    assert(cpu.queue_uart_receive('1'));
    assert(cpu.queue_uart_receive('2'));
    Access::advance(cpu, 0);
    Access::advance(cpu, frame_cycles);
    Access::advance(cpu, frame_cycles);
    const uint8_t error_status = Access::read(cpu, 0x2D);
    assert((error_status & 0x11u) == 0x11u); // OR and RDRF.
    assert(Access::read(cpu, 0x2C) == '1');
    assert((Access::read(cpu, 0x2D) & 0x10u) == 0); // Reading URTS clears OR.

    // The UART remains reachable through the selected register-file window.
    Access::write(cpu, 0x1E, 0x28);
    Access::write(cpu, 0x06, 0x1C); // RP 0x28 + R6 -> URTC 0x2E.
    assert(Access::read(cpu, 0x06) == 0x1C);

    std::free(machine);
    std::cout << "SM8521 UART tests passed (registers, 9600-baud timing, RX/TX, overrun).\n";
    return 0;
}
