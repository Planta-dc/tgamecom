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

// Clean-room Sharp SM8521 CPU and on-chip peripheral model.
class Sm8521Cpu {
public:
    explicit Sm8521Cpu(GcMachine& machine);

    void reset();
    bool step();
    bool halted() const noexcept { return m_halted != 0; }
    bool stopped() const noexcept { return m_stopped != 0; }
    void set_stop_mode_enabled(bool enabled) noexcept { m_stop_mode_enabled = enabled; }
    bool stop_mode_enabled() const noexcept { return m_stop_mode_enabled; }
    void clear_stop() noexcept { m_stopped = 0; m_machine.cpu.stopped = false; }
    void set_rtc_sync(bool enabled) noexcept { m_rtc_sync_enabled = enabled; }
    bool rtc_sync() const noexcept { return m_rtc_sync_enabled; }
    void set_rtc_override(uint8_t year, uint8_t month, uint8_t day,
                          uint8_t hour, uint8_t minute, uint8_t second) noexcept;
    void clear_rtc_override() noexcept { m_rtc_override_valid = false; }
    uint16_t pc() const noexcept { return m_PC; }
    uint64_t cycles() const noexcept { return m_cycles; }
    uint64_t instructions() const noexcept { return m_instructions; }
    uint64_t dma_transfers() const noexcept { return m_dma_transfers; }
    uint64_t clock_ticks() const noexcept { return m_clock_ticks; }
    uint64_t vblank_ticks() const noexcept { return m_vblank_ticks; }
    unsigned cpu_prescale() const noexcept { return m_cpu_prescale; }
    const std::vector<SoundRegisterWrite>& sound_writes() const noexcept { return m_sound_writes; }
    void clear_sound_writes() noexcept { m_sound_writes.clear(); }
    const std::vector<UartTransmitByte>& uart_transmits() const noexcept { return m_uart_transmits; }
    void clear_uart_transmits() noexcept { m_uart_transmits.clear(); }
    bool queue_uart_receive(uint8_t value);
    bool uart_rx_idle() const noexcept
    {
        return !m_uart_rx_active && m_uart_rx_queue.empty() && (m_uart_status & 0x01u) == 0u;
    }
    void resume() noexcept { m_halted = 0; }
    void set_external_vblank(bool enabled) noexcept { m_external_vblank = enabled; }
    bool external_vblank() const noexcept { return m_external_vblank; }
    void trigger_vblank() noexcept;
    void set_clock_hz(uint32_t cpu_hz) noexcept;
    uint32_t clock_hz() const noexcept { return m_cpu_hz; }

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
    void commit_ckc_prescale() noexcept;
    void seed_host_rtc_mirror() noexcept;
    void update_lcv_status() noexcept;
    void process_interrupts();
    void take_interrupt(uint16_t vector);
    void request_interrupt(unsigned line);
    void advance_devices(unsigned cycles);
    void advance_cycles_chunked(uint64_t total_cycles);
    void advance_uart();
    void uart_write_data(uint8_t value);
    void uart_write_control(uint8_t value);
    uint8_t uart_read_register(uint8_t address);
    uint64_t uart_frame_cycles() const noexcept;
    void start_uart_tx();
    uint64_t perform_dma();

    struct TimerState {
        bool enabled = false;
        uint8_t reload = 0;
        uint8_t counter = 0;
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
    uint8_t m_lcv_cfg = 0x27;
    uint8_t m_clock_changed = 0;
    uint8_t m_halted = 0;
    uint8_t m_stopped = 0;
    bool m_stop_mode_enabled = false;
    bool m_rtc_sync_enabled = true;
    bool m_rtc_seeded = false;
    bool m_rtc_override_valid = false;
    uint8_t m_rtc_override[6]{};
    unsigned m_cpu_prescale = 1;
    uint8_t m_register_ram[0x108]{};
    uint16_t m_IFLAGS = 0;
    bool m_dma_wake_pending = false;
    bool m_irq_defer_one = false;
    TimerState m_timers[2]{};
    uint32_t m_cpu_hz = GC_CPU_HZ;
    bool m_external_vblank = false;
    uint64_t m_next_vblank = GC_LCD_NATIVE_CYCLES; /* 320 * 207 = 66,240 phi0 cycles (~75.483 Hz). */
    uint64_t m_vblank_end_cycle = 0;
    uint64_t m_next_clock_tick = 0;
    uint64_t m_clock_tick_period = GC_CPU_HZ;
    uint64_t m_cycles = 0;
    uint64_t m_instructions = 0;
    uint64_t m_dma_transfers = 0;
    uint64_t m_clock_ticks = 0;
    uint64_t m_vblank_ticks = 0;
    uint8_t m_uart_tx_data = 0xFF;
    uint8_t m_uart_rx_data = 0;
    uint8_t m_uart_status = 0x02;
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
