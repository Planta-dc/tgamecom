#ifndef GAMECOM_LAUNCHER_HPP
#define GAMECOM_LAUNCHER_HPP

#include <array>
#include <string>

enum GcTouchMode : unsigned {
    GC_TOUCH_SINGLE_TAP = 0,    /* Auto-releases 1 VBlank after CPU scans touched column (no hold repeat). */
    GC_TOUCH_DEBOUNCED_TAP = 1, /* Auto-releases 2 VBlanks after CPU scans touched column. */
    GC_TOUCH_HOLD_DRAG = 2      /* Continuous stylus contact while mouse button is held. */
};

struct GcTimingOptions {
    double lcd_hz = 60.0;               /* Emulated LCD VBlank refresh rate (Hz), decoupled from monitor VSync & CPU. */
    unsigned cpu_speed_percent = 100;   /* Emulated SM8521 CPU clock scale (% of 5.5296 MHz). */
    unsigned game_pace_percent = 100;   /* Overall emulation pace multiplier (%, scales CPU + LCD together). */
    GcTouchMode touch_mode = GC_TOUCH_SINGLE_TAP;
    bool audio_muted = false;
    unsigned audio_volume_percent = 100;
    bool integer_scaling = false;
    bool auto_hide_menubar = false;
};

struct GcLaunchSelection {
    std::string firmware_dir;
    std::string cartridge_path;
    GcTimingOptions timing{};
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

GcLaunchSelection gc_load_launch_selection();
bool gc_save_launch_selection(const GcLaunchSelection& selection);

// Opens the standalone launcher window; returns true when the user chose to boot, false on quit.
bool gc_sdl_launcher_run(GcLaunchSelection& selection, GcInputBindings& bindings);

#endif
