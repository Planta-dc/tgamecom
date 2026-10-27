#include "gamecom_launcher.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(GAMECOM_HAVE_GTK3)
#include <gtk/gtk.h>
#endif

namespace {
namespace fs = std::filesystem;

struct Color { Uint8 r, g, b, a; };
constexpr Color background{13, 22, 35, 255};
constexpr Color overlay_dim{8, 14, 24, 215};
constexpr Color panel{24, 39, 56, 255};
constexpr Color panel_light{34, 53, 72, 255};
constexpr Color panel_hover{44, 68, 92, 255};
constexpr Color ink{232, 241, 247, 255};
constexpr Color muted{150, 171, 188, 255};
constexpr Color accent{73, 211, 179, 255};
constexpr Color blue{93, 157, 244, 255};
constexpr Color warning{255, 190, 102, 255};
constexpr Color disabled{67, 79, 92, 255};

const std::array<const char *, GC_CONTROL_COUNT> action_names = {
    "UP", "DOWN", "LEFT", "RIGHT", "A BUTTON", "B BUTTON", "C BUTTON", "D BUTTON",
    "MENU", "PAUSE", "SOUND", "POWER"
};
const std::array<const char *, GC_CONTROL_COUNT> action_keys = {
    "up", "down", "left", "right", "a", "b", "c", "d",
    "menu", "pause", "sound", "power"
};

constexpr std::array<double, 7> lcd_hz_presets = {
    30.0, 50.0, 55.2, 59.732155, 60.0, 75.0, 120.0
};
constexpr std::array<unsigned, 6> cpu_speed_presets = {
    75u, 100u, 120u, 150u, 200u, 300u
};

std::array<unsigned char, 7> glyph(char c)
{
    switch (c) {
    case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
    case 'C': return {14,17,16,16,16,17,14}; case 'D': return {30,17,17,17,17,17,30};
    case 'E': return {31,16,16,30,16,16,31}; case 'F': return {31,16,16,30,16,16,16};
    case 'G': return {14,17,16,23,17,17,15}; case 'H': return {17,17,17,31,17,17,17};
    case 'I': return {14,4,4,4,4,4,14}; case 'J': return {7,2,2,2,18,18,12};
    case 'K': return {17,18,20,24,20,18,17}; case 'L': return {16,16,16,16,16,16,31};
    case 'M': return {17,27,21,21,17,17,17}; case 'N': return {17,25,21,19,17,17,17};
    case 'O': return {14,17,17,17,17,17,14}; case 'P': return {30,17,17,30,16,16,16};
    case 'Q': return {14,17,17,17,21,18,13}; case 'R': return {30,17,17,30,20,18,17};
    case 'S': return {15,16,16,14,1,1,30}; case 'T': return {31,4,4,4,4,4,4};
    case 'U': return {17,17,17,17,17,17,14}; case 'V': return {17,17,17,17,17,10,4};
    case 'W': return {17,17,17,21,21,21,10}; case 'X': return {17,17,10,4,10,17,17};
    case 'Y': return {17,17,10,4,4,4,4}; case 'Z': return {31,1,2,4,8,16,31};
    case '0': return {14,17,19,21,25,17,14}; case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31}; case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2}; case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14}; case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14}; case '9': return {14,17,17,15,1,1,14};
    case '.': return {0,0,0,0,0,12,12}; case ',': return {0,0,0,0,4,4,8};
    case ':': return {0,12,12,0,12,12,0}; case ';': return {0,12,12,0,4,4,8};
    case '-': return {0,0,0,31,0,0,0}; case '_': return {0,0,0,0,0,0,31};
    case '/': return {1,2,2,4,8,8,16}; case '\\': return {16,8,8,4,2,2,1};
    case '+': return {0,4,4,31,4,4,0}; case '=': return {0,31,0,31,0,0,0};
    case '(': return {2,4,8,8,8,4,2}; case ')': return {8,4,2,2,2,4,8};
    case '[': return {14,8,8,8,8,8,14}; case ']': return {14,2,2,2,2,2,14};
    case '<': return {2,4,8,16,8,4,2}; case '>': return {8,4,2,1,2,4,8};
    case '%': return {25,25,2,4,8,19,19}; case '~': return {0,0,8,21,2,0,0};
    case '#': return {10,10,31,10,31,10,10};
    case '\'': return {4,4,8,0,0,0,0}; case '!': return {4,4,4,4,4,0,4};
    case '?': return {14,17,1,2,4,0,4}; case '*': return {0,17,10,31,10,17,0};
    case ' ': return {0,0,0,0,0,0,0};
    default: return {14,17,1,2,4,0,4};
    }
}

class FontAtlas {
public:
    explicit FontAtlas(SDL_Renderer *renderer) : m_renderer(renderer)
    {
        constexpr int cell_w = 6;
        constexpr int cell_h = 7;
        constexpr int tex_w = 128 * cell_w;
        constexpr int tex_h = cell_h;
        m_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STATIC, tex_w, tex_h);
        if (!m_texture) return;
        SDL_SetTextureBlendMode(m_texture, SDL_BLENDMODE_BLEND);
        std::array<Uint32, tex_w * tex_h> pixels{};
        pixels.fill(0u);
        for (int code = 0; code < 128; ++code) {
            const auto rows = glyph(static_cast<char>(code));
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if ((rows[static_cast<std::size_t>(row)] & (1u << (4 - col))) != 0) {
                        pixels[static_cast<std::size_t>(row * tex_w + code * cell_w + col)] =
                            0xFFFFFFFFu;
                    }
                }
            }
        }
        if (SDL_UpdateTexture(m_texture, nullptr, pixels.data(),
                              static_cast<int>(tex_w * sizeof(Uint32))) != 0) {
            SDL_DestroyTexture(m_texture);
            m_texture = nullptr;
        }
    }

    ~FontAtlas()
    {
        if (m_texture) SDL_DestroyTexture(m_texture);
    }

    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    void draw(int x, int y, const std::string& value,
              Color c = ink, int scale = 2, int max_chars = 120) const
    {
        const int limit = std::min<int>(static_cast<int>(value.size()), max_chars);
        if (limit <= 0) return;
        if (m_texture) {
            SDL_SetTextureColorMod(m_texture, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(m_texture, c.a);
            for (int i = 0; i < limit; ++i) {
                const unsigned char raw = static_cast<unsigned char>(value[static_cast<std::size_t>(i)]);
                const int code = std::toupper(raw) & 0x7F;
                if (code == ' ') continue;
                const SDL_Rect src{code * 6, 0, 5, 7};
                const SDL_Rect dst{x + i * 6 * scale, y, 5 * scale, 7 * scale};
                SDL_RenderCopy(m_renderer, m_texture, &src, &dst);
            }
            return;
        }
        SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
        for (int i = 0; i < limit; ++i) {
            const unsigned char raw = static_cast<unsigned char>(value[static_cast<std::size_t>(i)]);
            const char ch = static_cast<char>(std::toupper(raw));
            if (ch == ' ') continue;
            const auto rows = glyph(ch);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if ((rows[static_cast<std::size_t>(row)] & (1u << (4 - col))) != 0) {
                        SDL_Rect pixel{x + i * 6 * scale + col * scale,
                                       y + row * scale, scale, scale};
                        SDL_RenderFillRect(m_renderer, &pixel);
                    }
                }
            }
        }
    }

private:
    SDL_Renderer *m_renderer = nullptr;
    SDL_Texture *m_texture = nullptr;
};

void fill(SDL_Renderer *r, SDL_Rect rect, Color c)
{
    if (c.a < 255) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rect);
    if (c.a < 255) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void outline(SDL_Renderer *r, SDL_Rect rect, Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderDrawRect(r, &rect);
}

void present_frame(SDL_Renderer *r, bool vsync_active)
{
    SDL_RenderPresent(r);
    if (!vsync_active) SDL_Delay(8);
}

std::string short_path(const std::string& path, std::size_t max_chars = 72)
{
    if (path.size() <= max_chars) return path;
    if (max_chars < 5) return path.substr(path.size() - max_chars);
    return "..." + path.substr(path.size() - max_chars + 3);
}

bool inside(int x, int y, const SDL_Rect& rect)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

void map_mouse_coords(SDL_Window *window, int render_w, int render_h,
                      int raw_x, int raw_y, int& out_x, int& out_y)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    out_x = (ww > 0 && render_w > 0) ? raw_x * render_w / ww : raw_x;
    out_y = (wh > 0 && render_h > 0) ? raw_y * render_h / wh : raw_y;
}

fs::path home_directory()
{
    if (const char *home = std::getenv("HOME")) {
        if (*home) return fs::path(home);
    }
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    return ec ? fs::path(".") : current;
}

fs::path config_directory()
{
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) return fs::path(xdg) / "gamecom-emulator";
    }
    return home_directory() / ".config" / "gamecom-emulator";
}

fs::path bindings_file()
{
    return config_directory() / "controls.ini";
}

fs::path launcher_state_file()
{
    return config_directory() / "launcher.ini";
}

void trim_string(std::string& s)
{
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s.clear(); return; }
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
}

bool valid_firmware(const std::string& directory)
{
    if (directory.empty()) return false;
    std::error_code ec;
    const fs::path base(directory);
    const bool internal = fs::is_regular_file(base / "internal.bin", ec) && !ec;
    ec.clear();
    const bool external = fs::is_regular_file(base / "external.bin", ec) && !ec;
    return internal && external;
}

bool key_is_used(const GcInputBindings& bindings, int key, unsigned except)
{
    for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
        if (i != except && bindings.keycodes[i] == key) return true;
    }
    return false;
}

std::string lower_extension(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool is_rom_extension(const fs::path& path)
{
    const std::string ext = lower_extension(path);
    return ext == ".bin" || ext == ".rom" || ext == ".tgc";
}

double clamp_lcd_hz(double hz)
{
    if (!std::isfinite(hz)) return 60.0;
    return std::max(10.0, std::min(240.0, hz));
}

unsigned clamp_cpu_speed(unsigned pct)
{
    return std::max(25u, std::min(800u, pct));
}

double step_lcd_hz(double current, int delta)
{
    std::size_t best = 0;
    double best_diff = std::fabs(current - lcd_hz_presets[0]);
    for (std::size_t i = 1; i < lcd_hz_presets.size(); ++i) {
        const double diff = std::fabs(current - lcd_hz_presets[i]);
        if (diff < best_diff) { best = i; best_diff = diff; }
    }
    const int next = static_cast<int>(best) + delta;
    if (next < 0) return lcd_hz_presets.front();
    if (next >= static_cast<int>(lcd_hz_presets.size())) return lcd_hz_presets.back();
    return lcd_hz_presets[static_cast<std::size_t>(next)];
}

unsigned step_cpu_speed(unsigned current, int delta)
{
    std::size_t best = 0;
    unsigned best_diff = current > cpu_speed_presets[0]
        ? current - cpu_speed_presets[0] : cpu_speed_presets[0] - current;
    for (std::size_t i = 1; i < cpu_speed_presets.size(); ++i) {
        const unsigned diff = current > cpu_speed_presets[i]
            ? current - cpu_speed_presets[i] : cpu_speed_presets[i] - current;
        if (diff < best_diff) { best = i; best_diff = diff; }
    }
    const int next = static_cast<int>(best) + delta;
    if (next < 0) return cpu_speed_presets.front();
    if (next >= static_cast<int>(cpu_speed_presets.size())) return cpu_speed_presets.back();
    return cpu_speed_presets[static_cast<std::size_t>(next)];
}

std::string format_lcd_hz(double hz)
{
    char buf[48];
    if (std::fabs(hz - 59.732155) < 0.01)
        std::snprintf(buf, sizeof(buf), "59.73 HZ (REF)");
    else if (std::fabs(hz - std::round(hz)) < 0.005)
        std::snprintf(buf, sizeof(buf), "%.0f HZ", hz);
    else
        std::snprintf(buf, sizeof(buf), "%.2f HZ", hz);
    return buf;
}

std::string format_cpu_speed(unsigned pct)
{
    char buf[48];
    const double mhz = (5.5296 * static_cast<double>(pct)) / 100.0;
    std::snprintf(buf, sizeof(buf), "%u%% (%.2f MHZ)", pct, mhz);
    return buf;
}

void draw_button(SDL_Renderer *r, const FontAtlas& font, const SDL_Rect& rect,
                 const std::string& label, bool enabled,
                 bool highlighted = false, bool hovered = false)
{
    const Color base = !enabled ? disabled
        : (highlighted ? accent : (hovered ? panel_hover : panel_light));
    fill(r, rect, base);
    outline(r, rect, enabled ? (highlighted ? accent : blue) : muted);
    const int scale = 2;
    const int max_chars = std::max(1, (rect.w - 12) / (6 * scale));
    const int shown_chars = std::min<int>(static_cast<int>(label.size()), max_chars);
    const int label_w = shown_chars * 6 * scale - scale;
    const int label_h = 7 * scale;
    const int tx = rect.x + std::max(6, (rect.w - label_w) / 2);
    const int ty = rect.y + (rect.h - label_h) / 2;
    font.draw(tx, ty, label, enabled && highlighted ? background : ink, scale, max_chars);
}

enum class BrowserMode { FirmwareDirectory, CartridgeFile };
struct BrowserEntry { fs::path path; std::string name; bool directory; };
enum class NativePickResult { Unavailable, Cancelled, Selected, UseBuiltin };

void draw_dialog_wait_overlay(SDL_Window *window, SDL_Renderer *renderer,
                              const FontAtlas& font, BrowserMode mode,
                              SDL_Rect& builtin_btn, SDL_Rect& cancel_btn,
                              int mouse_x, int mouse_y)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
    SDL_RenderClear(renderer);
    fill(renderer, SDL_Rect{0, 0, w, h}, overlay_dim);

    SDL_Rect modal{(w - 660) / 2, (h - 240) / 2, 660, 240};
    if (modal.x < 16) { modal.x = 16; modal.w = w - 32; }
    if (modal.y < 16) { modal.y = 16; modal.h = h - 32; }
    fill(renderer, modal, panel);
    outline(renderer, modal, accent);

    font.draw(modal.x + 28, modal.y + 28,
              mode == BrowserMode::FirmwareDirectory
                  ? "SYSTEM FOLDER DIALOG OPEN"
                  : "SYSTEM FILE DIALOG OPEN",
              accent, 3, 44);
    font.draw(modal.x + 28, modal.y + 78,
              "CHOOSE YOUR PATH IN THE SYSTEM WINDOW, OR SWITCH TO THE",
              ink, 2, 52);
    font.draw(modal.x + 28, modal.y + 104,
              "BUILT-IN BROWSER BELOW IF THE DIALOG IS HIDDEN.",
              muted, 2, 52);

    builtin_btn = SDL_Rect{modal.x + 28, modal.y + modal.h - 68, 280, 44};
    cancel_btn = SDL_Rect{modal.x + modal.w - 198, modal.y + modal.h - 68, 170, 44};
    int mx = 0, my = 0;
    map_mouse_coords(window, w, h, mouse_x, mouse_y, mx, my);
    draw_button(renderer, font, builtin_btn, "IN-APP BROWSER", true, false, inside(mx, my, builtin_btn));
    draw_button(renderer, font, cancel_btn, "CANCEL (ESC)", true, false, inside(mx, my, cancel_btn));
}

#if defined(__unix__) || defined(__APPLE__)
enum class ExternalDialogTool { None, Zenity, Qarma, Kdialog };

std::string find_host_executable(const char *name)
{
    const char *path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return {};
    const char *appdir = std::getenv("APPDIR");
    const std::string appdir_prefix = (appdir && *appdir) ? std::string(appdir) : std::string();

    std::string paths(path_env);
    std::size_t start = 0;
    while (start <= paths.size()) {
        const std::size_t colon = paths.find(':', start);
        const std::string dir = colon == std::string::npos
            ? paths.substr(start) : paths.substr(start, colon - start);
        start = colon == std::string::npos ? paths.size() + 1u : colon + 1u;
        if (dir.empty()) continue;
        if (!appdir_prefix.empty() && dir.rfind(appdir_prefix, 0) == 0) continue;
        const fs::path candidate = fs::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return {};
}

void sanitize_child_environment_for_host_dialog()
{
    /* When packaged as an AppImage, AppRun injects LD_LIBRARY_PATH and toolkit
     * variables pointing inside $APPDIR. Host dialog binaries (zenity/kdialog)
     * must use the host's own GLib/GTK/Qt libraries and schemas instead. */
    static constexpr const char *unset_vars[] = {
        "LD_LIBRARY_PATH", "LD_PRELOAD",
        "GTK_PATH", "GTK_EXE_PREFIX", "GTK_DATA_PREFIX", "GTK_IM_MODULE_FILE",
        "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR",
        "GIO_MODULE_DIR", "GIO_EXTRA_MODULES", "GSETTINGS_SCHEMA_DIR",
        "QT_PLUGIN_PATH", "QML2_IMPORT_PATH",
        "PERLLIB", "PYTHONHOME", "PYTHONPATH"
    };
    const char *appdir = std::getenv("APPDIR");
    if (appdir && *appdir) {
        for (const char *var : unset_vars) ::unsetenv(var);
        if (const char *xdg_data = std::getenv("XDG_DATA_DIRS")) {
            const std::string appdir_str(appdir);
            std::string cleaned;
            std::string raw(xdg_data);
            std::size_t pos = 0;
            while (pos <= raw.size()) {
                const std::size_t colon = raw.find(':', pos);
                const std::string item = colon == std::string::npos
                    ? raw.substr(pos) : raw.substr(pos, colon - pos);
                pos = colon == std::string::npos ? raw.size() + 1u : colon + 1u;
                if (item.empty() || item.rfind(appdir_str, 0) == 0) continue;
                if (!cleaned.empty()) cleaned.push_back(':');
                cleaned += item;
            }
            if (cleaned.empty()) cleaned = "/usr/local/share:/usr/share";
            ::setenv("XDG_DATA_DIRS", cleaned.c_str(), 1);
        }
    }
}

void terminate_child(pid_t pid)
{
    if (pid <= 0) return;
    (void)::kill(pid, SIGTERM);
    for (int i = 0; i < 15; ++i) {
        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid || waited < 0) return;
        SDL_Delay(5);
    }
    (void)::kill(pid, SIGKILL);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
}

NativePickResult external_portal_dialog(SDL_Window *window, SDL_Renderer *renderer,
                                        const FontAtlas& font, bool vsync_active,
                                        BrowserMode mode, const std::string& initial_folder,
                                        std::string& selected_path, bool& quit_requested)
{
    const char *driver = SDL_GetCurrentVideoDriver();
    if (driver && std::strcmp(driver, "dummy") == 0) return NativePickResult::Unavailable;

    ExternalDialogTool tool = ExternalDialogTool::None;
    std::string exe = find_host_executable("zenity");
    if (!exe.empty()) tool = ExternalDialogTool::Zenity;
    if (tool == ExternalDialogTool::None) {
        exe = find_host_executable("qarma");
        if (!exe.empty()) tool = ExternalDialogTool::Qarma;
    }
    if (tool == ExternalDialogTool::None) {
        exe = find_host_executable("kdialog");
        if (!exe.empty()) tool = ExternalDialogTool::Kdialog;
    }
    if (tool == ExternalDialogTool::None) return NativePickResult::Unavailable;

    const bool choose_folder = mode == BrowserMode::FirmwareDirectory;
    std::string start_dir = initial_folder;
    if (start_dir.empty()) start_dir = home_directory().string();
    if (!start_dir.empty() && start_dir.back() != '/') start_dir.push_back('/');

    std::vector<std::string> args;
    args.push_back(exe);
    if (tool == ExternalDialogTool::Zenity || tool == ExternalDialogTool::Qarma) {
        args.push_back("--file-selection");
        args.push_back(choose_folder
            ? "--title=Select Game.com Firmware Folder"
            : "--title=Select Game.com Cartridge ROM");
        args.push_back("--filename=" + start_dir);
        if (choose_folder) {
            args.push_back("--directory");
        } else {
            args.push_back("--file-filter=Game.com ROMs (*.bin *.rom *.tgc) | *.bin *.BIN *.rom *.ROM *.tgc *.TGC");
            args.push_back("--file-filter=All files | *");
        }
    } else {
        args.push_back("--title");
        args.push_back(choose_folder
            ? "Select Game.com Firmware Folder"
            : "Select Game.com Cartridge ROM");
        if (choose_folder) {
            args.push_back("--getexistingdirectory");
            args.push_back(start_dir);
        } else {
            args.push_back("--getopenfilename");
            args.push_back(start_dir);
            args.push_back("*.bin *.BIN *.rom *.ROM *.tgc *.TGC|Game.com Cartridge Images");
        }
    }

    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) return NativePickResult::Unavailable;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return NativePickResult::Unavailable;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        (void)::dup2(pipefd[1], STDOUT_FILENO);
        if (pipefd[1] != STDOUT_FILENO) ::close(pipefd[1]);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)::dup2(devnull, STDERR_FILENO);
            if (devnull != STDERR_FILENO) ::close(devnull);
        }
        sanitize_child_environment_for_host_dialog();
        std::vector<char *> argv_ptrs;
        argv_ptrs.reserve(args.size() + 1u);
        for (std::string& a : args) argv_ptrs.push_back(a.data());
        argv_ptrs.push_back(nullptr);
        ::execvp(exe.c_str(), argv_ptrs.data());
        ::_exit(127);
    }

    ::close(pipefd[1]);
    const int flags = ::fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string captured;
    auto drain_pipe = [&]() {
        char buf[512];
        for (;;) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
            if (n > 0) captured.append(buf, static_cast<std::size_t>(n));
            else break;
        }
    };

    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_Rect builtin_btn{}, cancel_btn{};
    bool user_cancelled = false;
    bool switch_to_builtin = false;
    int child_status = 0;

    for (;;) {
        drain_pipe();
        const pid_t waited = ::waitpid(pid, &child_status, WNOHANG);
        if (waited == pid || waited < 0) break;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit_requested = true;
                user_cancelled = true;
            } else if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                       event.key.keysym.sym == SDLK_ESCAPE) {
                user_cancelled = true;
            } else if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                       event.button.button == SDL_BUTTON_LEFT) {
                int w = 0, h = 0, mx = 0, my = 0;
                SDL_GetRendererOutputSize(renderer, &w, &h);
                map_mouse_coords(window, w, h, event.button.x, event.button.y, mx, my);
                if (inside(mx, my, cancel_btn)) user_cancelled = true;
                else if (inside(mx, my, builtin_btn)) switch_to_builtin = true;
            }
        }
        if (user_cancelled || switch_to_builtin) {
            terminate_child(pid);
            ::close(pipefd[0]);
            return switch_to_builtin ? NativePickResult::UseBuiltin : NativePickResult::Cancelled;
        }

        draw_dialog_wait_overlay(window, renderer, font, mode,
                                 builtin_btn, cancel_btn, mouse_x, mouse_y);
        present_frame(renderer, vsync_active);
    }

    drain_pipe();
    ::close(pipefd[0]);

    while (!captured.empty() && (captured.back() == '\n' || captured.back() == '\r'))
        captured.pop_back();

    if (WIFEXITED(child_status)) {
        const int code = WEXITSTATUS(child_status);
        if (code == 0 && !captured.empty()) {
            selected_path = captured;
            return NativePickResult::Selected;
        }
        if (code == 1) return NativePickResult::Cancelled;
    }
    return NativePickResult::Unavailable;
}
#endif

NativePickResult native_file_dialog(SDL_Window *window, SDL_Renderer *renderer,
                                    const FontAtlas& font, bool vsync_active,
                                    BrowserMode mode, const std::string& initial_folder,
                                    std::string& selected_path, bool& quit_requested)
{
#if defined(GAMECOM_HAVE_GTK3)
    const char *driver = SDL_GetCurrentVideoDriver();
    if (driver && std::string(driver) == "dummy") return NativePickResult::Unavailable;
    static bool gtk_checked = false;
    static bool gtk_ready = false;
    if (!gtk_checked) {
        gtk_checked = true;
        int argc = 0;
        char **argv = nullptr;
        gtk_ready = gtk_init_check(&argc, &argv) != FALSE;
    }
    if (gtk_ready) {
        const bool choose_folder = mode == BrowserMode::FirmwareDirectory;
        GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
            choose_folder ? "Select Game.com firmware folder" : "Select Game.com cartridge ROM",
            nullptr,
            choose_folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
            choose_folder ? "Select Folder" : "Open",
            "Cancel");
        if (dialog) {
            gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), TRUE);
            if (!initial_folder.empty())
                (void)gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), initial_folder.c_str());
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);
            if (!choose_folder) {
                /* gtk_file_chooser_add_filter sinks the floating GtkFileFilter
                 * reference and takes ownership; do NOT call g_object_unref here. */
                GtkFileFilter *filter = gtk_file_filter_new();
                gtk_file_filter_set_name(filter, "Game.com cartridge images (*.bin, *.rom, *.tgc)");
                gtk_file_filter_add_pattern(filter, "*.bin");
                gtk_file_filter_add_pattern(filter, "*.BIN");
                gtk_file_filter_add_pattern(filter, "*.rom");
                gtk_file_filter_add_pattern(filter, "*.ROM");
                gtk_file_filter_add_pattern(filter, "*.tgc");
                gtk_file_filter_add_pattern(filter, "*.TGC");
                gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
                GtkFileFilter *all = gtk_file_filter_new();
                gtk_file_filter_set_name(all, "All files");
                gtk_file_filter_add_pattern(all, "*");
                gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);
            }

            struct GtkResponseState { bool finished = false; gint response = GTK_RESPONSE_CANCEL; } state;
            const gulong handler_id = g_signal_connect(
                dialog, "response",
                G_CALLBACK(+[](GtkNativeDialog *, gint resp, gpointer user_data) {
                    auto *st = static_cast<GtkResponseState *>(user_data);
                    st->response = resp;
                    st->finished = true;
                }),
                &state);

            gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
            int mouse_x = 0, mouse_y = 0;
            SDL_GetMouseState(&mouse_x, &mouse_y);
            SDL_Rect builtin_btn{}, cancel_btn{};
            bool switch_to_builtin = false;

            while (!state.finished && !quit_requested) {
                while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT) {
                        quit_requested = true;
                        state.finished = true;
                    } else if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                               event.key.keysym.sym == SDLK_ESCAPE) {
                        state.finished = true;
                    } else if (event.type == SDL_MOUSEMOTION) {
                        mouse_x = event.motion.x;
                        mouse_y = event.motion.y;
                    } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                               event.button.button == SDL_BUTTON_LEFT) {
                        int w = 0, h = 0, mx = 0, my = 0;
                        SDL_GetRendererOutputSize(renderer, &w, &h);
                        map_mouse_coords(window, w, h, event.button.x, event.button.y, mx, my);
                        if (inside(mx, my, cancel_btn)) state.finished = true;
                        else if (inside(mx, my, builtin_btn)) {
                            switch_to_builtin = true;
                            state.finished = true;
                        }
                    }
                }
                if (state.finished) break;
                draw_dialog_wait_overlay(window, renderer, font, mode,
                                         builtin_btn, cancel_btn, mouse_x, mouse_y);
                present_frame(renderer, vsync_active);
            }

            NativePickResult result = switch_to_builtin
                ? NativePickResult::UseBuiltin : NativePickResult::Cancelled;
            if (!switch_to_builtin && !quit_requested && state.response == GTK_RESPONSE_ACCEPT) {
                gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) {
                    selected_path = filename;
                    g_free(filename);
                    result = NativePickResult::Selected;
                }
            }
            if (handler_id != 0) g_signal_handler_disconnect(dialog, handler_id);
            gtk_native_dialog_hide(GTK_NATIVE_DIALOG(dialog));
            gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog));
            g_object_unref(dialog);
            /* Pump pending GDK/Wayland/X11 teardown events so the dialog window
             * actually unmaps and closes immediately instead of lingering frozen. */
            for (int i = 0; i < 32 && gtk_events_pending(); ++i)
                gtk_main_iteration_do(FALSE);
            return result;
        }
    }
#endif

#if defined(__unix__) || defined(__APPLE__)
    return external_portal_dialog(window, renderer, font, vsync_active,
                                  mode, initial_folder, selected_path, quit_requested);
#else
    (void)window;
    (void)renderer;
    (void)font;
    (void)vsync_active;
    (void)mode;
    (void)initial_folder;
    (void)selected_path;
    (void)quit_requested;
    return NativePickResult::Unavailable;
#endif
}

void scan_directory(const fs::path& directory, BrowserMode mode, bool show_all_files,
                    std::vector<BrowserEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();
    std::error_code ec;
    if (directory.has_parent_path() && directory != directory.root_path())
        entries.push_back({directory.parent_path(), "..", true});

    std::vector<BrowserEntry> folders, files;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        error = "CANNOT READ THIS FOLDER: " + ec.message();
        return;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code item_ec;
        const fs::path path = it->path();
        const std::string fname = path.filename().string();
        if (fname.empty()) continue;
        const bool is_dir = it->is_directory(item_ec);
        if (item_ec) continue;
        if (is_dir) {
            folders.push_back({path, fname, true});
        } else if (mode == BrowserMode::CartridgeFile && it->is_regular_file(item_ec) &&
                   (show_all_files || is_rom_extension(path))) {
            files.push_back({path, fname, false});
        }
    }
    const auto by_name = [](const BrowserEntry& a, const BrowserEntry& b) {
        std::string an = a.name, bn = b.name;
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return an < bn;
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    entries.insert(entries.end(), folders.begin(), folders.end());
    entries.insert(entries.end(), files.begin(), files.end());
}

bool run_browser(SDL_Window *window, SDL_Renderer *renderer, const FontAtlas& font,
                 bool vsync_active, BrowserMode mode,
                 const std::string& initial_folder, std::string& result,
                 bool& quit_requested)
{
    fs::path current = initial_folder.empty() ? home_directory() : fs::path(initial_folder);
    std::error_code initial_ec;
    if (!fs::is_directory(current, initial_ec) || initial_ec) current = home_directory();
    std::vector<BrowserEntry> entries;
    std::string scan_error;
    int selected = 0, first_visible = 0;
    bool show_all_files = false;
    bool done = false, accepted = false;
    constexpr int row_height = 28;
    constexpr int visible_rows = 11;
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    auto refresh = [&]() {
        scan_directory(current, mode, show_all_files, entries, scan_error);
        selected = 0;
        first_visible = 0;
    };
    auto enter_selected = [&]() {
        if (selected < 0 || selected >= static_cast<int>(entries.size())) return;
        const BrowserEntry& entry = entries[static_cast<std::size_t>(selected)];
        if (entry.directory) {
            current = entry.path;
            refresh();
        } else if (mode == BrowserMode::CartridgeFile) {
            result = entry.path.string();
            accepted = true;
            done = true;
        }
    };
    refresh();

    while (!done && !quit_requested) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_Rect modal{(w - 820) / 2, (h - 560) / 2, 820, 560};
        if (modal.x < 12) { modal.x = 12; modal.w = w - 24; }
        if (modal.y < 12) { modal.y = 12; modal.h = h - 24; }
        const SDL_Rect up_button{modal.x + modal.w - 336, modal.y + 18, 96, 34};
        const SDL_Rect home_button{modal.x + modal.w - 230, modal.y + 18, 96, 34};
        const SDL_Rect filter_button{modal.x + modal.w - 124, modal.y + 18, 100, 34};
        const SDL_Rect list{modal.x + 24, modal.y + 102, modal.w - 48, row_height * visible_rows};
        const SDL_Rect accept_button{modal.x + 24, modal.y + modal.h - 60, 260, 42};
        const SDL_Rect cancel_button{modal.x + modal.w - 184, modal.y + modal.h - 60, 160, 42};

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit_requested = true; done = true; break; }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }
            if (event.type == SDL_MOUSEWHEEL && !entries.empty()) {
                const int step = event.wheel.y > 0 ? -3 : (event.wheel.y < 0 ? 3 : 0);
                first_visible = std::max(0, std::min(first_visible + step,
                    std::max(0, static_cast<int>(entries.size()) - visible_rows)));
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) done = true;
                else if (key == SDLK_UP && selected > 0) {
                    --selected;
                    if (selected < first_visible) first_visible = selected;
                } else if (key == SDLK_DOWN && selected + 1 < static_cast<int>(entries.size())) {
                    ++selected;
                    if (selected >= first_visible + visible_rows)
                        first_visible = selected - visible_rows + 1;
                } else if (key == SDLK_PAGEUP) {
                    selected = std::max(0, selected - visible_rows);
                    if (selected < first_visible) first_visible = selected;
                } else if (key == SDLK_PAGEDOWN && !entries.empty()) {
                    selected = std::min(static_cast<int>(entries.size()) - 1, selected + visible_rows);
                    if (selected >= first_visible + visible_rows)
                        first_visible = selected - visible_rows + 1;
                } else if (key == SDLK_HOME) {
                    selected = 0;
                    first_visible = 0;
                } else if (key == SDLK_END && !entries.empty()) {
                    selected = static_cast<int>(entries.size()) - 1;
                    first_visible = std::max(0, selected - visible_rows + 1);
                } else if (key == SDLK_BACKSPACE) {
                    if (current.has_parent_path() && current != current.root_path()) {
                        current = current.parent_path();
                        refresh();
                    }
                } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) enter_selected();
                else if (mode == BrowserMode::FirmwareDirectory && key == SDLK_s) {
                    result = current.string(); accepted = true; done = true;
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int mx = 0, my = 0;
                map_mouse_coords(window, w, h, event.button.x, event.button.y, mx, my);
                if (inside(mx, my, cancel_button)) done = true;
                else if (inside(mx, my, up_button)) {
                    if (current.has_parent_path() && current != current.root_path()) {
                        current = current.parent_path();
                        refresh();
                    }
                } else if (inside(mx, my, home_button)) {
                    current = home_directory();
                    refresh();
                } else if (mode == BrowserMode::CartridgeFile && inside(mx, my, filter_button)) {
                    show_all_files = !show_all_files;
                    refresh();
                } else if (inside(mx, my, accept_button)) {
                    if (mode == BrowserMode::FirmwareDirectory) {
                        result = current.string(); accepted = true; done = true;
                    } else if (selected >= 0 && selected < static_cast<int>(entries.size())) {
                        if (entries[static_cast<std::size_t>(selected)].directory) {
                            enter_selected();
                        } else {
                            result = entries[static_cast<std::size_t>(selected)].path.string();
                            accepted = true; done = true;
                        }
                    }
                } else if (inside(mx, my, list)) {
                    const int row = (my - list.y) / row_height;
                    const int index = first_visible + row;
                    if (index >= 0 && index < static_cast<int>(entries.size())) {
                        if (selected == index && event.button.clicks >= 2) enter_selected();
                        else selected = index;
                    }
                }
            }
        }
        if (done || quit_requested) break;

        first_visible = std::max(0, std::min(first_visible,
            std::max(0, static_cast<int>(entries.size()) - visible_rows)));

        int mx = 0, my = 0;
        map_mouse_coords(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        fill(renderer, modal, panel);
        outline(renderer, modal, blue);
        font.draw(modal.x + 24, modal.y + 22,
                  mode == BrowserMode::FirmwareDirectory ? "CHOOSE FIRMWARE FOLDER" : "CHOOSE GAME ROM",
                  accent, 3, 26);
        draw_button(renderer, font, up_button, "UP", true, false, inside(mx, my, up_button));
        draw_button(renderer, font, home_button, "HOME", true, false, inside(mx, my, home_button));
        if (mode == BrowserMode::CartridgeFile) {
            draw_button(renderer, font, filter_button, show_all_files ? "ALL" : ".BIN",
                        true, show_all_files, inside(mx, my, filter_button));
        }

        const bool dir_has_fw = mode == BrowserMode::FirmwareDirectory && valid_firmware(current.string());
        font.draw(modal.x + 24, modal.y + 66, short_path(current.string(), 62),
                  dir_has_fw ? accent : muted, 2, 62);
        fill(renderer, list, background);
        outline(renderer, list, panel_light);
        for (int row = 0; row < visible_rows; ++row) {
            const int index = first_visible + row;
            if (index >= static_cast<int>(entries.size())) break;
            const BrowserEntry& entry = entries[static_cast<std::size_t>(index)];
            const SDL_Rect item{list.x + 2, list.y + row * row_height + 1, list.w - 4, row_height - 2};
            const bool chosen = index == selected;
            const bool hovered = inside(mx, my, item);
            if (chosen) fill(renderer, item, panel_light);
            else if (hovered) fill(renderer, item, Color{20, 33, 48, 255});
            const std::string label = entry.directory ? "[DIR]  " + entry.name : "[ROM]  " + entry.name;
            font.draw(item.x + 8, item.y + 6, label, chosen ? accent : ink, 2,
                      std::max(1, (item.w - 16) / 12));
        }
        if (!scan_error.empty())
            font.draw(modal.x + 24, modal.y + modal.h - 114, scan_error, warning, 2, 62);
        else if (dir_has_fw)
            font.draw(modal.x + 24, modal.y + modal.h - 114,
                      "INTERNAL.BIN + EXTERNAL.BIN DETECTED IN THIS FOLDER!", accent, 2, 62);

        const std::string hint = mode == BrowserMode::FirmwareDirectory
            ? "WHEEL/ARROWS SCROLL  DOUBLE-CLICK/ENTER OPENS  BACKSPACE UP  S SELECTS FOLDER"
            : "WHEEL/ARROWS SCROLL  DOUBLE-CLICK/ENTER OPENS ROM OR FOLDER  BACKSPACE UP";
        font.draw(modal.x + 24, modal.y + modal.h - 86, hint, muted, 1,
                  std::max(1, (modal.w - 48) / 6));
        const bool can_accept = mode == BrowserMode::FirmwareDirectory ||
            (selected >= 0 && selected < static_cast<int>(entries.size()) &&
             !entries[static_cast<std::size_t>(selected)].directory);
        draw_button(renderer, font, accept_button,
            mode == BrowserMode::FirmwareDirectory ? "USE THIS FOLDER" : "SELECT ROM",
            can_accept, dir_has_fw, inside(mx, my, accept_button));
        draw_button(renderer, font, cancel_button, "CANCEL", true, false, inside(mx, my, cancel_button));
        present_frame(renderer, vsync_active);
    }
    return accepted;
}

bool select_path(SDL_Window *window, SDL_Renderer *renderer, const FontAtlas& font,
                 bool vsync_active, BrowserMode mode,
                 const std::string& initial_folder, std::string& result,
                 bool& quit_requested, bool& used_internal_browser)
{
    used_internal_browser = false;
    const NativePickResult native = native_file_dialog(
        window, renderer, font, vsync_active, mode, initial_folder, result, quit_requested);
    if (native == NativePickResult::Selected) return true;
    if (native == NativePickResult::Cancelled) return false;
    used_internal_browser = true;
    return run_browser(window, renderer, font, vsync_active, mode,
                       initial_folder, result, quit_requested);
}

std::string key_label(int key)
{
    if (key == SDLK_UNKNOWN) return "UNASSIGNED";
    const char *name = SDL_GetKeyName(static_cast<SDL_Keycode>(key));
    return name && *name ? name : "UNKNOWN";
}

bool run_controls(SDL_Window *window, SDL_Renderer *renderer, const FontAtlas& font,
                  bool vsync_active, GcInputBindings& bindings, bool& quit_requested,
                  std::string& status)
{
    bool done = false;
    int waiting_for = -1;
    constexpr int row_height = 32;
    constexpr int first_y = 104;
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    while (!done && !quit_requested) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        const SDL_Rect defaults_button{48, h - 68, 220, 44};
        const SDL_Rect done_button{w - 190, h - 68, 142, 44};
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit_requested = true; done = true; break; }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (waiting_for >= 0) {
                    if (key == SDLK_ESCAPE) {
                        waiting_for = -1;
                        status = "KEY CHANGE CANCELLED.";
                    } else if (key == SDLK_UNKNOWN ||
                               key_is_used(bindings, static_cast<int>(key),
                                           static_cast<unsigned>(waiting_for))) {
                        status = "THAT KEY IS ALREADY ASSIGNED. PRESS A DIFFERENT KEY.";
                    } else {
                        bindings.keycodes[static_cast<std::size_t>(waiting_for)] = static_cast<int>(key);
                        waiting_for = -1;
                        status = gc_save_input_bindings(bindings)
                            ? "CONTROL BINDING SAVED."
                            : "BINDING ACTIVE, BUT COULD NOT SAVE CONFIGURATION.";
                    }
                } else if (key == SDLK_ESCAPE) {
                    done = true;
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int mx = 0, my = 0;
                map_mouse_coords(window, w, h, event.button.x, event.button.y, mx, my);
                if (inside(mx, my, done_button)) done = true;
                else if (inside(mx, my, defaults_button)) {
                    bindings = gc_default_input_bindings();
                    status = gc_save_input_bindings(bindings)
                        ? "DEFAULT CONTROLS RESTORED AND SAVED."
                        : "DEFAULTS ACTIVE, BUT COULD NOT SAVE CONFIGURATION.";
                    waiting_for = -1;
                } else {
                    for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
                        const SDL_Rect change_button{w - 232,
                            first_y + static_cast<int>(i) * row_height + 2, 176, 28};
                        if (inside(mx, my, change_button)) {
                            waiting_for = static_cast<int>(i);
                            status = "PRESS A KEY FOR " + std::string(action_names[i]) +
                                     ". ESC CANCELS THIS CHANGE.";
                            break;
                        }
                    }
                }
            }
        }
        if (done || quit_requested) break;

        int mx = 0, my = 0;
        map_mouse_coords(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        font.draw(48, 28, "CONTROL SETTINGS", accent, 4, 60);
        font.draw(52, 74, "SELECT A BUTTON TO REBIND. IN-GAME HOTKEYS: F5/F6 SPEED, F7 LCD HZ, F8 FAST-FWD, ESC QUIT.", muted, 1, 140);
        const SDL_Rect panel_rect{40, 96, w - 80, row_height * static_cast<int>(GC_CONTROL_COUNT) + 16};
        fill(renderer, panel_rect, panel);
        outline(renderer, panel_rect, blue);
        for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
            const int y = first_y + static_cast<int>(i) * row_height;
            const SDL_Rect row{52, y, w - 104, row_height - 2};
            const SDL_Rect change_button{w - 232, y + 2, 176, 28};
            if (waiting_for == static_cast<int>(i)) fill(renderer, row, panel_light);
            font.draw(72, y + 8, action_names[i],
                      waiting_for == static_cast<int>(i) ? accent : ink, 2, 36);
            draw_button(renderer, font, change_button,
                waiting_for == static_cast<int>(i) ? "PRESS A KEY" : key_label(bindings.keycodes[i]),
                true, waiting_for == static_cast<int>(i), inside(mx, my, change_button));
        }
        font.draw(52, first_y + row_height * static_cast<int>(GC_CONTROL_COUNT) + 22,
                  status,
                  status.find("ALREADY") != std::string::npos || status.find("COULD NOT") != std::string::npos
                      ? warning : muted, 1, 140);
        draw_button(renderer, font, defaults_button, "RESET DEFAULTS", true, false, inside(mx, my, defaults_button));
        draw_button(renderer, font, done_button, "DONE", true, false, inside(mx, my, done_button));
        present_frame(renderer, vsync_active);
    }
    return !quit_requested;
}
} // namespace

GcInputBindings gc_default_input_bindings()
{
    GcInputBindings bindings;
    bindings.keycodes = {
        SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
        SDLK_a, SDLK_b, SDLK_c, SDLK_d,
        SDLK_m, SDLK_p, SDLK_s, SDLK_n
    };
    return bindings;
}

GcInputBindings gc_load_input_bindings()
{
    const GcInputBindings defaults = gc_default_input_bindings();
    std::array<int, GC_CONTROL_COUNT> stored{};
    stored.fill(SDLK_UNKNOWN);
    std::ifstream input(bindings_file());
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string name = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        trim_string(name);
        trim_string(value);
        for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
            if (name != action_keys[i]) continue;
            const SDL_Keycode key = SDL_GetKeyFromName(value.c_str());
            if (key != SDLK_UNKNOWN && key != SDLK_ESCAPE)
                stored[i] = static_cast<int>(key);
            break;
        }
    }

    GcInputBindings result;
    result.keycodes.fill(SDLK_UNKNOWN);
    for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
        const int key = stored[i];
        if (key == SDLK_UNKNOWN || key_is_used(result, key, i)) continue;
        result.keycodes[i] = key;
    }
    for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
        if (result.keycodes[i] != SDLK_UNKNOWN) continue;
        if (!key_is_used(result, defaults.keycodes[i], i)) {
            result.keycodes[i] = defaults.keycodes[i];
            continue;
        }
        for (int key = SDLK_F1; key <= SDLK_F12; ++key) {
            if (!key_is_used(result, key, i)) {
                result.keycodes[i] = key;
                break;
            }
        }
    }
    return result;
}

bool gc_save_input_bindings(const GcInputBindings& bindings)
{
    std::error_code ec;
    const fs::path path = bindings_file();
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "# Game.com emulator keyboard bindings; edit in the launcher.\n";
    for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
        output << action_keys[i] << '='
               << SDL_GetKeyName(static_cast<SDL_Keycode>(bindings.keycodes[i])) << '\n';
    }
    return output.good();
}

GcLaunchSelection gc_load_launch_selection()
{
    GcLaunchSelection selection;
    std::ifstream input(launcher_state_file());
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string name = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        trim_string(name);
        trim_string(value);
        if (name == "firmware_dir") selection.firmware_dir = value;
        else if (name == "cartridge_path") selection.cartridge_path = value;
        else if (name == "lcd_hz") {
            char *end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            if (end && end != value.c_str())
                selection.timing.lcd_hz = clamp_lcd_hz(parsed);
        } else if (name == "cpu_speed_percent") {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (end && end != value.c_str())
                selection.timing.cpu_speed_percent = clamp_cpu_speed(static_cast<unsigned>(parsed));
        }
    }
    if (!selection.cartridge_path.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(selection.cartridge_path, ec) || ec)
            selection.cartridge_path.clear();
    }
    if (selection.firmware_dir.empty() && valid_firmware("firmware")) {
        std::error_code ec;
        const fs::path local_fw = fs::absolute("firmware", ec);
        selection.firmware_dir = ec ? std::string("firmware") : local_fw.string();
    }
    return selection;
}

bool gc_save_launch_selection(const GcLaunchSelection& selection)
{
    std::error_code ec;
    const fs::path path = launcher_state_file();
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "# Game.com launcher paths and decoupled clock settings.\n";
    output << "firmware_dir=" << selection.firmware_dir << '\n';
    output << "cartridge_path=" << selection.cartridge_path << '\n';
    output << "lcd_hz=" << selection.timing.lcd_hz << '\n';
    output << "cpu_speed_percent=" << selection.timing.cpu_speed_percent << '\n';
    return output.good();
}

bool gc_sdl_launcher_run(GcLaunchSelection& selection, GcInputBindings& bindings)
{
    const bool owns_sdl = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (owns_sdl && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return false;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *window = SDL_CreateWindow("Game.com Emulator — Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 680,
        SDL_WINDOW_RESIZABLE);
    if (!window) {
        if (owns_sdl) SDL_Quit();
        return false;
    }
    SDL_SetWindowMinimumSize(window, 820, 640);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        if (owns_sdl) SDL_Quit();
        return false;
    }

    SDL_RendererInfo rinfo{};
    const bool vsync_active = (SDL_GetRendererInfo(renderer, &rinfo) == 0) &&
                              ((rinfo.flags & SDL_RENDERER_PRESENTVSYNC) != 0);
    FontAtlas font(renderer);

    if (selection.firmware_dir.empty() && selection.cartridge_path.empty())
        selection = gc_load_launch_selection();
    selection.timing.lcd_hz = clamp_lcd_hz(selection.timing.lcd_hz);
    selection.timing.cpu_speed_percent = clamp_cpu_speed(selection.timing.cpu_speed_percent);

    bool done = false, quit = false, started = false;
    bool choose_firmware = false, choose_rom = false, choose_controls = false;
    bool force_builtin_picker = false;
    std::string status = valid_firmware(selection.firmware_dir)
        ? "FIRMWARE READY. CHOOSE A CARTRIDGE OR CLICK BOOT BIOS / BOOT GAME."
        : "BROWSE OR DRAG-AND-DROP YOUR FIRMWARE FOLDER AND OPTIONAL GAME ROM.";
    int focus = 0;
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    while (!done && !quit) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        const SDL_Rect controls_button{w - 220, 34, 172, 44};
        const SDL_Rect firmware_button{w - 220, 146, 172, 42};
        const SDL_Rect rom_clear_button{w - 340, 268, 108, 42};
        const SDL_Rect rom_button{w - 220, 268, 172, 42};

        const int half_w = (w - 96) / 2;
        const SDL_Rect lcd_minus{60 + half_w - 270, 404, 44, 38};
        const SDL_Rect lcd_value_box{60 + half_w - 220, 404, 168, 38};
        const SDL_Rect lcd_plus{60 + half_w - 46, 404, 44, 38};

        const int right_x = 56 + half_w;
        const SDL_Rect cpu_minus{right_x + half_w - 294, 404, 44, 38};
        const SDL_Rect cpu_value_box{right_x + half_w - 244, 404, 192, 38};
        const SDL_Rect cpu_plus{right_x + half_w - 46, 404, 44, 38};

        const SDL_Rect bios_button{48, h - 98, 224, 52};
        const SDL_Rect game_button{292, h - 98, 244, 52};
        const SDL_Rect quit_button{w - 190, h - 98, 142, 52};

        const bool firmware_ok = valid_firmware(selection.firmware_dir);
        const bool rom_ok = !selection.cartridge_path.empty();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit = true; break; }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }
            if (event.type == SDL_DROPFILE && event.drop.file) {
                const std::string dropped(event.drop.file);
                SDL_free(event.drop.file);
                std::error_code ec;
                const fs::path p(dropped);
                if (fs::is_directory(p, ec) && !ec) {
                    selection.firmware_dir = p.string();
                    (void)gc_save_launch_selection(selection);
                    status = valid_firmware(selection.firmware_dir)
                        ? "DROPPED FIRMWARE FOLDER DETECTED AND SAVED."
                        : "FOLDER SET, BUT INTERNAL.BIN AND/OR EXTERNAL.BIN IS MISSING.";
                } else if (fs::is_regular_file(p, ec) && !ec) {
                    const std::string fname = p.filename().string();
                    if (fname == "internal.bin" || fname == "external.bin") {
                        selection.firmware_dir = p.parent_path().string();
                        (void)gc_save_launch_selection(selection);
                        status = valid_firmware(selection.firmware_dir)
                            ? "FIRMWARE FOLDER DETECTED FROM DROPPED BIOS FILE."
                            : "FIRMWARE PARTIAL: BOTH INTERNAL.BIN AND EXTERNAL.BIN ARE NEEDED.";
                    } else {
                        selection.cartridge_path = p.string();
                        (void)gc_save_launch_selection(selection);
                        status = "CARTRIDGE ROM SET VIA DRAG AND DROP.";
                    }
                }
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                const bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                if (key == SDLK_ESCAPE) done = true;
                else if (key == SDLK_1) { choose_firmware = true; force_builtin_picker = shift; }
                else if (key == SDLK_2) { choose_rom = true; force_builtin_picker = shift; }
                else if (key == SDLK_3) choose_controls = true;
                else if (key == SDLK_F7) {
                    selection.timing.lcd_hz = step_lcd_hz(selection.timing.lcd_hz, shift ? -1 : 1);
                    if (selection.timing.lcd_hz >= 120.0 && !shift)
                        selection.timing.lcd_hz = lcd_hz_presets.front();
                    (void)gc_save_launch_selection(selection);
                    status = "LCD VBLANK RATE SET TO " + format_lcd_hz(selection.timing.lcd_hz) + ".";
                } else if (key == SDLK_F5) {
                    selection.timing.cpu_speed_percent = step_cpu_speed(selection.timing.cpu_speed_percent, -1);
                    (void)gc_save_launch_selection(selection);
                    status = "CPU CLOCK SET TO " + format_cpu_speed(selection.timing.cpu_speed_percent) + ".";
                } else if (key == SDLK_F6) {
                    selection.timing.cpu_speed_percent = step_cpu_speed(selection.timing.cpu_speed_percent, 1);
                    (void)gc_save_launch_selection(selection);
                    status = "CPU CLOCK SET TO " + format_cpu_speed(selection.timing.cpu_speed_percent) + ".";
                } else if (key == SDLK_TAB || key == SDLK_RIGHT) focus = (focus + 1) % 6;
                else if (key == SDLK_LEFT) focus = (focus + 5) % 6;
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    if (focus == 0) { choose_firmware = true; force_builtin_picker = shift; }
                    else if (focus == 1) { choose_rom = true; force_builtin_picker = shift; }
                    else if (focus == 2) choose_controls = true;
                    else if (focus == 3 && firmware_ok) started = true;
                    else if (focus == 4 && firmware_ok && rom_ok) started = true;
                    else if (focus == 5) done = true;
                    if (started) done = true;
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int mx = 0, my = 0;
                map_mouse_coords(window, w, h, event.button.x, event.button.y, mx, my);
                const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
                if (inside(mx, my, controls_button)) { choose_controls = true; focus = 2; }
                else if (inside(mx, my, firmware_button)) {
                    choose_firmware = true; force_builtin_picker = shift; focus = 0;
                } else if (inside(mx, my, rom_button)) {
                    choose_rom = true; force_builtin_picker = shift; focus = 1;
                } else if (rom_ok && inside(mx, my, rom_clear_button)) {
                    selection.cartridge_path.clear();
                    (void)gc_save_launch_selection(selection);
                    status = "CARTRIDGE CLEARED (BIOS MENU ONLY).";
                } else if (inside(mx, my, lcd_minus)) {
                    selection.timing.lcd_hz = step_lcd_hz(selection.timing.lcd_hz, -1);
                    (void)gc_save_launch_selection(selection);
                    status = "LCD VBLANK RATE SET TO " + format_lcd_hz(selection.timing.lcd_hz) + ".";
                } else if (inside(mx, my, lcd_plus) || inside(mx, my, lcd_value_box)) {
                    const double prev = selection.timing.lcd_hz;
                    selection.timing.lcd_hz = step_lcd_hz(selection.timing.lcd_hz, 1);
                    if (inside(mx, my, lcd_value_box) && std::fabs(selection.timing.lcd_hz - prev) < 1e-6)
                        selection.timing.lcd_hz = lcd_hz_presets.front();
                    (void)gc_save_launch_selection(selection);
                    status = "LCD VBLANK RATE SET TO " + format_lcd_hz(selection.timing.lcd_hz) + ".";
                } else if (inside(mx, my, cpu_minus)) {
                    selection.timing.cpu_speed_percent = step_cpu_speed(selection.timing.cpu_speed_percent, -1);
                    (void)gc_save_launch_selection(selection);
                    status = "CPU CLOCK SET TO " + format_cpu_speed(selection.timing.cpu_speed_percent) + ".";
                } else if (inside(mx, my, cpu_plus) || inside(mx, my, cpu_value_box)) {
                    const unsigned prev = selection.timing.cpu_speed_percent;
                    selection.timing.cpu_speed_percent = step_cpu_speed(selection.timing.cpu_speed_percent, 1);
                    if (inside(mx, my, cpu_value_box) && selection.timing.cpu_speed_percent == prev)
                        selection.timing.cpu_speed_percent = cpu_speed_presets.front();
                    (void)gc_save_launch_selection(selection);
                    status = "CPU CLOCK SET TO " + format_cpu_speed(selection.timing.cpu_speed_percent) + ".";
                } else if (inside(mx, my, bios_button) && firmware_ok) {
                    selection.cartridge_path.clear();
                    started = true; done = true;
                } else if (inside(mx, my, game_button) && firmware_ok && rom_ok) {
                    started = true; done = true;
                } else if (inside(mx, my, quit_button)) {
                    done = true;
                }
            }
        }
        if (done || quit) break;

        if (choose_firmware) {
            choose_firmware = false;
            const std::string initial = selection.firmware_dir.empty()
                ? home_directory().string() : selection.firmware_dir;
            std::string path;
            bool used_fallback = force_builtin_picker;
            const bool picked = force_builtin_picker
                ? run_browser(window, renderer, font, vsync_active,
                              BrowserMode::FirmwareDirectory, initial, path, quit)
                : select_path(window, renderer, font, vsync_active,
                              BrowserMode::FirmwareDirectory, initial, path, quit, used_fallback);
            force_builtin_picker = false;
            if (picked) {
                selection.firmware_dir = path;
                (void)gc_save_launch_selection(selection);
                status = valid_firmware(path)
                    ? "FIRMWARE FOUND. YOU CAN BOOT THE BIOS OR LOAD A GAME."
                    : "FOLDER SELECTED, BUT INTERNAL.BIN AND/OR EXTERNAL.BIN IS MISSING.";
                if (used_fallback) status = "IN-APP PICKER: " + status;
            }
        }
        if (choose_rom) {
            choose_rom = false;
            std::string initial = selection.firmware_dir.empty()
                ? home_directory().string() : selection.firmware_dir;
            if (!selection.cartridge_path.empty())
                initial = fs::path(selection.cartridge_path).parent_path().string();
            std::string path;
            bool used_fallback = force_builtin_picker;
            const bool picked = force_builtin_picker
                ? run_browser(window, renderer, font, vsync_active,
                              BrowserMode::CartridgeFile, initial, path, quit)
                : select_path(window, renderer, font, vsync_active,
                              BrowserMode::CartridgeFile, initial, path, quit, used_fallback);
            force_builtin_picker = false;
            if (picked) {
                selection.cartridge_path = path;
                (void)gc_save_launch_selection(selection);
                status = "GAME ROM SELECTED AND SAVED.";
                if (used_fallback) status = "IN-APP PICKER: " + status;
            }
        }
        if (choose_controls) {
            choose_controls = false;
            status = "CONTROLS SAVE AUTOMATICALLY TO YOUR USER CONFIGURATION.";
            (void)run_controls(window, renderer, font, vsync_active, bindings, quit, status);
            focus = 2;
        }
        if (quit) break;

        int mx = 0, my = 0;
        map_mouse_coords(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        font.draw(48, 34, "GAME.COM EMULATOR", accent, 4, 60);
        draw_button(renderer, font, controls_button, "CONTROLS", true, focus == 2, inside(mx, my, controls_button));
        font.draw(50, 78, "SELECT FIRMWARE, OPTIONAL CARTRIDGE, AND INDEPENDENT CLOCK RATES", muted, 2, 74);

        const SDL_Rect fw_panel{40, 114, w - 80, 104};
        const SDL_Rect rom_panel{40, 236, w - 80, 104};
        const SDL_Rect timing_panel{40, 358, w - 80, 102};
        fill(renderer, fw_panel, panel); outline(renderer, fw_panel, blue);
        fill(renderer, rom_panel, panel); outline(renderer, rom_panel, blue);
        fill(renderer, timing_panel, panel); outline(renderer, timing_panel, blue);

        font.draw(60, 130, "1  SYSTEM FIRMWARE (INTERNAL.BIN + EXTERNAL.BIN)", ink, 2, 54);
        font.draw(60, 162,
                  selection.firmware_dir.empty() ? "NO FIRMWARE FOLDER SELECTED" : short_path(selection.firmware_dir, 54),
                  firmware_ok ? accent : warning, 2, 56);
        font.draw(60, 192, "TIP: HOLD SHIFT + CLICK BROWSE TO FORCE THE IN-APP BROWSER.", muted, 1, 80);
        draw_button(renderer, font, firmware_button, "BROWSE...", true, focus == 0, inside(mx, my, firmware_button));

        font.draw(60, 252, "2  GAME CARTRIDGE (.BIN, OPTIONAL)", ink, 2, 46);
        font.draw(60, 284,
                  selection.cartridge_path.empty() ? "NO GAME SELECTED (BOOT BIOS MENU)" : short_path(selection.cartridge_path, 44),
                  rom_ok ? accent : muted, 2, 46);
        font.draw(60, 314, "YOU CAN ALSO DRAG AND DROP A FOLDER OR ROM INTO THIS WINDOW.", muted, 1, 80);
        if (rom_ok) {
            draw_button(renderer, font, rom_clear_button, "CLEAR", true, false, inside(mx, my, rom_clear_button));
        }
        draw_button(renderer, font, rom_button, "BROWSE...", true, focus == 1, inside(mx, my, rom_button));

        font.draw(60, 372, "3  DECOUPLED TIMING (MONITOR VSYNC IS SEPARATE)", ink, 2, 58);
        font.draw(60, 416, "LCD HZ", muted, 2, 10);
        draw_button(renderer, font, lcd_minus, "<", true, false, inside(mx, my, lcd_minus));
        draw_button(renderer, font, lcd_value_box, format_lcd_hz(selection.timing.lcd_hz),
                    true, false, inside(mx, my, lcd_value_box));
        draw_button(renderer, font, lcd_plus, ">", true, false, inside(mx, my, lcd_plus));

        font.draw(right_x, 416, "CPU", muted, 2, 8);
        draw_button(renderer, font, cpu_minus, "<", true, false, inside(mx, my, cpu_minus));
        draw_button(renderer, font, cpu_value_box, format_cpu_speed(selection.timing.cpu_speed_percent),
                    true, false, inside(mx, my, cpu_value_box));
        draw_button(renderer, font, cpu_plus, ">", true, false, inside(mx, my, cpu_plus));

        font.draw(50, 480, status, status.find("MISSING") != std::string::npos ? warning : muted, 2, 74);
        font.draw(50, 510,
                  "IN-GAME HOTKEYS: F5/F6 CPU SPEED   F7 LCD HZ   HOLD F8 FAST-FORWARD (4X)   ESC QUIT",
                  muted, 1, 120);

        draw_button(renderer, font, bios_button, "BOOT BIOS", firmware_ok, focus == 3, inside(mx, my, bios_button));
        draw_button(renderer, font, game_button, "BOOT GAME", firmware_ok && rom_ok, focus == 4, inside(mx, my, game_button));
        draw_button(renderer, font, quit_button, "QUIT", true, focus == 5, inside(mx, my, quit_button));
        font.draw(50, h - 30, "KEYS: 1 FIRMWARE  2 GAME  3 CONTROLS  F5/F6 CPU  F7 LCD HZ  TAB CYCLE  ENTER BOOT", muted, 1, 130);
        present_frame(renderer, vsync_active);
    }

    if (started && !quit) (void)gc_save_launch_selection(selection);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (owns_sdl) SDL_Quit();
    return started && !quit;
}
