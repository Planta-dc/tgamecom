#ifndef GAMECOM_LAUNCHER_HPP
#define GAMECOM_LAUNCHER_HPP

#include <array>
#include <string>

enum GcTouchMode : unsigned {
    GC_TOUCH_SINGLE_TAP = 0,    /* Auto-releases 1 VBlank after CPU scans touched column (no hold repeat). */
    GC_TOUCH_DEBOUNCED_TAP = 1, /* Auto-releases 2 VBlanks after CPU scans touched column. */
    GC_TOUCH_HOLD_DRAG = 2      /* Continuous stylus contact while mouse button is held. */
};

enum GcSerialMode : unsigned {
    GC_SERIAL_OFF = 0,
    GC_SERIAL_MODEM = 1,      /* Virtual 9600-baud Hayes AT modem + text portal / TCP dialer. */
    GC_SERIAL_TCP_HOST = 2,   /* 2-player Compete.com null-modem TCP host. */
    GC_SERIAL_TCP_CLIENT = 3  /* 2-player Compete.com null-modem TCP client. */
};

struct GcTimingOptions {
    double lcd_hz = 5000000.0 / 66240.0; /* Native SM8521 LCDC VBlank (~75.483 Hz = 5 MHz / (320 * 207)). */
    unsigned cpu_speed_percent = 100;    /* Emulated SM8521 CPU clock scale (% of 5.000 MHz phi0). */
    unsigned game_pace_percent = 100;    /* Overall emulation pace multiplier (%, scales CPU + LCD together). */
    bool vsync = false;                  /* Off by default so host frame rate paces directly to lcd_hz. */
    bool scale_cpu_with_lcd = false;     /* Optional: scale SM8521 CPU/TIM1 speed proportionally with lcd_hz. */
    GcTouchMode touch_mode = GC_TOUCH_SINGLE_TAP;
    bool audio_muted = false;
    unsigned audio_volume_percent = 100;
    unsigned lcd_palette = 0;            /* 0=Natural STN, 1=Greenish, 2=Grayish, 3=High-Contrast */
    bool integer_scaling = false;
    bool auto_hide_menubar = false;
};

struct GcLaunchSelection {
    std::string firmware_dir;
    std::string cartridge_path;
    std::string cartridge2_path;
    std::string nvram_path;
    bool warm_boot_nvram = false;
    GcSerialMode serial_mode = GC_SERIAL_OFF;
    std::string serial_endpoint = "127.0.0.1:1997";
    std::string phonebook_path;
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
std::string gc_default_nvram_path();

// Opens the standalone launcher window; returns true when the user chose to boot, false on quit.
bool gc_sdl_launcher_run(GcLaunchSelection& selection, GcInputBindings& bindings);

#endif
