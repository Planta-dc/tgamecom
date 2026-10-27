// Clean-room Sharp SM8521 CPU and on-chip peripheral model.
// Instruction decode and execution live in sm8521_ops.inc, modeling SM8521
// addressing modes, ALU flags, 5.0 MHz phi0 timers, sticky interrupts, UART
// baud timing, CKC prescaler, LCV status, sound SFR readback, and DMA blitter.
#include "sm8521_cpu.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>

namespace standalone {

static constexpr uint64_t GAMECOM_FRAME_CYCLES = GC_LCD_NATIVE_CYCLES; // 320 * 207 = 66,240 phi0 cycles

Sm8521Cpu::Sm8521Cpu(GcMachine& machine) : m_machine(machine)
{
    m_sound_writes.reserve(128);
    m_uart_transmits.reserve(64);
    reset();
}

void Sm8521Cpu::reset()
{
    m_PC = 0x1020;
    m_SP = 0;
    m_SYS = m_machine.bus.reg_file[0x19] = 0;
    m_CKC = m_machine.bus.reg_file[0x1A] = 0;
    m_PS0 = m_machine.bus.reg_file[0x1E] = 0;
    m_PS1 = m_machine.bus.reg_file[0x1F] = 0;
    m_lcv_cfg = m_machine.bus.reg_file[0x32] & 0x7Fu;
    if (m_lcv_cfg == 0) m_lcv_cfg = 0x27u;
    m_clock_changed = 0;
    m_halted = 0;
    m_stopped = 0;
    m_machine.cpu.stopped = false;
    m_rtc_seeded = false;
    m_cpu_prescale = 1; /* Start at normal fCK/2 (5.0 MHz) so direct/patched boots run full speed. */
    m_IFLAGS = 0;
    m_dma_wake_pending = false;
    m_irq_defer_one = false;
    m_timers[0] = TimerState{};
    m_timers[1] = TimerState{};
    m_next_vblank = GAMECOM_FRAME_CYCLES;
    m_vblank_end_cycle = 0;
    m_next_clock_tick = 0;
    m_clock_tick_period = m_cpu_hz;
    m_cycles = 0;
    m_instructions = 0;
    m_dma_transfers = 0;
    m_clock_ticks = 0;
    m_vblank_ticks = 0;
    m_uart_tx_data = 0xFF;
    m_uart_rx_data = 0;
    m_uart_status = 0x02; /* Hardware reset: TDRE=1 (bit 1), all other status bits 0. */
    m_uart_control = 0;
    m_uart_tx_pending = false;
    m_uart_tx_active = false;
    m_uart_tx_shift = 0xFF;
    m_uart_tx_complete_cycle = 0;
    m_uart_rx_active = false;
    m_uart_rx_shift = 0;
    m_uart_rx_complete_cycle = 0;
    m_uart_rx_queue.clear();
    m_uart_transmits.clear();
    m_sound_writes.clear();
    m_machine.bus.reg_file[0x2B] = m_uart_tx_data;
    m_machine.bus.reg_file[0x2C] = m_uart_rx_data;
    m_machine.bus.reg_file[0x2D] = m_uart_status;
    m_machine.bus.reg_file[0x2E] = m_uart_control;
    update_lcv_status();
    for (auto& byte : m_register_ram) byte = 0;
    get_sp();
}

void Sm8521Cpu::commit_ckc_prescale() noexcept
{
    /* CKC[5:3] (FCPUS) committed by STOP when CKC[7] (FCPUEN) is set:
     * 000=fCK/32 (16x), 001=fCK/16 (8x), 010=fCK/8 (4x), 011=fCK/4 (2x),
     * 100=fCK/2 (1x), 111=fXT/2 subclock (~152x phi0). 101/110 are reserved. */
    switch ((m_CKC >> 3) & 0x07u) {
    case 0u: m_cpu_prescale = 16u; break;
    case 1u: m_cpu_prescale = 8u; break;
    case 2u: m_cpu_prescale = 4u; break;
    case 3u: m_cpu_prescale = 2u; break;
    case 4u: m_cpu_prescale = 1u; break;
    case 7u: m_cpu_prescale = 152u; break;
    default: break;
    }
}

void Sm8521Cpu::set_rtc_override(uint8_t year, uint8_t month, uint8_t day,
                                 uint8_t hour, uint8_t minute, uint8_t second) noexcept
{
    m_rtc_override[0] = static_cast<uint8_t>(year % 100u);
    m_rtc_override[1] = static_cast<uint8_t>(std::clamp<unsigned>(month, 1u, 12u));
    m_rtc_override[2] = static_cast<uint8_t>(std::clamp<unsigned>(day, 1u, 31u));
    m_rtc_override[3] = static_cast<uint8_t>(std::min<unsigned>(hour, 23u));
    m_rtc_override[4] = static_cast<uint8_t>(std::min<unsigned>(minute, 59u));
    m_rtc_override[5] = static_cast<uint8_t>(std::min<unsigned>(second, 59u));
    m_rtc_override_valid = true;
}

void Sm8521Cpu::seed_host_rtc_mirror() noexcept
{
    if (!m_rtc_sync_enabled || m_rtc_seeded) return;
    m_rtc_seeded = true;

    auto to_bcd = [](unsigned val) -> uint8_t {
        val %= 100u;
        return static_cast<uint8_t>(((val / 10u) << 4) | (val % 10u));
    };

    uint8_t fields[6]{};
    if (m_rtc_override_valid) {
        for (int i = 0; i < 6; ++i) fields[i] = m_rtc_override[i];
    } else {
        const std::time_t now = std::time(nullptr);
        std::tm local_tm{};
#if defined(_WIN32)
        localtime_s(&local_tm, &now);
#else
        localtime_r(&now, &local_tm);
#endif
        fields[0] = static_cast<uint8_t>(local_tm.tm_year % 100);
        fields[1] = static_cast<uint8_t>(local_tm.tm_mon + 1);
        fields[2] = static_cast<uint8_t>(local_tm.tm_mday);
        fields[3] = static_cast<uint8_t>(local_tm.tm_hour);
        fields[4] = static_cast<uint8_t>(local_tm.tm_min);
        fields[5] = static_cast<uint8_t>(std::min(local_tm.tm_sec, 59));
    }
    /* BIOS date/time mirror lives at IRAM 0x0124..0x0129 (ram[0xA4..0xA9]). */
    for (unsigned i = 0; i < 6u; ++i)
        m_machine.bus.ram[0xA4u + i] = to_bcd(fields[i]);
}

void Sm8521Cpu::update_lcv_status() noexcept
{
    const bool lcd_on = (m_machine.bus.reg_file[0x30] & 0x80u) != 0;
    const bool in_vblank = lcd_on && (m_cycles < m_vblank_end_cycle);
    m_machine.bus.reg_file[0x32] =
        static_cast<uint8_t>((in_vblank ? 0x80u : 0x00u) | (m_lcv_cfg & 0x7Fu));
}

bool Sm8521Cpu::queue_uart_receive(uint8_t value)
{
    if ((m_uart_control & 0x08u) == 0 || m_uart_rx_queue.size() >= 4096u)
        return false;
    m_uart_rx_queue.push_back(value);
    advance_uart();
    return true;
}

void Sm8521Cpu::trigger_vblank() noexcept
{
    ++m_vblank_ticks;
    const bool lcd_on = (m_machine.bus.reg_file[0x30] & 0x80u) != 0;
    const uint64_t vb_lines = m_lcv_cfg & 0x0Fu;
    if (lcd_on && vb_lines > 0u) {
        m_vblank_end_cycle = m_cycles + vb_lines * 320ull;
        request_interrupt(5);
    } else if (!lcd_on) {
        m_vblank_end_cycle = 0;
    }
    update_lcv_status();
}

void Sm8521Cpu::set_clock_hz(uint32_t cpu_hz) noexcept
{
    if (cpu_hz < 100000u) cpu_hz = 100000u;
    if (m_cpu_hz == cpu_hz) return;
    m_cpu_hz = cpu_hz;
    if (m_next_clock_tick != 0) {
        const uint8_t clkt = m_machine.bus.reg_file[0x54];
        const uint64_t new_period = (clkt & 0x40u)
            ? 60ull * static_cast<uint64_t>(m_cpu_hz)
            : static_cast<uint64_t>(m_cpu_hz);
        m_clock_tick_period = new_period;
        if (m_next_clock_tick > m_cycles + new_period)
            m_next_clock_tick = m_cycles + new_period;
    }
}

uint64_t Sm8521Cpu::uart_frame_cycles() const noexcept
{
    if (!m_timers[0].enabled) return 0;
    const uint64_t steps = m_timers[0].reload ? m_timers[0].reload : 1u;
    const uint64_t timer_period = static_cast<uint64_t>(m_timers[0].prescale_max) * steps;
    /* Datasheet: UART baud is Timer 0 output / 32, and Timer 0's output pin
     * inverts on each compare, so one bit period is 2 * 32 = 64 compares. */
    const uint64_t bit_cycles = timer_period * 64u;
    const unsigned parity_bits = (m_uart_control & 0x04u) ? 0u : 1u;
    const unsigned stop_bits = (m_uart_control & 0x01u) ? 2u : 1u;
    return bit_cycles * (1u + 8u + parity_bits + stop_bits);
}

void Sm8521Cpu::start_uart_tx()
{
    const uint64_t frame_cycles = uart_frame_cycles();
    if (!m_uart_tx_pending || m_uart_tx_active || (m_uart_control & 0x10u) == 0 || frame_cycles == 0)
        return;
    m_uart_tx_shift = m_uart_tx_data;
    m_uart_tx_pending = false;
    m_uart_tx_active = true;
    m_uart_tx_complete_cycle = m_cycles + frame_cycles;
}

void Sm8521Cpu::uart_write_data(uint8_t value)
{
    m_uart_tx_data = value;
    m_uart_tx_pending = true;
    m_uart_status &= static_cast<uint8_t>(~0x02u); /* TDRE clears on a data write. */
    start_uart_tx();
}

void Sm8521Cpu::uart_write_control(uint8_t value)
{
    m_uart_control = value & 0x1Fu;
    if ((m_uart_control & 0x10u) == 0) {
        m_uart_tx_active = false;
        m_uart_tx_pending = false;
        m_uart_status |= 0x02u;
    } else {
        start_uart_tx();
    }
    if ((m_uart_control & 0x08u) == 0) {
        m_uart_rx_active = false;
        m_uart_rx_queue.clear();
        m_uart_status &= static_cast<uint8_t>(~0x20u); /* RBSY */
    }
}

uint8_t Sm8521Cpu::uart_read_register(uint8_t address)
{
    uint8_t value = 0xFF;
    switch (address) {
    case 0x2B: value = m_uart_tx_data; break;
    case 0x2C:
        value = m_uart_rx_data;
        m_uart_status &= static_cast<uint8_t>(~0x01u); /* RDRF clears on read. */
        break;
    case 0x2D:
        value = m_uart_status;
        m_uart_status &= static_cast<uint8_t>(~0x1Cu); /* OR, FE, PE clear on status read. */
        break;
    case 0x2E: value = m_uart_control; break;
    default: break;
    }
    m_machine.bus.reg_file[0x2B] = m_uart_tx_data;
    m_machine.bus.reg_file[0x2C] = m_uart_rx_data;
    m_machine.bus.reg_file[0x2D] = m_uart_status;
    m_machine.bus.reg_file[0x2E] = m_uart_control;
    return value;
}

uint8_t Sm8521Cpu::mem_readbyte(uint32_t address)
{
    address &= 0xFFFFu;
    if (address < 0x10u)
        return m_register_ram[address + (m_PS0 & 0xF8u)];
    if (address >= 0x2Bu && address <= 0x2Eu)
        return uart_read_register(static_cast<uint8_t>(address));
    if (address == 0x32u) {
        update_lcv_status();
        return m_machine.bus.reg_file[0x32];
    }
    if (address == 0x42u || address == 0x44u || address == 0x4Au)
        return static_cast<uint8_t>(m_machine.bus.reg_file[address] & 0x1Fu);
    if (address == 0x46u || address == 0x48u || address == 0x4Cu)
        return static_cast<uint8_t>(m_machine.bus.reg_file[address] & 0x0Fu);
    if (address == 0x4Eu)
        return 0x00u; /* SGDA is write-only. */
    if (address >= 0x60u && address <= 0x6Fu && (m_machine.bus.reg_file[0x40] & 0x01u) != 0)
        return 0x00u; /* SG0W reads 00h while SG0 is active. */
    if (address >= 0x70u && address <= 0x7Fu && (m_machine.bus.reg_file[0x40] & 0x02u) != 0)
        return 0x00u; /* SG1W reads 00h while SG1 is active. */
    return gc_bus_read(&m_machine, static_cast<uint16_t>(address));
}

void Sm8521Cpu::mem_writebyte(uint32_t address, uint8_t value)
{
    address &= 0xFFFFu;
    if (address < 0x10u) {
        m_register_ram[address + (m_PS0 & 0xF8u)] = value;
        gc_bus_write(&m_machine, static_cast<uint16_t>(address), value);
        return;
    }

    if (address >= 0x40u && address < 0x80u)
        m_sound_writes.push_back({m_cycles, static_cast<uint8_t>(address), value});

    if (address >= 0x2Bu && address <= 0x2Eu) {
        if (address == 0x2Bu) uart_write_data(value);
        else if (address == 0x2Eu) uart_write_control(value);
        gc_bus_write(&m_machine, static_cast<uint16_t>(address), value);
        m_machine.bus.reg_file[0x2B] = m_uart_tx_data;
        m_machine.bus.reg_file[0x2C] = m_uart_rx_data;
        m_machine.bus.reg_file[0x2D] = m_uart_status;
        m_machine.bus.reg_file[0x2E] = m_uart_control;
        return;
    }

    gc_bus_write(&m_machine, static_cast<uint16_t>(address), value);

    switch (address) {
    case 0x12: /* IR0: writing 0 clears matching private pending interrupt latch. */
        value &= 0xD9u;
        m_machine.bus.reg_file[0x12] = value;
        if ((value & 0x80u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 1));
        if ((value & 0x40u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 2));
        if ((value & 0x10u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 3));
        if ((value & 0x08u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 4));
        if ((value & 0x01u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 5));
        break;
    case 0x13: /* IR1: writing 0 clears matching private pending interrupt latch. */
        value &= 0x54u;
        m_machine.bus.reg_file[0x13] = value;
        if ((value & 0x40u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 6));
        if ((value & 0x10u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 7));
        if ((value & 0x04u) == 0) m_IFLAGS &= static_cast<uint16_t>(~(1u << 8));
        break;
    case 0x19: m_SYS = value; break;
    case 0x1A:
        m_CKC = value;
        m_clock_changed = 1;
        break;
    case 0x1C:
    case 0x1D: get_sp(); break;
    case 0x1E:
        m_PS0 = value;
        for (uint8_t i = 0; i < 16; ++i)
            gc_bus_write(&m_machine, i, mem_readbyte(i));
        break;
    case 0x1F: m_PS1 = value; break;
    case 0x30:
        if ((value & 0x80u) == 0) m_vblank_end_cycle = 0;
        update_lcv_status();
        break;
    case 0x32:
        m_lcv_cfg = value & 0x7Fu;
        update_lcv_status();
        break;
    case 0x50:
    case 0x52: {
        const unsigned timer = address == 0x50 ? 0u : 1u;
        /* Hardware-verified SM8521 prescalers in 5.0 MHz phi0 ticks
         * (confirmed by Tiger SDK speech/timer constants: sel 0 = 1, sel 1..7 = 1024..65536). */
        static constexpr uint32_t timer_limits[8] = {
            1u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u
        };
        m_timers[timer].enabled = (value & 0x80u) != 0;
        m_timers[timer].prescale_max = timer_limits[value & 0x07u];
        m_timers[timer].prescale_count = 0;
        m_timers[timer].counter = 0;
        m_machine.bus.reg_file[address + 1u] = 0;
        if (timer == 0) start_uart_tx();
        break;
    }
    case 0x51:
        m_timers[0].reload = value;
        m_timers[0].counter = 0;
        m_machine.bus.reg_file[address] = 0;
        start_uart_tx();
        break;
    case 0x53:
        m_timers[1].reload = value;
        m_timers[1].counter = 0;
        m_machine.bus.reg_file[address] = 0;
        break;
    case 0x54:
        if (value & 0x80u) {
            seed_host_rtc_mirror();
            m_clock_tick_period = (value & 0x40u)
                ? 60ull * static_cast<uint64_t>(m_cpu_hz)
                : static_cast<uint64_t>(m_cpu_hz);
            m_next_clock_tick = m_cycles + m_clock_tick_period;
        } else {
            m_next_clock_tick = 0;
            m_machine.bus.reg_file[address] &= 0xC0u;
        }
        break;
    default: break;
    }
}

uint16_t Sm8521Cpu::mem_readword(uint32_t address)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(mem_readbyte(address)) << 8) |
                                  mem_readbyte(address + 1u));
}

void Sm8521Cpu::mem_writeword(uint32_t address, uint16_t value)
{
    mem_writebyte(address, static_cast<uint8_t>(value >> 8));
    mem_writebyte(address + 1u, static_cast<uint8_t>(value));
}

void Sm8521Cpu::get_sp()
{
    m_SP = m_machine.bus.reg_file[0x1D];
    if (m_SYS & 0x40u)
        m_SP |= static_cast<uint16_t>(m_machine.bus.reg_file[0x1C]) << 8;
}

void Sm8521Cpu::load_sfr_state()
{
    m_SYS = m_machine.bus.reg_file[0x19];
    m_CKC = m_machine.bus.reg_file[0x1A];
    m_PS0 = m_machine.bus.reg_file[0x1E];
    m_PS1 = m_machine.bus.reg_file[0x1F];
    get_sp();
}

void Sm8521Cpu::request_interrupt(unsigned line)
{
    if (line >= 11u) return;
    m_IFLAGS |= static_cast<uint16_t>(1u << line);
    switch (line) {
    case 1: m_machine.bus.reg_file[0x12] |= 0x80u; break; /* DMA */
    case 2: m_machine.bus.reg_file[0x12] |= 0x40u; break; /* TIM0 */
    case 3: m_machine.bus.reg_file[0x12] |= 0x10u; break; /* EXT */
    case 4: m_machine.bus.reg_file[0x12] |= 0x08u; break; /* UART */
    case 5: m_machine.bus.reg_file[0x12] |= 0x01u; break; /* LCDC */
    case 6: m_machine.bus.reg_file[0x13] |= 0x40u; break; /* TIM1 */
    case 7: m_machine.bus.reg_file[0x13] |= 0x10u; break; /* CLKT */
    case 8: m_machine.bus.reg_file[0x13] |= 0x04u; break; /* PIO */
    default: break;
    }
}

void Sm8521Cpu::take_interrupt(uint16_t vector)
{
    get_sp();
    m_SYS = m_machine.bus.reg_file[0x19];
    m_PS1 = m_machine.bus.reg_file[0x1F];
    const auto push = [this](uint8_t value) {
        --m_SP;
        if ((m_SYS & 0x40u) == 0) m_SP &= 0x00FFu;
        mem_writebyte(m_SP, value);
    };
    push(static_cast<uint8_t>(m_PC));
    push(static_cast<uint8_t>(m_PC >> 8));
    push(m_PS1);
    m_PS1 &= static_cast<uint8_t>(~FLAG_I);
    mem_writebyte(0x1F, m_PS1);
    mem_writebyte(0x1D, static_cast<uint8_t>(m_SP));
    if (m_SYS & 0x40u) mem_writebyte(0x1C, static_cast<uint8_t>(m_SP >> 8));
    m_PC = mem_readword(vector);
}

void Sm8521Cpu::process_interrupts()
{
    if (m_IFLAGS == 0 && !m_dma_wake_pending) {
        m_irq_defer_one = false;
        return;
    }

    const uint8_t ie0 = m_machine.bus.reg_file[0x10];
    const uint8_t ie1 = m_machine.bus.reg_file[0x11];
    m_SYS = m_machine.bus.reg_file[0x19];
    m_PS0 = m_machine.bus.reg_file[0x1E];
    m_PS1 = m_machine.bus.reg_file[0x1F];

    /* Non-maskable traps (WDT, NMI, Illegal instruction) take precedence immediately. */
    if (m_IFLAGS & (1u << 9)) {
        m_IFLAGS &= static_cast<uint16_t>(~(1u << 9));
        m_halted = 0;
        m_dma_wake_pending = false;
        m_irq_defer_one = false;
        take_interrupt(0x101Cu);
        advance_cycles_chunked(((m_SYS & 0x40u) ? 16ull : 14ull) * m_cpu_prescale);
        return;
    }
    if (m_IFLAGS & ((1u << 10) | (1u << 0))) {
        const unsigned bit = (m_IFLAGS & (1u << 10)) ? 10u : 0u;
        m_IFLAGS &= static_cast<uint16_t>(~(1u << bit));
        m_halted = 0;
        m_dma_wake_pending = false;
        m_irq_defer_one = false;
        take_interrupt(0x101Eu);
        advance_cycles_chunked(((m_SYS & 0x40u) ? 16ull : 14ull) * m_cpu_prescale);
        return;
    }

    /* HALT wake: DMA completion always wakes DMA-HALT; otherwise any IE-enabled
     * pending interrupt wakes HALT without consuming the pending request. */
    if (m_halted != 0) {
        const bool dma_armed = (m_machine.bus.reg_file[0x34] & 0x80u) != 0;
        const bool ie_wake = !dma_armed && (
            ((m_IFLAGS & (1u << 1)) && (ie0 & 0x80u)) ||
            ((m_IFLAGS & (1u << 2)) && (ie0 & 0x40u)) ||
            ((m_IFLAGS & (1u << 3)) && (ie0 & 0x10u)) ||
            ((m_IFLAGS & (1u << 4)) && (ie0 & 0x08u)) ||
            ((m_IFLAGS & (1u << 5)) && (ie0 & 0x01u)) ||
            ((m_IFLAGS & (1u << 6)) && (ie1 & 0x40u)) ||
            ((m_IFLAGS & (1u << 7)) && (ie1 & 0x10u)) ||
            ((m_IFLAGS & (1u << 8)) && (ie1 & 0x04u)));
        if (m_dma_wake_pending || ie_wake) {
            m_halted = 0;
            m_dma_wake_pending = false;
        }
    } else {
        m_dma_wake_pending = false;
    }

    /* IRET defers maskable IRQ arbitration for one restored instruction while
     * retaining all pending private interrupt latches. */
    if (m_irq_defer_one && m_halted == 0) {
        m_irq_defer_one = false;
        return;
    }

    if ((m_PS1 & FLAG_I) == 0) return;

    struct IrqEntry {
        unsigned line;
        bool enabled;
        bool priority_ok;
        uint16_t vector;
    };
    const unsigned prio = m_PS0 & 0x07u;
    const IrqEntry table[8] = {
        { 1u, (ie0 & 0x80u) != 0, true,      0x1000u }, /* DMA */
        { 2u, (ie0 & 0x40u) != 0, true,      0x1002u }, /* TIM0 */
        { 3u, (ie0 & 0x10u) != 0, prio < 7u, 0x1006u }, /* EXT */
        { 4u, (ie0 & 0x08u) != 0, prio < 6u, 0x1008u }, /* UART */
        { 5u, (ie0 & 0x01u) != 0, prio < 5u, 0x100Eu }, /* LCDC */
        { 6u, (ie1 & 0x40u) != 0, prio < 4u, 0x1012u }, /* TIM1 */
        { 7u, (ie1 & 0x10u) != 0, prio < 3u, 0x1016u }, /* CLKT */
        { 8u, (ie1 & 0x04u) != 0, prio < 2u, 0x101Au }, /* PIO */
    };

    for (const auto& entry : table) {
        if ((m_IFLAGS & (1u << entry.line)) && entry.enabled && entry.priority_ok) {
            /* Consume only the private edge latch; visible IR0/IR1 stays set
             * until software clears it explicitly. */
            m_IFLAGS &= static_cast<uint16_t>(~(1u << entry.line));
            m_halted = 0;
            m_dma_wake_pending = false;
            take_interrupt(entry.vector);
            advance_cycles_chunked(((m_SYS & 0x40u) ? 16ull : 14ull) * m_cpu_prescale);
            return;
        }
    }
}

uint64_t Sm8521Cpu::perform_dma()
{
    auto& rf = m_machine.bus.reg_file;
    const uint8_t dmc = rf[0x34];
    if ((dmc & 0x80u) == 0) return 4ull * m_cpu_prescale;

    const bool overwrite = (dmc & 0x01u) != 0;
    const unsigned transfer_mode = dmc & 0x06u;
    const int adjust_x = (dmc & 0x08u) ? -1 : 1;
    const bool decrement_y = (dmc & 0x10u) != 0;
    const unsigned block_width = rf[0x37];
    const unsigned block_height = rf[0x38];
    const unsigned source_x = rf[0x35];
    const unsigned source_y = rf[0x36];
    const unsigned dest_x = rf[0x39];
    const unsigned dest_y = rf[0x3A];
    const uint8_t palette = rf[0x3B];
    const unsigned page = rf[0x3D];
    unsigned source_width = (rf[0x31] & 0x20u) ? 50u : 40u;
    unsigned dest_width = source_width;
    uint32_t source_mask = 0x1FFFu;
    uint32_t dest_mask = 0x1FFFu;
    size_t source_base = 0;
    size_t dest_base = 0;
    enum class SourceKind { Vram, Rom, ExtRam } source_kind = SourceKind::Vram;
    enum class DestKind { Vram, ExtRam } dest_kind = DestKind::Vram;

    switch (transfer_mode) {
    case 0x00: /* VRAM -> VRAM */
        source_base = (page & 0x01u) ? 0x2000u : 0;
        dest_base = (page & 0x02u) ? 0x2000u : 0;
        break;
    case 0x02: /* ROM -> VRAM */
        source_kind = SourceKind::Rom;
        source_width = 64;
        source_mask = 0x3FFFu;
        source_base = static_cast<size_t>(rf[0x3C] & 0x7Fu) << 14;
        dest_base = (page & 0x02u) ? 0x2000u : 0;
        break;
    case 0x04: /* Extended RAM -> VRAM */
        source_kind = SourceKind::ExtRam;
        source_width = 64;
        source_base = 0;
        dest_base = (page & 0x02u) ? 0x2000u : 0;
        break;
    case 0x06: /* VRAM -> Extended RAM */
        source_base = (page & 0x01u) ? 0x2000u : 0;
        dest_kind = DestKind::ExtRam;
        dest_width = 64;
        dest_base = 0;
        break;
    }

    auto read_source = [&](uint32_t offset) -> uint8_t {
        const size_t index = source_base + (offset & source_mask);
        switch (source_kind) {
        case SourceKind::Vram:
            return m_machine.bus.vram[index & (GC_VRAM_SIZE - 1u)];
        case SourceKind::Rom:
            if ((rf[0x3C] & 0x7Fu) < 16u)
                return index < sizeof(m_machine.bus.system_rom) ? m_machine.bus.system_rom[index] : 0xFF;
            if (!m_machine.bus.cart_active) return 0xFF;
            if (m_machine.bus.active_slot == 2u)
                return index < sizeof(m_machine.bus.rom2) ? m_machine.bus.rom2[index] : 0xFF;
            return index < sizeof(m_machine.bus.rom) ? m_machine.bus.rom[index] : 0xFF;
        case SourceKind::ExtRam:
            if (index < GC_EXT_RAM_SIZE) return m_machine.bus.ext_ram[index];
            if (index < GC_EXT_RAM_SIZE + sizeof(m_machine.bus.ext_io))
                return m_machine.bus.ext_io[index - GC_EXT_RAM_SIZE];
            return 0xFF;
        }
        return 0xFF;
    };
    auto read_dest = [&](uint32_t offset) -> uint8_t {
        const size_t index = dest_base + (offset & dest_mask);
        if (dest_kind == DestKind::Vram)
            return m_machine.bus.vram[index & (GC_VRAM_SIZE - 1u)];
        if (index < GC_EXT_RAM_SIZE) return m_machine.bus.ext_ram[index];
        if (index < GC_EXT_RAM_SIZE + sizeof(m_machine.bus.ext_io))
            return m_machine.bus.ext_io[index - GC_EXT_RAM_SIZE];
        return 0xFF;
    };
    auto write_dest = [&](uint32_t offset, uint8_t value) {
        const size_t index = dest_base + (offset & dest_mask);
        if (dest_kind == DestKind::Vram)
            m_machine.bus.vram[index & (GC_VRAM_SIZE - 1u)] = value;
        else if (index < GC_EXT_RAM_SIZE)
            m_machine.bus.ext_ram[index] = value;
        else if (index < GC_EXT_RAM_SIZE + sizeof(m_machine.bus.ext_io))
            m_machine.bus.ext_io[index - GC_EXT_RAM_SIZE] = value;
    };

    uint32_t source_current = source_width * source_y + (source_x >> 2);
    uint32_t dest_current = dest_width * dest_y + (dest_x >> 2);
    uint32_t source_line = source_current;
    uint32_t dest_line = dest_current;
    int source_x_current = static_cast<int>(source_x & 3u);
    unsigned dest_x_current = dest_x & 3u;

    for (unsigned y = 0; y <= block_height; ++y) {
        for (unsigned x = 0; x <= block_width; ++x) {
            const uint32_t src_addr = source_current & source_mask;
            const uint32_t dst_addr = dest_current & dest_mask;
            const unsigned dst_shift = (dest_x_current ^ 3u) << 1;
            const unsigned src_shift = (static_cast<unsigned>(source_x_current) ^ 3u) << 1;
            const uint8_t source_pixel = static_cast<uint8_t>((read_source(src_addr) >> src_shift) & 3u);
            if (overwrite || source_pixel != 0) {
                const uint8_t other = static_cast<uint8_t>(read_dest(dst_addr) & ~(3u << dst_shift));
                const uint8_t mapped = static_cast<uint8_t>((palette >> (source_pixel << 1)) & 3u);
                write_dest(dst_addr, static_cast<uint8_t>(other | (mapped << dst_shift)));
            }
            source_x_current += adjust_x;
            if ((source_x_current & 4) != 0) {
                source_current = static_cast<uint32_t>(source_current + adjust_x);
                source_x_current &= 3;
            }
            ++dest_x_current;
            if (dest_x_current & 4u) {
                ++dest_current;
                dest_x_current &= 3u;
            }
        }
        source_x_current = static_cast<int>(source_x & 3u);
        dest_x_current = dest_x & 3u;
        source_line = static_cast<uint32_t>(source_line + (decrement_y ? -static_cast<int>(source_width)
                                                                       : static_cast<int>(source_width)));
        dest_line += dest_width;
        source_current = source_line;
        dest_current = dest_line;
    }

    rf[0x34] &= 0x7Fu;
    ++m_dma_transfers;
    m_dma_wake_pending = true;
    request_interrupt(1); /* DMA completion wakes HALT even if IE0[7] is masked. */

    /* Hardware DMA runs on unprescaled phi0 beats:
     * - Byte-aligned ROM/ExtRAM -> VRAM overwrite overlaps external source
     *   prefetch with VRAM writes (~2 phi0 beats per 4-pixel byte + 2 setup).
     * - Unaligned, VRAM-source, or transparent/compound blits read-modify-write
     *   destination packets (~4..6 phi0 beats per 4-pixel packet). */
    const uint64_t rows = static_cast<uint64_t>(block_height) + 1ull;
    const uint64_t packets_per_row = ((dest_x & 3u) + block_width) / 4u + 1u;
    const bool dual_bus_prefetch =
        (transfer_mode == 0x02u || transfer_mode == 0x04u) &&
        overwrite && adjust_x > 0 &&
        ((source_x & 3u) == 0u) && ((dest_x & 3u) == 0u) &&
        ((block_width & 3u) == 3u);
    if (dual_bus_prefetch)
        return rows * (2ull + 2ull * packets_per_row);
    const uint64_t per_packet = overwrite ? 4ull : 6ull;
    return 2ull + rows * packets_per_row * per_packet;
}

void Sm8521Cpu::advance_uart()
{
    const uint64_t frame_cycles = uart_frame_cycles();
    if (frame_cycles == 0) return;

    start_uart_tx();
    while (m_uart_tx_active && m_cycles >= m_uart_tx_complete_cycle) {
        m_uart_transmits.push_back({m_uart_tx_complete_cycle, m_uart_tx_shift});
        m_uart_tx_active = false;
        request_interrupt(4);
        if (m_uart_tx_pending) {
            start_uart_tx();
        } else {
            m_uart_status |= 0x02u; /* TDRE */
        }
    }

    if (!m_uart_rx_active && (m_uart_control & 0x08u) && !m_uart_rx_queue.empty()) {
        m_uart_rx_shift = m_uart_rx_queue.front();
        m_uart_rx_queue.pop_front();
        m_uart_rx_active = true;
        m_uart_rx_complete_cycle = m_cycles + frame_cycles;
        m_uart_status |= 0x20u; /* RBSY */
    }
    while (m_uart_rx_active && m_cycles >= m_uart_rx_complete_cycle) {
        const uint64_t completed_at = m_uart_rx_complete_cycle;
        m_uart_rx_active = false;
        m_uart_status &= static_cast<uint8_t>(~0x20u); /* RBSY */
        if (m_uart_status & 0x01u) {
            m_uart_status |= 0x10u; /* OR: unread receive data was overwritten by a new frame. */
        } else {
            m_uart_rx_data = m_uart_rx_shift;
            m_uart_status |= 0x01u; /* RDRF */
        }
        request_interrupt(4);
        if ((m_uart_control & 0x08u) && !m_uart_rx_queue.empty()) {
            m_uart_rx_shift = m_uart_rx_queue.front();
            m_uart_rx_queue.pop_front();
            m_uart_rx_active = true;
            m_uart_rx_complete_cycle = completed_at + frame_cycles;
            m_uart_status |= 0x20u;
        }
    }

    m_machine.bus.reg_file[0x2B] = m_uart_tx_data;
    m_machine.bus.reg_file[0x2C] = m_uart_rx_data;
    m_machine.bus.reg_file[0x2D] = m_uart_status;
    m_machine.bus.reg_file[0x2E] = m_uart_control;
}

void Sm8521Cpu::advance_devices(unsigned cycles)
{
    for (unsigned timer = 0; timer < 2; ++timer) {
        TimerState& state = m_timers[timer];
        if (!state.enabled) continue;
        state.prescale_count += cycles;
        const unsigned data_reg = timer == 0 ? 0x51u : 0x53u;
        const uint32_t compare_steps = state.reload == 0 ? 1u : state.reload;
        while (state.prescale_count >= state.prescale_max) {
            state.prescale_count -= state.prescale_max;
            const uint32_t next_cnt = static_cast<uint32_t>(state.counter) + 1u;
            if (next_cnt >= compare_steps) {
                state.counter = 0;
                /* Hardware always latches IR0[6]/IR1[6] and the pending edge;
                 * IE, FLAG_I, and PS0 priority gate acceptance in process_interrupts. */
                request_interrupt(timer == 0 ? 2u : 6u);
            } else {
                state.counter = static_cast<uint8_t>(next_cnt);
            }
        }
        m_machine.bus.reg_file[data_reg] = state.counter;
    }
    if (!m_external_vblank) {
        while (m_cycles >= m_next_vblank) {
            ++m_vblank_ticks;
            const bool lcd_on = (m_machine.bus.reg_file[0x30] & 0x80u) != 0;
            const uint64_t vb_lines = m_lcv_cfg & 0x0Fu;
            if (lcd_on && vb_lines > 0u) {
                m_vblank_end_cycle = m_next_vblank + vb_lines * 320ull;
                request_interrupt(5);
            }
            m_next_vblank += GAMECOM_FRAME_CYCLES;
        }
    }
    update_lcv_status();
    while (m_next_clock_tick && m_cycles >= m_next_clock_tick) {
        uint8_t value = static_cast<uint8_t>(m_machine.bus.reg_file[0x54] + 1u);
        m_machine.bus.reg_file[0x54] = static_cast<uint8_t>(
            (m_machine.bus.reg_file[0x54] & 0xC0u) | (value & 0x3Fu));
        request_interrupt(7); /* One-second/minute clock interrupt. */
        ++m_clock_ticks;
        m_next_clock_tick += m_clock_tick_period;
    }
    advance_uart();
}

void Sm8521Cpu::advance_cycles_chunked(uint64_t total_cycles)
{
    while (total_cycles > 0u) {
        const unsigned step = static_cast<unsigned>(std::min<uint64_t>(total_cycles, 256ull));
        m_cycles += step;
        advance_devices(step);
        total_cycles -= step;
    }
}

bool Sm8521Cpu::step()
{
    if (m_stopped) {
        /* Front-panel Power button (IN2 bit 0 active-low) or external clear wakes STOP. */
        if (!m_machine.cpu.stopped || (m_machine.bus.input_buttons[2] & 0x01u) == 0u) {
            m_stopped = 0;
            m_machine.cpu.stopped = false;
        } else {
            advance_cycles_chunked(4ull * m_cpu_prescale);
            return false;
        }
    }
    if (m_halted && (m_machine.bus.input_buttons[2] & 0x01u) == 0u &&
        (m_machine.bus.reg_file[0x34] & 0x80u) == 0u) {
        m_halted = 0;
    }

    process_interrupts();
    if (m_halted) {
        uint64_t halted_cycles = 4ull * m_cpu_prescale;
        if ((m_machine.bus.reg_file[0x34] & 0x80u) != 0)
            halted_cycles = perform_dma();
        advance_cycles_chunked(halted_cycles);
        return false;
    }

    int mycycles = 0;

    load_sfr_state();
    const uint8_t prev_ps0 = m_PS0;
    const uint8_t op = mem_readbyte(m_PC++);
#include "sm8521_ops.inc"

    if (m_SYS & 0x40u) gc_bus_write(&m_machine, 0x1C, static_cast<uint8_t>(m_SP >> 8));
    gc_bus_write(&m_machine, 0x1D, static_cast<uint8_t>(m_SP));
    if (m_PS0 != prev_ps0 || m_machine.bus.reg_file[0x1E] != m_PS0)
        mem_writebyte(0x1E, m_PS0);
    mem_writebyte(0x1F, m_PS1);

    if (mycycles <= 0) mycycles = 1;
    const uint64_t phi0_cycles = static_cast<uint64_t>(mycycles) * m_cpu_prescale;
    m_cycles += phi0_cycles;
    advance_devices(static_cast<unsigned>(phi0_cycles));
    ++m_instructions;
    return m_halted == 0 && m_stopped == 0;
}

} // namespace standalone
