#include "audio_synth.hpp"

/* Clean-room SM8521 sound synthesizer following Sharp SM8521 / Tiger Game.com
 * hardware behavior:
 * - Master sound clock: 5.000 MHz phi0 (GC_CPU_HZ).
 * - SG0W/SG1W: 32 signed 4-bit two's-complement samples (-8..+7), low nibble first.
 * - SG0T/SG1T/SG2T: 12-bit active period (SGxTH[3:0]:SGxTL), interval (period + 1)
 *   phi0 ticks, active when period > 1.
 * - SG2: 15-bit LFSR (bit0 ^ bit1 -> bit14, reset 0x7FFF), +7/-8 scaled by SG2L[4:0].
 * - SGDA: exclusive 8-bit unsigned direct DAC when (SGC & 0x8F) == 0x88. */
#include <algorithm>
#include <cmath>

namespace standalone {
namespace {
constexpr unsigned kSgcAddress = 0x40u;
constexpr unsigned kSgdaAddress = 0x4Eu;
constexpr unsigned kMasterEnable = 0x80u;
constexpr double kSoundClockHz = static_cast<double>(GC_CPU_HZ); // 5,000,000 Hz phi0
constexpr float kMasterHeadroom = 0.80f;

std::int16_t to_pcm(float value) noexcept
{
    value = std::max(-1.0f, std::min(1.0f, value));
    const float scaled = value * 32767.0f;
    return static_cast<std::int16_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

unsigned read_period12(const std::array<std::uint8_t, 0x40>& regs, unsigned hi_idx) noexcept
{
    return ((static_cast<unsigned>(regs[hi_idx]) & 0x0Fu) << 8) |
           static_cast<unsigned>(regs[hi_idx + 1u]);
}

int scale_channel_dac8(int sample_s4, unsigned level_u5) noexcept
{
    /* SM8521 expands 4-bit channel data 16x and attenuates by SGxL / 32. */
    return (sample_s4 * 16 * static_cast<int>(level_u5 & 0x1Fu)) / 32;
}
} // namespace

void AudioSynth::reset() noexcept
{
    m_registers.fill(0);
    m_wave[0] = WaveChannel{};
    m_wave[1] = WaveChannel{};
    m_noise_tick_accum = 0.0;
    m_lfsr = 0x7FFFu;
    m_noise_sample = 7;
    m_dac_byte = 0x80u;
    m_lpf_state = 0.0f;
    m_dc_baseline = 0.0f;
}

void AudioSynth::apply_write(const SoundRegisterWrite& write) noexcept
{
    if (write.address < 0x40u || write.address >= 0x80u) return;
    const unsigned index = write.address - 0x40u;

    if (write.address == kSgcAddress) {
        m_registers[index] = write.value;
        /* SGC[2:0] channel disables pause advancement without resetting wave/LFSR
         * phase. Clearing SGC[7] powers down the master sound block. */
        if ((write.value & kMasterEnable) == 0) {
            m_lpf_state = 0.0f;
            m_dc_baseline = 0.0f;
        }
        return;
    }

    m_registers[index] = write.value;
    if (write.address == kSgdaAddress) {
        /* Latch SGDA writes when direct-DAC mode is active, and also remember
         * the written byte so immediate SGC->0x88 transitions see the sample. */
        m_dac_byte = write.value;
    }
}

void AudioSynth::advance_ticks(double phi0_ticks) noexcept
{
    if (phi0_ticks <= 0.0) return;
    const unsigned sgc = m_registers[0];
    if ((sgc & kMasterEnable) == 0) return;

    /* SG0 & SG1 32-step wavetable channels. */
    for (unsigned ch = 0; ch < 2u; ++ch) {
        if ((sgc & (1u << ch)) == 0) continue;
        const unsigned period = read_period12(m_registers, ch == 0u ? 0x06u : 0x08u);
        if (period <= 1u) continue;
        const double divider = static_cast<double>(period + 1u);
        m_wave[ch].phase = std::fmod(m_wave[ch].phase + (phi0_ticks / divider), 32.0);
    }

    /* SG2 15-bit LFSR noise generator (period 32,767). */
    if ((sgc & 0x04u) != 0) {
        const unsigned period = read_period12(m_registers, 0x0Cu);
        if (period > 1u) {
            const double divider = static_cast<double>(period + 1u);
            m_noise_tick_accum += phi0_ticks;
            if (m_noise_tick_accum >= divider) {
                const auto total_shifts =
                    static_cast<std::uint64_t>(m_noise_tick_accum / divider);
                m_noise_tick_accum = std::fmod(m_noise_tick_accum, divider);
                /* Maximal 15-bit LFSR repeats every 32,767 shifts exactly. */
                unsigned shifts = static_cast<unsigned>(total_shifts % 32767ull);
                std::uint16_t lfsr = (m_lfsr == 0u) ? 0x7FFFu : m_lfsr;
                int sample = m_noise_sample;
                while (shifts-- > 0u) {
                    const std::uint16_t fb = static_cast<std::uint16_t>((lfsr ^ (lfsr >> 1u)) & 1u);
                    lfsr = static_cast<std::uint16_t>((lfsr >> 1u) | (fb << 14u));
                    if (lfsr == 0u) lfsr = 0x7FFFu;
                    sample = (lfsr & 1u) ? -8 : 7;
                }
                m_lfsr = lfsr;
                m_noise_sample = sample;
            }
        }
    }
}

float AudioSynth::mix_instantaneous() const noexcept
{
    const unsigned sgc = m_registers[0];
    if ((sgc & kMasterEnable) == 0) return 0.0f;

    /* Direct DAC mode is exclusive with SG0/SG1/SG2 when SGC[3]=1 and SGC[2:0]=0. */
    if ((sgc & 0x8Fu) == 0x88u) {
        const int delta = static_cast<int>(m_dac_byte) - 128;
        return (static_cast<float>(delta) / 128.0f) * kMasterHeadroom;
    }

    int mix_dac8 = 0;
    for (unsigned ch = 0; ch < 2u; ++ch) {
        if ((sgc & (1u << ch)) == 0) continue;
        const unsigned period = read_period12(m_registers, ch == 0u ? 0x06u : 0x08u);
        if (period <= 1u) continue;
        const unsigned level = m_registers[ch == 0u ? 0x02u : 0x04u] & 0x1Fu;
        if (level == 0u) continue;

        const unsigned step = static_cast<unsigned>(m_wave[ch].phase) & 31u;
        const unsigned wave_base = ch == 0u ? 0x20u : 0x30u;
        const unsigned packed = m_registers[wave_base + (step >> 1u)];
        const int nib = static_cast<int>((packed >> ((step & 1u) * 4u)) & 0x0Fu);
        const int signed_s4 = (nib >= 8) ? (nib - 16) : nib; // -8 .. +7
        mix_dac8 += scale_channel_dac8(signed_s4, level);
    }

    if ((sgc & 0x04u) != 0) {
        const unsigned period = read_period12(m_registers, 0x0Cu);
        const unsigned level = m_registers[0x0Au] & 0x1Fu;
        if (period > 1u && level > 0u)
            mix_dac8 += scale_channel_dac8(m_noise_sample, level);
    }

    mix_dac8 = std::max(-128, std::min(127, mix_dac8));
    return (static_cast<float>(mix_dac8) / 128.0f) * kMasterHeadroom;
}

float AudioSynth::step_and_mix(std::uint64_t cpu_cycles, std::uint32_t cpu_hz) noexcept
{
    if (cpu_cycles == 0u) return mix_instantaneous();
    const double total_ticks =
        static_cast<double>(cpu_cycles) * (kSoundClockHz / static_cast<double>(cpu_hz));
    const unsigned sgc = m_registers[0];
    if ((sgc & kMasterEnable) == 0 || (sgc & 0x07u) == 0) {
        advance_ticks(total_ticks);
        return mix_instantaneous();
    }

    /* Sub-sample across up to 8 slices within the audio frame to suppress
     * ultrasonic aliasing on fast wavetable and SG2 noise periods. */
    constexpr unsigned kSubSlices = 8u;
    const double slice_ticks = total_ticks / static_cast<double>(kSubSlices);
    float accum = 0.0f;
    for (unsigned s = 0; s < kSubSlices; ++s) {
        advance_ticks(slice_ticks);
        accum += mix_instantaneous();
    }
    return accum * (1.0f / static_cast<float>(kSubSlices));
}

void AudioSynth::render(const std::vector<SoundRegisterWrite>& writes,
                        std::uint64_t start_cycle, std::uint64_t end_cycle,
                        std::int16_t *output, std::size_t frames,
                        unsigned sample_rate,
                        std::uint32_t cpu_hz) noexcept
{
    if (end_cycle < start_cycle) return;
    if (cpu_hz == 0u) cpu_hz = GC_CPU_HZ;
    const std::uint64_t span = end_cycle - start_cycle;
    std::uint64_t cursor = start_cycle;
    std::size_t event_index = 0;

    if (output == nullptr || sample_rate == 0u || frames == 0u) {
        for (const SoundRegisterWrite& write : writes) {
            if (write.cycle > end_cycle) break;
            const std::uint64_t event_cycle = std::max(cursor, std::max(start_cycle, write.cycle));
            advance_ticks(static_cast<double>(event_cycle - cursor) *
                          (kSoundClockHz / static_cast<double>(cpu_hz)));
            cursor = event_cycle;
            apply_write(write);
        }
        advance_ticks(static_cast<double>(end_cycle - cursor) *
                      (kSoundClockHz / static_cast<double>(cpu_hz)));
        return;
    }

    if (sample_rate != m_cached_rate) {
        m_cached_rate = sample_rate;
        /* Match hardware ~12.4 kHz reconstruction LPF (tau = 64 / 5 MHz = 12.8 us)
         * and gentle AC-coupling DC servo (~4 Hz, tau = 40 ms). */
        const double dt = 1.0 / static_cast<double>(sample_rate);
        m_lpf_alpha = static_cast<float>(1.0 - std::exp(-dt / 0.0000128));
        m_dc_alpha = static_cast<float>(1.0 - std::exp(-dt / 0.040));
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint64_t sample_end = start_cycle +
            static_cast<std::uint64_t>((static_cast<long double>(frame + 1u) * span) / frames);
        while (event_index < writes.size() &&
               (writes[event_index].cycle < sample_end ||
                writes[event_index].cycle <= start_cycle)) {
            const std::uint64_t event_cycle = std::max(cursor,
                std::max(start_cycle, writes[event_index].cycle));
            advance_ticks(static_cast<double>(event_cycle - cursor) *
                          (kSoundClockHz / static_cast<double>(cpu_hz)));
            cursor = event_cycle;
            apply_write(writes[event_index]);
            ++event_index;
        }
        const std::uint64_t rem = (sample_end > cursor) ? (sample_end - cursor) : 0u;
        const float raw = step_and_mix(rem, cpu_hz);
        cursor = std::max(cursor, sample_end);

        if ((m_registers[0] & kMasterEnable) == 0) {
            output[frame] = 0;
        } else {
            m_lpf_state += (raw - m_lpf_state) * m_lpf_alpha;
            m_dc_baseline += (m_lpf_state - m_dc_baseline) * m_dc_alpha;
            output[frame] = to_pcm(m_lpf_state - m_dc_baseline);
        }
    }

    while (event_index < writes.size() && writes[event_index].cycle <= end_cycle) {
        const std::uint64_t event_cycle = std::max(cursor,
            std::max(start_cycle, writes[event_index].cycle));
        advance_ticks(static_cast<double>(event_cycle - cursor) *
                      (kSoundClockHz / static_cast<double>(cpu_hz)));
        cursor = event_cycle;
        apply_write(writes[event_index]);
        ++event_index;
    }
    if (end_cycle > cursor) {
        advance_ticks(static_cast<double>(end_cycle - cursor) *
                      (kSoundClockHz / static_cast<double>(cpu_hz)));
    }
}

} // namespace standalone
