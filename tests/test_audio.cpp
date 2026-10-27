#include "audio_synth.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

int main()
{
    standalone::AudioSynth synth;
    constexpr unsigned rate = 48000;
    constexpr std::size_t frames = 12000;
    constexpr std::uint64_t cycle_span =
        static_cast<std::uint64_t>(frames) * GC_CPU_HZ / rate;
    auto pcm = std::make_unique<std::int16_t[]>(frames);

    /* A pair of writes configures and enables SG0; wave-RAM writes are timed
     * events too, so the channel sees the table at its emulated start time. */
    std::vector<standalone::SoundRegisterWrite> writes = {
        {0, 0x40, 0x81}, {0, 0x42, 31},
        {0, 0x46, 0x03}, {0, 0x47, 0xE8} /* period 1000 */
    };
    for (unsigned i = 0; i < 16; ++i)
        writes.push_back({0, static_cast<std::uint8_t>(0x60 + i), 0xF0});
    synth.render(writes, 0, cycle_span, pcm.get(), frames, rate);

    bool positive = false, negative = false;
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        positive = positive || pcm[i] > 1000;
        negative = negative || pcm[i] < -1000;
        const std::int32_t magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (magnitude > peak) peak = magnitude;
    }
    assert(positive && negative && peak > 1000);

    /* Master disable is applied at its register-write cycle and mutes SG0. */
    const std::vector<standalone::SoundRegisterWrite> mute = {{cycle_span, 0x40, 0x01}};
    synth.render(mute, cycle_span, cycle_span * 2, pcm.get(), frames, rate);
    for (std::size_t i = 0; i < frames; ++i) assert(pcm[i] == 0);

    /* SGDA writes are individual audio events: a mid-block value change must
     * create a corresponding polarity change, not be collapsed to the final
     * register snapshot for the whole video frame. */
    constexpr std::size_t dac_frames = 128;
    constexpr std::uint64_t dac_span =
        static_cast<std::uint64_t>(dac_frames) * GC_CPU_HZ / rate;
    const std::uint64_t dac_start = cycle_span * 2;
    const std::vector<standalone::SoundRegisterWrite> dac_events = {
        {dac_start, 0x40, 0x88},
        {dac_start, 0x4e, 0xff},
        {dac_start + dac_span / 2, 0x4e, 0x00}
    };
    synth.render(dac_events, dac_start, dac_start + dac_span,
                 pcm.get(), dac_frames, rate);
    for (std::size_t i = 0; i < dac_frames / 2; ++i) assert(pcm[i] > 1000);
    for (std::size_t i = dac_frames / 2 + 2; i < dac_frames; ++i) assert(pcm[i] < -1000);

    const std::vector<standalone::SoundRegisterWrite> dac_mute = {
        {dac_start + dac_span, 0x40, 0x00}
    };
    synth.render(dac_mute, dac_start + dac_span,
                 dac_start + dac_span * 2, pcm.get(), dac_frames, rate);
    for (std::size_t i = 0; i < dac_frames; ++i) assert(pcm[i] == 0);

    std::puts("SM8521 sound tests passed (SG0 waveform, timed DAC writes, enable gates).");
    return 0;
}
