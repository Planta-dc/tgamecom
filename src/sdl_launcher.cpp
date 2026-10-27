#include "gamecom.h"
#include "sdl_ui.hpp"

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
using namespace gc_ui_palette;

const std::array<const char *, GC_CONTROL_COUNT> action_names = {
    "UP", "DOWN", "LEFT", "RIGHT", "A BUTTON", "B BUTTON", "C BUTTON", "D BUTTON",
    "MENU", "PAUSE", "SOUND", "POWER"
};
const std::array<const char *, GC_CONTROL_COUNT> action_keys = {
    "up", "down", "left", "right", "a", "b", "c", "d",
    "menu", "pause", "sound", "power"
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
    case '#': return {10,10,31,10,31,10,10}; case '|': return {4,4,4,4,4,4,4};
    case '\'': return {4,4,8,0,0,0,0}; case '!': return {4,4,4,4,4,0,4};
    case '?': return {14,17,1,2,4,0,4}; case '*': return {0,17,10,31,10,17,0};
    case ' ': return {0,0,0,0,0,0,0};
    default: return {14,17,1,2,4,0,4};
    }
}

fs::path home_dir_path()
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
    return home_dir_path() / ".config" / "gamecom-emulator";
}

fs::path bindings_file()
{
    return config_directory() / "controls.ini";
}

fs::path launcher_state_file()
{
    return config_directory() / "launcher.ini";
}

fs::path default_nvram_file()
{
    return config_directory() / "nvram.sav";
}

void trim_string(std::string& s)
{
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s.clear(); return; }
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
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

enum class BrowserMode { FirmwareDirectory, CartridgeFile };
struct BrowserEntry { fs::path path; std::string name; bool directory; };
enum class NativePickResult { Unavailable, Cancelled, Selected, UseBuiltin };

void draw_dialog_wait_overlay(SDL_Window *window, SDL_Renderer *renderer,
                              const GcUiFont& font, BrowserMode mode,
                              SDL_Rect& builtin_btn, SDL_Rect& cancel_btn,
                              int mouse_x, int mouse_y)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
    SDL_RenderClear(renderer);
    gc_ui_fill(renderer, SDL_Rect{0, 0, w, h}, overlay_dim);

    SDL_Rect modal{(w - 660) / 2, (h - 240) / 2, 660, 240};
    if (modal.x < 16) { modal.x = 16; modal.w = w - 32; }
    if (modal.y < 16) { modal.y = 16; modal.h = h - 32; }
    gc_ui_fill(renderer, modal, panel);
    gc_ui_outline(renderer, modal, accent);

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
    gc_ui_map_mouse(window, w, h, mouse_x, mouse_y, mx, my);
    gc_ui_draw_button(renderer, font, builtin_btn, "IN-APP BROWSER", true, false,
                      gc_ui_inside(mx, my, builtin_btn));
    gc_ui_draw_button(renderer, font, cancel_btn, "CANCEL (ESC)", true, false,
                      gc_ui_inside(mx, my, cancel_btn));
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
                                        const GcUiFont& font, bool vsync_active,
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
    if (start_dir.empty()) start_dir = home_dir_path().string();
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
                gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                if (gc_ui_inside(mx, my, cancel_btn)) user_cancelled = true;
                else if (gc_ui_inside(mx, my, builtin_btn)) switch_to_builtin = true;
            }
        }
        if (user_cancelled || switch_to_builtin) {
            terminate_child(pid);
            ::close(pipefd[0]);
            return switch_to_builtin ? NativePickResult::UseBuiltin : NativePickResult::Cancelled;
        }

        draw_dialog_wait_overlay(window, renderer, font, mode,
                                 builtin_btn, cancel_btn, mouse_x, mouse_y);
        gc_ui_present(renderer, vsync_active);
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
                                    const GcUiFont& font, bool vsync_active,
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
                        gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                        if (gc_ui_inside(mx, my, cancel_btn)) state.finished = true;
                        else if (gc_ui_inside(mx, my, builtin_btn)) {
                            switch_to_builtin = true;
                            state.finished = true;
                        }
                    }
                }
                if (state.finished) break;
                draw_dialog_wait_overlay(window, renderer, font, mode,
                                         builtin_btn, cancel_btn, mouse_x, mouse_y);
                gc_ui_present(renderer, vsync_active);
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

bool run_browser(SDL_Window *window, SDL_Renderer *renderer, const GcUiFont& font,
                 bool vsync_active, BrowserMode mode,
                 const std::string& initial_folder, std::string& result,
                 bool& quit_requested)
{
    fs::path current = initial_folder.empty() ? home_dir_path() : fs::path(initial_folder);
    std::error_code initial_ec;
    if (!fs::is_directory(current, initial_ec) || initial_ec) current = home_dir_path();
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
                gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                if (gc_ui_inside(mx, my, cancel_button)) done = true;
                else if (gc_ui_inside(mx, my, up_button)) {
                    if (current.has_parent_path() && current != current.root_path()) {
                        current = current.parent_path();
                        refresh();
                    }
                } else if (gc_ui_inside(mx, my, home_button)) {
                    current = home_dir_path();
                    refresh();
                } else if (mode == BrowserMode::CartridgeFile && gc_ui_inside(mx, my, filter_button)) {
                    show_all_files = !show_all_files;
                    refresh();
                } else if (gc_ui_inside(mx, my, accept_button)) {
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
                } else if (gc_ui_inside(mx, my, list)) {
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
        gc_ui_map_mouse(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        gc_ui_fill(renderer, modal, panel);
        gc_ui_outline(renderer, modal, blue);
        font.draw(modal.x + 24, modal.y + 22,
                  mode == BrowserMode::FirmwareDirectory ? "CHOOSE FIRMWARE FOLDER" : "CHOOSE GAME ROM",
                  accent, 3, 26);
        gc_ui_draw_button(renderer, font, up_button, "UP", true, false, gc_ui_inside(mx, my, up_button));
        gc_ui_draw_button(renderer, font, home_button, "HOME", true, false, gc_ui_inside(mx, my, home_button));
        if (mode == BrowserMode::CartridgeFile) {
            gc_ui_draw_button(renderer, font, filter_button, show_all_files ? "ALL" : ".BIN",
                              true, show_all_files, gc_ui_inside(mx, my, filter_button));
        }

        const bool dir_has_fw = mode == BrowserMode::FirmwareDirectory && gc_ui_valid_firmware(current.string());
        font.draw(modal.x + 24, modal.y + 66, gc_ui_short_path(current.string(), 62),
                  dir_has_fw ? accent : muted, 2, 62);
        gc_ui_fill(renderer, list, background);
        gc_ui_outline(renderer, list, panel_light);
        for (int row = 0; row < visible_rows; ++row) {
            const int index = first_visible + row;
            if (index >= static_cast<int>(entries.size())) break;
            const BrowserEntry& entry = entries[static_cast<std::size_t>(index)];
            const SDL_Rect item{list.x + 2, list.y + row * row_height + 1, list.w - 4, row_height - 2};
            const bool chosen = index == selected;
            const bool hovered = gc_ui_inside(mx, my, item);
            if (chosen) gc_ui_fill(renderer, item, panel_light);
            else if (hovered) gc_ui_fill(renderer, item, GcUiColor{20, 33, 48, 255});
            const std::string label = entry.directory ? "[DIR]  " + entry.name : "[ROM]  " + entry.name;
            font.draw(item.x + 8, item.y + 6, label, chosen ? accent : ink, 2,
                      std::max(1, (item.w - 16) / 12));
        }
        if (!scan_error.empty())
            font.draw(modal.x + 24, modal.y + modal.h - 114, scan_error, warning, 2, 62);
        else if (dir_has_fw)
            font.draw(modal.x + 24, modal.y + modal.h - 114,
                      "EXTERNAL.BIN DETECTED IN THIS FOLDER (INTERNAL.BIN OPTIONAL)!", accent, 2, 62);

        const std::string hint = mode == BrowserMode::FirmwareDirectory
            ? "WHEEL/ARROWS SCROLL  DOUBLE-CLICK/ENTER OPENS  BACKSPACE UP  S SELECTS FOLDER"
            : "WHEEL/ARROWS SCROLL  DOUBLE-CLICK/ENTER OPENS ROM OR FOLDER  BACKSPACE UP";
        font.draw(modal.x + 24, modal.y + modal.h - 86, hint, muted, 1,
                  std::max(1, (modal.w - 48) / 6));
        const bool can_accept = mode == BrowserMode::FirmwareDirectory ||
            (selected >= 0 && selected < static_cast<int>(entries.size()) &&
             !entries[static_cast<std::size_t>(selected)].directory);
        gc_ui_draw_button(renderer, font, accept_button,
            mode == BrowserMode::FirmwareDirectory ? "USE THIS FOLDER" : "SELECT ROM",
            can_accept, dir_has_fw, gc_ui_inside(mx, my, accept_button));
        gc_ui_draw_button(renderer, font, cancel_button, "CANCEL", true, false,
                          gc_ui_inside(mx, my, cancel_button));
        gc_ui_present(renderer, vsync_active);
    }
    return accepted;
}

std::string key_label(int key)
{
    if (key == SDLK_UNKNOWN) return "UNASSIGNED";
    const char *name = SDL_GetKeyName(static_cast<SDL_Keycode>(key));
    return name && *name ? name : "UNKNOWN";
}

struct TabSpec {
    SDL_Rect rect{};
    std::string label;
    int scale = 2;
};

std::array<TabSpec, 5> build_menubar_tabs(const GcLaunchSelection& sel, int win_w = 800)
{
    const int scale = (win_w > 0 && win_w < 700) ? 1 : 2;
    char lcd_buf[32];
    if (std::fabs(sel.timing.lcd_hz - std::round(sel.timing.lcd_hz)) < 0.05)
        std::snprintf(lcd_buf, sizeof(lcd_buf), "LCD:%.0fHZ", sel.timing.lcd_hz);
    else
        std::snprintf(lcd_buf, sizeof(lcd_buf), "LCD:%.1fHZ", sel.timing.lcd_hz);

    char cpu_buf[36];
    if (sel.timing.game_pace_percent != 100u) {
        std::snprintf(cpu_buf, sizeof(cpu_buf), "CPU:%u%%*%.1fX",
                      sel.timing.cpu_speed_percent,
                      static_cast<double>(sel.timing.game_pace_percent) / 100.0);
    } else {
        std::snprintf(cpu_buf, sizeof(cpu_buf), "CPU:%u%%", sel.timing.cpu_speed_percent);
    }

    std::array<std::string, 5> labels = {
        "SYSTEM", lcd_buf, cpu_buf, "INPUT", "AUDIO/VIEW"
    };
    std::array<TabSpec, 5> tabs{};
    int x = 6;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const int w = static_cast<int>(labels[i].size()) * 6 * scale + (scale == 2 ? 18 : 12);
        tabs[i] = TabSpec{SDL_Rect{x, 2, w, GcTopMenuBar::height - 4}, labels[i], scale};
        x += w + (scale == 2 ? 4 : 3);
    }
    return tabs;
}

enum class ItemOp {
    None = 0,
    Action,
    SetLcdHz,
    StepLcdHz,
    SetCpuSpeed,
    StepCpuSpeed,
    SetPace,
    ResetTiming,
    SetTouchMode,
    ToggleMute,
    SetVolume,
    ToggleIntegerScale,
    ToggleAutoHideBar,
    SetSerialMode,
    ToggleWarmBootSeed
};

struct DropdownItem {
    SDL_Rect rect{};
    std::string label;
    bool divider = false;
    bool enabled = true;
    bool marked = false;
    bool stay_open = false;
    ItemOp op = ItemOp::None;
    GcMenuAction action = GcMenuAction::None;
    double value = 0.0;
};

struct DropdownLayout {
    SDL_Rect panel_rect{};
    bool has_stepper = false;
    SDL_Rect step_m2{}, step_m1{}, step_val{}, step_p1{}, step_p2{};
    std::string step_m2_lbl, step_m1_lbl, step_val_lbl, step_p1_lbl, step_p2_lbl;
    ItemOp step_op = ItemOp::None;
    double step_d2 = 0.0, step_d1 = 0.0;
    GcMenuAction center_action = GcMenuAction::None;
    std::vector<DropdownItem> items;
    int text_scale = 2;
};

DropdownLayout build_dropdown_layout(int tab, int win_w, int win_h,
                                     const GcLaunchSelection& sel,
                                     bool in_launcher, bool emulation_ready,
                                     bool paused, bool is_fullscreen)
{
    DropdownLayout out{};
    const auto tabs = build_menubar_tabs(sel, win_w);
    if (tab < 0 || tab >= static_cast<int>(tabs.size())) return out;

    const bool compact = (win_h < 500) || (tab == 0 && win_h < 640);
    const int row_h = compact ? 20 : 25;
    const int div_h = compact ? 5 : 7;
    out.text_scale = compact ? 1 : 2;
    const int panel_w = std::min(win_w - 12, 392);
    int panel_x = tabs[static_cast<std::size_t>(tab)].rect.x;
    if (panel_x + panel_w > win_w - 6) panel_x = std::max(6, win_w - panel_w - 6);

    int y = GcTopMenuBar::height + 6;
    auto add_item = [&](const std::string& lbl, bool en, bool mark, bool keep,
                        ItemOp op, GcMenuAction act = GcMenuAction::None, double val = 0.0) {
        DropdownItem it{};
        it.rect = SDL_Rect{panel_x + 6, y, panel_w - 12, row_h};
        it.label = lbl;
        it.enabled = en;
        it.marked = mark;
        it.stay_open = keep;
        it.op = op;
        it.action = act;
        it.value = val;
        out.items.push_back(it);
        y += row_h + 1;
    };
    auto add_div = [&]() {
        DropdownItem it{};
        it.rect = SDL_Rect{panel_x + 10, y + div_h / 2, panel_w - 20, 1};
        it.divider = true;
        out.items.push_back(it);
        y += div_h;
    };

    const bool fw_ok = gc_ui_valid_firmware(sel.firmware_dir);

    if (tab == 0) {
        add_item("LOAD SLOT 1 ROM...", true, false, false,
                 ItemOp::Action, GcMenuAction::LoadCartridgeRom);
        add_item(sel.cartridge2_path.empty() ? "LOAD SLOT 2 ROM..." : "CHANGE SLOT 2 ROM...",
                 true, !sel.cartridge2_path.empty(), false,
                 ItemOp::Action, GcMenuAction::LoadSlot2Rom);
        if (!sel.cartridge2_path.empty()) {
            add_item("EJECT SLOT 2 ROM", true, false, false,
                     ItemOp::Action, GcMenuAction::EjectSlot2Rom);
        }
        add_item("SET FIRMWARE FOLDER...", true, false, false,
                 ItemOp::Action, GcMenuAction::SetFirmwareFolder);
        add_item("BOOT BIOS MENU (NO CART)", fw_ok, false, false,
                 ItemOp::Action, GcMenuAction::BootBiosOnly);
        add_div();
        add_item("SAVE 8KB NVRAM NOW", emulation_ready, false, false,
                 ItemOp::Action, GcMenuAction::SaveNvramNow);
        add_item(sel.warm_boot_nvram ? "[X] SEED WARM-BOOT NVRAM" : "[ ] SEED WARM-BOOT NVRAM",
                 true, sel.warm_boot_nvram, true, ItemOp::ToggleWarmBootSeed);
        add_div();
        add_item(sel.serial_mode == GC_SERIAL_OFF ? "(*) SERIAL: OFF" : "( ) SERIAL: OFF",
                 true, sel.serial_mode == GC_SERIAL_OFF, true,
                 ItemOp::SetSerialMode, GcMenuAction::None, 0.0);
        add_item(sel.serial_mode == GC_SERIAL_MODEM
                     ? "(*) SERIAL: MODEM (9600 AT)" : "( ) SERIAL: MODEM (9600 AT)",
                 true, sel.serial_mode == GC_SERIAL_MODEM, true,
                 ItemOp::SetSerialMode, GcMenuAction::None, 1.0);
        add_item(sel.serial_mode == GC_SERIAL_TCP_HOST
                     ? "(*) SERIAL: 2P HOST (TCP)" : "( ) SERIAL: 2P HOST (TCP)",
                 true, sel.serial_mode == GC_SERIAL_TCP_HOST, true,
                 ItemOp::SetSerialMode, GcMenuAction::None, 2.0);
        add_item(sel.serial_mode == GC_SERIAL_TCP_CLIENT
                     ? ("(*) SERIAL: 2P JOIN " + sel.serial_endpoint)
                     : "( ) SERIAL: 2P JOIN (TCP)...",
                 true, sel.serial_mode == GC_SERIAL_TCP_CLIENT, false,
                 ItemOp::Action, GcMenuAction::PromptSerialEndpoint);
        add_div();
        add_item("RESET EMULATION", emulation_ready, false, false,
                 ItemOp::Action, GcMenuAction::ResetEmulation);
        add_item(paused ? "[X] PAUSE EMULATION (F9)" : "[ ] PAUSE EMULATION (F9)",
                 emulation_ready, paused, true,
                 ItemOp::Action, GcMenuAction::TogglePause);
        if (in_launcher) {
            add_item(emulation_ready ? "RETURN TO RUNNING GAME" : "BOOT SELECTED SYSTEM",
                     emulation_ready || fw_ok, false, false,
                     ItemOp::Action,
                     emulation_ready ? GcMenuAction::ToggleLauncherView : GcMenuAction::BootSelected);
        } else {
            add_item("OPEN LAUNCHER SCREEN", true, false, false,
                     ItemOp::Action, GcMenuAction::ToggleLauncherView);
        }
        add_div();
        add_item("QUIT EMULATOR", true, false, false,
                 ItemOp::Action, GcMenuAction::Quit);
    } else if (tab == 1) {
        out.has_stepper = true;
        const int sh = compact ? 28 : 34;
        const int bx = panel_x + 8;
        const int bw = 48;
        const int cw = panel_w - 16 - 4 * bw - 16;
        out.step_m2 = SDL_Rect{bx, y, bw, sh};
        out.step_m1 = SDL_Rect{bx + bw + 4, y, bw, sh};
        out.step_val = SDL_Rect{bx + 2 * (bw + 4), y, cw, sh};
        out.step_p1 = SDL_Rect{bx + 2 * (bw + 4) + cw + 4, y, bw, sh};
        out.step_p2 = SDL_Rect{bx + 2 * (bw + 4) + cw + 4 + bw + 4, y, bw, sh};
        out.step_m2_lbl = "-5";
        out.step_m1_lbl = "-1";
        out.step_val_lbl = gc_ui_format_lcd_hz(sel.timing.lcd_hz);
        out.step_p1_lbl = "+1";
        out.step_p2_lbl = "+5";
        out.step_op = ItemOp::StepLcdHz;
        out.step_d2 = 5.0;
        out.step_d1 = 1.0;
        out.center_action = GcMenuAction::PromptCustomLcdHz;
        y += sh + 6;
        add_div();

        struct Preset { double hz; const char *name; };
        static constexpr std::array<Preset, 8> presets = {{
            {30.0, "30.0 HZ  (HALF RATE)"},
            {50.0, "50.0 HZ  (PAL)"},
            {59.732155, "59.73 HZ (MAME LEGACY)"},
            {60.0, "60.0 HZ  (TV / VSYNC)"},
            {75.0, "75.0 HZ"},
            {GC_LCD_NATIVE_HZ, "75.48 HZ (HW NATIVE)"},
            {90.0, "90.0 HZ"},
            {120.0, "120.0 HZ"}
        }};
        for (const auto& p : presets) {
            const bool active = std::fabs(sel.timing.lcd_hz - p.hz) < 0.06;
            add_item(std::string(active ? "(*) " : "( ) ") + p.name,
                     true, active, false, ItemOp::SetLcdHz, GcMenuAction::None, p.hz);
        }
        add_div();
        add_item("TYPE EXACT LCD HZ...", true, false, false,
                 ItemOp::Action, GcMenuAction::PromptCustomLcdHz);
    } else if (tab == 2) {
        out.has_stepper = true;
        const int sh = compact ? 28 : 34;
        const int bx = panel_x + 8;
        const int bw = 50;
        const int cw = panel_w - 16 - 4 * bw - 16;
        out.step_m2 = SDL_Rect{bx, y, bw, sh};
        out.step_m1 = SDL_Rect{bx + bw + 4, y, bw, sh};
        out.step_val = SDL_Rect{bx + 2 * (bw + 4), y, cw, sh};
        out.step_p1 = SDL_Rect{bx + 2 * (bw + 4) + cw + 4, y, bw, sh};
        out.step_p2 = SDL_Rect{bx + 2 * (bw + 4) + cw + 4 + bw + 4, y, bw, sh};
        out.step_m2_lbl = "-10";
        out.step_m1_lbl = "-5";
        char cbuf[32];
        std::snprintf(cbuf, sizeof(cbuf), "%u%%", sel.timing.cpu_speed_percent);
        out.step_val_lbl = cbuf;
        out.step_p1_lbl = "+5";
        out.step_p2_lbl = "+10";
        out.step_op = ItemOp::StepCpuSpeed;
        out.step_d2 = 10.0;
        out.step_d1 = 5.0;
        out.center_action = GcMenuAction::PromptCustomCpuSpeed;
        y += sh + 6;
        add_div();

        struct CpuPreset { unsigned pct; const char *name; };
        static constexpr std::array<CpuPreset, 6> cpu_presets = {{
            {75u,  "CPU 75%  (3.75 MHZ)"},
            {100u, "CPU 100% (5.00 MHZ HW)"},
            {120u, "CPU 120% (6.00 MHZ)"},
            {150u, "CPU 150% (7.50 MHZ)"},
            {200u, "CPU 200% (10.00 MHZ)"},
            {300u, "CPU 300% (15.00 MHZ)"}
        }};
        for (const auto& p : cpu_presets) {
            const bool active = sel.timing.cpu_speed_percent == p.pct;
            add_item(std::string(active ? "(*) " : "( ) ") + p.name,
                     true, active, false, ItemOp::SetCpuSpeed, GcMenuAction::None,
                     static_cast<double>(p.pct));
        }
        add_item("TYPE EXACT CPU %...", true, false, false,
                 ItemOp::Action, GcMenuAction::PromptCustomCpuSpeed);
        add_div();

        struct PacePreset { unsigned pct; const char *name; };
        static constexpr std::array<PacePreset, 5> pace_presets = {{
            {50u,  "PACE 0.5X (SLOW-MO)"},
            {100u, "PACE 1.0X (NORMAL)"},
            {150u, "PACE 1.5X (FAST)"},
            {200u, "PACE 2.0X (DOUBLE)"},
            {400u, "PACE 4.0X (TURBO / F8)"}
        }};
        for (const auto& p : pace_presets) {
            const bool active = sel.timing.game_pace_percent == p.pct;
            add_item(std::string(active ? "(*) " : "( ) ") + p.name,
                     true, active, false, ItemOp::SetPace, GcMenuAction::None,
                     static_cast<double>(p.pct));
        }
        add_div();
        add_item("RESET TIMING DEFAULTS", true, false, false, ItemOp::ResetTiming);
    } else if (tab == 3) {
        const GcTouchMode tm = sel.timing.touch_mode;
        add_item(std::string(tm == GC_TOUCH_SINGLE_TAP ? "(*) " : "( ) ") +
                 "TOUCH: SINGLE TAP (1 SCAN)",
                 true, tm == GC_TOUCH_SINGLE_TAP, false,
                 ItemOp::SetTouchMode, GcMenuAction::None, 0.0);
        add_item(std::string(tm == GC_TOUCH_DEBOUNCED_TAP ? "(*) " : "( ) ") +
                 "TOUCH: TAP (2-SCAN DEBOUNCE)",
                 true, tm == GC_TOUCH_DEBOUNCED_TAP, false,
                 ItemOp::SetTouchMode, GcMenuAction::None, 1.0);
        add_item(std::string(tm == GC_TOUCH_HOLD_DRAG ? "(*) " : "( ) ") +
                 "TOUCH: HOLD / DRAG STYLUS",
                 true, tm == GC_TOUCH_HOLD_DRAG, false,
                 ItemOp::SetTouchMode, GcMenuAction::None, 2.0);
        add_div();
        add_item("CONFIGURE KEY CONTROLS...", true, false, false,
                 ItemOp::Action, GcMenuAction::ConfigureControls);
        add_item("RESET DEFAULT CONTROLS", true, false, false,
                 ItemOp::Action, GcMenuAction::ResetControls);
    } else if (tab == 4) {
        const bool sound_on = !sel.timing.audio_muted;
        add_item(sound_on ? "[X] ENABLE AUDIO" : "[ ] ENABLE AUDIO",
                 true, sound_on, true, ItemOp::ToggleMute);
        for (unsigned vol : {25u, 50u, 75u, 100u}) {
            const bool active = !sel.timing.audio_muted && sel.timing.audio_volume_percent == vol;
            char vbuf[32];
            std::snprintf(vbuf, sizeof(vbuf), "%sVOLUME: %u%%", active ? "(*) " : "( ) ", vol);
            add_item(vbuf, true, active, true, ItemOp::SetVolume,
                     GcMenuAction::None, static_cast<double>(vol));
        }
        add_div();
        add_item(sel.timing.integer_scaling ? "[X] INTEGER PIXEL SCALING" : "[ ] INTEGER PIXEL SCALING",
                 true, sel.timing.integer_scaling, true, ItemOp::ToggleIntegerScale);
        add_item(sel.timing.auto_hide_menubar ? "[X] AUTO-HIDE TOP BAR IN GAME" : "[ ] AUTO-HIDE TOP BAR IN GAME",
                 true, sel.timing.auto_hide_menubar, true, ItemOp::ToggleAutoHideBar);
        add_item("WINDOW SIZE: 2X", !is_fullscreen, false, false,
                 ItemOp::Action, GcMenuAction::ResizeWindow2x);
        add_item("WINDOW SIZE: 3X", !is_fullscreen, false, false,
                 ItemOp::Action, GcMenuAction::ResizeWindow3x);
        add_item("WINDOW SIZE: 4X", !is_fullscreen, false, false,
                 ItemOp::Action, GcMenuAction::ResizeWindow4x);
        add_item(is_fullscreen ? "[X] FULLSCREEN (F11)" : "[ ] FULLSCREEN (F11)",
                 true, is_fullscreen, false,
                 ItemOp::Action, GcMenuAction::ToggleFullscreen);
    }

    out.panel_rect = SDL_Rect{
        panel_x, GcTopMenuBar::height, panel_w, (y - GcTopMenuBar::height) + 6
    };
    return out;
}

} // namespace

GcUiFont::GcUiFont(SDL_Renderer *renderer) : m_renderer(renderer)
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

GcUiFont::~GcUiFont()
{
    if (m_texture) SDL_DestroyTexture(m_texture);
}

void GcUiFont::draw(int x, int y, const std::string& value,
                    GcUiColor c, int scale, int max_chars) const
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

void gc_ui_fill(SDL_Renderer *r, SDL_Rect rect, GcUiColor c)
{
    if (c.a < 255) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rect);
    if (c.a < 255) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void gc_ui_outline(SDL_Renderer *r, SDL_Rect rect, GcUiColor c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderDrawRect(r, &rect);
}

void gc_ui_present(SDL_Renderer *r, bool vsync_active)
{
    SDL_RenderPresent(r);
    if (!vsync_active) SDL_Delay(8);
}

bool gc_ui_inside(int x, int y, const SDL_Rect& rect)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

void gc_ui_map_mouse(SDL_Window *window, int render_w, int render_h,
                     int raw_x, int raw_y, int& out_x, int& out_y)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    out_x = (ww > 0 && render_w > 0) ? raw_x * render_w / ww : raw_x;
    out_y = (wh > 0 && render_h > 0) ? raw_y * render_h / wh : raw_y;
}

std::string gc_ui_short_path(const std::string& path, std::size_t max_chars)
{
    if (path.size() <= max_chars) return path;
    if (max_chars < 5) return path.substr(path.size() - max_chars);
    return "..." + path.substr(path.size() - max_chars + 3);
}

std::string gc_ui_home_directory()
{
    return home_dir_path().string();
}

bool gc_ui_valid_firmware(const std::string& directory)
{
    if (directory.empty()) return false;
    std::error_code ec;
    const fs::path base(directory);
    const bool external = fs::is_regular_file(base / "external.bin", ec) && !ec;
    ec.clear();
    const bool boot_alias = fs::is_regular_file(base / "boot.rom", ec) && !ec;
    return external || boot_alias;
}

double gc_ui_clamp_lcd_hz(double hz)
{
    if (!std::isfinite(hz)) return GC_LCD_NATIVE_HZ;
    return std::max(10.0, std::min(240.0, hz));
}

unsigned gc_ui_clamp_cpu_speed(unsigned pct)
{
    return std::max(25u, std::min(800u, pct));
}

unsigned gc_ui_clamp_game_pace(unsigned pct)
{
    return std::max(25u, std::min(400u, pct));
}

std::string gc_ui_format_lcd_hz(double hz)
{
    char buf[48];
    if (std::fabs(hz - GC_LCD_NATIVE_HZ) < 0.01)
        std::snprintf(buf, sizeof(buf), "75.48 HZ");
    else if (std::fabs(hz - 59.732155) < 0.01)
        std::snprintf(buf, sizeof(buf), "59.73 HZ");
    else if (std::fabs(hz - std::round(hz)) < 0.005)
        std::snprintf(buf, sizeof(buf), "%.0f HZ", hz);
    else
        std::snprintf(buf, sizeof(buf), "%.2f HZ", hz);
    return buf;
}

std::string gc_ui_format_cpu_speed(unsigned pct)
{
    char buf[48];
    const double mhz = ((static_cast<double>(GC_CPU_HZ) / 1000000.0) *
                        static_cast<double>(pct)) / 100.0;
    std::snprintf(buf, sizeof(buf), "%u%% (%.2f MHZ)", pct, mhz);
    return buf;
}

void gc_ui_draw_button(SDL_Renderer *r, const GcUiFont& font, const SDL_Rect& rect,
                       const std::string& label, bool enabled,
                       bool highlighted, bool hovered, int text_scale)
{
    const GcUiColor base = !enabled ? disabled
        : (highlighted ? accent : (hovered ? panel_hover : panel_light));
    gc_ui_fill(r, rect, base);
    gc_ui_outline(r, rect, enabled ? (highlighted ? accent : blue) : muted);
    const int scale = std::max(1, text_scale);
    const int max_chars = std::max(1, (rect.w - 10) / (6 * scale));
    const int shown_chars = std::min<int>(static_cast<int>(label.size()), max_chars);
    const int label_w = shown_chars * 6 * scale - scale;
    const int label_h = 7 * scale;
    const int tx = rect.x + std::max(4, (rect.w - label_w) / 2);
    const int ty = rect.y + (rect.h - label_h) / 2;
    font.draw(tx, ty, label, enabled && highlighted ? background : ink, scale, max_chars);
}

bool gc_ui_pick_path(SDL_Window *window, SDL_Renderer *renderer, const GcUiFont& font,
                     bool vsync_active, bool choose_directory,
                     const std::string& initial_folder, std::string& out_path,
                     bool& quit_requested, bool force_builtin, bool& used_builtin)
{
    const BrowserMode mode = choose_directory
        ? BrowserMode::FirmwareDirectory : BrowserMode::CartridgeFile;
    if (!force_builtin) {
        used_builtin = false;
        const NativePickResult native = native_file_dialog(
            window, renderer, font, vsync_active, mode, initial_folder, out_path, quit_requested);
        if (native == NativePickResult::Selected) return true;
        if (native == NativePickResult::Cancelled) return false;
    }
    used_builtin = true;
    return run_browser(window, renderer, font, vsync_active, mode,
                       initial_folder, out_path, quit_requested);
}

bool gc_ui_run_controls_modal(SDL_Window *window, SDL_Renderer *renderer,
                              const GcUiFont& font, bool vsync_active,
                              GcInputBindings& bindings, bool& quit_requested,
                              std::string& status)
{
    bool done = false;
    int waiting_for = -1;
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    while (!done && !quit_requested) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        const int row_height = h < 560 ? 26 : 31;
        const int first_y = h < 560 ? 78 : 98;
        const SDL_Rect defaults_button{48, h - 62, 220, 42};
        const SDL_Rect done_button{w - 190, h - 62, 142, 42};

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
                gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                if (gc_ui_inside(mx, my, done_button)) done = true;
                else if (gc_ui_inside(mx, my, defaults_button)) {
                    bindings = gc_default_input_bindings();
                    status = gc_save_input_bindings(bindings)
                        ? "DEFAULT CONTROLS RESTORED AND SAVED."
                        : "DEFAULTS ACTIVE, BUT COULD NOT SAVE CONFIGURATION.";
                    waiting_for = -1;
                } else {
                    for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
                        const SDL_Rect change_button{w - 232,
                            first_y + static_cast<int>(i) * row_height + 2, 176, row_height - 4};
                        if (gc_ui_inside(mx, my, change_button)) {
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
        gc_ui_map_mouse(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        font.draw(48, 22, "CONTROL SETTINGS", accent, 3, 60);
        font.draw(50, 56, "CLICK A KEY TO REBIND. RIGHT-CLICK OR SHIFT+CLICK ALWAYS DRAGS STYLUS.", muted, 1, 140);
        const SDL_Rect panel_rect{40, first_y - 8, w - 80, row_height * static_cast<int>(GC_CONTROL_COUNT) + 14};
        gc_ui_fill(renderer, panel_rect, panel);
        gc_ui_outline(renderer, panel_rect, blue);
        for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
            const int y = first_y + static_cast<int>(i) * row_height;
            const SDL_Rect row{52, y, w - 104, row_height - 2};
            const SDL_Rect change_button{w - 232, y + 2, 176, row_height - 4};
            if (waiting_for == static_cast<int>(i)) gc_ui_fill(renderer, row, panel_light);
            font.draw(72, y + (row_height - 14) / 2, action_names[i],
                      waiting_for == static_cast<int>(i) ? accent : ink, 2, 36);
            gc_ui_draw_button(renderer, font, change_button,
                waiting_for == static_cast<int>(i) ? "PRESS A KEY" : key_label(bindings.keycodes[i]),
                true, waiting_for == static_cast<int>(i), gc_ui_inside(mx, my, change_button));
        }
        font.draw(52, first_y + row_height * static_cast<int>(GC_CONTROL_COUNT) + 14,
                  status,
                  status.find("ALREADY") != std::string::npos || status.find("COULD NOT") != std::string::npos
                      ? warning : muted, 1, 140);
        gc_ui_draw_button(renderer, font, defaults_button, "RESET DEFAULTS", true, false,
                          gc_ui_inside(mx, my, defaults_button));
        gc_ui_draw_button(renderer, font, done_button, "DONE", true, false,
                          gc_ui_inside(mx, my, done_button));
        gc_ui_present(renderer, vsync_active);
    }
    return !quit_requested;
}

bool gc_ui_prompt_number_modal(SDL_Window *window, SDL_Renderer *renderer,
                               const GcUiFont& font, bool vsync_active,
                               const std::string& title, const std::string& subtitle,
                               double initial_value, double min_val, double max_val,
                               bool allow_decimal, double& out_value,
                               bool& quit_requested)
{
    char init_buf[48];
    if (allow_decimal && std::fabs(initial_value - std::round(initial_value)) >= 0.005)
        std::snprintf(init_buf, sizeof(init_buf), "%.2f", initial_value);
    else
        std::snprintf(init_buf, sizeof(init_buf), "%.0f", initial_value);

    std::string text_buf(init_buf);
    std::string err_msg;
    bool done = false;
    bool accepted = false;
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    SDL_StartTextInput();

    auto try_apply = [&]() {
        if (text_buf.empty()) {
            err_msg = "ENTER A NUMBER FIRST.";
            return;
        }
        char *end = nullptr;
        const double parsed = std::strtod(text_buf.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(parsed) ||
            parsed < min_val || parsed > max_val) {
            char ebuf[96];
            std::snprintf(ebuf, sizeof(ebuf), "ENTER A VALUE BETWEEN %.0f AND %.0f.",
                          min_val, max_val);
            err_msg = ebuf;
            return;
        }
        out_value = allow_decimal ? parsed : std::round(parsed);
        accepted = true;
        done = true;
    };

    while (!done && !quit_requested) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_Rect modal{(w - 540) / 2, (h - 250) / 2, 540, 250};
        if (modal.x < 12) { modal.x = 12; modal.w = w - 24; }
        if (modal.y < 12) { modal.y = 12; modal.h = h - 24; }
        const SDL_Rect input_box{modal.x + 28, modal.y + 104, modal.w - 56, 48};
        const SDL_Rect apply_btn{modal.x + 28, modal.y + modal.h - 62, 180, 42};
        const SDL_Rect cancel_btn{modal.x + modal.w - 188, modal.y + modal.h - 62, 160, 42};

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit_requested = true; done = true; break; }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }
            if (event.type == SDL_TEXTINPUT) {
                for (const char *p = event.text.text; *p; ++p) {
                    const char ch = *p;
                    if (ch >= '0' && ch <= '9' && text_buf.size() < 10u) {
                        text_buf.push_back(ch);
                        err_msg.clear();
                    } else if (allow_decimal && (ch == '.' || ch == ',') &&
                               text_buf.find('.') == std::string::npos && text_buf.size() < 10u) {
                        text_buf.push_back('.');
                        err_msg.clear();
                    }
                }
            }
            if (event.type == SDL_KEYDOWN) {
                const SDL_Keycode sym = event.key.keysym.sym;
                if (sym == SDLK_ESCAPE) done = true;
                else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) try_apply();
                else if (sym == SDLK_BACKSPACE && !text_buf.empty()) {
                    text_buf.pop_back();
                    err_msg.clear();
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int mx = 0, my = 0;
                gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                if (gc_ui_inside(mx, my, apply_btn)) try_apply();
                else if (gc_ui_inside(mx, my, cancel_btn)) done = true;
            }
        }
        if (done || quit_requested) break;

        int mx = 0, my = 0;
        gc_ui_map_mouse(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        gc_ui_fill(renderer, SDL_Rect{0, 0, w, h}, overlay_dim);
        gc_ui_fill(renderer, modal, panel);
        gc_ui_outline(renderer, modal, accent);
        font.draw(modal.x + 28, modal.y + 24, title, accent, 3, 30);
        font.draw(modal.x + 28, modal.y + 68, subtitle, muted, 2, 42);
        gc_ui_fill(renderer, input_box, background);
        gc_ui_outline(renderer, input_box, blue);
        font.draw(input_box.x + 14, input_box.y + 13, text_buf + "_", ink, 3, 20);
        if (!err_msg.empty())
            font.draw(modal.x + 28, modal.y + 162, err_msg, warning, 1, 78);
        gc_ui_draw_button(renderer, font, apply_btn, "APPLY (ENTER)", true, true,
                          gc_ui_inside(mx, my, apply_btn));
        gc_ui_draw_button(renderer, font, cancel_btn, "CANCEL", true, false,
                          gc_ui_inside(mx, my, cancel_btn));
        gc_ui_present(renderer, vsync_active);
    }

    SDL_StopTextInput();
    return accepted && !quit_requested;
}

bool gc_ui_prompt_text_modal(SDL_Window *window, SDL_Renderer *renderer,
                             const GcUiFont& font, bool vsync_active,
                             const std::string& title, const std::string& subtitle,
                             const std::string& initial_value, std::string& out_value,
                             bool& quit_requested)
{
    std::string text_buf = initial_value;
    std::string err_msg;
    bool done = false;
    bool accepted = false;
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    SDL_StartTextInput();

    auto try_apply = [&]() {
        std::string trimmed = text_buf;
        trim_string(trimmed);
        if (trimmed.empty()) {
            err_msg = "ENTER HOST:PORT (E.G. 127.0.0.1:1997).";
            return;
        }
        out_value = trimmed;
        accepted = true;
        done = true;
    };

    while (!done && !quit_requested) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_Rect modal{(w - 580) / 2, (h - 250) / 2, 580, 250};
        if (modal.x < 12) { modal.x = 12; modal.w = w - 24; }
        if (modal.y < 12) { modal.y = 12; modal.h = h - 24; }
        const SDL_Rect input_box{modal.x + 28, modal.y + 104, modal.w - 56, 48};
        const SDL_Rect apply_btn{modal.x + 28, modal.y + modal.h - 62, 180, 42};
        const SDL_Rect cancel_btn{modal.x + modal.w - 188, modal.y + modal.h - 62, 160, 42};

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit_requested = true; done = true; break; }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }
            if (event.type == SDL_TEXTINPUT) {
                for (const char *p = event.text.text; *p; ++p) {
                    const unsigned char ch = static_cast<unsigned char>(*p);
                    if (ch >= 0x20u && ch < 0x7Fu && text_buf.size() < 48u) {
                        text_buf.push_back(static_cast<char>(ch));
                        err_msg.clear();
                    }
                }
            }
            if (event.type == SDL_KEYDOWN) {
                const SDL_Keycode sym = event.key.keysym.sym;
                if (sym == SDLK_ESCAPE) done = true;
                else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) try_apply();
                else if (sym == SDLK_BACKSPACE && !text_buf.empty()) {
                    text_buf.pop_back();
                    err_msg.clear();
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int mx = 0, my = 0;
                gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                if (gc_ui_inside(mx, my, apply_btn)) try_apply();
                else if (gc_ui_inside(mx, my, cancel_btn)) done = true;
            }
        }
        if (done || quit_requested) break;

        int mx = 0, my = 0;
        gc_ui_map_mouse(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        gc_ui_fill(renderer, SDL_Rect{0, 0, w, h}, overlay_dim);
        gc_ui_fill(renderer, modal, panel);
        gc_ui_outline(renderer, modal, accent);
        font.draw(modal.x + 28, modal.y + 24, title, accent, 3, 30);
        font.draw(modal.x + 28, modal.y + 68, subtitle, muted, 2, 44);
        gc_ui_fill(renderer, input_box, background);
        gc_ui_outline(renderer, input_box, blue);
        font.draw(input_box.x + 14, input_box.y + 15, text_buf + "_", ink, 2, 38);
        if (!err_msg.empty())
            font.draw(modal.x + 28, modal.y + 162, err_msg, warning, 1, 78);
        gc_ui_draw_button(renderer, font, apply_btn, "CONNECT", true, true,
                          gc_ui_inside(mx, my, apply_btn));
        gc_ui_draw_button(renderer, font, cancel_btn, "CANCEL", true, false,
                          gc_ui_inside(mx, my, cancel_btn));
        gc_ui_present(renderer, vsync_active);
    }

    SDL_StopTextInput();
    return accepted && !quit_requested;
}

void GcTopMenuBar::show_toast(const std::string& message, double duration_sec)
{
    m_toast_text = message;
    m_toast_timer = duration_sec;
}

void GcTopMenuBar::tick(double dt) noexcept
{
    if (m_toast_timer > 0.0) {
        m_toast_timer -= dt;
        if (m_toast_timer <= 0.0) {
            m_toast_timer = 0.0;
            m_toast_text.clear();
        }
    }
}

bool GcTopMenuBar::is_visible(bool in_launcher, const GcTimingOptions& timing,
                              int mouse_render_y) const noexcept
{
    if (in_launcher || m_open_tab >= 0 || !timing.auto_hide_menubar) return true;
    return mouse_render_y >= 0 && mouse_render_y <= height + 8;
}

int GcTopMenuBar::reserved_top_height(bool in_launcher,
                                      const GcTimingOptions& timing) const noexcept
{
    return (in_launcher || !timing.auto_hide_menubar) ? height : 0;
}

bool GcTopMenuBar::handle_event(const SDL_Event& event, SDL_Window *window,
                                SDL_Renderer *renderer, GcLaunchSelection& selection,
                                bool in_launcher, bool emulation_ready,
                                bool paused, bool is_fullscreen,
                                GcMenuAction& out_action, bool& consumed_pointer)
{
    out_action = GcMenuAction::None;
    consumed_pointer = false;

    int win_w = 0, win_h = 0;
    SDL_GetRendererOutputSize(renderer, &win_w, &win_h);

    if (event.type == SDL_KEYDOWN && !event.key.repeat &&
        event.key.keysym.sym == SDLK_ESCAPE && m_open_tab >= 0) {
        m_open_tab = -1;
        return true;
    }

    if (event.type == SDL_MOUSEMOTION) {
        int mx = 0, my = 0;
        gc_ui_map_mouse(window, win_w, win_h, event.motion.x, event.motion.y, mx, my);
        if (m_open_tab >= 0) {
            consumed_pointer = true;
            if (my >= 0 && my < height) {
                const auto tabs = build_menubar_tabs(selection, win_w);
                for (std::size_t i = 0; i < tabs.size(); ++i) {
                    if (gc_ui_inside(mx, my, tabs[i].rect)) {
                        m_open_tab = static_cast<int>(i);
                        break;
                    }
                }
            }
            return true;
        }
        if (is_visible(in_launcher, selection.timing, my) && my >= 0 && my < height) {
            consumed_pointer = true;
        }
        return false;
    }

    if (event.type == SDL_MOUSEWHEEL && event.wheel.y != 0) {
        int raw_x = 0, raw_y = 0, mx = 0, my = 0;
        SDL_GetMouseState(&raw_x, &raw_y);
        gc_ui_map_mouse(window, win_w, win_h, raw_x, raw_y, mx, my);
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const auto tabs = build_menubar_tabs(selection, win_w);
        const int dir = event.wheel.y > 0 ? 1 : -1;

        const bool on_lcd = (m_open_tab == 1) ||
            (is_visible(in_launcher, selection.timing, my) && gc_ui_inside(mx, my, tabs[1].rect));
        const bool on_cpu = (m_open_tab == 2) ||
            (is_visible(in_launcher, selection.timing, my) && gc_ui_inside(mx, my, tabs[2].rect));

        if (on_lcd) {
            const double step = shift ? 0.2 : 1.0;
            selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(selection.timing.lcd_hz + dir * step);
            (void)gc_save_launch_selection(selection);
            show_toast("LCD REFRESH: " + gc_ui_format_lcd_hz(selection.timing.lcd_hz));
            out_action = GcMenuAction::TimingChanged;
            consumed_pointer = true;
            return true;
        }
        if (on_cpu) {
            const int step = shift ? 1 : 5;
            const int next = static_cast<int>(selection.timing.cpu_speed_percent) + dir * step;
            selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(
                static_cast<unsigned>(std::max(25, next)));
            (void)gc_save_launch_selection(selection);
            show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
            out_action = GcMenuAction::TimingChanged;
            consumed_pointer = true;
            return true;
        }
    }

    if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
        int mx = 0, my = 0;
        gc_ui_map_mouse(window, win_w, win_h, event.button.x, event.button.y, mx, my);
        const bool bar_shown = is_visible(in_launcher, selection.timing, my);

        if (bar_shown && my >= 0 && my < height) {
            consumed_pointer = true;
            const auto tabs = build_menubar_tabs(selection, win_w);
            for (std::size_t i = 0; i < tabs.size(); ++i) {
                if (gc_ui_inside(mx, my, tabs[i].rect)) {
                    const int idx = static_cast<int>(i);
                    m_open_tab = (m_open_tab == idx) ? -1 : idx;
                    return true;
                }
            }
            m_open_tab = -1;
            return true;
        }

        if (m_open_tab >= 0) {
            consumed_pointer = true;
            const DropdownLayout layout = build_dropdown_layout(
                m_open_tab, win_w, win_h, selection, in_launcher,
                emulation_ready, paused, is_fullscreen);

            if (!gc_ui_inside(mx, my, layout.panel_rect)) {
                m_open_tab = -1;
                return true;
            }

            auto apply_step = [&](ItemOp op, double delta) {
                if (op == ItemOp::StepLcdHz) {
                    selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(selection.timing.lcd_hz + delta);
                    (void)gc_save_launch_selection(selection);
                    show_toast("LCD REFRESH: " + gc_ui_format_lcd_hz(selection.timing.lcd_hz));
                    out_action = GcMenuAction::TimingChanged;
                } else if (op == ItemOp::StepCpuSpeed) {
                    const int next = static_cast<int>(selection.timing.cpu_speed_percent) +
                                     static_cast<int>(delta);
                    selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(
                        static_cast<unsigned>(std::max(25, next)));
                    (void)gc_save_launch_selection(selection);
                    show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
                    out_action = GcMenuAction::TimingChanged;
                }
            };

            if (layout.has_stepper) {
                if (gc_ui_inside(mx, my, layout.step_m2)) {
                    apply_step(layout.step_op, -layout.step_d2);
                    return true;
                }
                if (gc_ui_inside(mx, my, layout.step_m1)) {
                    apply_step(layout.step_op, -layout.step_d1);
                    return true;
                }
                if (gc_ui_inside(mx, my, layout.step_p1)) {
                    apply_step(layout.step_op, layout.step_d1);
                    return true;
                }
                if (gc_ui_inside(mx, my, layout.step_p2)) {
                    apply_step(layout.step_op, layout.step_d2);
                    return true;
                }
                if (gc_ui_inside(mx, my, layout.step_val)) {
                    m_open_tab = -1;
                    out_action = layout.center_action;
                    return true;
                }
            }

            for (const DropdownItem& item : layout.items) {
                if (item.divider || !item.enabled) continue;
                if (!gc_ui_inside(mx, my, item.rect)) continue;

                if (!item.stay_open) m_open_tab = -1;

                switch (item.op) {
                case ItemOp::Action:
                    out_action = item.action;
                    break;
                case ItemOp::SetLcdHz:
                    selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(item.value);
                    (void)gc_save_launch_selection(selection);
                    show_toast("LCD REFRESH: " + gc_ui_format_lcd_hz(selection.timing.lcd_hz));
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::SetCpuSpeed:
                    selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(
                        static_cast<unsigned>(std::lround(item.value)));
                    (void)gc_save_launch_selection(selection);
                    show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::SetPace:
                    selection.timing.game_pace_percent = gc_ui_clamp_game_pace(
                        static_cast<unsigned>(std::lround(item.value)));
                    (void)gc_save_launch_selection(selection);
                    {
                        char pbuf[64];
                        std::snprintf(pbuf, sizeof(pbuf), "GAME PACE: %.1fX",
                                      static_cast<double>(selection.timing.game_pace_percent) / 100.0);
                        show_toast(pbuf);
                    }
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::ResetTiming:
                    selection.timing.lcd_hz = GC_LCD_NATIVE_HZ;
                    selection.timing.cpu_speed_percent = 100u;
                    selection.timing.game_pace_percent = 100u;
                    (void)gc_save_launch_selection(selection);
                    show_toast("TIMING RESET: 75.48 HZ HW / 100% (5.00 MHZ) / 1.0X");
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::SetTouchMode: {
                    const unsigned m = static_cast<unsigned>(std::lround(item.value));
                    selection.timing.touch_mode = m == 2u ? GC_TOUCH_HOLD_DRAG
                        : (m == 1u ? GC_TOUCH_DEBOUNCED_TAP : GC_TOUCH_SINGLE_TAP);
                    (void)gc_save_launch_selection(selection);
                    show_toast(selection.timing.touch_mode == GC_TOUCH_HOLD_DRAG
                        ? "TOUCH MODE: HOLD / DRAG STYLUS"
                        : (selection.timing.touch_mode == GC_TOUCH_DEBOUNCED_TAP
                            ? "TOUCH MODE: 2-SCAN DEBOUNCED TAP"
                            : "TOUCH MODE: SINGLE TAP (NO REPEAT)"));
                    out_action = GcMenuAction::TimingChanged;
                    break;
                }
                case ItemOp::ToggleMute:
                    selection.timing.audio_muted = !selection.timing.audio_muted;
                    (void)gc_save_launch_selection(selection);
                    show_toast(selection.timing.audio_muted ? "AUDIO MUTED" : "AUDIO ENABLED");
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::SetVolume:
                    selection.timing.audio_muted = false;
                    selection.timing.audio_volume_percent =
                        std::min(100u, static_cast<unsigned>(std::lround(item.value)));
                    (void)gc_save_launch_selection(selection);
                    show_toast("AUDIO VOLUME: " + std::to_string(selection.timing.audio_volume_percent) + "%");
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::SetSerialMode: {
                    const unsigned sm = static_cast<unsigned>(std::lround(item.value));
                    selection.serial_mode = sm == 2u ? GC_SERIAL_TCP_HOST
                        : (sm == 1u ? GC_SERIAL_MODEM : GC_SERIAL_OFF);
                    (void)gc_save_launch_selection(selection);
                    out_action = GcMenuAction::SerialModeChanged;
                    break;
                }
                case ItemOp::ToggleWarmBootSeed:
                    selection.warm_boot_nvram = !selection.warm_boot_nvram;
                    (void)gc_save_launch_selection(selection);
                    out_action = GcMenuAction::SeedWarmBootNvram;
                    break;
                case ItemOp::ToggleIntegerScale:
                    selection.timing.integer_scaling = !selection.timing.integer_scaling;
                    (void)gc_save_launch_selection(selection);
                    show_toast(selection.timing.integer_scaling
                        ? "INTEGER PIXEL SCALING ON" : "SMOOTH ASPECT FIT ON");
                    out_action = GcMenuAction::TimingChanged;
                    break;
                case ItemOp::ToggleAutoHideBar:
                    selection.timing.auto_hide_menubar = !selection.timing.auto_hide_menubar;
                    (void)gc_save_launch_selection(selection);
                    show_toast(selection.timing.auto_hide_menubar
                        ? "TOP BAR AUTO-HIDE ON (MOVE CURSOR TO TOP)"
                        : "TOP BAR ALWAYS VISIBLE");
                    out_action = GcMenuAction::TimingChanged;
                    break;
                default:
                    break;
                }
                return true;
            }
            return true;
        }
    }

    if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
        int mx = 0, my = 0;
        gc_ui_map_mouse(window, win_w, win_h, event.button.x, event.button.y, mx, my);
        if (m_open_tab >= 0 || (is_visible(in_launcher, selection.timing, my) && my >= 0 && my < height))
            consumed_pointer = true;
    }
    return false;
}

void GcTopMenuBar::render(SDL_Window *window, SDL_Renderer *renderer, const GcUiFont& font,
                          const GcLaunchSelection& selection, bool in_launcher,
                          bool emulation_ready, bool paused, bool fast_forward,
                          bool is_fullscreen, double host_fps,
                          int raw_mouse_x, int raw_mouse_y) const
{
    int win_w = 0, win_h = 0, mx = 0, my = 0;
    SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
    gc_ui_map_mouse(window, win_w, win_h, raw_mouse_x, raw_mouse_y, mx, my);

    const bool bar_shown = is_visible(in_launcher, selection.timing, my);
    if (bar_shown) {
        gc_ui_fill(renderer, SDL_Rect{0, 0, win_w, height}, menubar_bg);
        gc_ui_fill(renderer, SDL_Rect{0, height - 1, win_w, 1}, menubar_border);

        const auto tabs = build_menubar_tabs(selection, win_w);
        int right_limit = 0;
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            const bool active = m_open_tab == static_cast<int>(i);
            const bool hovered = gc_ui_inside(mx, my, tabs[i].rect);
            if (active || hovered) {
                gc_ui_fill(renderer, tabs[i].rect, active ? accent : panel_hover);
            }
            const int ty = tabs[i].rect.y + (tabs[i].rect.h - 7 * tabs[i].scale) / 2;
            font.draw(tabs[i].rect.x + (tabs[i].scale == 2 ? 9 : 6), ty,
                      tabs[i].label, active ? background : ink, tabs[i].scale, 28);
            right_limit = tabs[i].rect.x + tabs[i].rect.w;
        }

        std::string pill;
        GcUiColor pill_color = muted;
        if (fast_forward) {
            pill = "4X TURBO";
            pill_color = accent;
        } else if (!in_launcher && paused) {
            pill = "PAUSED";
            pill_color = warning;
        } else if (!in_launcher && host_fps > 0.5) {
            char fbuf[24];
            std::snprintf(fbuf, sizeof(fbuf), "%.0f FPS", host_fps);
            pill = fbuf;
        } else if (in_launcher) {
            pill = gc_ui_valid_firmware(selection.firmware_dir) ? "READY" : "SETUP";
            pill_color = gc_ui_valid_firmware(selection.firmware_dir) ? accent : warning;
        }

        if (!pill.empty()) {
            const int pw = static_cast<int>(pill.size()) * 12;
            const int px = win_w - pw - 12;
            if (px > right_limit + 12) {
                font.draw(px, 9, pill, pill_color, 2, 16);
            }
        }
    }

    if (m_toast_timer > 0.0 && !m_toast_text.empty() && m_open_tab < 0) {
        const int chars = std::min<int>(static_cast<int>(m_toast_text.size()), 56);
        const int tw = chars * 12 + 24;
        const int tx = std::max(8, (win_w - tw) / 2);
        const int ty = (bar_shown ? height : 0) + 8;
        const SDL_Rect toast_rect{tx, ty, tw, 28};
        gc_ui_fill(renderer, toast_rect, dropdown_bg);
        gc_ui_outline(renderer, toast_rect, accent);
        font.draw(tx + 12, ty + 7, m_toast_text, accent, 2, chars);
    }

    if (m_open_tab >= 0) {
        const DropdownLayout layout = build_dropdown_layout(
            m_open_tab, win_w, win_h, selection, in_launcher,
            emulation_ready, paused, is_fullscreen);

        gc_ui_fill(renderer, layout.panel_rect, dropdown_bg);
        gc_ui_outline(renderer, layout.panel_rect, blue);

        if (layout.has_stepper) {
            gc_ui_draw_button(renderer, font, layout.step_m2, layout.step_m2_lbl,
                              true, false, gc_ui_inside(mx, my, layout.step_m2), layout.text_scale);
            gc_ui_draw_button(renderer, font, layout.step_m1, layout.step_m1_lbl,
                              true, false, gc_ui_inside(mx, my, layout.step_m1), layout.text_scale);
            gc_ui_draw_button(renderer, font, layout.step_val, layout.step_val_lbl,
                              true, true, gc_ui_inside(mx, my, layout.step_val), layout.text_scale);
            gc_ui_draw_button(renderer, font, layout.step_p1, layout.step_p1_lbl,
                              true, false, gc_ui_inside(mx, my, layout.step_p1), layout.text_scale);
            gc_ui_draw_button(renderer, font, layout.step_p2, layout.step_p2_lbl,
                              true, false, gc_ui_inside(mx, my, layout.step_p2), layout.text_scale);
        }

        for (const DropdownItem& item : layout.items) {
            if (item.divider) {
                gc_ui_fill(renderer, item.rect, menubar_border);
                continue;
            }
            const bool hovered = item.enabled && gc_ui_inside(mx, my, item.rect);
            if (hovered) gc_ui_fill(renderer, item.rect, panel_hover);
            const GcUiColor fg = !item.enabled ? disabled
                : (item.marked ? accent : (hovered ? ink : ink));
            const int text_h = 7 * layout.text_scale;
            const int ty = item.rect.y + (item.rect.h - text_h) / 2;
            const int max_ch = std::max(1, (item.rect.w - 16) / (6 * layout.text_scale));
            font.draw(item.rect.x + 10, ty, item.label, fg, layout.text_scale, max_ch);
        }
    }
}

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
    output << "# Game.com emulator keyboard bindings; edit in Input -> Configure Controls.\n";
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
        else if (name == "cartridge2_path") selection.cartridge2_path = value;
        else if (name == "nvram_path") selection.nvram_path = value;
        else if (name == "warm_boot_nvram")
            selection.warm_boot_nvram = (value == "1" || value == "true");
        else if (name == "serial_mode") {
            const unsigned long parsed = std::strtoul(value.c_str(), nullptr, 10);
            selection.serial_mode = parsed == 3ul ? GC_SERIAL_TCP_CLIENT
                : (parsed == 2ul ? GC_SERIAL_TCP_HOST
                    : (parsed == 1ul ? GC_SERIAL_MODEM : GC_SERIAL_OFF));
        } else if (name == "serial_endpoint" && !value.empty())
            selection.serial_endpoint = value;
        else if (name == "phonebook_path")
            selection.phonebook_path = value;
        else if (name == "lcd_hz") {
            char *end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            if (end && end != value.c_str())
                selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(parsed);
        } else if (name == "cpu_speed_percent") {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (end && end != value.c_str())
                selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(static_cast<unsigned>(parsed));
        } else if (name == "game_pace_percent") {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (end && end != value.c_str())
                selection.timing.game_pace_percent = gc_ui_clamp_game_pace(static_cast<unsigned>(parsed));
        } else if (name == "touch_mode") {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (end && end != value.c_str()) {
                selection.timing.touch_mode = parsed == 2ul ? GC_TOUCH_HOLD_DRAG
                    : (parsed == 1ul ? GC_TOUCH_DEBOUNCED_TAP : GC_TOUCH_SINGLE_TAP);
            }
        } else if (name == "audio_muted") {
            selection.timing.audio_muted = (value == "1" || value == "true");
        } else if (name == "audio_volume_percent") {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (end && end != value.c_str())
                selection.timing.audio_volume_percent = std::min<unsigned>(100u, static_cast<unsigned>(parsed));
        } else if (name == "integer_scaling") {
            selection.timing.integer_scaling = (value == "1" || value == "true");
        } else if (name == "auto_hide_menubar") {
            selection.timing.auto_hide_menubar = (value == "1" || value == "true");
        }
    }
    if (!selection.cartridge_path.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(selection.cartridge_path, ec) || ec)
            selection.cartridge_path.clear();
    }
    if (!selection.cartridge2_path.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(selection.cartridge2_path, ec) || ec)
            selection.cartridge2_path.clear();
    }
    if (selection.firmware_dir.empty() && gc_ui_valid_firmware("firmware")) {
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
    output << "# Game.com launcher paths, timing, touch, audio, and view settings.\n";
    output << "firmware_dir=" << selection.firmware_dir << '\n';
    output << "cartridge_path=" << selection.cartridge_path << '\n';
    output << "cartridge2_path=" << selection.cartridge2_path << '\n';
    output << "nvram_path=" << selection.nvram_path << '\n';
    output << "warm_boot_nvram=" << (selection.warm_boot_nvram ? 1 : 0) << '\n';
    output << "serial_mode=" << static_cast<unsigned>(selection.serial_mode) << '\n';
    output << "serial_endpoint=" << selection.serial_endpoint << '\n';
    output << "phonebook_path=" << selection.phonebook_path << '\n';
    output << "lcd_hz=" << selection.timing.lcd_hz << '\n';
    output << "cpu_speed_percent=" << selection.timing.cpu_speed_percent << '\n';
    output << "game_pace_percent=" << selection.timing.game_pace_percent << '\n';
    output << "touch_mode=" << static_cast<unsigned>(selection.timing.touch_mode) << '\n';
    output << "audio_muted=" << (selection.timing.audio_muted ? 1 : 0) << '\n';
    output << "audio_volume_percent=" << selection.timing.audio_volume_percent << '\n';
    output << "integer_scaling=" << (selection.timing.integer_scaling ? 1 : 0) << '\n';
    output << "auto_hide_menubar=" << (selection.timing.auto_hide_menubar ? 1 : 0) << '\n';
    return output.good();
}

std::string gc_default_nvram_path()
{
    return default_nvram_file().string();
}

bool gc_sdl_launcher_run(GcLaunchSelection& selection, GcInputBindings& bindings)
{
    const bool owns_sdl = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (owns_sdl && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return false;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *window = SDL_CreateWindow("Game.com Emulator — Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 640,
        SDL_WINDOW_RESIZABLE);
    if (!window) {
        if (owns_sdl) SDL_Quit();
        return false;
    }
    SDL_SetWindowMinimumSize(window, 760, 560);
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
    GcUiFont font(renderer);
    GcTopMenuBar menubar;

    if (selection.firmware_dir.empty() && selection.cartridge_path.empty())
        selection = gc_load_launch_selection();
    selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(selection.timing.lcd_hz);
    selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(selection.timing.cpu_speed_percent);
    selection.timing.game_pace_percent = gc_ui_clamp_game_pace(selection.timing.game_pace_percent);

    bool done = false, quit = false, started = false;
    bool choose_firmware = false, choose_rom = false, choose_controls = false;
    bool force_builtin = false;
    std::string status = gc_ui_valid_firmware(selection.firmware_dir)
        ? "FIRMWARE READY. USE THE TOP MENU BAR TO ADJUST LCD HZ, CPU SPEED, INPUT, OR AUDIO."
        : "SELECT OR DRAG-AND-DROP YOUR FIRMWARE FOLDER AND OPTIONAL CARTRIDGE ROM.";
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    Uint64 last_ticks = SDL_GetPerformanceCounter();
    const double perf_freq = static_cast<double>(std::max<Uint64>(1u, SDL_GetPerformanceFrequency()));

    while (!done && !quit) {
        const Uint64 now_ticks = SDL_GetPerformanceCounter();
        const double dt = std::min(0.1, static_cast<double>(now_ticks - last_ticks) / perf_freq);
        last_ticks = now_ticks;
        menubar.tick(dt);

        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        const SDL_Rect firmware_button{w - 220, 178, 172, 42};
        const SDL_Rect rom_clear_button{w - 340, 314, 108, 42};
        const SDL_Rect rom_button{w - 220, 314, 172, 42};
        const SDL_Rect bios_button{48, h - 92, 224, 52};
        const SDL_Rect game_button{292, h - 92, 244, 52};
        const SDL_Rect quit_button{w - 190, h - 92, 142, 52};

        const bool firmware_ok = gc_ui_valid_firmware(selection.firmware_dir);
        const bool rom_ok = !selection.cartridge_path.empty();
        const bool is_fullscreen = (SDL_GetWindowFlags(window) &
            (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit = true; break; }
            if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            }

            GcMenuAction menu_action = GcMenuAction::None;
            bool consumed = false;
            (void)menubar.handle_event(event, window, renderer, selection,
                                       true, false, false, is_fullscreen,
                                       menu_action, consumed);
            if (menu_action == GcMenuAction::LoadCartridgeRom) choose_rom = true;
            else if (menu_action == GcMenuAction::SetFirmwareFolder) choose_firmware = true;
            else if (menu_action == GcMenuAction::BootBiosOnly && firmware_ok) {
                selection.cartridge_path.clear();
                started = true; done = true;
            } else if (menu_action == GcMenuAction::BootSelected && firmware_ok) {
                started = true; done = true;
            } else if (menu_action == GcMenuAction::ConfigureControls) choose_controls = true;
            else if (menu_action == GcMenuAction::ResetControls) {
                bindings = gc_default_input_bindings();
                (void)gc_save_input_bindings(bindings);
                menubar.show_toast("CONTROLS RESET TO DEFAULTS");
            } else if (menu_action == GcMenuAction::ToggleFullscreen) {
                (void)SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else if (menu_action == GcMenuAction::PromptCustomLcdHz) {
                double val = selection.timing.lcd_hz;
                if (gc_ui_prompt_number_modal(window, renderer, font, vsync_active,
                        "CUSTOM LCD REFRESH HZ", "ENTER EMULATED LCD VBLANK RATE (10 - 240 HZ):",
                        selection.timing.lcd_hz, 10.0, 240.0, true, val, quit)) {
                    selection.timing.lcd_hz = gc_ui_clamp_lcd_hz(val);
                    (void)gc_save_launch_selection(selection);
                    menubar.show_toast("LCD REFRESH: " + gc_ui_format_lcd_hz(selection.timing.lcd_hz));
                }
            } else if (menu_action == GcMenuAction::PromptCustomCpuSpeed) {
                double val = static_cast<double>(selection.timing.cpu_speed_percent);
                if (gc_ui_prompt_number_modal(window, renderer, font, vsync_active,
                        "CUSTOM CPU CLOCK %", "ENTER SM8521 CPU CLOCK PERCENTAGE (25 - 800%):",
                        val, 25.0, 800.0, false, val, quit)) {
                    selection.timing.cpu_speed_percent = gc_ui_clamp_cpu_speed(static_cast<unsigned>(val));
                    (void)gc_save_launch_selection(selection);
                    menubar.show_toast("CPU CLOCK: " + gc_ui_format_cpu_speed(selection.timing.cpu_speed_percent));
                }
            } else if (menu_action == GcMenuAction::Quit) {
                done = true;
            }
            if (consumed) continue;

            if (event.type == SDL_DROPFILE && event.drop.file) {
                const std::string dropped(event.drop.file);
                SDL_free(event.drop.file);
                std::error_code ec;
                const fs::path p(dropped);
                if (fs::is_directory(p, ec) && !ec) {
                    selection.firmware_dir = p.string();
                    (void)gc_save_launch_selection(selection);
                    status = gc_ui_valid_firmware(selection.firmware_dir)
                        ? "DROPPED FIRMWARE FOLDER DETECTED AND SAVED."
                        : "FOLDER SET, BUT INTERNAL.BIN AND/OR EXTERNAL.BIN IS MISSING.";
                } else if (fs::is_regular_file(p, ec) && !ec) {
                    const std::string fname = p.filename().string();
                    if (fname == "internal.bin" || fname == "external.bin") {
                        selection.firmware_dir = p.parent_path().string();
                    } else {
                        selection.cartridge_path = p.string();
                    }
                    (void)gc_save_launch_selection(selection);
                    status = "UPDATED PATH VIA DRAG AND DROP.";
                }
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                const bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                if (key == SDLK_ESCAPE) done = true;
                else if (key == SDLK_1) { choose_firmware = true; force_builtin = shift; }
                else if (key == SDLK_2) { choose_rom = true; force_builtin = shift; }
                else if (key == SDLK_3) choose_controls = true;
                else if (key == SDLK_F11) {
                    (void)SDL_SetWindowFullscreen(window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && firmware_ok) {
                    started = true; done = true;
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int mx = 0, my = 0;
                gc_ui_map_mouse(window, w, h, event.button.x, event.button.y, mx, my);
                const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
                if (gc_ui_inside(mx, my, firmware_button)) { choose_firmware = true; force_builtin = shift; }
                else if (gc_ui_inside(mx, my, rom_button)) { choose_rom = true; force_builtin = shift; }
                else if (rom_ok && gc_ui_inside(mx, my, rom_clear_button)) {
                    selection.cartridge_path.clear();
                    (void)gc_save_launch_selection(selection);
                } else if (gc_ui_inside(mx, my, bios_button) && firmware_ok) {
                    selection.cartridge_path.clear();
                    started = true; done = true;
                } else if (gc_ui_inside(mx, my, game_button) && firmware_ok && rom_ok) {
                    started = true; done = true;
                } else if (gc_ui_inside(mx, my, quit_button)) {
                    done = true;
                }
            }
        }
        if (done || quit) break;

        if (choose_firmware) {
            choose_firmware = false;
            const std::string initial = selection.firmware_dir.empty()
                ? gc_ui_home_directory() : selection.firmware_dir;
            std::string path;
            bool used_builtin = false;
            if (gc_ui_pick_path(window, renderer, font, vsync_active, true,
                                initial, path, quit, force_builtin, used_builtin)) {
                selection.firmware_dir = path;
                (void)gc_save_launch_selection(selection);
                status = gc_ui_valid_firmware(path)
                    ? "FIRMWARE READY. CLICK BOOT BIOS OR BOOT GAME."
                    : "FOLDER SELECTED, BUT INTERNAL.BIN AND/OR EXTERNAL.BIN IS MISSING.";
            }
            force_builtin = false;
        }
        if (choose_rom) {
            choose_rom = false;
            std::string initial = selection.firmware_dir.empty()
                ? gc_ui_home_directory() : selection.firmware_dir;
            if (!selection.cartridge_path.empty())
                initial = fs::path(selection.cartridge_path).parent_path().string();
            std::string path;
            bool used_builtin = false;
            if (gc_ui_pick_path(window, renderer, font, vsync_active, false,
                                initial, path, quit, force_builtin, used_builtin)) {
                selection.cartridge_path = path;
                (void)gc_save_launch_selection(selection);
                status = "GAME ROM SELECTED AND SAVED.";
            }
            force_builtin = false;
        }
        if (choose_controls) {
            choose_controls = false;
            std::string ctrl_status = "CONTROLS SAVE AUTOMATICALLY TO YOUR USER CONFIGURATION.";
            (void)gc_ui_run_controls_modal(window, renderer, font, vsync_active,
                                           bindings, quit, ctrl_status);
        }
        if (quit) break;

        int mx = 0, my = 0;
        gc_ui_map_mouse(window, w, h, mouse_x, mouse_y, mx, my);

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        font.draw(48, 56, "GAME.COM EMULATOR", accent, 4, 60);
        font.draw(50, 102, "SELECT FIRMWARE & ROM BELOW — USE THE TOP MENU BAR FOR HZ, SPEED, INPUT & AUDIO", muted, 2, 74);

        const SDL_Rect fw_panel{40, 144, w - 80, 110};
        const SDL_Rect rom_panel{40, 280, w - 80, 110};
        gc_ui_fill(renderer, fw_panel, panel); gc_ui_outline(renderer, fw_panel, blue);
        gc_ui_fill(renderer, rom_panel, panel); gc_ui_outline(renderer, rom_panel, blue);

        font.draw(60, 162, "1  SYSTEM FIRMWARE (INTERNAL.BIN + EXTERNAL.BIN)", ink, 2, 54);
        font.draw(60, 194,
                  selection.firmware_dir.empty() ? "NO FIRMWARE FOLDER SELECTED" : gc_ui_short_path(selection.firmware_dir, 52),
                  firmware_ok ? accent : warning, 2, 54);
        font.draw(60, 226, "HOLD SHIFT + CLICK BROWSE TO USE THE IN-APP BROWSER DIRECTLY.", muted, 1, 80);
        gc_ui_draw_button(renderer, font, firmware_button, "BROWSE...", true, false,
                          gc_ui_inside(mx, my, firmware_button));

        font.draw(60, 298, "2  GAME CARTRIDGE (.BIN, OPTIONAL)", ink, 2, 46);
        font.draw(60, 330,
                  selection.cartridge_path.empty() ? "NO GAME SELECTED (BOOT BIOS MENU)" : gc_ui_short_path(selection.cartridge_path, 42),
                  rom_ok ? accent : muted, 2, 44);
        font.draw(60, 362, "DRAG AND DROP A FOLDER OR CARTRIDGE ROM ANYWHERE INTO THIS WINDOW.", muted, 1, 80);
        if (rom_ok) {
            gc_ui_draw_button(renderer, font, rom_clear_button, "CLEAR", true, false,
                              gc_ui_inside(mx, my, rom_clear_button));
        }
        gc_ui_draw_button(renderer, font, rom_button, "BROWSE...", true, false,
                          gc_ui_inside(mx, my, rom_button));

        font.draw(50, 418, status, status.find("MISSING") != std::string::npos ? warning : muted, 2, 74);

        gc_ui_draw_button(renderer, font, bios_button, "BOOT BIOS", firmware_ok, false,
                          gc_ui_inside(mx, my, bios_button));
        gc_ui_draw_button(renderer, font, game_button, "BOOT GAME", firmware_ok && rom_ok, false,
                          gc_ui_inside(mx, my, game_button));
        gc_ui_draw_button(renderer, font, quit_button, "QUIT", true, false,
                          gc_ui_inside(mx, my, quit_button));

        menubar.render(window, renderer, font, selection, true, false, false, false,
                       is_fullscreen, 0.0, mouse_x, mouse_y);
        gc_ui_present(renderer, vsync_active);
    }

    if (started && !quit) (void)gc_save_launch_selection(selection);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (owns_sdl) SDL_Quit();
    return started && !quit;
}
