#ifndef GAMECOM_LAUNCHER_HPP
#define GAMECOM_LAUNCHER_HPP

#include <string>

struct GcLaunchSelection {
    std::string firmware_dir;
    std::string cartridge_path;
};

// Opens a small SDL launcher. Returns true when the user chose to boot; false on quit.
bool gc_sdl_launcher_run(GcLaunchSelection& selection);

#endif
