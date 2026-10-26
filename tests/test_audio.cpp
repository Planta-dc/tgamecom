#include "audio_synth.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

int main()
{
    auto machine = std::make_unique<GcMachine>();
    gc_machine_init(machine.get());
    standalone::AudioSynth synth;
    constexpr unsigned rate = 48000;
    constexpr std::size_t frames = 12000;
    auto pcm = std::make_unique<std::int16_t[]>(frames);

    /* Channel 0: enabled looping 4-bit waveform with a non-zero timer and level. */
    machine->bus.reg_file[0x40] = 0x81;
    machine->bus.reg_file[0x42] = 31;
    machine->bus.reg_file[0x46] = 0x03;
    machine->bus.reg_file[0x47] = 0xE8; /* period 1000 */
    for (unsigned i = 0; i < 16; ++i) machine->bus.reg_file[0x60 + i] = 0xF0;
    synth.render(*machine, pcm.get(), frames, rate);

    bool positive = false, negative = false;
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        positive = positive || pcm[i] > 1000;
        negative = negative || pcm[i] < -1000;
        const std::int32_t magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (magnitude > peak) peak = magnitude;
    }
    assert(positive && negative && peak > 1000);

    /* Master disable must mute the waveform without requiring a device. */
    machine->bus.reg_file[0x40] = 0x01;
    synth.render(*machine, pcm.get(), frames, rate);
    for (std::size_t i = 0; i < frames; ++i) assert(pcm[i] == 0);

    /* The direct DAC path is independently gated by master + DAC enable. */
    machine->bus.reg_file[0x40] = 0x88;
    machine->bus.reg_file[0x4e] = 0xFF;
    synth.render(*machine, pcm.get(), 128, rate);
    assert(pcm[0] > 1000);
    machine->bus.reg_file[0x40] = 0x00;
    synth.render(*machine, pcm.get(), 128, rate);
    for (std::size_t i = 0; i < 128; ++i) assert(pcm[i] == 0);

    std::puts("SM8521 sound synthesis tests passed (wave channels, mute, DAC gate).");
    return 0;
}
