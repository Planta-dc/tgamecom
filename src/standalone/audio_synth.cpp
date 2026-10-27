#include "audio_synth.hpp"

/* Register/timer behavior follows the MAME Game.com reference driver
 * gamecom_m.cpp (BSD-3-Clause; copyright-holders Wilbert Pol and Robbbert).
 * The event-timed mixer below is newly written for the standalone emulator. */
#include <algorithm>
#include <cmath>
#include <limits>

namespace standalone {
namespace {
constexpr unsigned kSgcAddress = 0x40;
constexpr unsigned kSgdaAddress = 0x4e;
constexpr unsigned kMasterEnable = 0x80;
constexpr unsigned kDacEnable = 0x08;
constexpr unsigned kWaveRateClock = 2764800;
constexpr float kWaveGain = 0.25f; // MAME's DAC_4BIT_R2R speaker route
constexpr float kDirectDacGain = 0.5f; // MAME's DAC_8BIT_R2R speaker route

std::int16_t to_pcm(float value)
{
    value = std::max(-1.0f, std::min(1.0f, value));
    const float scaled = value * 32767.0f;
    return static_cast<std::int16_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}
}

void AudioSynth::reset() noexcept
{
    m_registers.fill(0);
    m_channels[0] = Channel{};
    m_channels[1] = Channel{};
    m_dac_value = 0.0f;
}

void AudioSynth::apply_write(const SoundRegisterWrite& write) noexcept
{
    if (write.address < 0x40u || write.address >= 0x80u) return;
    const unsigned index = write.address - 0x40u;
    if (write.address == kSgcAddress) {
        const unsigned old_sgc = m_registers[0];
        m_registers[index] = write.value;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const unsigned enable = 1u << channel;
            const bool was_enabled = (old_sgc & kMasterEnable) && (old_sgc & enable);
            const bool now_enabled = (write.value & kMasterEnable) && (write.value & enable);
            if (was_enabled != now_enabled || !now_enabled)
                m_channels[channel].phase = 0.0;
            m_channels[channel].enabled = now_enabled;
        }
        return;
    }

    m_registers[index] = write.value;
    if (write.address == kSgdaAddress) {
        const unsigned sgc = m_registers[0];
        /* MAME routes SGDA to the 8-bit DAC only when the master and DAC
         * enables are set and all three wavetable/noise enables are clear. */
        if ((sgc & 0x8fu) == 0x88u)
            m_dac_value = (static_cast<float>(write.value) - 127.5f) / 127.5f;
    }
}

void AudioSynth::advance_cycles(std::uint64_t cycles, std::uint32_t cpu_hz) noexcept
{
    if (cycles == 0) return;
    if (cpu_hz == 0) cpu_hz = GC_CPU_HZ;
    const unsigned sgc = m_registers[0];
    for (unsigned channel = 0; channel < 2; ++channel) {
        const unsigned enable = 1u << channel;
        const bool enabled = (sgc & kMasterEnable) && (sgc & enable);
        m_channels[channel].enabled = enabled;
        if (!enabled) {
            m_channels[channel].phase = 0.0;
            continue;
        }
        const unsigned timer_index = channel == 0 ? 0x06u : 0x08u;
        const unsigned period = (static_cast<unsigned>(m_registers[timer_index]) << 8) |
                                m_registers[timer_index + 1u];
        if (period == 0) continue;
        const double increment = static_cast<double>(cycles) * kWaveRateClock /
            (static_cast<double>(period) * static_cast<double>(cpu_hz));
        m_channels[channel].phase = std::fmod(m_channels[channel].phase + increment, 32.0);
    }
}

float AudioSynth::channel_value(unsigned channel) const noexcept
{
    if (channel >= 2 || !m_channels[channel].enabled) return 0.0f;
    const unsigned timer_index = channel == 0 ? 0x06u : 0x08u;
    const unsigned period = (static_cast<unsigned>(m_registers[timer_index]) << 8) |
                            m_registers[timer_index + 1u];
    if (period == 0) return 0.0f;

    const unsigned level_index = channel == 0 ? 0x02u : 0x04u;
    const unsigned level = std::min<unsigned>(m_registers[level_index], 31u);
    if (level == 0) return 0.0f;

    const unsigned nibble = static_cast<unsigned>(m_channels[channel].phase) & 31u;
    const unsigned wave_base = channel == 0 ? 0x20u : 0x30u;
    const unsigned packed = m_registers[wave_base + (nibble >> 1)];
    const unsigned sample = (packed >> ((nibble & 1u) * 4u)) & 0x0fu;
    const float centered = (static_cast<float>(sample) - 7.5f) / 7.5f;
    return centered * (static_cast<float>(level) / 31.0f) * kWaveGain;
}

float AudioSynth::direct_dac_value() const noexcept
{
    const unsigned sgc = m_registers[0];
    return (sgc & 0x8fu) == 0x88u ? m_dac_value * kDirectDacGain : 0.0f;
}

void AudioSynth::render(const std::vector<SoundRegisterWrite>& writes,
                        std::uint64_t start_cycle, std::uint64_t end_cycle,
                        std::int16_t *output, std::size_t frames,
                        unsigned sample_rate,
                        std::uint32_t cpu_hz) noexcept
{
    if (end_cycle < start_cycle) return;
    if (cpu_hz == 0) cpu_hz = GC_CPU_HZ;
    const std::uint64_t span = end_cycle - start_cycle;
    std::uint64_t cursor = start_cycle;
    std::size_t event_index = 0;

    if (output == nullptr || sample_rate == 0 || frames == 0) {
        for (const SoundRegisterWrite& write : writes) {
            if (write.cycle > end_cycle) break;
            const std::uint64_t event_cycle = std::max(cursor, std::max(start_cycle, write.cycle));
            advance_cycles(event_cycle - cursor, cpu_hz);
            cursor = event_cycle;
            apply_write(write);
        }
        advance_cycles(end_cycle - cursor, cpu_hz);
        return;
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint64_t sample_cycle = start_cycle +
            static_cast<std::uint64_t>((static_cast<long double>(frame) * span) / frames);
        while (event_index < writes.size() && writes[event_index].cycle <= sample_cycle) {
            const std::uint64_t event_cycle = std::max(cursor,
                std::max(start_cycle, writes[event_index].cycle));
            advance_cycles(event_cycle - cursor, cpu_hz);
            cursor = event_cycle;
            apply_write(writes[event_index]);
            ++event_index;
        }
        if (sample_cycle > cursor) {
            advance_cycles(sample_cycle - cursor, cpu_hz);
            cursor = sample_cycle;
        }
        output[frame] = to_pcm(channel_value(0) + channel_value(1) + direct_dac_value());
    }

    while (event_index < writes.size() && writes[event_index].cycle <= end_cycle) {
        const std::uint64_t event_cycle = std::max(cursor,
            std::max(start_cycle, writes[event_index].cycle));
        advance_cycles(event_cycle - cursor, cpu_hz);
        cursor = event_cycle;
        apply_write(writes[event_index]);
        ++event_index;
    }
    advance_cycles(end_cycle - cursor, cpu_hz);
}

}
