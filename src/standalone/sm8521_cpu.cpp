// Standalone adaptation of MAME's SM8500/SM8521 CPU operation table.
// Original operation table: BSD-3-Clause, Copyright Wilbert Pol.
// The adapter below replaces MAME's device/memory framework with this
// project's independent bus API. MAME is not linked or called at runtime.
#include "sm8521_cpu.hpp"

#include <cstdio>
#include <cstdint>

#define logerror(...) (void)std::fprintf(stderr, __VA_ARGS__)

namespace standalone {

static constexpr uint8_t sm8500_b2w[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };
static constexpr uint64_t GAMECOM_FRAME_CYCLES = 92573;

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
    m_clock_changed = 0;
    m_halted = 0;
    m_IFLAGS = 0;
    m_check_interrupts = false;
    m_timers[0] = TimerState{};
    m_timers[1] = TimerState{};
    m_next_vblank = GAMECOM_FRAME_CYCLES;
    m_next_clock_tick = 0;
    m_clock_tick_period = 5529600;
    m_cycles = 0;
    m_instructions = 0;
    m_dma_transfers = 0;
    m_clock_ticks = 0;
    m_vblank_ticks = 0;
    m_uart_tx_data = 0xFF;
    m_uart_rx_data = 0;
    m_uart_status = 0x42; /* Preserve the existing MAME-reference reset value. */
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
    for (auto& byte : m_register_ram) byte = 0;
    get_sp();
}

bool Sm8521Cpu::queue_uart_receive(uint8_t value)
{
    if ((m_uart_control & 0x08u) == 0 || m_uart_rx_queue.size() >= 4096u)
        return false;
    m_uart_rx_queue.push_back(value);
    return true;
}

uint64_t Sm8521Cpu::uart_frame_cycles() const noexcept
{
    if (!m_timers[0].enabled) return 0;
    const uint64_t timer_period = static_cast<uint64_t>(m_timers[0].prescale_max) *
                                  (m_timers[0].upcounter_max ? m_timers[0].upcounter_max : 1u);
    const uint64_t bit_cycles = timer_period * 32u;
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
    case 0x2B: value = 0xFF; break; /* URTT is write-only. */
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
    if (address < 0x10u) {
        const uint32_t effective = address + (m_PS0 & 0xF8u);
        if (effective >= 0x2Bu && effective <= 0x2Eu)
            return uart_read_register(static_cast<uint8_t>(effective));
        return m_register_ram[effective];
    }
    if (address >= 0x2Bu && address <= 0x2Eu)
        return uart_read_register(static_cast<uint8_t>(address));
    return gc_bus_read(&m_machine, static_cast<uint16_t>(address));
}

void Sm8521Cpu::mem_writebyte(uint32_t address, uint8_t value)
{
    address &= 0xFFFFu;
    const uint32_t effective = address < 0x10u ? address + (m_PS0 & 0xF8u) : address;
    if (effective >= 0x40u && effective < 0x80u)
        m_sound_writes.push_back({m_cycles, static_cast<uint8_t>(effective), value});
    if (address < 0x10u)
        m_register_ram[effective] = value;

    if (effective >= 0x2Bu && effective <= 0x2Eu) {
        if (address < 0x10u)
            gc_bus_write(&m_machine, static_cast<uint16_t>(address), value);
        if (effective == 0x2Bu) uart_write_data(value);
        else if (effective == 0x2Eu) uart_write_control(value);
        gc_bus_write(&m_machine, static_cast<uint16_t>(effective), value);
        m_machine.bus.reg_file[0x2B] = m_uart_tx_data;
        m_machine.bus.reg_file[0x2C] = m_uart_rx_data;
        m_machine.bus.reg_file[0x2D] = m_uart_status;
        m_machine.bus.reg_file[0x2E] = m_uart_control;
        return;
    }

    if (address < 0x10u && effective >= 0x40u && effective < 0x80u) {
        gc_bus_write(&m_machine, static_cast<uint16_t>(address), value);
        gc_bus_write(&m_machine, static_cast<uint16_t>(effective), value);
        return;
    }

    gc_bus_write(&m_machine, static_cast<uint16_t>(address), value);

    switch (address) {
    case 0x19: m_SYS = value; break;
    case 0x1A: m_CKC = value; break;
    case 0x1C:
    case 0x1D: get_sp(); break;
    case 0x1E:
        m_PS0 = value;
        for (uint8_t i = 0; i < 16; ++i)
            gc_bus_write(&m_machine, i, mem_readbyte(i));
        break;
    case 0x1F: m_PS1 = value; break;
    case 0x50:
    case 0x52: {
        const unsigned timer = address == 0x50 ? 0u : 1u;
        static constexpr uint32_t timer_limits[8] = { 1, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
        m_timers[timer].enabled = (value & 0x80u) != 0;
        m_timers[timer].prescale_max = timer_limits[value & 0x07u];
        m_timers[timer].prescale_count = 0;
        m_machine.bus.reg_file[address + 1u] = 0;
        break;
    }
    case 0x51: m_timers[0].upcounter_max = value; m_machine.bus.reg_file[address] = 0; break;
    case 0x53: m_timers[1].upcounter_max = value; m_machine.bus.reg_file[address] = 0; break;
    case 0x54:
        if (value & 0x80u) {
            m_clock_tick_period = (value & 0x40u) ? 60ull * 5529600ull : 5529600ull;
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
    if (line < 11u) {
        m_IFLAGS |= static_cast<uint16_t>(1u << line);
        m_check_interrupts = true;
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
    if (!m_check_interrupts) return;
    unsigned irqline = 0;
    while (irqline < 11u) {
        if (m_IFLAGS & (1u << irqline)) {
            m_halted = 0;
            const uint8_t ie0 = m_machine.bus.reg_file[0x10];
            const uint8_t ie1 = m_machine.bus.reg_file[0x11];
            uint8_t ir0 = m_machine.bus.reg_file[0x12];
            uint8_t ir1 = m_machine.bus.reg_file[0x13];
            m_PS0 = m_machine.bus.reg_file[0x1E];
            m_PS1 = m_machine.bus.reg_file[0x1F];
            switch (irqline) {
            case 9: take_interrupt(0x101C); break; /* WDT */
            case 0:
            case 10: take_interrupt(0x101E); break; /* Illegal instruction / NMI */
            case 1:
                ir0 |= 0x80;
                if ((ie0 & 0x80u) && (m_PS1 & FLAG_I)) take_interrupt(0x1000);
                break;
            case 2:
                ir0 |= 0x40;
                if ((ie0 & 0x40u) && (m_PS1 & FLAG_I)) take_interrupt(0x1002);
                break;
            case 3:
                ir0 |= 0x10;
                if ((ie0 & 0x10u) && ((m_PS0 & 7u) < 7u) && (m_PS1 & FLAG_I)) take_interrupt(0x1006);
                break;
            case 4:
                ir0 |= 0x08;
                if ((ie0 & 0x08u) && ((m_PS0 & 7u) < 6u) && (m_PS1 & FLAG_I)) take_interrupt(0x1008);
                break;
            case 5:
                ir0 |= 0x01;
                if ((ie0 & 0x01u) && ((m_PS0 & 7u) < 5u) && (m_PS1 & FLAG_I)) take_interrupt(0x100E);
                break;
            case 6:
                ir1 |= 0x40;
                if ((ie1 & 0x40u) && ((m_PS0 & 7u) < 4u) && (m_PS1 & FLAG_I)) take_interrupt(0x1012);
                break;
            case 7:
                ir1 |= 0x10;
                if ((ie1 & 0x10u) && ((m_PS0 & 7u) < 3u) && (m_PS1 & FLAG_I)) take_interrupt(0x1016);
                break;
            case 8:
                ir1 |= 0x04;
                if ((ie1 & 0x04u) && ((m_PS0 & 7u) < 2u) && (m_PS1 & FLAG_I)) take_interrupt(0x101A);
                break;
            default: break;
            }
            m_IFLAGS &= static_cast<uint16_t>(~(1u << irqline));
            m_machine.bus.reg_file[0x12] = ir0;
            m_machine.bus.reg_file[0x13] = ir1;
        }
        ++irqline;
    }
    if (m_IFLAGS == 0) m_check_interrupts = false;
}

void Sm8521Cpu::perform_dma()
{
    auto& rf = m_machine.bus.reg_file;
    const uint8_t dmc = rf[0x34];
    if ((dmc & 0x80u) == 0) return;

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
    request_interrupt(1); /* DMA completion wakes HALT even if not mask-enabled. */
}

void Sm8521Cpu::advance_uart()
{
    const uint64_t frame_cycles = uart_frame_cycles();
    if (frame_cycles == 0) return;

    start_uart_tx();
    if (m_uart_tx_active && m_cycles >= m_uart_tx_complete_cycle) {
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
    if (m_uart_rx_active && m_cycles >= m_uart_rx_complete_cycle) {
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
            m_uart_rx_complete_cycle = m_cycles + frame_cycles;
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
        const unsigned enable_reg = timer == 0 ? 0x10u : 0x11u;
        const uint8_t enable_bit = timer == 0 ? 0x40u : 0x40u;
        while (state.prescale_count >= state.prescale_max) {
            state.prescale_count -= state.prescale_max;
            ++m_machine.bus.reg_file[data_reg];
            if (m_machine.bus.reg_file[data_reg] >= state.upcounter_max) {
                m_machine.bus.reg_file[data_reg] = 0;
                if ((m_machine.bus.reg_file[enable_reg] & enable_bit) &&
                    (m_machine.bus.reg_file[0x1F] & FLAG_I))
                    request_interrupt(timer == 0 ? 2u : 6u);
            }
        }
    }
    while (m_cycles >= m_next_vblank) {
        request_interrupt(5); /* Approximate MAME's periodic LCD vblank IRQ. */
        ++m_vblank_ticks;
        m_next_vblank += GAMECOM_FRAME_CYCLES;
    }
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

bool Sm8521Cpu::step()
{
    process_interrupts();
    if (m_halted) {
        m_cycles += 4;
        perform_dma(); /* MAME invokes the DMA callback only during the HALT wait path. */
        advance_devices(4);
        return false;
    }

    int mycycles = 0;
    uint8_t r1 = 0, r2 = 0;
    uint16_t s1 = 0, s2 = 0;
    uint32_t d1 = 0, d2 = 0;
    uint32_t res = 0;

    load_sfr_state();
    uint8_t op = mem_readbyte(m_PC++);
    switch (op) {
#include "sm85ops_mame.inc"
    }

    if (m_SYS & 0x40u) gc_bus_write(&m_machine, 0x1C, static_cast<uint8_t>(m_SP >> 8));
    gc_bus_write(&m_machine, 0x1D, static_cast<uint8_t>(m_SP));
    mem_writebyte(0x1E, m_PS0);
    mem_writebyte(0x1F, m_PS1);

    if (mycycles <= 0) mycycles = 1;
    m_cycles += static_cast<uint64_t>(mycycles);
    advance_devices(static_cast<unsigned>(mycycles));
    ++m_instructions;
    return m_halted == 0;
}

} // namespace standalone
