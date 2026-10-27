#ifndef GAMECOM_SDL_UI_HPP
#define GAMECOM_SDL_UI_HPP

#include "gamecom_launcher.hpp"

#include <SDL.h>
#include <string>

struct GcUiColor {
    Uint8 r, g, b, a;
};

namespace gc_ui_palette {
constexpr GcUiColor background{13, 22, 35, 255};
constexpr GcUiColor menubar_bg{18, 30, 45, 255};
constexpr GcUiColor menubar_border{58, 96, 138, 255};
constexpr GcUiColor dropdown_bg{20, 33, 49, 250};
constexpr GcUiColor overlay_dim{8, 14, 24, 215};
constexpr GcUiColor panel{24, 39, 56, 255};
constexpr GcUiColor panel_light{34, 53, 72, 255};
constexpr GcUiColor panel_hover{46, 72, 98, 255};
constexpr GcUiColor ink{232, 241, 247, 255};
constexpr GcUiColor muted{150, 171, 188, 255};
constexpr GcUiColor accent{73, 211, 179, 255};
constexpr GcUiColor blue{93, 157, 244, 255};
constexpr GcUiColor warning{255, 190, 102, 255};
constexpr GcUiColor disabled{67, 79, 92, 255};
} // namespace gc_ui_palette

class GcUiFont {
public:
    explicit GcUiFont(SDL_Renderer *renderer);
    ~GcUiFont();
    GcUiFont(const GcUiFont&) = delete;
    GcUiFont& operator=(const GcUiFont&) = delete;

    void draw(int x, int y, const std::string& value,
              GcUiColor c = gc_ui_palette::ink, int scale = 2,
              int max_chars = 140) const;

private:
    SDL_Renderer *m_renderer = nullptr;
    SDL_Texture *m_texture = nullptr;
};

void gc_ui_fill(SDL_Renderer *r, SDL_Rect rect, GcUiColor c);
void gc_ui_outline(SDL_Renderer *r, SDL_Rect rect, GcUiColor c);
void gc_ui_present(SDL_Renderer *r, bool vsync_active);
bool gc_ui_inside(int x, int y, const SDL_Rect& rect);
void gc_ui_map_mouse(SDL_Window *window, int render_w, int render_h,
                     int raw_x, int raw_y, int& out_x, int& out_y);
std::string gc_ui_short_path(const std::string& path, std::size_t max_chars = 72);
std::string gc_ui_home_directory();
bool gc_ui_valid_firmware(const std::string& directory);

double gc_ui_clamp_lcd_hz(double hz);
unsigned gc_ui_clamp_cpu_speed(unsigned pct);
unsigned gc_ui_clamp_game_pace(unsigned pct);
std::string gc_ui_format_lcd_hz(double hz);
std::string gc_ui_format_cpu_speed(unsigned pct);

void gc_ui_draw_button(SDL_Renderer *r, const GcUiFont& font, const SDL_Rect& rect,
                       const std::string& label, bool enabled,
                       bool highlighted = false, bool hovered = false,
                       int text_scale = 2);

bool gc_ui_pick_path(SDL_Window *window, SDL_Renderer *renderer, const GcUiFont& font,
                     bool vsync_active, bool choose_directory,
                     const std::string& initial_folder, std::string& out_path,
                     bool& quit_requested, bool force_builtin, bool& used_builtin);

bool gc_ui_run_controls_modal(SDL_Window *window, SDL_Renderer *renderer,
                              const GcUiFont& font, bool vsync_active,
                              GcInputBindings& bindings, bool& quit_requested,
                              std::string& status);

bool gc_ui_prompt_number_modal(SDL_Window *window, SDL_Renderer *renderer,
                               const GcUiFont& font, bool vsync_active,
                               const std::string& title, const std::string& subtitle,
                               double initial_value, double min_val, double max_val,
                               bool allow_decimal, double& out_value,
                               bool& quit_requested);

enum class GcMenuAction {
    None = 0,
    TimingChanged,
    LoadCartridgeRom,
    SetFirmwareFolder,
    BootBiosOnly,
    BootSelected,
    ResetEmulation,
    TogglePause,
    ToggleLauncherView,
    ConfigureControls,
    ResetControls,
    ResizeWindow2x,
    ResizeWindow3x,
    ResizeWindow4x,
    ToggleFullscreen,
    PromptCustomLcdHz,
    PromptCustomCpuSpeed,
    Quit
};

class GcTopMenuBar {
public:
    static constexpr int height = 32;

    int open_tab() const noexcept { return m_open_tab; }
    void close() noexcept { m_open_tab = -1; }
    void show_toast(const std::string& message, double duration_sec = 2.2);
    void tick(double dt) noexcept;

    bool is_visible(bool in_launcher, const GcTimingOptions& timing,
                    int mouse_render_y) const noexcept;
    int reserved_top_height(bool in_launcher, const GcTimingOptions& timing) const noexcept;

    bool handle_event(const SDL_Event& event, SDL_Window *window, SDL_Renderer *renderer,
                      GcLaunchSelection& selection, bool in_launcher,
                      bool emulation_ready, bool paused, bool is_fullscreen,
                      GcMenuAction& out_action, bool& consumed_pointer);

    void render(SDL_Window *window, SDL_Renderer *renderer, const GcUiFont& font,
                const GcLaunchSelection& selection, bool in_launcher,
                bool emulation_ready, bool paused, bool fast_forward,
                bool is_fullscreen, double host_fps,
                int raw_mouse_x, int raw_mouse_y) const;

private:
    int m_open_tab = -1;
    std::string m_toast_text;
    double m_toast_timer = 0.0;
};

#endif
