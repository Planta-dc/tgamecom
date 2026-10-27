#ifndef GAMECOM_SDL_HPP
#define GAMECOM_SDL_HPP

#include "gamecom.h"
#include "sm8521_cpu.hpp"
#include "gamecom_launcher.hpp"

int gc_sdl_frontend_run(GcMachine& machine, standalone::Sm8521Cpu& cpu,
                        const GcInputBindings& bindings);

#endif
