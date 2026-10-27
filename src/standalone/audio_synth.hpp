#ifndef GAMECOM_AUDIO_SYNTH_HPP
#define GAMECOM_AUDIO_SYNTH_HPP

#include "gamecom.h"
#include "sound_events.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace standalone {

/* Cycle-timed SM8521 sound synthesizer:
 * - SG0 / SG1: 32-step signed 4-bit two's-complement wavetable channels (-8..+7,
 *   low nibble first), 12-bit (period + 1) divider clocked from 5.0 MHz phi0.
 * - SG2: 15-bit LFSR noise generator (x^15 + x + 1, +7/-8), 12-bit (period + 1)
 *   divider clocked from 5.0 MHz phi0, scaled by SG2L[4:0].
 * - SGDA: exclusive 8-bit unsigned direct DAC when SGC == 0x88 (masked by 0x8F).
 * - Output stage: sub-sample averaging + 1-pole reconstruction LPF + DC blocker. */
class AudioSynth {
public:
    void reset() noexcept;
    void render(const std::vector<SoundRegisterWrite>& writes,
                std::uint64_t start_cycle, std::uint64_t end_cycle,
                std::int16_t *output, std::size_t frames,
                unsigned sample_rate,
                std::uint32_t cpu_hz = GC_CPU_HZ) noexcept;

private:
    struct WaveChannel {
        double phase = 0.0;
    };

    void apply_write(const SoundRegisterWrite& write) noexcept;
    void advance_ticks(double phi0_ticks) noexcept;
    float mix_instantaneous() const noexcept;
    float step_and_mix(std::uint64_t cpu_cycles, std::uint32_t cpu_hz) noexcept;

    std::array<std::uint8_t, 0x40> m_registers{}; // sound register window 0x40..0x7f
    WaveChannel m_wave[2]{};
    double m_noise_tick_accum = 0.0;
    std::uint16_t m_lfsr = 0x7FFFu;
    int m_noise_sample = 7;
    std::uint8_t m_dac_byte = 0x80u;
    float m_lpf_state = 0.0f;
    float m_dc_baseline = 0.0f;
    unsigned m_cached_rate = 0;
    float m_lpf_alpha = 0.80f;
    float m_dc_alpha = 0.0005f;
};

} // namespace standalone
#endif
