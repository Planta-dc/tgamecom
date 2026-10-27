#ifndef GAMECOM_SDL_UI_HPP
#define GAMECOM_SDL_UI_HPP

#include "gamecom_launcher.hpp"

#include <SDL.h>
#include <string>

struct GcUiColor {
    Uint8 r, g, b, a;
};

/* Neutral Windows-style light chrome with a restrained system-blue accent. */
namespace gc_ui_palette {
constexpr GcUiColor background{243, 243, 243, 255};
constexpr GcUiColor menubar_bg{248, 248, 248, 255};
constexpr GcUiColor menubar_border{218, 218, 218, 255};
constexpr GcUiColor dropdown_bg{255, 255, 255, 255};
constexpr GcUiColor overlay_dim{25, 25, 25, 150};
constexpr GcUiColor panel{255, 255, 255, 255};
constexpr GcUiColor panel_light{247, 247, 247, 255};
constexpr GcUiColor panel_hover{232, 242, 252, 255};
constexpr GcUiColor ink{32, 32, 32, 255};
constexpr GcUiColor muted{96, 96, 96, 255};
constexpr GcUiColor accent{0, 103, 192, 255};
constexpr GcUiColor blue{210, 210, 210, 255};
constexpr GcUiColor warning{174, 75, 0, 255};
constexpr GcUiColor disabled{218, 218, 218, 255};
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

bool gc_ui_prompt_text_modal(SDL_Window *window, SDL_Renderer *renderer,
                             const GcUiFont& font, bool vsync_active,
                             const std::string& title, const std::string& subtitle,
                             const std::string& initial_value, std::string& out_value,
                             bool& quit_requested);

enum class GcMenuAction {
    None = 0,
    TimingChanged,
    VsyncToggled,
    LoadCartridgeRom,
    LoadSlot2Rom,
    EjectSlot2Rom,
    SetFirmwareFolder,
    BootBiosOnly,
    BootSelected,
    SaveNvramNow,
    SeedWarmBootNvram,
    SerialModeChanged,
    PromptSerialEndpoint,
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
    void show_toast(const std::string& message, double duration_sec = 5.0);
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
                bool is_fullscreen, double host_fps, double emulated_lcd_hz,
                int raw_mouse_x, int raw_mouse_y) const;

private:
    int m_open_tab = -1;
    std::string m_toast_text;
    double m_toast_timer = 0.0;
};

#endif
