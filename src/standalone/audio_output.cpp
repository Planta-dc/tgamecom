#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_output.hpp"

#include <new>

namespace standalone {

AudioOutput::~AudioOutput()
{
    if (m_device != nullptr) {
        if (m_running) (void)ma_device_stop(m_device);
        if (m_initialized) ma_device_uninit(m_device);
        delete m_device;
    }
}

bool AudioOutput::start(std::string& error)
{
    if (m_running) return true;
    m_device = new (std::nothrow) ma_device{};
    if (m_device == nullptr) {
        error = "could not allocate audio device state";
        return false;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = 2;
    config.sampleRate = sample_rate;
    config.dataCallback = &AudioOutput::data_callback;
    config.pUserData = this;

    ma_result result = ma_device_init(nullptr, &config, m_device);
    if (result != MA_SUCCESS) {
        error = ma_result_description(result);
        delete m_device;
        m_device = nullptr;
        return false;
    }
    m_initialized = true;
    result = ma_device_start(m_device);
    if (result != MA_SUCCESS) {
        error = ma_result_description(result);
        ma_device_uninit(m_device);
        delete m_device;
        m_device = nullptr;
        m_initialized = false;
        return false;
    }
    m_running = true;
    return true;
}

void AudioOutput::push(const std::int16_t *samples, std::size_t frames) noexcept
{
    if (!m_running || samples == nullptr || frames == 0) return;
    std::uint64_t write = m_write_index.load(std::memory_order_relaxed);
    const std::uint64_t read = m_read_index.load(std::memory_order_acquire);
    const std::uint64_t free = ring_capacity - (write - read);
    if (frames > free) {
        /* Keep producer lock-free; if the host is lagging, discard this block. */
        return;
    }
    for (std::size_t i = 0; i < frames; ++i)
        m_ring[(write + i) & ring_mask] = samples[i];
    m_write_index.store(write + frames, std::memory_order_release);
}

void AudioOutput::data_callback(ma_device *device, void *output, const void *,
                                std::uint32_t frame_count)
{
    auto *self = static_cast<AudioOutput *>(device->pUserData);
    self->consume(static_cast<std::int16_t *>(output), frame_count);
}

void AudioOutput::consume(std::int16_t *output, std::uint32_t frames) noexcept
{
    std::uint64_t read = m_read_index.load(std::memory_order_relaxed);
    const std::uint64_t write = m_write_index.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::int16_t sample = read != write ? m_ring[read++ & ring_mask] : 0;
        output[2u * i] = sample;
        output[2u * i + 1u] = sample;
    }
    m_read_index.store(read, std::memory_order_release);
}

}
