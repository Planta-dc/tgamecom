#ifndef GAMECOM_LAUNCHER_HPP
#define GAMECOM_LAUNCHER_HPP

#include <array>
#include <string>

struct GcLaunchSelection {
    std::string firmware_dir;
    std::string cartridge_path;
};

enum GcControlIndex : unsigned {
    GC_CONTROL_UP = 0,
    GC_CONTROL_DOWN,
    GC_CONTROL_LEFT,
    GC_CONTROL_RIGHT,
    GC_CONTROL_A,
    GC_CONTROL_B,
    GC_CONTROL_C,
    GC_CONTROL_D,
    GC_CONTROL_MENU,
    GC_CONTROL_PAUSE,
    GC_CONTROL_SOUND,
    GC_CONTROL_POWER,
    GC_CONTROL_COUNT
};

struct GcInputBindings {
    std::array<int, GC_CONTROL_COUNT> keycodes{};
};

GcInputBindings gc_default_input_bindings();
GcInputBindings gc_load_input_bindings();
bool gc_save_input_bindings(const GcInputBindings& bindings);

// Opens the launcher; returns true when the user chose to boot, false on quit.
bool gc_sdl_launcher_run(GcLaunchSelection& selection, GcInputBindings& bindings);

#endif
