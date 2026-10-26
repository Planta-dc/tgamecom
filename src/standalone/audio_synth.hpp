#ifndef GAMECOM_AUDIO_SYNTH_HPP
#define GAMECOM_AUDIO_SYNTH_HPP

#include "gamecom.h"
#include <cstddef>
#include <cstdint>

namespace standalone {

/* Frame-based first-pass SM8521 sound generator. The sound registers are
 * sampled at video-frame boundaries; timer/wave behavior follows the MAME
 * reference, but sub-frame register-write timing is not yet modeled. */
class AudioSynth {
public:
    void reset() noexcept;
    void render(const GcMachine& machine, std::int16_t *output,
                std::size_t frames, unsigned sample_rate) noexcept;

private:
    struct Channel {
        double phase = 0.0;
        bool enabled = false;
    };
    Channel m_channels[2]{};
};

}
#endif
