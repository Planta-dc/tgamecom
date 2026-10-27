#include "gamecom_sdl.hpp"
#include "gamecom_video.h"
#include "standalone/audio_output.hpp"
#include "standalone/audio_synth.hpp"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {
constexpr unsigned frame_cycles = 92573u; /* MAME default LCD frame, ~59.732 Hz. */

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
    const unsigned lcd_x = static_cast<unsigned>(
        (output_x - destination.x) * static_cast<int>(GC_LCD_WIDTH) / destination.w);
    const unsigned lcd_y = static_cast<unsigned>(
        (output_y - destination.y) * static_cast<int>(GC_LCD_HEIGHT) / destination.h);
    const unsigned column = lcd_x * 13u / GC_LCD_WIDTH;
    const unsigned row = lcd_y * 10u / GC_LCD_HEIGHT;
    (void)gc_input_set_touch(&machine, true, column < 13u ? column : 12u,
                             row < 10u ? row : 9u);
}

void render_centered(SDL_Renderer *renderer, SDL_Texture *texture)
{
    int output_w = 0, output_h = 0;
    const SDL_Rect destination = centered_rect(renderer, output_w, output_h);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, &destination);
    SDL_RenderPresent(renderer);
}
}

int gc_sdl_frontend_run(GcMachine& machine, standalone::Sm8521Cpu& cpu,
                        const GcInputBindings& bindings)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
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
        SDL_Quit();
        return 1;
    }
    /* Do not tie emulated time to the monitor refresh rate: a 120/144 Hz
     * display would otherwise run the Game.com CPU that many times per second.
     * The explicit frame deadline below is the authoritative 59.732 Hz cap. */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, static_cast<int>(GC_LCD_WIDTH),
        static_cast<int>(GC_LCD_HEIGHT));
    if (!texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::array<std::uint32_t, GC_LCD_WIDTH * GC_LCD_HEIGHT> pixels{};
    bool done = false;

    const std::uint64_t counter_frequency = SDL_GetPerformanceFrequency();
    const std::uint64_t frame_ticks = static_cast<std::uint64_t>(
        static_cast<double>(counter_frequency) / 59.732155);
    std::uint64_t next_deadline = SDL_GetPerformanceCounter();
    std::uint64_t frame_number = 0;

    standalone::AudioSynth sound;
    standalone::AudioOutput audio_output;
    std::string audio_error;
    if (!audio_output.start(audio_error))
        std::fprintf(stderr, "Audio unavailable (%s); continuing silently.\n",
                     audio_error.c_str());
    std::uint64_t audio_cycle_remainder = 0;
    constexpr std::size_t max_audio_frames =
        (static_cast<std::uint64_t>(frame_cycles) * standalone::AudioOutput::sample_rate +
         GC_CPU_HZ - 1u) / GC_CPU_HZ + 1u;
    std::array<std::int16_t, max_audio_frames> audio_samples{};

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) done = true;
            else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const bool pressed = event.type == SDL_KEYDOWN;
                if (event.key.keysym.sym == SDLK_ESCAPE && pressed) done = true;
                else update_key(machine, bindings, event.key.keysym.sym, pressed);
            } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT)
                    update_touch_from_mouse(machine, window, renderer,
                        event.button.x, event.button.y,
                        event.type == SDL_MOUSEBUTTONDOWN);
            } else if (event.type == SDL_MOUSEMOTION &&
                       (event.motion.state & SDL_BUTTON_LMASK) != 0) {
                update_touch_from_mouse(machine, window, renderer,
                                        event.motion.x, event.motion.y, true);
            }
        }
        if (done) break;

        const std::uint64_t frame_start = cpu.cycles();
        const std::uint64_t frame_end = frame_start + frame_cycles;
        while (cpu.cycles() < frame_end) (void)cpu.step();
        const std::uint64_t frame_stop = cpu.cycles();
        const std::uint64_t elapsed_cycles = frame_stop - frame_start;

        audio_cycle_remainder +=
            elapsed_cycles * standalone::AudioOutput::sample_rate;
        const std::size_t audio_frames = static_cast<std::size_t>(audio_cycle_remainder / GC_CPU_HZ);
        audio_cycle_remainder %= GC_CPU_HZ;
        sound.render(cpu.sound_writes(), frame_start, frame_stop,
                     audio_samples.data(), audio_frames,
                     standalone::AudioOutput::sample_rate);
        cpu.clear_sound_writes();
        audio_output.push(audio_samples.data(), audio_frames);

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
        render_centered(renderer, texture);

        next_deadline += frame_ticks;
        std::uint64_t now = SDL_GetPerformanceCounter();
        if (next_deadline > now) {
            /* SDL_Delay is millisecond-granular. Sleep most of the remaining
             * interval, then wait the final sub-millisecond against the high-
             * resolution counter so gameplay stays close to the 59.732 Hz LCD. */
            const std::uint64_t remaining = next_deadline - now;
            const std::uint64_t wait_ms = remaining * 1000u /
                                          (counter_frequency ? counter_frequency : 1u);
            if (wait_ms > 1u) SDL_Delay(static_cast<std::uint32_t>(wait_ms - 1u));
            while ((now = SDL_GetPerformanceCounter()) < next_deadline) { }
        } else {
            /* If a frame overruns, don't try to catch up by running future
             * frames without pacing. Resume from the current wall-clock time. */
            next_deadline = now;
        }
        if (++frame_number % 300u == 0u) {
            char title[128];
            std::snprintf(title, sizeof(title), "Game.com LCD | %llu cycles",
                          static_cast<unsigned long long>(cpu.cycles()));
            SDL_SetWindowTitle(window, title);
        }
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
