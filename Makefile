# Game Boy (DMG) emulator - build script
#
#   make            build gbplay (SDL2 window) and gbcli (headless)
#   make demo       generate roms/demo.gb (homebrew demo ROM)
#   make test       build and run the self-test suite
#   make clean
#
# Needs: a C11 compiler, make, and SDL2 development files (for gbplay only).
# Windows (MSYS2 UCRT64):  pacman -S mingw-w64-ucrt-x86_64-gcc make mingw-w64-ucrt-x86_64-SDL2

CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CFLAGS  += -Isrc

ifeq ($(OS),Windows_NT)
EXE := .exe
endif

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

CORE := src/cart.c src/bus.c src/cpu.c src/ppu.c src/gb.c
HDR  := src/gb.h

all: gbcli$(EXE) gbplay$(EXE)

gbcli$(EXE): src/cli.c $(CORE) $(HDR)
	$(CC) $(CFLAGS) -o $@ src/cli.c $(CORE)

gbplay$(EXE): src/frontend_sdl.c $(CORE) $(HDR)
	@if [ -z "$(SDL_LIBS)" ]; then echo "SDL2 not found (sdl2-config missing). Install SDL2 development files."; exit 1; fi
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ src/frontend_sdl.c $(CORE) $(SDL_LIBS)

make_demo_rom$(EXE): tools/make_demo_rom.c
	$(CC) $(CFLAGS) -o $@ tools/make_demo_rom.c

roms/demo.gb: make_demo_rom$(EXE)
	./make_demo_rom$(EXE) roms/demo.gb

demo: roms/demo.gb

selftest$(EXE): tests/selftest.c $(CORE) $(HDR)
	$(CC) $(CFLAGS) -o $@ tests/selftest.c $(CORE)

test: selftest$(EXE) roms/demo.gb
	./selftest$(EXE)

clean:
	rm -f gbcli gbplay selftest make_demo_rom *.exe roms/demo.gb

.PHONY: all demo test clean
