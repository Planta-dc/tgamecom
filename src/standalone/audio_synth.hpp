#ifndef GAMECOM_AUDIO_SYNTH_HPP
#define GAMECOM_AUDIO_SYNTH_HPP

#include "gamecom.h"
#include "sound_events.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace standalone {

/* Event-driven first-pass SM8521 mixer. CPU writes to the sound register file
 * are timed against emulated cycles; SG0/SG1 and the direct DAC are synthesized
 * from that timeline. The SG2/noise behavior remains unimplemented. */
class AudioSynth {
public:
    void reset() noexcept;
    void render(const std::vector<SoundRegisterWrite>& writes,
                std::uint64_t start_cycle, std::uint64_t end_cycle,
                std::int16_t *output, std::size_t frames,
                unsigned sample_rate,
                std::uint32_t cpu_hz = GC_CPU_HZ) noexcept;

private:
    struct Channel {
        double phase = 0.0;
        bool enabled = false;
    };

    void apply_write(const SoundRegisterWrite& write) noexcept;
    void advance_cycles(std::uint64_t cycles, std::uint32_t cpu_hz) noexcept;
    float channel_value(unsigned channel) const noexcept;
    float direct_dac_value() const noexcept;

    std::array<std::uint8_t, 0x40> m_registers{}; // sound register window 0x40..0x7f
    Channel m_channels[2]{};
    float m_dac_value = 0.0f;
};

}
#endif
