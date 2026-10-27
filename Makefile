CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude
WARNINGS = -Wall -Wextra -Wpedantic
C_WARNINGS = -std=c11 $(WARNINGS)
CXX_WARNINGS = -std=c++17 $(WARNINGS)
HOST_OS := $(shell uname -s 2>/dev/null)
ifeq ($(HOST_OS),Linux)
PORTABLE_LDFLAGS ?= -static
else
PORTABLE_LDFLAGS ?=
endif

CORE_SRC = src/core.c src/disasm.c src/video.c
CORE_OBJ = $(CORE_SRC:.c=.o)
DEMO_OBJ = src/homebrew_demo.o
STANDALONE_CPU_OBJ = src/standalone/sm8521_cpu.o
AUDIO_SYNTH_OBJ = src/standalone/audio_synth.o
SERIAL_LINK_OBJ = src/standalone/serial_link.o
SERIAL_LINK_STATIC_OBJ = src/standalone/serial_link_static.o

ifeq ($(HOST_OS),Linux)
MINIAUDIO_LDLIBS = -ldl -lpthread -lm
SDL_PORTABLE_LDFLAGS ?= -static-libstdc++ -static-libgcc
USE_GTK3 ?= 0
ifeq ($(USE_GTK3),1)
GTK3_AVAILABLE := $(shell pkg-config --exists gtk+-3.0 2>/dev/null && echo 1)
ifeq ($(GTK3_AVAILABLE),1)
GTK3_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK3_LDLIBS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
endif
endif
else ifeq ($(HOST_OS),Darwin)
MINIAUDIO_LDLIBS = -lpthread -lm -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
endif

.PHONY: all test test-audio test-uart test-serial clean sdl
all: gamecom-debug gamecom-demo gamecom-standalone-demo gamecom-boot

gamecom-debug: $(CORE_OBJ) src/monitor.o
	$(CC) $(CFLAGS) $(C_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

gamecom-demo: $(CORE_OBJ) $(DEMO_OBJ) src/demo_main.o
	$(CC) $(CFLAGS) $(C_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

gamecom-standalone-demo: $(CORE_OBJ) $(DEMO_OBJ) $(STANDALONE_CPU_OBJ) src/standalone/demo_main.o
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

gamecom-boot: $(CORE_OBJ) $(STANDALONE_CPU_OBJ) src/standalone/boot_main.o
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

test-core: $(CORE_OBJ) $(DEMO_OBJ) tests/test_core.o
	$(CC) $(CFLAGS) $(C_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

gamecom-test-standalone: $(CORE_OBJ) $(DEMO_OBJ) $(STANDALONE_CPU_OBJ) tests/test_standalone_cpu.o
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

gamecom-test-video: $(CORE_OBJ) tests/test_video.o
	$(CC) $(CFLAGS) $(C_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

test-audio: $(CORE_OBJ) $(AUDIO_SYNTH_OBJ) tests/test_audio.o
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

test-uart: $(CORE_OBJ) $(STANDALONE_CPU_OBJ) tests/test_uart.o
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

$(SERIAL_LINK_STATIC_OBJ): src/standalone/serial_link.cpp src/standalone/serial_link.hpp src/standalone/sm8521_cpu.hpp include/gamecom.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXX_WARNINGS) -Isrc/standalone -DGAMECOM_STATIC_NO_DNS=1 -c -o $@ $<

test-serial: $(CORE_OBJ) $(STANDALONE_CPU_OBJ) $(SERIAL_LINK_STATIC_OBJ) tests/test_serial_link.o
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(PORTABLE_LDFLAGS) -o $@ $^

test: test-core gamecom-test-standalone gamecom-test-video test-audio test-uart test-serial
	./test-core
	./gamecom-test-standalone
	./gamecom-test-video
	./test-audio
	./test-uart
	./test-serial

src/homebrew_demo.o src/demo_main.o: include/gamecom_demo.h
src/demo_main.o src/video.o: include/gamecom_video.h
src/homebrew_demo.o: include/gamecom.h
src/standalone/sm8521_cpu.o src/standalone/demo_main.o src/standalone/boot_main.o: src/standalone/sm8521_cpu.hpp src/standalone/sound_events.hpp include/gamecom.h
src/standalone/audio_synth.o tests/test_audio.o: src/standalone/audio_synth.hpp src/standalone/sound_events.hpp include/gamecom.h
src/standalone/serial_link.o tests/test_serial_link.o: src/standalone/serial_link.hpp src/standalone/sm8521_cpu.hpp src/standalone/sound_events.hpp include/gamecom.h
tests/test_uart.o: src/standalone/sm8521_cpu.hpp src/standalone/sound_events.hpp
src/standalone/boot_main.o: include/gamecom_video.h
src/standalone/sm8521_cpu.o: src/standalone/sm8521_ops.inc
src/standalone/demo_main.o src/sdl_main.o tests/test_standalone_cpu.o: include/gamecom_demo.h
src/sdl_main.o src/sdl_frontend.o: include/gamecom_sdl.hpp src/standalone/sm8521_cpu.hpp
src/sdl_frontend.o: include/gamecom_video.h src/standalone/audio_synth.hpp src/standalone/audio_output.hpp src/standalone/serial_link.hpp
src/audio_output.o: src/standalone/audio_output.cpp src/standalone/audio_output.hpp third_party/miniaudio/miniaudio.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Ithird_party/miniaudio -w -c -o $@ $<
src/standalone/demo_main.o tests/test_standalone_cpu.o: include/gamecom_demo.h
tests/test_standalone_cpu.o: src/standalone/sm8521_cpu.hpp
tests/test_video.o: include/gamecom_video.h

%.o: %.c include/gamecom.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(C_WARNINGS) -c -o $@ $<

%.o: %.cpp include/gamecom.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXX_WARNINGS) -Isrc/standalone -c -o $@ $<

ifeq ($(SDL_AVAILABLE),1)
SDL_OBJECTS = src/sdl_main.o src/sdl_frontend.o src/sdl_launcher.o src/audio_output.o $(AUDIO_SYNTH_OBJ) $(SERIAL_LINK_OBJ)
src/sdl_main.o src/sdl_frontend.o src/sdl_launcher.o: CPPFLAGS += $(shell pkg-config --cflags sdl2 2>/dev/null)
ifeq ($(GTK3_AVAILABLE),1)
src/sdl_launcher.o: CPPFLAGS += $(GTK3_CFLAGS) -DGAMECOM_HAVE_GTK3=1
endif
src/sdl_main.o src/sdl_frontend.o src/sdl_launcher.o: include/gamecom_launcher.hpp
src/sdl_frontend.o src/sdl_launcher.o: src/sdl_ui.hpp
gamecom-sdl: $(CORE_OBJ) $(DEMO_OBJ) $(STANDALONE_CPU_OBJ) $(SDL_OBJECTS)
	$(CXX) $(CXXFLAGS) $(CXX_WARNINGS) $(SDL_PORTABLE_LDFLAGS) -o $@ $^ $(shell pkg-config --libs sdl2 2>/dev/null) $(GTK3_LDLIBS) $(MINIAUDIO_LDLIBS)
endif

sdl:
	@if pkg-config --exists sdl2; then \
	  $(MAKE) SDL_AVAILABLE=1 gamecom-sdl; \
	else echo "SDL2 development files not found; install SDL2, then run 'make sdl'."; exit 1; fi

clean:
	rm -f $(CORE_OBJ) $(DEMO_OBJ) $(STANDALONE_CPU_OBJ) $(AUDIO_SYNTH_OBJ) $(SERIAL_LINK_OBJ) $(SERIAL_LINK_STATIC_OBJ) src/audio_output.o src/monitor.o src/demo_main.o \
	  src/standalone/demo_main.o src/standalone/boot_main.o src/sdl_main.o src/sdl_frontend.o src/sdl_launcher.o \
	  tests/test_core.o tests/test_standalone_cpu.o tests/test_video.o tests/test_audio.o tests/test_uart.o tests/test_serial_link.o \
	  gamecom-debug gamecom-demo gamecom-standalone-demo gamecom-boot test-core gamecom-test-standalone gamecom-test-video test-audio test-uart test-serial gamecom-sdl
