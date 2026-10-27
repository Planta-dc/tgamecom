#ifndef GAMECOM_SOUND_EVENTS_HPP
#define GAMECOM_SOUND_EVENTS_HPP

#include <cstdint>

namespace standalone {

struct SoundRegisterWrite {
    std::uint64_t cycle = 0;
    std::uint8_t address = 0;
    std::uint8_t value = 0;
};

}
#endif
