#include "audio_synth.hpp"

/* Register/timer behavior follows the MAME Game.com reference driver
 * gamecom_m.cpp (BSD-3-Clause; copyright-holders Wilbert Pol and Robbbert).
 * This sample mixer/output path is newly written for the standalone emulator. */
#include <algorithm>
#include <cmath>
#include <limits>

namespace standalone {
namespace {
constexpr unsigned kSgc = 0x40;
constexpr unsigned kMaster = 0x80;
constexpr unsigned kDacEnable = 0x08;
constexpr unsigned kWaveRateClock = 2764800;

std::uint16_t read_period(const GcMachine& machine, unsigned high_reg)
{
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(machine.bus.reg_file[high_reg]) << 8) |
        machine.bus.reg_file[high_reg + 1]);
}

float wave_value(const GcMachine& machine, unsigned channel, unsigned nibble)
{
    const unsigned base = channel == 0 ? 0x60u : 0x70u;
    const unsigned packed = machine.bus.reg_file[base + (nibble >> 1)];
    const unsigned value = (packed >> ((nibble & 1u) * 4u)) & 0x0fu;
    /* The hardware DAC is unipolar; center it for an AC-coupled host output. */
    return (static_cast<float>(value) - 7.5f) / 7.5f;
}

std::int16_t to_pcm(float value)
{
    value = std::max(-1.0f, std::min(1.0f, value));
    const float scaled = value * 32767.0f;
    return static_cast<std::int16_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}
}

void AudioSynth::reset() noexcept
{
    m_channels[0] = Channel{};
    m_channels[1] = Channel{};
}

void AudioSynth::render(const GcMachine& machine, std::int16_t *output,
                        std::size_t frames, unsigned sample_rate) noexcept
{
    if (output == nullptr || sample_rate == 0) return;
    const unsigned sgc = machine.bus.reg_file[kSgc];
    const bool master = (sgc & kMaster) != 0;
    const unsigned enable_bits[2] = { 0x01u, 0x02u };
    const unsigned level_regs[2] = { 0x42u, 0x44u };
    const unsigned timer_regs[2] = { 0x46u, 0x48u };

    for (unsigned channel = 0; channel < 2; ++channel) {
        const bool enabled = master && (sgc & enable_bits[channel]) != 0;
        if (enabled && !m_channels[channel].enabled) m_channels[channel].phase = 0.0;
        if (!enabled) m_channels[channel].phase = 0.0;
        m_channels[channel].enabled = enabled;
    }

    const bool dac_enabled = master && (sgc & 0x0fu) == kDacEnable;
    const float dac_value = (static_cast<float>(machine.bus.reg_file[0x4e]) - 127.5f) / 127.5f;

    for (std::size_t frame = 0; frame < frames; ++frame) {
        float mixed = 0.0f;
        for (unsigned channel = 0; channel < 2; ++channel) {
            if (!m_channels[channel].enabled) continue;
            const std::uint16_t period = read_period(machine, timer_regs[channel]);
            const unsigned raw_level = machine.bus.reg_file[level_regs[channel]];
            const float level = static_cast<float>(std::min(raw_level, 31u)) / 31.0f;
            if (period == 0 || level == 0.0f) continue;

            const unsigned nibble = static_cast<unsigned>(m_channels[channel].phase) & 31u;
            mixed += wave_value(machine, channel, nibble) * level * 0.32f;
            m_channels[channel].phase +=
                static_cast<double>(kWaveRateClock) /
                (static_cast<double>(period) * sample_rate);
            if (m_channels[channel].phase >= 32.0)
                m_channels[channel].phase = std::fmod(m_channels[channel].phase, 32.0);
        }
        if (dac_enabled) mixed += dac_value * 0.25f;
        output[frame] = to_pcm(mixed);
    }
}

}
