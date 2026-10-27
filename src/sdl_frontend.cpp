#include "gamecom_sdl.hpp"
#include "gamecom_video.h"
#include "standalone/audio_output.hpp"
#include "standalone/audio_synth.hpp"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr std::array<double, 7> lcd_hz_presets = {
    30.0, 50.0, 55.2, 59.732155, 60.0, 75.0, 120.0
};
constexpr std::array<unsigned, 6> cpu_speed_presets = {
    75u, 100u, 120u, 150u, 200u, 300u
};

double clamp_lcd_hz(double hz)
{
    if (!std::isfinite(hz)) return 60.0;
    return std::max(10.0, std::min(240.0, hz));
}

unsigned clamp_cpu_speed(unsigned pct)
{
    return std::max(25u, std::min(800u, pct));
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

SDL_Rect centered_rect(SDL_Renderer *renderer, int& output_w, int& output_h)
{
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
    const double scale = (output_w > 0 && output_h > 0)
        ? std::min(output_w / static_cast<double>(GC_LCD_WIDTH),
                   output_h / static_cast<double>(GC_LCD_HEIGHT)) : 1.0;
    SDL_Rect destination{};
    destination.w = static_cast<int>(GC_LCD_WIDTH * scale);
    destination.h = static_cast<int>(GC_LCD_HEIGHT * scale);
    destination.x = (output_w - destination.w) / 2;
    destination.y = (output_h - destination.h) / 2;
    return destination;
}

void update_touch_from_mouse(GcMachine& machine, SDL_Window *window,
                             SDL_Renderer *renderer, int x, int y, bool down)
{
    if (!down) {
        (void)gc_input_set_touch(&machine, false, 0, 0);
        return;
    }
    int window_w = 0, window_h = 0, output_w = 0, output_h = 0;
    SDL_GetWindowSize(window, &window_w, &window_h);
    const SDL_Rect destination = centered_rect(renderer, output_w, output_h);
    if (window_w <= 0 || window_h <= 0 || destination.w <= 0 || destination.h <= 0) {
        (void)gc_input_set_touch(&machine, false, 0, 0);
        return;
    }
    const int output_x = x * output_w / window_w;
    const int output_y = y * output_h / window_h;
    if (output_x < destination.x || output_x >= destination.x + destination.w ||
        output_y < destination.y || output_y >= destination.y + destination.h) {
        (void)gc_input_set_touch(&machine, false, 0, 0);
        return;
    }
    const unsigned lcd_x = std::min<unsigned>(
        GC_LCD_WIDTH - 1u,
        static_cast<unsigned>((output_x - destination.x) * static_cast<int>(GC_LCD_WIDTH) / destination.w));
    const unsigned lcd_y = std::min<unsigned>(
        GC_LCD_HEIGHT - 1u,
        static_cast<unsigned>((output_y - destination.y) * static_cast<int>(GC_LCD_HEIGHT) / destination.h));
    /* Game.com's 13x10 touch matrix covers the 200x160 LCD with twelve 16px
     * columns (x = 0..191 -> cols 0..11) plus an 8px column 12 (x = 192..199),
     * and ten 16px rows (y = 0..159 -> rows 0..9). */
    const unsigned column = std::min<unsigned>(12u, lcd_x / 16u);
    const unsigned row = std::min<unsigned>(9u, lcd_y / 16u);
    (void)gc_input_set_touch(&machine, true, column, row);
}

void render_centered(SDL_Renderer *renderer, SDL_Texture *texture)
{
    int output_w = 0, output_h = 0;
    const SDL_Rect destination = centered_rect(renderer, output_w, output_h);
    SDL_SetRenderDrawColor(renderer, 12, 16, 22, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, &destination);
    SDL_RenderPresent(renderer);
}

void update_window_title(SDL_Window *window, double lcd_hz, unsigned cpu_speed_pct,
                         bool fast_forward, double host_fps)
{
    char title[160];
    const double mhz = (5.5296 * static_cast<double>(cpu_speed_pct)) / 100.0;
    if (fast_forward) {
        std::snprintf(title, sizeof(title),
                      "Game.com LCD [4x FAST-FWD] | LCD %.2f Hz | CPU %u%% (%.2f MHz)",
                      lcd_hz, cpu_speed_pct, mhz);
    } else if (host_fps > 0.5) {
        std::snprintf(title, sizeof(title),
                      "Game.com LCD | LCD %.2f Hz | CPU %u%% (%.2f MHz) | Display %.0f FPS",
                      lcd_hz, cpu_speed_pct, mhz, host_fps);
    } else {
        std::snprintf(title, sizeof(title),
                      "Game.com LCD | LCD %.2f Hz | CPU %u%% (%.2f MHz)",
                      lcd_hz, cpu_speed_pct, mhz);
    }
    SDL_SetWindowTitle(window, title);
}

} // namespace

int gc_sdl_frontend_run(GcMachine& machine, standalone::Sm8521Cpu& cpu,
                        const GcInputBindings& bindings,
                        const GcTimingOptions& initial_timing)
{
    const bool owns_sdl = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (owns_sdl && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *window = SDL_CreateWindow("Game.com LCD",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(GC_LCD_WIDTH * 4u), static_cast<int>(GC_LCD_HEIGHT * 4u),
        SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (owns_sdl) SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(GC_LCD_WIDTH), static_cast<int>(GC_LCD_HEIGHT));

    /* Host presentation runs at the display's native VSync (60/120/144+ Hz),
     * completely decoupled from the emulated LCD VBlank rate and SM8521 CPU
     * clock accumulators below. This avoids compositor frame-callback misses
     * on Wayland/X11 and eliminates CPU busy-spin loops. */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        if (owns_sdl) SDL_Quit();
        return 1;
    }

    SDL_RendererInfo rinfo{};
    const bool vsync_active = (SDL_GetRendererInfo(renderer, &rinfo) == 0) &&
                              ((rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0);

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, static_cast<int>(GC_LCD_WIDTH),
        static_cast<int>(GC_LCD_HEIGHT));
    if (!texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        if (owns_sdl) SDL_Quit();
        return 1;
    }

    double lcd_hz = clamp_lcd_hz(initial_timing.lcd_hz);
    unsigned cpu_speed_pct = clamp_cpu_speed(initial_timing.cpu_speed_percent);
    bool fast_forward = false;

    const bool prev_ext_vblank = cpu.external_vblank();
    cpu.set_external_vblank(true);

    auto apply_cpu_clock = [&]() -> std::uint32_t {
        const double scaled = static_cast<double>(GC_CPU_HZ) *
                              static_cast<double>(cpu_speed_pct) / 100.0;
        const std::uint32_t hz = static_cast<std::uint32_t>(
            std::max(100000.0, std::min(100000000.0, std::round(scaled))));
        cpu.set_clock_hz(hz);
        return hz;
    };
    std::uint32_t nominal_cpu_hz = apply_cpu_clock();

    standalone::AudioSynth sound;
    standalone::AudioOutput audio_output;
    std::string audio_error;
    if (!audio_output.start(audio_error)) {
        std::fprintf(stderr, "Audio unavailable (%s); continuing silently.\n",
                     audio_error.c_str());
    }

    std::array<std::uint32_t, GC_LCD_WIDTH * GC_LCD_HEIGHT> pixels{};
    std::vector<std::int16_t> audio_samples(4096u, 0);
    std::uint64_t audio_cycle_remainder = 0;

    const double counter_freq = static_cast<double>(
        std::max<std::uint64_t>(1u, SDL_GetPerformanceFrequency()));
    std::uint64_t last_counter = SDL_GetPerformanceCounter();
    double cycle_accum = 0.0;
    double vblank_accum = 0.0;
    double fps_timer = 0.0;
    unsigned fps_frames = 0;
    double measured_fps = 0.0;
    bool lcd_texture_initialized = false;
    bool title_dirty = true;
    bool done = false;

    while (!done) {
        const std::uint64_t tick_begin_counter = SDL_GetPerformanceCounter();
        double dt = static_cast<double>(tick_begin_counter - last_counter) / counter_freq;
        last_counter = tick_begin_counter;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.10) dt = 0.10; /* Clamp OS stalls/window drags to 100 ms max. */

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                done = true;
            } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const bool pressed = event.type == SDL_KEYDOWN;
                const SDL_Keycode sym = event.key.keysym.sym;
                if (sym == SDLK_ESCAPE && pressed) {
                    done = true;
                } else if (!is_bound_key(bindings, sym) && sym == SDLK_F8) {
                    if (fast_forward != pressed) {
                        fast_forward = pressed;
                        title_dirty = true;
                    }
                } else if (!is_bound_key(bindings, sym) && pressed && !event.key.repeat &&
                           (sym == SDLK_F5 || sym == SDLK_F6 || sym == SDLK_F7)) {
                    if (sym == SDLK_F5) {
                        cpu_speed_pct = step_cpu_speed_preset(cpu_speed_pct, -1);
                        nominal_cpu_hz = apply_cpu_clock();
                    } else if (sym == SDLK_F6) {
                        cpu_speed_pct = step_cpu_speed_preset(cpu_speed_pct, 1);
                        nominal_cpu_hz = apply_cpu_clock();
                    } else if (sym == SDLK_F7) {
                        const int dir = (event.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                        lcd_hz = cycle_lcd_hz_preset(lcd_hz, dir);
                    }
                    GcLaunchSelection persisted = gc_load_launch_selection();
                    persisted.timing.lcd_hz = lcd_hz;
                    persisted.timing.cpu_speed_percent = cpu_speed_pct;
                    (void)gc_save_launch_selection(persisted);
                    title_dirty = true;
                } else {
                    update_key(machine, bindings, sym, pressed);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    update_touch_from_mouse(machine, window, renderer,
                        event.button.x, event.button.y,
                        event.type == SDL_MOUSEBUTTONDOWN);
                }
            } else if (event.type == SDL_MOUSEMOTION &&
                       (event.motion.state & SDL_BUTTON_LMASK) != 0) {
                update_touch_from_mouse(machine, window, renderer,
                                        event.motion.x, event.motion.y, true);
            }
        }
        if (done) break;

        const double ff_mult = fast_forward ? 4.0 : 1.0;
        const double effective_cpu_hz = static_cast<double>(nominal_cpu_hz) * ff_mult;
        const double effective_lcd_hz = lcd_hz * ff_mult;

        cycle_accum += dt * effective_cpu_hz;
        vblank_accum += dt * effective_lcd_hz;

        const std::uint64_t total_cycles = static_cast<std::uint64_t>(cycle_accum);
        cycle_accum -= static_cast<double>(total_cycles);

        unsigned vblanks_due = static_cast<unsigned>(vblank_accum);
        vblank_accum -= static_cast<double>(vblanks_due);
        if (vblanks_due > 16u) vblanks_due = 16u;

        const std::uint64_t step_start = cpu.cycles();
        if (vblanks_due == 0u) {
            const std::uint64_t target = step_start + total_cycles;
            while (cpu.cycles() < target) (void)cpu.step();
        } else {
            for (unsigned v = 0; v < vblanks_due; ++v) {
                const std::uint64_t slice_target =
                    step_start + ((total_cycles * static_cast<std::uint64_t>(v + 1u)) / vblanks_due);
                while (cpu.cycles() < slice_target) (void)cpu.step();
                cpu.trigger_vblank();
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
            if (audio_frames > 0)
                audio_output.push(audio_samples.data(), audio_frames);
        }

        if (vblanks_due > 0u || !lcd_texture_initialized) {
            if (!gc_lcd_render_argb(&machine, pixels.data(), GC_LCD_WIDTH)) {
                std::fprintf(stderr, "Could not render Game.com LCD frame.\n");
                done = true;
                break;
            }
            if (SDL_UpdateTexture(texture, nullptr, pixels.data(),
                                  static_cast<int>(GC_LCD_WIDTH * sizeof(std::uint32_t))) != 0) {
                std::fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
                done = true;
                break;
            }
            lcd_texture_initialized = true;
        }

        render_centered(renderer, texture);

        if (!vsync_active) {
            /* Fallback cap (~250 Hz) when using a software or un-vsynced renderer. */
            const double frame_elapsed_ms =
                (static_cast<double>(SDL_GetPerformanceCounter() - tick_begin_counter) * 1000.0) /
                counter_freq;
            if (frame_elapsed_ms < 4.0)
                SDL_Delay(static_cast<Uint32>(4.0 - frame_elapsed_ms));
        }

        fps_timer += dt;
        ++fps_frames;
        if (fps_timer >= 0.5) {
            measured_fps = static_cast<double>(fps_frames) / fps_timer;
            fps_timer = 0.0;
            fps_frames = 0;
            title_dirty = true;
        }
        if (title_dirty) {
            update_window_title(window, lcd_hz, cpu_speed_pct, fast_forward, measured_fps);
            title_dirty = false;
        }
    }

    cpu.set_external_vblank(prev_ext_vblank);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (owns_sdl) SDL_Quit();
    return 0;
}
