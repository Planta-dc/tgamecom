#include "gamecom_sdl.hpp"
#include "gamecom_demo.h"
#include "gamecom_video.h"
#include "sdl_ui.hpp"
#include "standalone/audio_output.hpp"
#include "standalone/audio_synth.hpp"
#include "standalone/serial_link.hpp"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace gc_ui_palette;

constexpr std::array<double, 8> lcd_hz_presets = {
    30.0, 50.0, 59.732155, 60.0, 75.0, GC_LCD_NATIVE_HZ, 90.0, 120.0
};
constexpr std::array<unsigned, 6> cpu_speed_presets = {
    75u, 100u, 120u, 150u, 200u, 300u
};

std::string join_path(const std::string& directory, const char *name)
{
    std::string path = directory;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path.push_back('/');
    path += name;
    return path;
}

double cycle_lcd_hz_preset(double current, int delta)
{
    std::size_t best = 0;
    double best_diff = std::fabs(current - lcd_hz_presets[0]);
    for (std::size_t i = 1; i < lcd_hz_presets.size(); ++i) {
        const double diff = std::fabs(current - lcd_hz_presets[i]);
        if (diff < best_diff) { best = i; best_diff = diff; }
    }
    const int count = static_cast<int>(lcd_hz_presets.size());
    const int next = ((static_cast<int>(best) + delta) % count + count) % count;
    return lcd_hz_presets[static_cast<std::size_t>(next)];
}

unsigned step_cpu_speed_preset(unsigned current, int delta)
{
    std::size_t best = 0;
    unsigned best_diff = current > cpu_speed_presets[0]
        ? current - cpu_speed_presets[0] : cpu_speed_presets[0] - current;
    for (std::size_t i = 1; i < cpu_speed_presets.size(); ++i) {
        const unsigned diff = current > cpu_speed_presets[i]
            ? current - cpu_speed_presets[i] : cpu_speed_presets[i] - current;
        if (diff < best_diff) { best = i; best_diff = diff; }
    }
    const int next = std::max(0, std::min(static_cast<int>(cpu_speed_presets.size()) - 1,
                                          static_cast<int>(best) + delta));
    return cpu_speed_presets[static_cast<std::size_t>(next)];
}

bool is_bound_key(const GcInputBindings& bindings, SDL_Keycode key)
{
    for (unsigned action = 0; action < GC_CONTROL_COUNT; ++action) {
        if (bindings.keycodes[action] == static_cast<int>(key)) return true;
    }
    return false;
}

void update_key(GcMachine& machine, const GcInputBindings& bindings,
                SDL_Keycode key, bool down)
{
    for (unsigned action = 0; action < GC_CONTROL_COUNT; ++action) {
        if (bindings.keycodes[action] != static_cast<int>(key)) continue;
        switch (action) {
        case GC_CONTROL_UP:    gc_input_set_button(&machine, GC_BUTTON_UP, down); break;
        case GC_CONTROL_DOWN:  gc_input_set_button(&machine, GC_BUTTON_DOWN, down); break;
        case GC_CONTROL_LEFT:  gc_input_set_button(&machine, GC_BUTTON_LEFT, down); break;
        case GC_CONTROL_RIGHT: gc_input_set_button(&machine, GC_BUTTON_RIGHT, down); break;
        case GC_CONTROL_A:     gc_input_set_button(&machine, GC_BUTTON_A, down); break;
        case GC_CONTROL_B:     gc_input_set_button(&machine, GC_BUTTON_B, down); break;
        case GC_CONTROL_C:     gc_input_set_button(&machine, GC_BUTTON_C, down); break;
        case GC_CONTROL_D:     gc_input_set_button(&machine, GC_BUTTON_D, down); break;
        case GC_CONTROL_MENU:  gc_input_set_button(&machine, GC_BUTTON_MENU, down); break;
        case GC_CONTROL_PAUSE: gc_input_set_button(&machine, GC_BUTTON_PAUSE, down); break;
        case GC_CONTROL_SOUND: gc_input_set_button(&machine, GC_BUTTON_SOUND, down); break;
        case GC_CONTROL_POWER: gc_input_set_button(&machine, GC_BUTTON_POWER, down); break;
        default: break;
        }
    }
}

void clear_machine_inputs(GcMachine& machine)
{
    static constexpr std::array<GcButton, 12> buttons = {
        GC_BUTTON_UP, GC_BUTTON_DOWN, GC_BUTTON_LEFT, GC_BUTTON_RIGHT,
        GC_BUTTON_MENU, GC_BUTTON_PAUSE, GC_BUTTON_SOUND,
        GC_BUTTON_A, GC_BUTTON_B, GC_BUTTON_C, GC_BUTTON_POWER, GC_BUTTON_D
    };
    for (GcButton b : buttons) gc_input_set_button(&machine, b, false);
    (void)gc_input_set_touch(&machine, false, 0, 0);
}

SDL_Rect compute_lcd_rect(SDL_Renderer *renderer, int top_reserved,
                          bool integer_scaling, int& output_w, int& output_h)
{
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
    const int avail_w = std::max(1, output_w);
    const int avail_h = std::max(1, output_h - top_reserved);

    SDL_Rect destination{};
    if (integer_scaling && avail_w >= static_cast<int>(GC_LCD_WIDTH) &&
        avail_h >= static_cast<int>(GC_LCD_HEIGHT)) {
        const int scale = std::max(1, std::min(
            avail_w / static_cast<int>(GC_LCD_WIDTH),
            avail_h / static_cast<int>(GC_LCD_HEIGHT)));
        destination.w = static_cast<int>(GC_LCD_WIDTH) * scale;
        destination.h = static_cast<int>(GC_LCD_HEIGHT) * scale;
    } else {
        const double scale = std::min(
            avail_w / static_cast<double>(GC_LCD_WIDTH),
            avail_h / static_cast<double>(GC_LCD_HEIGHT));
        destination.w = std::max(1, static_cast<int>(GC_LCD_WIDTH * scale));
        destination.h = std::max(1, static_cast<int>(GC_LCD_HEIGHT * scale));
    }
    destination.x = (avail_w - destination.w) / 2;
    destination.y = top_reserved + (avail_h - destination.h) / 2;
    return destination;
}

bool map_mouse_to_touch_cell(SDL_Window *window, SDL_Renderer *renderer,
                             int top_reserved, bool integer_scaling,
                             int raw_x, int raw_y,
                             unsigned& out_col, unsigned& out_row)
{
    int window_w = 0, window_h = 0, output_w = 0, output_h = 0;
    SDL_GetWindowSize(window, &window_w, &window_h);
    const SDL_Rect destination = compute_lcd_rect(
        renderer, top_reserved, integer_scaling, output_w, output_h);
    if (window_w <= 0 || window_h <= 0 || destination.w <= 0 || destination.h <= 0)
        return false;
    const int output_x = raw_x * output_w / window_w;
    const int output_y = raw_y * output_h / window_h;
    if (output_x < destination.x || output_x >= destination.x + destination.w ||
        output_y < destination.y || output_y >= destination.y + destination.h) {
        return false;
    }
    const unsigned lcd_x = std::min<unsigned>(
        GC_LCD_WIDTH - 1u,
        static_cast<unsigned>((output_x - destination.x) * static_cast<int>(GC_LCD_WIDTH) / destination.w));
    const unsigned lcd_y = std::min<unsigned>(
        GC_LCD_HEIGHT - 1u,
        static_cast<unsigned>((output_y - destination.y) * static_cast<int>(GC_LCD_HEIGHT) / destination.h));
    /* Twelve 16px columns (0..191 -> 0..11) plus 8px column 12 (192..199); ten 16px rows. */
    out_col = std::min<unsigned>(12u, lcd_x / 16u);
    out_row = std::min<unsigned>(9u, lcd_y / 16u);
    return true;
}

void update_window_title(SDL_Window *window, const GcLaunchSelection& sel,
                         bool in_launcher, bool paused, bool fast_forward,
                         double host_fps, double measured_lcd_hz)
{
    if (in_launcher) {
        SDL_SetWindowTitle(window, "Game.com Emulator — Launcher");
        return;
    }
    char title[200];
    double mhz = ((static_cast<double>(GC_CPU_HZ) / 1000000.0) *
                  static_cast<double>(sel.timing.cpu_speed_percent)) / 100.0;
    if (sel.timing.scale_cpu_with_lcd)
        mhz *= (sel.timing.lcd_hz / GC_LCD_NATIVE_HZ);
    const char *state_tag = fast_forward ? " [4x TURBO]" : (paused ? " [PAUSED]" : "");
    if (host_fps > 0.5 && !paused) {
        std::snprintf(title, sizeof(title),
                      "Game.com%s | LCD %.2f Hz (%.1f/s) | CPU %u%% (%.2f MHz) | %.1f FPS%s",
                      state_tag, sel.timing.lcd_hz,
                      measured_lcd_hz > 0.1 ? measured_lcd_hz : sel.timing.lcd_hz,
                      sel.timing.cpu_speed_percent, mhz, host_fps,
                      sel.timing.vsync ? " [VSync]" : "");
    } else {
        std::snprintf(title, sizeof(title),
                      "Game.com%s | LCD %.2f Hz | CPU %u%% (%.2f MHz)%s",
                      state_tag, sel.timing.lcd_hz, sel.timing.cpu_speed_percent, mhz,
                      sel.timing.vsync ? " [VSync]" : "");
    }
    SDL_SetWindowTitle(window, title);
}

std::string effective_nvram_path(const GcLaunchSelection& sel)
{
    return sel.nvram_path.empty() ? gc_default_nvram_path() : sel.nvram_path;
}

bool save_machine_nvram(const GcMachine& machine, const GcLaunchSelection& sel,
                        std::string *out_error = nullptr)
{
    const std::string path = effective_nvram_path(sel);
    if (path.empty()) return false;
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    char error[160] = {};
    if (!gc_nvram_save(&machine, path.c_str(), error, sizeof(error))) {
        if (out_error) *out_error = error;
        return false;
    }
    return true;
}

bool load_machine_media(GcMachine& machine, const GcLaunchSelection& sel,
                        bool demo_mode, std::string& out_error)
{
    gc_machine_init(&machine);
    char error[192] = {};
    if (demo_mode) {
        constexpr std::size_t capacity = (200u * 40u / 2u) * 8u + 1u;
        std::vector<std::uint8_t> program(capacity);
        const std::size_t length = gc_build_vram_demo(program.data(), program.size());
        if (!length || length > GC_PATCH_SIZE - 0x1020u) {
            out_error = "COULD NOT BUILD THE HOMEBREW VRAM DEMO.";
            return false;
        }
        std::memcpy(machine.bus.patch + 0x1020u, program.data(), length);
        std::memset(machine.bus.patch_valid + 0x1020u, 1, length);
        machine.bus.reg_file[0x30] = static_cast<std::uint8_t>(
            (machine.bus.reg_file[0x30] & 0x7Fu) | 0x80u);
    } else {
        if (sel.firmware_dir.empty()) {
            out_error = "SELECT A FIRMWARE FOLDER FIRST.";
            return false;
        }
        std::error_code ec;
        const std::string internal_cand = join_path(sel.firmware_dir, "internal.bin");
        const bool has_internal = fs::is_regular_file(internal_cand, ec) && !ec;
        ec.clear();
        std::string external_path = join_path(sel.firmware_dir, "external.bin");
        if (!fs::is_regular_file(external_path, ec) || ec) {
            ec.clear();
            const std::string alias = join_path(sel.firmware_dir, "boot.rom");
            if (fs::is_regular_file(alias, ec) && !ec) external_path = alias;
        }
        if (!gc_load_firmware(&machine,
                              has_internal ? internal_cand.c_str() : nullptr,
                              external_path.c_str(), error, sizeof(error))) {
            out_error = std::string("FIRMWARE ERROR: ") + error;
            return false;
        }
        if (!sel.cartridge_path.empty() &&
            !gc_load_cartridge_slot(&machine, 1u, sel.cartridge_path.c_str(),
                                    error, sizeof(error))) {
            out_error = std::string("SLOT 1 ROM ERROR: ") + error;
            return false;
        }
        if (!sel.cartridge2_path.empty() &&
            !gc_load_cartridge_slot(&machine, 2u, sel.cartridge2_path.c_str(),
                                    error, sizeof(error))) {
            out_error = std::string("SLOT 2 ROM ERROR: ") + error;
            return false;
        }
        const std::string nvram = effective_nvram_path(sel);
        ec.clear();
        if (!nvram.empty() && fs::is_regular_file(nvram, ec) && !ec) {
            (void)gc_nvram_load(&machine, nvram.c_str(), error, sizeof(error));
        }
        if (sel.warm_boot_nvram) gc_nvram_seed_warm_boot(&machine);
    }
    gc_machine_reset(&machine);
    return true;
}

} // namespace

int gc_sdl_session_run(GcMachine& machine, standalone::Sm8521Cpu& cpu,
                       GcLaunchSelection& selection, GcInputBindings& bindings,
                       bool start_in_launcher, bool demo_mode)
{
    const bool owns_sdl = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (owns_sdl && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(selection.timing.lcd_hz);
    selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(selection.timing.cpu_speed_percent);
    selection.timing.game_pace_percent = gc_ui_clamp_game_pace(selection.timing.game_pace_percent);

    const int init_w = start_in_launcher ? 920 : static_cast<int>(GC_LCD_WIDTH * 4u);
    const int init_h = start_in_launcher
        ? 640 : static_cast<int>(GC_LCD_HEIGHT * 4u) + GcTopMenuBar::height;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *window = SDL_CreateWindow("Game.com Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        init_w, init_h, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (owns_sdl) SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 440, 360);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    const Uint32 initial_vsync_flag = selection.timing.vsync
        ? static_cast<Uint32>(SDL_RENDERER_PRESENTVSYNC) : 0u;
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | initial_vsync_flag);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        if (owns_sdl) SDL_Quit();
        return 1;
    }

    auto query_vsync = [&]() -> bool {
        SDL_RendererInfo rinfo{};
        return (SDL_GetRendererInfo(renderer, &rinfo) == 0) &&
               ((rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0);
    };
    bool vsync_active = query_vsync();

    auto apply_vsync_setting = [&]() {
#if SDL_VERSION_ATLEAST(2, 0, 18)
        (void)SDL_RenderSetVSync(renderer, selection.timing.vsync ? 1 : 0);
        vsync_active = query_vsync();
#endif
    };
    apply_vsync_setting();

    SDL_Texture *lcd_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, static_cast<int>(GC_LCD_WIDTH),
        static_cast<int>(GC_LCD_HEIGHT));
    if (!lcd_texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        if (owns_sdl) SDL_Quit();
        return 1;
    }

    GcUiFont font(renderer);
    GcTopMenuBar menubar;

    const bool prev_ext_vblank = cpu.external_vblank();
    cpu.set_external_vblank(true);

    auto apply_cpu_clock = [&]() -> std::uint32_t {
        double scaled = static_cast<double>(GC_CPU_HZ) *
                        static_cast<double>(selection.timing.cpu_speed_percent) / 100.0;
        if (selection.timing.scale_cpu_with_lcd)
            scaled *= (selection.timing.lcd_hz / GC_LCD_NATIVE_HZ);
        const std::uint32_t hz = static_cast<std::uint32_t>(
            std::max(100000.0, std::min(100000000.0, std::round(scaled))));
        cpu.set_clock_hz(hz);
        return hz;
    };
    std::uint32_t nominal_cpu_hz = apply_cpu_clock();

    standalone::AudioSynth sound;
    standalone::AudioOutput audio_output;
    standalone::SerialLink serial_link;
    std::string audio_error;
    if (!audio_output.start(audio_error)) {
        std::fprintf(stderr, "Audio unavailable (%s); continuing silently.\n",
                     audio_error.c_str());
    }

    auto apply_serial_mode = [&](bool announce) {
        std::string serr;
        switch (selection.serial_mode) {
        case GC_SERIAL_OFF:
            serial_link.disconnect();
            gc_serial_set_inputs(&machine, true, true);
            break;
        case GC_SERIAL_MODEM:
            serial_link.enable_virtual_modem();
            if (!selection.phonebook_path.empty())
                (void)serial_link.load_phonebook(selection.phonebook_path, &serr);
            break;
        case GC_SERIAL_TCP_HOST: {
            std::string host;
            std::uint16_t port = 1997;
            if (!standalone::parse_host_port(selection.serial_endpoint, host, port, 1997))
                port = 1997;
            if (!serial_link.listen_tcp(port, &serr)) {
                menubar.show_toast("TCP LISTEN FAILED: " + serr, 6.0);
                return;
            }
            break;
        }
        case GC_SERIAL_TCP_CLIENT: {
            std::string host = "127.0.0.1";
            std::uint16_t port = 1997;
            if (!standalone::parse_host_port(selection.serial_endpoint, host, port, 1997) ||
                !serial_link.connect_tcp(host, port, &serr)) {
                menubar.show_toast("TCP CONNECT FAILED: " + serr, 6.0);
                return;
            }
            break;
        }
        }
        if (announce) menubar.show_toast(serial_link.status_summary());
    };
    apply_serial_mode(false);

    std::array<std::uint32_t, GC_LCD_WIDTH * GC_LCD_HEIGHT> pixels{};
    std::array<std::uint32_t, GC_LCD_WIDTH * GC_LCD_HEIGHT> blend_scratch{};
    std::vector<std::uint32_t> blend_accum(GC_LCD_WIDTH * GC_LCD_HEIGHT * 3u, 0u);
    std::vector<std::int16_t> audio_samples(4096u, 0);
    std::uint64_t audio_cycle_remainder = 0;

    bool in_launcher = start_in_launcher;
    bool emulation_ready = !start_in_launcher;
    bool paused = false;
    bool fast_forward = false;
    bool lcd_texture_initialized = false;
    bool title_dirty = true;
    bool done = false;

    /* Touch state machine: single-tap pulse vs continuous stylus drag. */
    bool suppress_touch_until_release = false;
    bool tap_button_held = false;
    bool tap_pulse_active = false;
    unsigned tap_vblanks_total = 0;
    unsigned tap_vblanks_strobed = 0;
    bool continuous_drag_active = false;
    Uint8 continuous_drag_button = 0;

    auto reset_input_state = [&]() {
        clear_machine_inputs(machine);
        fast_forward = false;
        tap_button_held = false;
        tap_pulse_active = false;
        tap_vblanks_total = 0;
        tap_vblanks_strobed = 0;
        continuous_drag_active = false;
        continuous_drag_button = 0;
        suppress_touch_until_release = false;
    };

    double cycle_accum = 0.0;
    double vblank_accum = 0.0;

    std::string launcher_status = gc_ui_valid_firmware(selection.firmware_dir)
        ? "Firmware ready. Use the menus to adjust display, speed, controls, and audio."
        : "Select a firmware folder and, optionally, a cartridge ROM.";

    auto boot_system = [&](bool bios_only) -> bool {
        if (emulation_ready && !demo_mode)
            (void)save_machine_nvram(machine, selection);
        reset_input_state();
        GcLaunchSelection boot_sel = selection;
        if (bios_only) {
            boot_sel.cartridge_path.clear();
            boot_sel.cartridge2_path.clear();
        }
        std::string err;
        if (!load_machine_media(machine, boot_sel, demo_mode, err)) {
            launcher_status = err;
            menubar.show_toast(err, 6.0);
            return false;
        }
        cpu.reset();
        cpu.set_external_vblank(true);
        nominal_cpu_hz = apply_cpu_clock();
        apply_serial_mode(false);
        sound.reset();
        audio_cycle_remainder = 0;
        cycle_accum = 0.0;
        vblank_accum = 0.0;
        emulation_ready = true;
        in_launcher = false;
        paused = false;
        lcd_texture_initialized = false;
        title_dirty = true;
        if (!demo_mode) (void)gc_save_launch_selection(selection);
        return true;
    };

    const double counter_freq = static_cast<double>(
        std::max<std::uint64_t>(1u, SDL_GetPerformanceFrequency()));
    std::uint64_t last_counter = SDL_GetPerformanceCounter();
    std::uint64_t next_present_tick = last_counter;
    double fps_timer = 0.0;
    unsigned fps_frames = 0;
    unsigned fps_vblanks = 0;
    double measured_fps = 0.0;
    double measured_lcd_hz = 0.0;

    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    bool pending_pick_firmware = false;
    bool pending_pick_rom = false;
    bool pending_pick_rom2 = false;
    bool pending_boot_after_rom = false;
    bool pending_controls = false;
    bool pending_custom_lcd = false;
    bool pending_custom_cpu = false;
    bool pending_serial_endpoint = false;
    bool force_builtin_picker = false;

    while (!done) {
        const std::uint64_t tick_begin = SDL_GetPerformanceCounter();
        double dt = static_cast<double>(tick_begin - last_counter) / counter_freq;
        last_counter = tick_begin;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.10) dt = 0.10;
        menubar.tick(dt);

        int win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
        const bool is_fullscreen = (SDL_GetWindowFlags(window) &
            (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
        const int top_reserved = menubar.reserved_top_height(in_launcher, selection.timing);

        /* Launcher button rects when in_launcher is active. */
        const SDL_Rect fw_btn{win_w - 220, 178, 172, 42};
        const SDL_Rect rom_clear_btn{win_w - 340, 314, 108, 42};
        const SDL_Rect rom_btn{win_w - 220, 314, 172, 42};
        const SDL_Rect bios_btn{48, win_h - 92, 224, 52};
        const SDL_Rect game_btn{292, win_h - 92, 244, 52};
        const SDL_Rect resume_btn{556, win_h - 92, 180, 52};
        const SDL_Rect quit_btn{win_w - 190, win_h - 92, 142, 52};
        const bool fw_ok = gc_ui_valid_firmware(selection.firmware_dir);
        const bool rom_ok = !selection.cartridge_path.empty();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                done = true;
                break;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                reset_input_state();
            }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }

            GcMenuAction action = GcMenuAction::None;
            bool consumed_pointer = false;
            const bool menu_handled = menubar.handle_event(
                event, window, renderer, selection, in_launcher,
                emulation_ready, paused, is_fullscreen, action, consumed_pointer);

            if (consumed_pointer && event.type == SDL_MOUSEBUTTONDOWN) {
                suppress_touch_until_release = true;
                tap_button_held = false;
                if (continuous_drag_active) {
                    continuous_drag_active = false;
                    (void)gc_input_set_touch(&machine, false, 0, 0);
                }
            }

            if (action != GcMenuAction::None) {
                switch (action) {
                case GcMenuAction::TimingChanged:
                    nominal_cpu_hz = apply_cpu_clock();
                    next_present_tick = SDL_GetPerformanceCounter();
                    lcd_texture_initialized = false;
                    if (selection.timing.touch_mode != GC_TOUCH_HOLD_DRAG && continuous_drag_active) {
                        continuous_drag_active = false;
                        (void)gc_input_set_touch(&machine, false, 0, 0);
                    }
                    title_dirty = true;
                    break;
                case GcMenuAction::VsyncToggled:
                    apply_vsync_setting();
                    next_present_tick = SDL_GetPerformanceCounter();
                    title_dirty = true;
                    break;
                case GcMenuAction::LoadCartridgeRom:
                    pending_pick_rom = true;
                    pending_boot_after_rom = !in_launcher || fw_ok;
                    force_builtin_picker = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    break;
                case GcMenuAction::LoadSlot2Rom:
                    pending_pick_rom2 = true;
                    force_builtin_picker = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    break;
                case GcMenuAction::EjectSlot2Rom:
                    selection.cartridge2_path.clear();
                    gc_unload_cartridge_slot(&machine, 2u);
                    (void)gc_save_launch_selection(selection);
                    menubar.show_toast("SLOT 2 CARTRIDGE EJECTED");
                    break;
                case GcMenuAction::SetFirmwareFolder:
                    pending_pick_firmware = true;
                    force_builtin_picker = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    break;
                case GcMenuAction::BootBiosOnly:
                    demo_mode = false;
                    if (boot_system(true)) menubar.show_toast("BOOTED TIGER BIOS MENU");
                    break;
                case GcMenuAction::BootSelected:
                    demo_mode = false;
                    (void)boot_system(false);
                    break;
                case GcMenuAction::SaveNvramNow: {
                    std::string nerr;
                    if (save_machine_nvram(machine, selection, &nerr))
                        menubar.show_toast("SAVED 8KB SYSTEM NVRAM");
                    else
                        menubar.show_toast("NVRAM SAVE ERROR: " + nerr, 6.0);
                    break;
                }
                case GcMenuAction::SeedWarmBootNvram:
                    if (selection.warm_boot_nvram) {
                        gc_nvram_seed_warm_boot(&machine);
                        menubar.show_toast("WARM-BOOT NVRAM SEED ENABLED");
                    } else {
                        menubar.show_toast("WARM-BOOT NVRAM SEED DISABLED");
                    }
                    break;
                case GcMenuAction::SerialModeChanged:
                    apply_serial_mode(true);
                    break;
                case GcMenuAction::PromptSerialEndpoint:
                    pending_serial_endpoint = true;
                    break;
                case GcMenuAction::ResetEmulation:
                    if (emulation_ready) {
                        if (demo_mode) {
                            (void)boot_system(false);
                        } else {
                            (void)save_machine_nvram(machine, selection);
                            gc_machine_reset(&machine);
                            cpu.reset();
                            cpu.set_external_vblank(true);
                            nominal_cpu_hz = apply_cpu_clock();
                            apply_serial_mode(false);
                            sound.reset();
                            reset_input_state();
                            lcd_texture_initialized = false;
                        }
                        menubar.show_toast("EMULATION RESET");
                    }
                    break;
                case GcMenuAction::TogglePause:
                    if (emulation_ready) {
                        paused = !paused;
                        reset_input_state();
                        title_dirty = true;
                        menubar.show_toast(paused ? "EMULATION PAUSED" : "EMULATION RESUMED");
                    }
                    break;
                case GcMenuAction::ToggleLauncherView:
                    reset_input_state();
                    in_launcher = !in_launcher;
                    title_dirty = true;
                    break;
                case GcMenuAction::ConfigureControls:
                    pending_controls = true;
                    break;
                case GcMenuAction::ResetControls:
                    bindings = gc_default_input_bindings();
                    (void)gc_save_input_bindings(bindings);
                    menubar.show_toast("CONTROLS RESET TO DEFAULTS");
                    break;
                case GcMenuAction::ResizeWindow2x:
                case GcMenuAction::ResizeWindow3x:
                case GcMenuAction::ResizeWindow4x: {
                    const int mult = action == GcMenuAction::ResizeWindow2x ? 2
                        : (action == GcMenuAction::ResizeWindow3x ? 3 : 4);
                    const int bar_h = selection.timing.auto_hide_menubar ? 0 : GcTopMenuBar::height;
                    SDL_SetWindowSize(window,
                                      std::max(440, static_cast<int>(GC_LCD_WIDTH) * mult),
                                      static_cast<int>(GC_LCD_HEIGHT) * mult + bar_h);
                    break;
                }
                case GcMenuAction::ToggleFullscreen:
                    (void)SDL_SetWindowFullscreen(
                        window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                case GcMenuAction::PromptCustomLcdHz:
                    pending_custom_lcd = true;
                    break;
                case GcMenuAction::PromptCustomCpuSpeed:
                    pending_custom_cpu = true;
                    break;
                case GcMenuAction::Quit:
                    done = true;
                    break;
                default:
                    break;
                }
            }

            if (menu_handled || consumed_pointer) continue;

            if (event.type == SDL_DROPFILE && event.drop.file) {
                const std::string dropped(event.drop.file);
                SDL_free(event.drop.file);
                std::error_code ec;
                const fs::path p(dropped);
                if (fs::is_directory(p, ec) && !ec) {
                    selection.firmware_dir = p.string();
                    (void)gc_save_launch_selection(selection);
                    launcher_status = gc_ui_valid_firmware(selection.firmware_dir)
                        ? "FIRMWARE FOLDER SET VIA DRAG AND DROP."
                        : "FOLDER SET, BUT INTERNAL.BIN / EXTERNAL.BIN MISSING.";
                    menubar.show_toast(launcher_status);
                } else if (fs::is_regular_file(p, ec) && !ec) {
                    const std::string fname = p.filename().string();
                    if (fname == "internal.bin" || fname == "external.bin" || fname == "boot.rom") {
                        selection.firmware_dir = p.parent_path().string();
                        (void)gc_save_launch_selection(selection);
                        menubar.show_toast("FIRMWARE FOLDER DETECTED");
                    } else {
                        selection.cartridge_path = p.string();
                        (void)gc_save_launch_selection(selection);
                        if (!in_launcher && gc_ui_valid_firmware(selection.firmware_dir)) {
                            demo_mode = false;
                            if (boot_system(false)) menubar.show_toast("LOADED DROPPED ROM");
                        } else {
                            launcher_status = "CARTRIDGE ROM SET VIA DRAG AND DROP.";
                            menubar.show_toast("ROM SELECTED");
                        }
                    }
                }
                continue;
            }

            if (in_launcher) {
                if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                    const SDL_Keycode key = event.key.keysym.sym;
                    const bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                    if (key == SDLK_ESCAPE) {
                        if (emulation_ready) { in_launcher = false; title_dirty = true; }
                        else done = true;
                    } else if (key == SDLK_1) {
                        pending_pick_firmware = true; force_builtin_picker = shift;
                    } else if (key == SDLK_2) {
                        pending_pick_rom = true; pending_boot_after_rom = false; force_builtin_picker = shift;
                    } else if (key == SDLK_3) {
                        pending_controls = true;
                    } else if (key == SDLK_F11) {
                        (void)SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && fw_ok) {
                        demo_mode = false;
                        (void)boot_system(false);
                    }
                } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    int mx = 0, my = 0;
                    gc_ui_map_mouse(window, win_w, win_h, event.button.x, event.button.y, mx, my);
                    const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
                    if (gc_ui_inside(mx, my, fw_btn)) {
                        pending_pick_firmware = true; force_builtin_picker = shift;
                    } else if (gc_ui_inside(mx, my, rom_btn)) {
                        pending_pick_rom = true; pending_boot_after_rom = false; force_builtin_picker = shift;
                    } else if (rom_ok && gc_ui_inside(mx, my, rom_clear_btn)) {
                        selection.cartridge_path.clear();
                        (void)gc_save_launch_selection(selection);
                    } else if (gc_ui_inside(mx, my, bios_btn) && fw_ok) {
                        demo_mode = false;
                        (void)boot_system(true);
                    } else if (gc_ui_inside(mx, my, game_btn) && fw_ok && rom_ok) {
                        demo_mode = false;
                        (void)boot_system(false);
                    } else if (emulation_ready && gc_ui_inside(mx, my, resume_btn)) {
                        in_launcher = false;
                        title_dirty = true;
                    } else if (gc_ui_inside(mx, my, quit_btn)) {
                        done = true;
                    }
                }
                continue;
            }

            /* Active Emulator input handling. */
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const bool pressed = event.type == SDL_KEYDOWN;
                const SDL_Keycode sym = event.key.keysym.sym;
                if (sym == SDLK_ESCAPE && pressed && !event.key.repeat) {
                    done = true;
                } else if (!is_bound_key(bindings, sym) && sym == SDLK_F8) {
                    if (fast_forward != pressed) {
                        fast_forward = pressed;
                        title_dirty = true;
                    }
                } else if (!is_bound_key(bindings, sym) && pressed && !event.key.repeat &&
                           sym == SDLK_F9) {
                    paused = !paused;
                    reset_input_state();
                    title_dirty = true;
                    menubar.show_toast(paused ? "EMULATION PAUSED" : "EMULATION RESUMED");
                } else if (!is_bound_key(bindings, sym) && pressed && !event.key.repeat &&
                           sym == SDLK_F11) {
                    (void)SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if (!is_bound_key(bindings, sym) && pressed && !event.key.repeat &&
                           (sym == SDLK_F5 || sym == SDLK_F6 || sym == SDLK_F7)) {
                    if (sym == SDLK_F5) {
                        selection.timing.cpu_speed_percent =
                            step_cpu_speed_preset(selection.timing.cpu_speed_percent, -1);
                        nominal_cpu_hz = apply_cpu_clock();
                        menubar.show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
                    } else if (sym == SDLK_F6) {
                        selection.timing.cpu_speed_percent =
                            step_cpu_speed_preset(selection.timing.cpu_speed_percent, 1);
                        nominal_cpu_hz = apply_cpu_clock();
                        menubar.show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
                    } else if (sym == SDLK_F7) {
                        const int dir = (event.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                        selection.timing.lcd_hz = cycle_lcd_hz_preset(selection.timing.lcd_hz, dir);
                        nominal_cpu_hz = apply_cpu_clock();
                        next_present_tick = SDL_GetPerformanceCounter();
                        menubar.show_toast("LCD REFRESH: " + gc_ui_format_lcd_hz(selection.timing.lcd_hz));
                    }
                    (void)gc_save_launch_selection(selection);
                    title_dirty = true;
                } else if (!paused) {
                    update_key(machine, bindings, sym, pressed);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (paused || suppress_touch_until_release) continue;
                const Uint8 btn = event.button.button;
                const bool shift_held = (SDL_GetModState() & KMOD_SHIFT) != 0;
                const bool use_hold_drag = (btn == SDL_BUTTON_RIGHT) ||
                    (btn == SDL_BUTTON_LEFT && (shift_held || selection.timing.touch_mode == GC_TOUCH_HOLD_DRAG));

                if (use_hold_drag) {
                    unsigned col = 0, row = 0;
                    if (map_mouse_to_touch_cell(window, renderer, top_reserved,
                            selection.timing.integer_scaling,
                            event.button.x, event.button.y, col, row)) {
                        tap_pulse_active = false;
                        continuous_drag_active = true;
                        continuous_drag_button = btn;
                        (void)gc_input_set_touch(&machine, true, col, row);
                    }
                } else if (btn == SDL_BUTTON_LEFT && !tap_button_held) {
                    unsigned col = 0, row = 0;
                    if (map_mouse_to_touch_cell(window, renderer, top_reserved,
                            selection.timing.integer_scaling,
                            event.button.x, event.button.y, col, row)) {
                        tap_button_held = true;
                        tap_pulse_active = true;
                        tap_vblanks_total = 0;
                        tap_vblanks_strobed = 0;
                        (void)gc_input_set_touch(&machine, true, col, row);
                    }
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                if (!paused && continuous_drag_active) {
                    unsigned col = 0, row = 0;
                    if (map_mouse_to_touch_cell(window, renderer, top_reserved,
                            selection.timing.integer_scaling,
                            event.motion.x, event.motion.y, col, row)) {
                        (void)gc_input_set_touch(&machine, true, col, row);
                    } else {
                        (void)gc_input_set_touch(&machine, false, 0, 0);
                    }
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    suppress_touch_until_release = false;
                    tap_button_held = false;
                }
                if (continuous_drag_active && event.button.button == continuous_drag_button) {
                    continuous_drag_active = false;
                    continuous_drag_button = 0;
                    (void)gc_input_set_touch(&machine, false, 0, 0);
                }
            }
        }
        if (done) break;

        /* Handle deferred modal dialogs outside SDL_PollEvent loop. */
        if (pending_pick_firmware || pending_pick_rom || pending_pick_rom2 ||
            pending_controls || pending_custom_lcd || pending_custom_cpu ||
            pending_serial_endpoint) {
            reset_input_state();
            menubar.close();

            if (pending_pick_firmware) {
                pending_pick_firmware = false;
                const std::string initial = selection.firmware_dir.empty()
                    ? gc_ui_home_directory() : selection.firmware_dir;
                std::string path;
                bool used_builtin = false;
                if (gc_ui_pick_path(window, renderer, font, vsync_active, true,
                                    initial, path, done, force_builtin_picker, used_builtin)) {
                    selection.firmware_dir = path;
                    (void)gc_save_launch_selection(selection);
                    launcher_status = gc_ui_valid_firmware(path)
                        ? "FIRMWARE READY (INTERNAL.BIN OPTIONAL)."
                        : "FOLDER SELECTED, BUT EXTERNAL.BIN / BOOT.ROM MISSING.";
                    menubar.show_toast(launcher_status);
                }
                force_builtin_picker = false;
            }
            if (pending_pick_rom2 && !done) {
                pending_pick_rom2 = false;
                std::string initial = selection.firmware_dir.empty()
                    ? gc_ui_home_directory() : selection.firmware_dir;
                if (!selection.cartridge2_path.empty())
                    initial = fs::path(selection.cartridge2_path).parent_path().string();
                else if (!selection.cartridge_path.empty())
                    initial = fs::path(selection.cartridge_path).parent_path().string();
                std::string path;
                bool used_builtin = false;
                if (gc_ui_pick_path(window, renderer, font, vsync_active, false,
                                    initial, path, done, force_builtin_picker, used_builtin)) {
                    char rerr[192] = {};
                    if (gc_load_cartridge_slot(&machine, 2u, path.c_str(), rerr, sizeof(rerr))) {
                        selection.cartridge2_path = path;
                        (void)gc_save_launch_selection(selection);
                        menubar.show_toast("SLOT 2 CARTRIDGE LOADED");
                    } else {
                        menubar.show_toast(std::string("SLOT 2 ERROR: ") + rerr, 6.0);
                    }
                }
                force_builtin_picker = false;
            }
            if (pending_serial_endpoint && !done) {
                pending_serial_endpoint = false;
                std::string ep = selection.serial_endpoint;
                if (gc_ui_prompt_text_modal(window, renderer, font, vsync_active,
                        "2-PLAYER TCP JOIN", "ENTER HOST:PORT OF PEER EMULATOR:",
                        selection.serial_endpoint, ep, done)) {
                    selection.serial_endpoint = ep;
                    selection.serial_mode = GC_SERIAL_TCP_CLIENT;
                    (void)gc_save_launch_selection(selection);
                    apply_serial_mode(true);
                }
            }
            if (pending_pick_rom && !done) {
                pending_pick_rom = false;
                const bool auto_boot = pending_boot_after_rom;
                pending_boot_after_rom = false;
                std::string initial = selection.firmware_dir.empty()
                    ? gc_ui_home_directory() : selection.firmware_dir;
                if (!selection.cartridge_path.empty())
                    initial = fs::path(selection.cartridge_path).parent_path().string();
                std::string path;
                bool used_builtin = false;
                if (gc_ui_pick_path(window, renderer, font, vsync_active, false,
                                    initial, path, done, force_builtin_picker, used_builtin)) {
                    selection.cartridge_path = path;
                    (void)gc_save_launch_selection(selection);
                    launcher_status = "GAME ROM SELECTED.";
                    if (auto_boot && gc_ui_valid_firmware(selection.firmware_dir)) {
                        demo_mode = false;
                        if (boot_system(false)) menubar.show_toast("LOADED CARTRIDGE ROM");
                    } else {
                        menubar.show_toast("CARTRIDGE ROM SELECTED");
                    }
                }
                force_builtin_picker = false;
            }
            if (pending_controls && !done) {
                pending_controls = false;
                std::string ctrl_status = "CONTROLS SAVE AUTOMATICALLY TO YOUR USER CONFIGURATION.";
                (void)gc_ui_run_controls_modal(window, renderer, font, vsync_active,
                                               bindings, done, ctrl_status);
            }
            if (pending_custom_lcd && !done) {
                pending_custom_lcd = false;
                double val = selection.timing.lcd_hz;
                if (gc_ui_prompt_number_modal(window, renderer, font, vsync_active,
                        "CUSTOM LCD REFRESH HZ", "ENTER EMULATED LCD VBLANK RATE (10 - 240 HZ):",
                        selection.timing.lcd_hz, 10.0, 240.0, true, val, done)) {
                    selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(val);
                    nominal_cpu_hz = apply_cpu_clock();
                    (void)gc_save_launch_selection(selection);
                    title_dirty = true;
                    menubar.show_toast("LCD REFRESH: " + gc_ui_format_lcd_hz(selection.timing.lcd_hz));
                }
            }
            if (pending_custom_cpu && !done) {
                pending_custom_cpu = false;
                double val = static_cast<double>(selection.timing.cpu_speed_percent);
                if (gc_ui_prompt_number_modal(window, renderer, font, vsync_active,
                        "CUSTOM CPU CLOCK %", "ENTER SM8521 CPU CLOCK PERCENTAGE (25 - 800%):",
                        val, 25.0, 800.0, false, val, done)) {
                    selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(static_cast<unsigned>(val));
                    nominal_cpu_hz = apply_cpu_clock();
                    (void)gc_save_launch_selection(selection);
                    title_dirty = true;
                    menubar.show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
                }
            }
            last_counter = SDL_GetPerformanceCounter();
            next_present_tick = last_counter;
            SDL_GetMouseState(&mouse_x, &mouse_y);
            if (done) break;
        }

        if (in_launcher) {
            int mx = 0, my = 0;
            SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
            gc_ui_map_mouse(window, win_w, win_h, mouse_x, mouse_y, mx, my);
            const bool cur_fw_ok = gc_ui_valid_firmware(selection.firmware_dir);
            const bool cur_rom_ok = !selection.cartridge_path.empty();

            SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
            SDL_RenderClear(renderer);
            font.draw(48, 56, "Game.com Emulator", accent, 3, 60);
            font.draw(50, 102, "Choose firmware and an optional game ROM. Settings are in the menus above.", muted, 2, 74);

            const SDL_Rect fw_panel{40, 144, win_w - 80, 110};
            const SDL_Rect rom_panel{40, 280, win_w - 80, 110};
            gc_ui_fill(renderer, fw_panel, panel); gc_ui_outline(renderer, fw_panel, blue);
            gc_ui_fill(renderer, rom_panel, panel); gc_ui_outline(renderer, rom_panel, blue);

            font.draw(60, 162, "System firmware", ink, 2, 54);
            font.draw(60, 194,
                      selection.firmware_dir.empty() ? "No firmware folder selected" : gc_ui_short_path(selection.firmware_dir, 48),
                      cur_fw_ok ? accent : warning, 2, 50);
            font.draw(60, 226, "Choose the folder containing external.bin (internal.bin is optional).", muted, 1, 80);
            gc_ui_draw_button(renderer, font, fw_btn, "Browse...", true, false,
                              gc_ui_inside(mx, my, fw_btn));

            font.draw(60, 298, "Cartridge ROM (optional)", ink, 2, 46);
            font.draw(60, 330,
                      selection.cartridge_path.empty() ? "No game selected" : gc_ui_short_path(selection.cartridge_path, 38),
                      cur_rom_ok ? accent : muted, 2, 40);
            font.draw(60, 362, "Supported files: .bin, .rom, and .tgc. You can also drag a file here.", muted, 1, 80);
            if (cur_rom_ok) {
                gc_ui_draw_button(renderer, font, rom_clear_btn, "Clear", true, false,
                                  gc_ui_inside(mx, my, rom_clear_btn));
            }
            gc_ui_draw_button(renderer, font, rom_btn, "Browse...", true, false,
                              gc_ui_inside(mx, my, rom_btn));

            font.draw(50, 418, launcher_status,
                      launcher_status.find("ERROR") != std::string::npos ||
                      launcher_status.find("MISSING") != std::string::npos ? warning : muted,
                      2, 72);

            gc_ui_draw_button(renderer, font, bios_btn, "Start BIOS", cur_fw_ok, false,
                              gc_ui_inside(mx, my, bios_btn));
            gc_ui_draw_button(renderer, font, game_btn, "Start game", cur_fw_ok && cur_rom_ok, false,
                              gc_ui_inside(mx, my, game_btn));
            if (emulation_ready) {
                gc_ui_draw_button(renderer, font, resume_btn, "Resume", true, true,
                                  gc_ui_inside(mx, my, resume_btn));
            }
            gc_ui_draw_button(renderer, font, quit_btn, "Exit", true, false,
                              gc_ui_inside(mx, my, quit_btn));

            menubar.render(window, renderer, font, selection, true, emulation_ready,
                           paused, false, is_fullscreen, 0.0, 0.0, mouse_x, mouse_y);
            gc_ui_present(renderer, vsync_active);
            next_present_tick = SDL_GetPerformanceCounter();
            if (title_dirty) {
                update_window_title(window, selection, true, paused, false, 0.0, 0.0);
                title_dirty = false;
            }
            continue;
        }

        /* Step active emulation when not paused. */
        unsigned vblanks_due = 0;
        bool multi_vblank_blended = false;
        if (!paused) {
            const double pace_mult = fast_forward
                ? 4.0 : (static_cast<double>(selection.timing.game_pace_percent) / 100.0);
            const double effective_cpu_hz = static_cast<double>(nominal_cpu_hz) * pace_mult;
            const double effective_lcd_hz = selection.timing.lcd_hz * pace_mult;

            cycle_accum += dt * effective_cpu_hz;
            vblank_accum += dt * effective_lcd_hz;

            const std::uint64_t total_cycles = static_cast<std::uint64_t>(cycle_accum);
            cycle_accum -= static_cast<double>(total_cycles);

            vblanks_due = static_cast<unsigned>(vblank_accum);
            vblank_accum -= static_cast<double>(vblanks_due);
            if (vblanks_due > 16u) vblanks_due = 16u;

            const std::uint64_t step_start = cpu.cycles();
            if (vblanks_due == 0u) {
                const std::uint64_t target = step_start + total_cycles;
                while (cpu.cycles() < target) (void)cpu.step();
                serial_link.poll(machine, cpu);
            } else {
                const bool do_blend = (vblanks_due > 1u && vblanks_due <= 4u && !fast_forward);
                if (do_blend) std::fill(blend_accum.begin(), blend_accum.end(), 0u);
                unsigned blended_count = 0u;

                for (unsigned v = 0; v < vblanks_due; ++v) {
                    const std::uint64_t slice_target =
                        step_start + ((total_cycles * static_cast<std::uint64_t>(v + 1u)) / vblanks_due);
                    while (cpu.cycles() < slice_target) (void)cpu.step();
                    serial_link.poll(machine, cpu);
                    cpu.trigger_vblank();

                    if (do_blend && gc_lcd_render_argb_palette(
                            &machine, blend_scratch.data(), GC_LCD_WIDTH,
                            selection.timing.lcd_palette)) {
                        for (std::size_t px = 0; px < blend_scratch.size(); ++px) {
                            const std::uint32_t c = blend_scratch[px];
                            blend_accum[px * 3u + 0u] += (c >> 16) & 0xFFu;
                            blend_accum[px * 3u + 1u] += (c >> 8) & 0xFFu;
                            blend_accum[px * 3u + 2u] += c & 0xFFu;
                        }
                        ++blended_count;
                    }

                    if (tap_pulse_active) {
                        ++tap_vblanks_total;
                        const unsigned needed =
                            (selection.timing.touch_mode == GC_TOUCH_DEBOUNCED_TAP) ? 2u : 1u;
                        if (machine.bus.touch_strobed) {
                            ++tap_vblanks_strobed;
                            if (tap_vblanks_strobed < needed)
                                machine.bus.touch_strobed = false;
                        }
                        if (tap_vblanks_strobed >= needed || tap_vblanks_total >= 14u) {
                            tap_pulse_active = false;
                            (void)gc_input_set_touch(&machine, false, 0, 0);
                        }
                    }
                }

                if (do_blend && blended_count > 0u) {
                    for (std::size_t px = 0; px < pixels.size(); ++px) {
                        const std::uint32_t r = blend_accum[px * 3u + 0u] / blended_count;
                        const std::uint32_t g = blend_accum[px * 3u + 1u] / blended_count;
                        const std::uint32_t b = blend_accum[px * 3u + 2u] / blended_count;
                        pixels[px] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                    multi_vblank_blended = true;
                }
            }
            const std::uint64_t step_stop = cpu.cycles();
            const std::uint64_t elapsed_cycles = step_stop - step_start;

            if (elapsed_cycles > 0) {
                const std::uint64_t audio_rate_denom = std::max<std::uint64_t>(
                    1u, static_cast<std::uint64_t>(std::llround(effective_cpu_hz)));
                audio_cycle_remainder += elapsed_cycles * standalone::AudioOutput::sample_rate;
                const std::size_t audio_frames =
                    static_cast<std::size_t>(audio_cycle_remainder / audio_rate_denom);
                audio_cycle_remainder %= audio_rate_denom;
                if (audio_frames > audio_samples.size())
                    audio_samples.resize(audio_frames);
                sound.render(cpu.sound_writes(), step_start, step_stop,
                             audio_frames ? audio_samples.data() : nullptr,
                             audio_frames, standalone::AudioOutput::sample_rate,
                             nominal_cpu_hz);
                cpu.clear_sound_writes();

                if (audio_frames > 0) {
                    const int vol = selection.timing.audio_muted
                        ? 0 : static_cast<int>(std::min(100u, selection.timing.audio_volume_percent));
                    if (vol < 100) {
                        for (std::size_t i = 0; i < audio_frames; ++i) {
                            audio_samples[i] = static_cast<std::int16_t>(
                                (static_cast<int>(audio_samples[i]) * vol) / 100);
                        }
                    }
                    audio_output.push(audio_samples.data(), audio_frames);
                }
            }
        }

        if (vblanks_due > 0u || !lcd_texture_initialized) {
            if (!multi_vblank_blended &&
                !gc_lcd_render_argb_palette(&machine, pixels.data(), GC_LCD_WIDTH,
                                            selection.timing.lcd_palette)) {
                std::fprintf(stderr, "Could not render Game.com LCD frame.\n");
                done = true;
                break;
            }
            if (SDL_UpdateTexture(lcd_texture, nullptr, pixels.data(),
                                  static_cast<int>(GC_LCD_WIDTH * sizeof(std::uint32_t))) != 0) {
                std::fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
                done = true;
                break;
            }
            lcd_texture_initialized = true;
        }

        int out_w = 0, out_h = 0;
        const SDL_Rect lcd_dst = compute_lcd_rect(
            renderer, top_reserved, selection.timing.integer_scaling, out_w, out_h);
        SDL_SetRenderDrawColor(renderer, 12, 16, 22, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, lcd_texture, nullptr, &lcd_dst);

        menubar.render(window, renderer, font, selection, false, emulation_ready,
                       paused, fast_forward, is_fullscreen, measured_fps, measured_lcd_hz,
                       mouse_x, mouse_y);
        SDL_RenderPresent(renderer);

        if (!vsync_active) {
            if (paused) {
                SDL_Delay(8);
                next_present_tick = SDL_GetPerformanceCounter();
            } else if (fast_forward) {
                const double elapsed_ms =
                    (static_cast<double>(SDL_GetPerformanceCounter() - tick_begin) * 1000.0) /
                    counter_freq;
                if (elapsed_ms < 2.0) SDL_Delay(1);
                next_present_tick = SDL_GetPerformanceCounter();
            } else {
                const double pace_mult =
                    static_cast<double>(selection.timing.game_pace_percent) / 100.0;
                const double target_hz =
                    std::max(10.0, std::min(240.0, selection.timing.lcd_hz * pace_mult));
                const std::uint64_t frame_ticks = static_cast<std::uint64_t>(
                    std::max(1.0, std::round(counter_freq / target_hz)));
                next_present_tick += frame_ticks;
                const std::uint64_t now_tick = SDL_GetPerformanceCounter();
                if (now_tick > next_present_tick + frame_ticks * 2u) {
                    next_present_tick = now_tick;
                } else {
                    while (true) {
                        const std::uint64_t cur = SDL_GetPerformanceCounter();
                        if (cur >= next_present_tick) break;
                        const double rem_ms =
                            (static_cast<double>(next_present_tick - cur) * 1000.0) / counter_freq;
                        if (rem_ms > 2.0)
                            SDL_Delay(static_cast<Uint32>(rem_ms - 1.2));
                    }
                }
            }
        }

        fps_timer += dt;
        ++fps_frames;
        fps_vblanks += vblanks_due;
        if (fps_timer >= 0.5) {
            measured_fps = static_cast<double>(fps_frames) / fps_timer;
            measured_lcd_hz = static_cast<double>(fps_vblanks) / fps_timer;
            fps_timer = 0.0;
            fps_frames = 0;
            fps_vblanks = 0;
            title_dirty = true;
        }
        if (title_dirty) {
            update_window_title(window, selection, false, paused, fast_forward,
                                measured_fps, measured_lcd_hz);
            title_dirty = false;
        }
    }

    if (emulation_ready && !demo_mode)
        (void)save_machine_nvram(machine, selection);
    serial_link.disconnect();
    cpu.set_external_vblank(prev_ext_vblank);
    SDL_DestroyTexture(lcd_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (owns_sdl) SDL_Quit();
    return 0;
}

int gc_sdl_frontend_run(GcMachine& machine, standalone::Sm8521Cpu& cpu,
                        const GcInputBindings& bindings,
                        const GcTimingOptions& initial_timing)
{
    GcLaunchSelection selection = gc_load_launch_selection();
    selection.timing = initial_timing;
    GcInputBindings mutable_bindings = bindings;
    return gc_sdl_session_run(machine, cpu, selection, mutable_bindings, false, false);
}
