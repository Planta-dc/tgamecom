#include "gamecom_launcher.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(GAMECOM_HAVE_GTK3)
#include <gtk/gtk.h>
#endif

namespace {
namespace fs = std::filesystem;

struct Color { Uint8 r, g, b, a; };
constexpr Color background{13, 22, 35, 255};
constexpr Color panel{24, 39, 56, 255};
constexpr Color panel_light{34, 53, 72, 255};
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
    case '\'': return {4,4,8,0,0,0,0}; case '!': return {4,4,4,4,4,0,4};
    case '?': return {14,17,1,2,4,0,4}; case '*': return {0,17,10,31,10,17,0};
    case ' ': return {0,0,0,0,0,0,0};
    default: return {14,17,1,2,4,0,4};
    }
}

void fill(SDL_Renderer *r, SDL_Rect rect, Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &rect);
}

void outline(SDL_Renderer *r, SDL_Rect rect, Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderDrawRect(r, &rect);
}

void text(SDL_Renderer *r, int x, int y, const std::string& value,
          Color c = ink, int scale = 2, int max_chars = 120)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    const int limit = std::min<int>(static_cast<int>(value.size()), max_chars);
    for (int i = 0; i < limit; ++i) {
        const unsigned char raw = static_cast<unsigned char>(value[static_cast<std::size_t>(i)]);
        const char ch = static_cast<char>(std::toupper(raw));
        const auto rows = glyph(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((rows[static_cast<std::size_t>(row)] & (1u << (4 - col))) != 0) {
                    SDL_Rect pixel{x + i * 6 * scale + col * scale,
                                   y + row * scale, scale, scale};
                    SDL_RenderFillRect(r, &pixel);
                }
            }
        }
    }
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

fs::path home_directory()
{
    if (const char *home = std::getenv("HOME")) {
        if (*home) return fs::path(home);
    }
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    return ec ? fs::path(".") : current;
}

fs::path bindings_file()
{
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) return fs::path(xdg) / "gamecom-emulator" / "controls.ini";
    }
    return home_directory() / ".config" / "gamecom-emulator" / "controls.ini";
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

enum class BrowserMode { FirmwareDirectory, CartridgeFile };
struct BrowserEntry { fs::path path; std::string name; bool directory; };
enum class NativePickResult { Unavailable, Cancelled, Selected };

NativePickResult native_file_dialog(BrowserMode mode, const std::string& initial_folder,
                                    std::string& selected_path)
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
    if (!gtk_ready) return NativePickResult::Unavailable;

    const bool choose_folder = mode == BrowserMode::FirmwareDirectory;
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        choose_folder ? "Select Game.com firmware folder" : "Select Game.com cartridge ROM",
        nullptr,
        choose_folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
        choose_folder ? "Select Folder" : "Open",
        "Cancel");
    if (!dialog) return NativePickResult::Unavailable;
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), TRUE);
    if (!initial_folder.empty())
        (void)gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), initial_folder.c_str());
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);
    if (!choose_folder) {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Game.com cartridge images (*.bin)");
        gtk_file_filter_add_pattern(filter, "*.bin");
        gtk_file_filter_add_pattern(filter, "*.BIN");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        g_object_unref(filter);
        GtkFileFilter *all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "All files");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);
        g_object_unref(all);
    }
    const gint response = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog));
    NativePickResult result = NativePickResult::Cancelled;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            selected_path = filename;
            g_free(filename);
            result = NativePickResult::Selected;
        }
    }
    gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog));
    g_object_unref(dialog);
    return result;
#else
    (void)mode;
    (void)initial_folder;
    (void)selected_path;
    return NativePickResult::Unavailable;
#endif
}

void scan_directory(const fs::path& directory, BrowserMode mode,
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
        error = "Cannot read this folder: " + ec.message();
        return;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        std::error_code item_ec;
        const bool is_dir = it->is_directory(item_ec);
        if (item_ec) continue;
        const fs::path path = it->path();
        if (is_dir) {
            folders.push_back({path, path.filename().string(), true});
        } else if (mode == BrowserMode::CartridgeFile && it->is_regular_file(item_ec) &&
                   lower_extension(path) == ".bin") {
            files.push_back({path, path.filename().string(), false});
        }
    }
    const auto by_name = [](const BrowserEntry& a, const BrowserEntry& b) {
        std::string an = a.name, bn = b.name;
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return std::tolower(c); });
        return an < bn;
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    entries.insert(entries.end(), folders.begin(), folders.end());
    entries.insert(entries.end(), files.begin(), files.end());
}

void draw_button(SDL_Renderer *r, const SDL_Rect& rect, const std::string& label,
                 bool enabled, bool highlighted = false)
{
    const Color base = !enabled ? disabled : (highlighted ? accent : panel_light);
    fill(r, rect, base);
    outline(r, rect, enabled ? (highlighted ? accent : blue) : muted);
    const int scale = 2;
    const int label_w = static_cast<int>(label.size()) * 6 * scale - 2 * scale;
    const int label_h = 7 * scale;
    const int tx = rect.x + std::max(8, (rect.w - label_w) / 2);
    const int ty = rect.y + (rect.h - label_h) / 2;
    text(r, tx, ty, label, enabled && highlighted ? background : ink, scale,
         std::max(1, (rect.w - 12) / (6 * scale)));
}

bool run_browser(SDL_Window *window, SDL_Renderer *renderer, BrowserMode mode,
                 const std::string& initial_folder, std::string& result,
                 bool& quit_requested)
{
    fs::path current = initial_folder.empty() ? home_directory() : fs::path(initial_folder);
    std::error_code initial_ec;
    if (!fs::is_directory(current, initial_ec) || initial_ec) current = home_directory();
    std::vector<BrowserEntry> entries;
    std::string scan_error;
    int selected = 0, first_visible = 0;
    bool done = false, accepted = false;
    constexpr int row_height = 28;
    constexpr int visible_rows = 10;

    auto refresh = [&]() {
        scan_directory(current, mode, entries, scan_error);
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
        SDL_Rect modal{(w - 760) / 2, (h - 520) / 2, 760, 520};
        if (modal.x < 12) { modal.x = 12; modal.w = w - 24; }
        if (modal.y < 12) { modal.y = 12; modal.h = h - 24; }
        const SDL_Rect list{modal.x + 24, modal.y + 106, modal.w - 48, row_height * visible_rows};
        const SDL_Rect accept_button{modal.x + 24, modal.y + modal.h - 60, 260, 42};
        const SDL_Rect cancel_button{modal.x + modal.w - 184, modal.y + modal.h - 60, 160, 42};

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit_requested = true; done = true; break; }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) done = true;
                else if (key == SDLK_UP && selected > 0) --selected;
                else if (key == SDLK_DOWN && selected + 1 < static_cast<int>(entries.size())) ++selected;
                else if (key == SDLK_PAGEUP) selected = std::max(0, selected - visible_rows);
                else if (key == SDLK_PAGEDOWN)
                    selected = std::min(static_cast<int>(entries.size()) - 1, selected + visible_rows);
                else if (key == SDLK_BACKSPACE) {
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
                int ww = 0, wh = 0;
                SDL_GetWindowSize(window, &ww, &wh);
                const int mx = ww > 0 ? event.button.x * w / ww : event.button.x;
                const int my = wh > 0 ? event.button.y * h / wh : event.button.y;
                if (inside(mx, my, cancel_button)) done = true;
                else if (inside(mx, my, accept_button)) {
                    if (mode == BrowserMode::FirmwareDirectory) {
                        result = current.string(); accepted = true; done = true;
                    } else if (selected >= 0 && selected < static_cast<int>(entries.size()) &&
                               !entries[static_cast<std::size_t>(selected)].directory) {
                        result = entries[static_cast<std::size_t>(selected)].path.string();
                        accepted = true; done = true;
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

        if (selected < first_visible) first_visible = selected;
        if (selected >= first_visible + visible_rows) first_visible = selected - visible_rows + 1;
        first_visible = std::max(0, std::min(first_visible,
            std::max(0, static_cast<int>(entries.size()) - visible_rows)));

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        fill(renderer, modal, panel);
        outline(renderer, modal, blue);
        text(renderer, modal.x + 24, modal.y + 20,
             mode == BrowserMode::FirmwareDirectory ? "CHOOSE FIRMWARE FOLDER" : "CHOOSE GAME ROM", accent, 3, 60);
        text(renderer, modal.x + 24, modal.y + 62, short_path(current.string(), 83), muted, 2, 83);
        fill(renderer, list, background);
        for (int row = 0; row < visible_rows; ++row) {
            const int index = first_visible + row;
            if (index >= static_cast<int>(entries.size())) break;
            const BrowserEntry& entry = entries[static_cast<std::size_t>(index)];
            const SDL_Rect item{list.x + 2, list.y + row * row_height + 1, list.w - 4, row_height - 2};
            const bool chosen = index == selected;
            if (chosen) fill(renderer, item, panel_light);
            const std::string label = entry.directory ? "[FOLDER] " + entry.name : "[ROM] " + entry.name;
            text(renderer, item.x + 8, item.y + 6, label, chosen ? accent : ink, 2,
                 std::max(1, (item.w - 16) / 12));
        }
        if (!scan_error.empty()) text(renderer, modal.x + 24, modal.y + 392, scan_error, warning, 2, 80);
        const std::string hint = mode == BrowserMode::FirmwareDirectory
            ? "ARROWS SELECT  ENTER OPENS  BACKSPACE GOES UP  S USES THIS FOLDER"
            : "ARROWS SELECT  ENTER OPENS FILE OR FOLDER  BACKSPACE GOES UP";
        text(renderer, modal.x + 24, modal.y + modal.h - 92, hint, muted, 1,
             std::max(1, (modal.w - 48) / 6));
        draw_button(renderer, accept_button,
            mode == BrowserMode::FirmwareDirectory ? "USE THIS FOLDER" : "SELECT ROM",
            mode == BrowserMode::FirmwareDirectory ||
                (selected >= 0 && selected < static_cast<int>(entries.size()) &&
                 !entries[static_cast<std::size_t>(selected)].directory));
        draw_button(renderer, cancel_button, "CANCEL", true);
        SDL_RenderPresent(renderer);
        SDL_Delay(10);
    }
    return accepted;
}

bool select_path(SDL_Window *window, SDL_Renderer *renderer, BrowserMode mode,
                 const std::string& initial_folder, std::string& result,
                 bool& quit_requested, bool& used_internal_browser)
{
    used_internal_browser = false;
    const NativePickResult native = native_file_dialog(mode, initial_folder, result);
    if (native == NativePickResult::Selected) return true;
    if (native == NativePickResult::Cancelled) return false;
    used_internal_browser = true;
    return run_browser(window, renderer, mode, initial_folder, result, quit_requested);
}

std::string key_label(int key)
{
    if (key == SDLK_UNKNOWN) return "UNASSIGNED";
    const char *name = SDL_GetKeyName(static_cast<SDL_Keycode>(key));
    return name && *name ? name : "UNKNOWN";
}

bool run_controls(SDL_Window *window, SDL_Renderer *renderer,
                  GcInputBindings& bindings, bool& quit_requested,
                  std::string& status)
{
    bool done = false;
    int waiting_for = -1;
    constexpr int row_height = 32;
    constexpr int first_y = 108;

    while (!done && !quit_requested) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        const SDL_Rect defaults_button{48, h - 70, 210, 44};
        const SDL_Rect done_button{w - 190, h - 70, 142, 44};
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit_requested = true; done = true; break; }
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
                int ww = 0, wh = 0;
                SDL_GetWindowSize(window, &ww, &wh);
                const int mx = ww > 0 ? event.button.x * w / ww : event.button.x;
                const int my = wh > 0 ? event.button.y * h / wh : event.button.y;
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
                            first_y + static_cast<int>(i) * row_height + 2, 176, 30};
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

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        text(renderer, 48, 30, "CONTROL SETTINGS", accent, 4, 60);
        text(renderer, 52, 78, "SELECT CHANGE, THEN PRESS A KEY. MOUSE TOUCH AND ESC STAY FIXED.", muted, 1, 130);
        const SDL_Rect panel_rect{40, 100, w - 80, row_height * GC_CONTROL_COUNT + 18};
        fill(renderer, panel_rect, panel);
        outline(renderer, panel_rect, blue);
        for (unsigned i = 0; i < GC_CONTROL_COUNT; ++i) {
            const int y = first_y + static_cast<int>(i) * row_height;
            const SDL_Rect row{52, y, w - 104, row_height - 2};
            const SDL_Rect change_button{w - 232, y + 2, 176, 30};
            if (waiting_for == static_cast<int>(i)) fill(renderer, row, panel_light);
            text(renderer, 72, y + 9, action_names[i],
                 waiting_for == static_cast<int>(i) ? accent : ink, 2, 36);
            draw_button(renderer, change_button,
                waiting_for == static_cast<int>(i) ? "PRESS A KEY" : key_label(bindings.keycodes[i]),
                true, waiting_for == static_cast<int>(i));
        }
        text(renderer, 52, first_y + row_height * GC_CONTROL_COUNT + 28,
             status, status.find("ALREADY") != std::string::npos || status.find("COULD NOT") != std::string::npos
                 ? warning : muted, 1, 140);
        draw_button(renderer, defaults_button, "RESET DEFAULTS", true);
        draw_button(renderer, done_button, "DONE", true);
        SDL_RenderPresent(renderer);
        SDL_Delay(12);
    }
    return !quit_requested;
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
        const auto trim = [](std::string& s) {
            const std::size_t first = s.find_first_not_of(" \t\r");
            if (first == std::string::npos) { s.clear(); return; }
            const std::size_t last = s.find_last_not_of(" \t\r");
            s = s.substr(first, last - first + 1);
        };
        trim(name); trim(value);
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

bool gc_sdl_launcher_run(GcLaunchSelection& selection, GcInputBindings& bindings)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return false;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *window = SDL_CreateWindow("Game.com Emulator — Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 640,
        SDL_WINDOW_RESIZABLE);
    if (!window) { SDL_Quit(); return false; }
    SDL_SetWindowMinimumSize(window, 800, 600);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); return false; }

    bool done = false, quit = false, started = false;
    bool choose_firmware = false, choose_rom = false, choose_controls = false;
    std::string status = "BROWSE FOR YOUR OWN FIRMWARE AND OPTIONAL GAME ROM.";
    int focus = 0;
    while (!done && !quit) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        const SDL_Rect controls_button{w - 220, 40, 170, 46};
        const SDL_Rect firmware_button{w - 220, 166, 170, 46};
        const SDL_Rect rom_button{w - 220, 304, 170, 46};
        const SDL_Rect bios_button{48, h - 104, 230, 56};
        const SDL_Rect game_button{298, h - 104, 250, 56};
        const SDL_Rect quit_button{w - 190, h - 104, 142, 56};
        const bool firmware_ok = valid_firmware(selection.firmware_dir);
        const bool rom_ok = !selection.cartridge_path.empty();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { quit = true; break; }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) done = true;
                else if (key == SDLK_1) choose_firmware = true;
                else if (key == SDLK_2) choose_rom = true;
                else if (key == SDLK_3) choose_controls = true;
                else if (key == SDLK_TAB || key == SDLK_RIGHT || key == SDLK_LEFT) focus = (focus + 1) % 6;
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    if (focus == 0) choose_firmware = true;
                    else if (focus == 1) choose_rom = true;
                    else if (focus == 2) choose_controls = true;
                    else if (focus == 3 && firmware_ok) started = true;
                    else if (focus == 4 && firmware_ok && rom_ok) started = true;
                    else if (focus == 5) done = true;
                    if (started) done = true;
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                int ww = 0, wh = 0;
                SDL_GetWindowSize(window, &ww, &wh);
                const int mx = ww > 0 ? event.button.x * w / ww : event.button.x;
                const int my = wh > 0 ? event.button.y * h / wh : event.button.y;
                if (inside(mx, my, controls_button)) { choose_controls = true; focus = 2; }
                else if (inside(mx, my, firmware_button)) { choose_firmware = true; focus = 0; }
                else if (inside(mx, my, rom_button)) { choose_rom = true; focus = 1; }
                else if (inside(mx, my, bios_button) && firmware_ok) { started = true; done = true; }
                else if (inside(mx, my, game_button) && firmware_ok && rom_ok) { started = true; done = true; }
                else if (inside(mx, my, quit_button)) done = true;
            }
        }
        if (done || quit) break;

        if (choose_firmware) {
            choose_firmware = false;
            const std::string initial = selection.firmware_dir.empty()
                ? home_directory().string() : selection.firmware_dir;
            std::string path;
            bool used_fallback = false;
            if (select_path(window, renderer, BrowserMode::FirmwareDirectory,
                            initial, path, quit, used_fallback)) {
                selection.firmware_dir = path;
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
            bool used_fallback = false;
            if (select_path(window, renderer, BrowserMode::CartridgeFile,
                            initial, path, quit, used_fallback)) {
                selection.cartridge_path = path;
                status = "GAME ROM SELECTED. ROMS ARE NOT INCLUDED IN THE APPIMAGE.";
                if (used_fallback) status = "IN-APP PICKER: " + status;
            }
        }
        if (choose_controls) {
            choose_controls = false;
            status = "CONTROLS SAVE AUTOMATICALLY TO YOUR USER CONFIGURATION.";
            (void)run_controls(window, renderer, bindings, quit, status);
            focus = 2;
        }
        if (quit) break;

        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, background.a);
        SDL_RenderClear(renderer);
        text(renderer, 48, 42, "GAME.COM EMULATOR", accent, 4, 60);
        draw_button(renderer, controls_button, "CONTROLS", true, focus == 2);
        text(renderer, 52, 86, "SELECT YOUR FIRMWARE AND OPTIONAL CARTRIDGE", muted, 2, 88);

        const SDL_Rect fw_panel{40, 128, w - 80, 108};
        const SDL_Rect rom_panel{40, 266, w - 80, 108};
        fill(renderer, fw_panel, panel); outline(renderer, fw_panel, blue);
        fill(renderer, rom_panel, panel); outline(renderer, rom_panel, blue);
        text(renderer, 60, 146, "1  SYSTEM FIRMWARE", ink, 2, 45);
        text(renderer, 60, 180,
             selection.firmware_dir.empty() ? "NO FIRMWARE FOLDER SELECTED" : short_path(selection.firmware_dir, 58),
             firmware_ok ? accent : warning, 2, 60);
        draw_button(renderer, firmware_button, "BROWSE...", true, focus == 0);
        text(renderer, 60, 284, "2  GAME CARTRIDGE (OPTIONAL)", ink, 2, 55);
        text(renderer, 60, 318,
             selection.cartridge_path.empty() ? "NO GAME SELECTED (BOOT BIOS MENU)" : short_path(selection.cartridge_path, 58),
             rom_ok ? accent : muted, 2, 60);
        draw_button(renderer, rom_button, "BROWSE...", true, focus == 1);

        text(renderer, 50, 407, status, status.find("MISSING") != std::string::npos ? warning : muted, 2, 110);
        text(renderer, 50, 436, "FIRMWARE FOLDER MUST CONTAIN INTERNAL.BIN AND EXTERNAL.BIN.", muted, 1, 120);
        draw_button(renderer, bios_button, "BOOT BIOS", firmware_ok, focus == 3);
        draw_button(renderer, game_button, "BOOT GAME", firmware_ok && rom_ok, focus == 4);
        draw_button(renderer, quit_button, "QUIT", true, focus == 5);
        text(renderer, 50, h - 34, "KEYBOARD: 1 FIRMWARE  2 GAME  3 CONTROLS  TAB CYCLE  ENTER SELECT", muted, 1, 120);
        SDL_RenderPresent(renderer);
        SDL_Delay(12);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return started && !quit;
}
