#ifndef GAMECOM_AUDIO_OUTPUT_HPP
#define GAMECOM_AUDIO_OUTPUT_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

struct ma_device;

namespace standalone {

/* Optional miniaudio playback sink. The emulator thread is the sole producer;
 * the device callback is the sole consumer of this lock-free PCM ring. */
class AudioOutput {
public:
    static constexpr unsigned sample_rate = 48000;
    AudioOutput() = default;
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool start(std::string& error);
    void push(const std::int16_t *samples, std::size_t frames) noexcept;
    bool running() const noexcept { return m_running; }

private:
    static void data_callback(ma_device *device, void *output, const void *input,
                              std::uint32_t frame_count);
    void consume(std::int16_t *stereo_output, std::uint32_t frames) noexcept;

    static constexpr std::uint64_t ring_capacity = 1u << 16;
    static constexpr std::uint64_t ring_mask = ring_capacity - 1u;
    std::array<std::int16_t, ring_capacity> m_ring{};
    std::atomic<std::uint64_t> m_write_index{0};
    std::atomic<std::uint64_t> m_read_index{0};
    ma_device *m_device = nullptr;
    bool m_running = false;
    bool m_initialized = false;
};

}
#endif
