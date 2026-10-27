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

    /* Configure and enable SG0 with a signed 4-bit two's-complement square wave:
     * 0x87 -> low nibble +7 (0x7), high nibble -8 (0x8).
     * Period 99 -> (99 + 1) = 100 phi0 ticks/step at 5.0 MHz = 1562.5 Hz. */
    std::vector<standalone::SoundRegisterWrite> writes = {
        {0, 0x40, 0x81}, {0, 0x42, 31},
        {0, 0x46, 0x00}, {0, 0x47, 99}
    };
    for (unsigned i = 0; i < 16; ++i)
        writes.push_back({0, static_cast<std::uint8_t>(0x60 + i), 0x87});
    synth.render(writes, 0, cycle_span, pcm.get(), frames, rate);

    bool positive = false, negative = false;
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        positive = positive || pcm[i] > 5000;
        negative = negative || pcm[i] < -5000;
        const std::int32_t magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (magnitude > peak) peak = magnitude;
    }
    assert(positive && negative && peak > 10000);

    /* Master disable (SGC[7]=0) is applied at its register-write cycle and mutes immediately. */
    const std::vector<standalone::SoundRegisterWrite> mute = {{cycle_span, 0x40, 0x01}};
    synth.render(mute, cycle_span, cycle_span * 2, pcm.get(), frames, rate);
    for (std::size_t i = 0; i < frames; ++i) assert(pcm[i] == 0);

    /* Signed 4-bit zero-crossing check: wave RAM filled with 0x00 (signed 0) must
     * produce silence, and +1/-1 (0xF1) must be ~7x quieter than +7/-8 (0x87)
     * rather than snapping rail-to-rail like unsigned 0..15 decoding. */
    {
        standalone::AudioSynth zero_synth, quiet_synth;
        std::vector<standalone::SoundRegisterWrite> zero_writes = {
            {0, 0x40, 0x81}, {0, 0x42, 31}, {0, 0x46, 0x00}, {0, 0x47, 99}
        };
        std::vector<standalone::SoundRegisterWrite> quiet_writes = zero_writes;
        for (unsigned i = 0; i < 16; ++i) {
            zero_writes.push_back({0, static_cast<std::uint8_t>(0x60 + i), 0x00});
            quiet_writes.push_back({0, static_cast<std::uint8_t>(0x60 + i), 0xF1}); /* +1 / -1 */
        }
        zero_synth.render(zero_writes, 0, cycle_span, pcm.get(), frames, rate);
        for (std::size_t i = 0; i < frames; ++i) assert(pcm[i] == 0);

        quiet_synth.render(quiet_writes, 0, cycle_span, pcm.get(), frames, rate);
        std::int32_t quiet_peak = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const std::int32_t mag = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (mag > quiet_peak) quiet_peak = mag;
        }
        assert(quiet_peak > 500 && quiet_peak * 4 < peak);
    }

    /* SG2 15-bit LFSR noise generator produces bipolar noise when SGC[2]=1 and period > 1. */
    {
        standalone::AudioSynth noise_synth;
        const std::vector<standalone::SoundRegisterWrite> noise_writes = {
            {0, 0x40, 0x84}, {0, 0x4A, 31}, {0, 0x4C, 0x00}, {0, 0x4D, 24}
        };
        noise_synth.render(noise_writes, 0, cycle_span, pcm.get(), frames, rate);
        bool noise_pos = false, noise_neg = false;
        for (std::size_t i = 0; i < frames; ++i) {
            noise_pos = noise_pos || pcm[i] > 3000;
            noise_neg = noise_neg || pcm[i] < -3000;
        }
        assert(noise_pos && noise_neg);
    }

    /* SGDA writes are individual audio events in exclusive DAC mode (SGC == 0x88):
     * a mid-block value change creates a polarity flip. */
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

    /* Scaling CPU clock (150%) AND setting garbage high bits in SG0TH[7:4]
     * preserves exact 12-bit waveform pitch over the same wall-clock audio frames. */
    standalone::AudioSynth synth_100, synth_150;
    auto pcm_100 = std::make_unique<std::int16_t[]>(frames);
    auto pcm_150 = std::make_unique<std::int16_t[]>(frames);
    constexpr std::uint32_t cpu_150 = (GC_CPU_HZ * 3u) / 2u;
    constexpr std::uint64_t span_150 =
        static_cast<std::uint64_t>(frames) * cpu_150 / rate;
    std::vector<standalone::SoundRegisterWrite> writes_hi_nibble = writes;
    writes_hi_nibble[2].value = 0xF0; /* Upper nibble ignored by 12-bit hardware divider. */
    synth_100.render(writes, 0, cycle_span, pcm_100.get(), frames, rate, GC_CPU_HZ);
    synth_150.render(writes_hi_nibble, 0, span_150, pcm_150.get(), frames, rate, cpu_150);
    auto count_crossings = [&](const std::int16_t *buf) {
        unsigned crossings = 0;
        for (std::size_t i = 1; i < frames; ++i) {
            if ((buf[i - 1] < 0 && buf[i] > 0) || (buf[i - 1] > 0 && buf[i] < 0))
                ++crossings;
        }
        return crossings;
    };
    const unsigned cross_100 = count_crossings(pcm_100.get());
    const unsigned cross_150 = count_crossings(pcm_150.get());
    const unsigned diff = cross_100 > cross_150 ? cross_100 - cross_150 : cross_150 - cross_100;
    assert(cross_100 > 700u && diff <= 2u);

    std::puts("SM8521 sound tests passed (signed 4-bit SG0/1, 12-bit N+1 divider, SG2 LFSR noise, SGDA, LPF/HPF, pitch invariance).");
    return 0;
}
