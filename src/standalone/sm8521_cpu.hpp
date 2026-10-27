#ifndef GAMECOM_STANDALONE_SM8521_CPU_HPP
#define GAMECOM_STANDALONE_SM8521_CPU_HPP

#include "gamecom.h"
#include "sound_events.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace standalone {

struct UartTransmitByte {
    uint64_t cycle = 0;
    uint8_t value = 0;
};

// Standalone SM8521 execution core. It uses the Game.com bus API but has no
// dependency on MAME's device framework or runtime.
class Sm8521Cpu {
public:
    explicit Sm8521Cpu(GcMachine& machine);

    void reset();
    bool step();
    bool halted() const noexcept { return m_halted != 0; }
    uint16_t pc() const noexcept { return m_PC; }
    uint64_t cycles() const noexcept { return m_cycles; }
    uint64_t instructions() const noexcept { return m_instructions; }
    uint64_t dma_transfers() const noexcept { return m_dma_transfers; }
    uint64_t clock_ticks() const noexcept { return m_clock_ticks; }
    uint64_t vblank_ticks() const noexcept { return m_vblank_ticks; }
    const std::vector<SoundRegisterWrite>& sound_writes() const noexcept { return m_sound_writes; }
    void clear_sound_writes() noexcept { m_sound_writes.clear(); }
    const std::vector<UartTransmitByte>& uart_transmits() const noexcept { return m_uart_transmits; }
    void clear_uart_transmits() noexcept { m_uart_transmits.clear(); }
    bool queue_uart_receive(uint8_t value);
    void resume() noexcept { m_halted = 0; }

private:
    friend struct Sm8521CpuTestAccess;

    static constexpr uint8_t FLAG_C = 0x80;
    static constexpr uint8_t FLAG_Z = 0x40;
    static constexpr uint8_t FLAG_S = 0x20;
    static constexpr uint8_t FLAG_V = 0x10;
    static constexpr uint8_t FLAG_D = 0x08;
    static constexpr uint8_t FLAG_H = 0x04;
    static constexpr uint8_t FLAG_B = 0x02;
    static constexpr uint8_t FLAG_I = 0x01;

    uint8_t mem_readbyte(uint32_t address);
    void mem_writebyte(uint32_t address, uint8_t value);
    uint16_t mem_readword(uint32_t address);
    void mem_writeword(uint32_t address, uint16_t value);
    void load_sfr_state();
    void get_sp();
    void process_interrupts();
    void take_interrupt(uint16_t vector);
    void request_interrupt(unsigned line);
    void advance_devices(unsigned cycles);
    void advance_uart();
    void uart_write_data(uint8_t value);
    void uart_write_control(uint8_t value);
    uint8_t uart_read_register(uint8_t address);
    uint64_t uart_frame_cycles() const noexcept;
    void start_uart_tx();
    void perform_dma();

    struct TimerState {
        bool enabled = false;
        uint8_t upcounter_max = 0;
        uint64_t prescale_count = 0;
        uint32_t prescale_max = 1;
    };

    GcMachine& m_machine;
    uint16_t m_PC = 0;
    uint16_t m_SP = 0;
    uint8_t m_SYS = 0;
    uint8_t m_CKC = 0;
    uint8_t m_PS0 = 0;
    uint8_t m_PS1 = 0;
    uint8_t m_clock_changed = 0;
    uint8_t m_halted = 0;
    uint8_t m_register_ram[0x108]{};
    uint16_t m_IFLAGS = 0;
    bool m_check_interrupts = false;
    TimerState m_timers[2]{};
    uint64_t m_next_vblank = 92573; /* MAME's ~59.732 Hz screen timing, in CPU cycles. */
    uint64_t m_next_clock_tick = 0;
    uint64_t m_clock_tick_period = 5529600;
    uint64_t m_cycles = 0;
    uint64_t m_instructions = 0;
    uint64_t m_dma_transfers = 0;
    uint64_t m_clock_ticks = 0;
    uint64_t m_vblank_ticks = 0;
    uint8_t m_uart_tx_data = 0xFF;
    uint8_t m_uart_rx_data = 0;
    uint8_t m_uart_status = 0x42;
    uint8_t m_uart_control = 0;
    bool m_uart_tx_pending = false;
    bool m_uart_tx_active = false;
    uint8_t m_uart_tx_shift = 0xFF;
    uint64_t m_uart_tx_complete_cycle = 0;
    bool m_uart_rx_active = false;
    uint8_t m_uart_rx_shift = 0;
    uint64_t m_uart_rx_complete_cycle = 0;
    std::deque<uint8_t> m_uart_rx_queue;
    std::vector<UartTransmitByte> m_uart_transmits;
    std::vector<SoundRegisterWrite> m_sound_writes;
};

} // namespace standalone

#endif
