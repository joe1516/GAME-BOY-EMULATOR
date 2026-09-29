/*
 * frontend_sdl.c - SDL2 desktop frontend (window, input, frame pacing, saves).
 *
 *   gbplay game.gb [--frames N] [--screenshot out.bmp] [--scale N]
 *
 * --frames and --screenshot exist so the emulator can be smoke-tested
 * without a human (e.g. with SDL_VIDEODRIVER=dummy).
 */
#include "gb.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_INTERVAL_FRAMES 600            /* ~10 seconds */

typedef struct {
    SDL_Scancode key;
    SDL_Scancode alt_key;
    int button;
} KeyBinding;

static const KeyBinding BINDINGS[] = {
    { SDL_SCANCODE_RIGHT,     SDL_SCANCODE_D,         GB_BTN_RIGHT  },
    { SDL_SCANCODE_LEFT,      SDL_SCANCODE_A,         GB_BTN_LEFT   },
    { SDL_SCANCODE_UP,        SDL_SCANCODE_W,         GB_BTN_UP     },
    { SDL_SCANCODE_DOWN,      SDL_SCANCODE_S,         GB_BTN_DOWN   },
    { SDL_SCANCODE_X,         SDL_SCANCODE_K,         GB_BTN_A      },
    { SDL_SCANCODE_Z,         SDL_SCANCODE_J,         GB_BTN_B      },
    { SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RSHIFT,    GB_BTN_SELECT },
    { SDL_SCANCODE_RETURN,    SDL_SCANCODE_SPACE,     GB_BTN_START  },
};

static GB   gb;
static char rom_path[1024];
static char save_path[1100];
static int  screenshot_count;

static void make_save_path(void)
{
    snprintf(save_path, sizeof save_path, "%s.sav", rom_path);
}

static void write_save(void)
{
    if (gb.cart.battery && cart_write_save(&gb.cart, save_path))
        printf("Saved cartridge RAM to %s\n", save_path);
}

static bool load_game(const char *path)
{
    char err[128];

    write_save();                               /* flush the previous game */

    if (!gb_load_rom(&gb, path, err, sizeof err)) {
        fprintf(stderr, "Error: %s\n", err);
        return false;
    }

    snprintf(rom_path, sizeof rom_path, "%s", path);
    make_save_path();
    if (cart_load_save(&gb.cart, save_path))
        printf("Loaded cartridge RAM from %s\n", save_path);

    printf("Loaded '%s' (%zu KB ROM, %zu KB RAM, mapper %d)\n", gb.cart.title,
           gb.cart.rom_size / 1024, gb.cart.ram_size / 1024, (int)gb.cart.mapper);
    return true;
}

static bool save_screenshot(const char *path)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(
        gb.framebuffer, GB_SCREEN_W, GB_SCREEN_H, 32, GB_SCREEN_W * 4,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (!surf) return false;
    int rc = SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    return rc == 0;
}

static void usage(const char *argv0)
{
    printf("Usage: %s game.gb [--scale N] [--frames N] [--screenshot out.bmp]\n\n"
           "Controls\n"
           "  D-pad       Arrow keys / WASD\n"
           "  A / B       X / Z   (or K / J)\n"
           "  Start       Enter   (or Space)\n"
           "  Select      Backspace (or Right Shift)\n"
           "  Pause       P\n"
           "  Fast-fwd    hold Tab\n"
           "  Reset       F2\n"
           "  Screenshot  F12\n"
           "  Quit        Esc\n"
           "  Load ROM    drag a .gb file onto the window\n", argv0);
}

int main(int argc, char **argv)
{
    const char *path = NULL, *shot_path = NULL;
    int scale = 4;
    long max_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc)            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)      max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)  shot_path = argv[++i];
        else if (argv[i][0] != '-' && !path)                        path = argv[i];
        else { usage(argv[0]); return 1; }
    }
    if (!path) { usage(argv[0]); return 1; }
    if (scale < 1) scale = 1;

    gb_init(&gb);
    if (!load_game(path)) return 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");        /* crisp pixels */

    SDL_Window *window = SDL_CreateWindow("Game Boy Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GB_SCREEN_W * scale, GB_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "Cannot create window: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "Cannot create renderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, GB_SCREEN_W, GB_SCREEN_H);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, GB_SCREEN_W, GB_SCREEN_H);
    if (!texture) {
        fprintf(stderr, "Cannot create texture: %s\n", SDL_GetError());
        return 1;
    }

    const uint64_t freq = SDL_GetPerformanceFrequency();
    const double frame_ticks = (double)freq * GB_CYCLES_PER_FRAME / GB_CLOCK_HZ;   /* 59.73 Hz */
    double next_frame = (double)SDL_GetPerformanceCounter();

    bool running = true, paused = false;
    long frame = 0, fps_frames = 0;
    uint64_t fps_start = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_DROPFILE:
                if (load_game(ev.drop.file)) frame = 0;
                SDL_free(ev.drop.file);
                break;
            case SDL_KEYDOWN:
                if (ev.key.repeat) break;
                switch (ev.key.keysym.scancode) {
                    case SDL_SCANCODE_ESCAPE: running = false; break;
                    case SDL_SCANCODE_P:      paused = !paused; break;
                    case SDL_SCANCODE_F2:
                        write_save();
                        gb_reset(&gb);
                        printf("Reset\n");
                        break;
                    case SDL_SCANCODE_F12: {
                        char name[1200];
                        snprintf(name, sizeof name, "%s-%03d.bmp", rom_path, ++screenshot_count);
                        if (save_screenshot(name)) printf("Screenshot: %s\n", name);
                        else fprintf(stderr, "Screenshot failed: %s\n", SDL_GetError());
                        break;
                    }
                    default: break;
                }
                break;
            default:
                break;
            }
        }

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        for (size_t i = 0; i < sizeof BINDINGS / sizeof BINDINGS[0]; i++) {
            bool down = keys[BINDINGS[i].key] || keys[BINDINGS[i].alt_key];
            gb_set_button(&gb, BINDINGS[i].button, down);
        }
        bool fast = keys[SDL_SCANCODE_TAB];

        if (!paused) {
            gb_run_frame(&gb);
            frame++;

            if (gb.cpu.locked) {
                fprintf(stderr, "CPU locked up: illegal opcode 0x%02X\n", gb.cpu.locked_opcode);
                paused = true;
            }
            if (gb.cart.battery && frame % SAVE_INTERVAL_FRAMES == 0) write_save();
        }

        SDL_UpdateTexture(texture, NULL, gb.framebuffer, GB_SCREEN_W * 4);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Show the frame rate in the title bar once per second. */
        fps_frames++;
        uint64_t now = SDL_GetPerformanceCounter();
        if (now - fps_start >= freq) {
            char title[160];
            snprintf(title, sizeof title, "%s - %.1f FPS%s%s", gb.cart.title,
                     (double)fps_frames * freq / (double)(now - fps_start),
                     paused ? " [paused]" : "", fast ? " [fast]" : "");
            SDL_SetWindowTitle(window, title);
            fps_frames = 0;
            fps_start = now;
        }

        if (max_frames > 0 && frame >= max_frames) running = false;

        /* Frame pacing: sleep most of the wait, spin for the last bit. */
        if (fast) {
            next_frame = (double)SDL_GetPerformanceCounter();
        } else {
            next_frame += frame_ticks;
            double remaining = next_frame - (double)SDL_GetPerformanceCounter();
            if (remaining > 0) {
                Uint32 ms = (Uint32)(remaining * 1000.0 / (double)freq);
                if (ms > 1) SDL_Delay(ms - 1);
                while ((double)SDL_GetPerformanceCounter() < next_frame) { }
            } else if (remaining < -5 * frame_ticks) {
                next_frame = (double)SDL_GetPerformanceCounter();   /* fell behind: resync */
            }
        }
    }

    if (shot_path) {
        if (save_screenshot(shot_path)) printf("Screenshot: %s\n", shot_path);
        else fprintf(stderr, "Screenshot failed: %s\n", SDL_GetError());
    }

    write_save();
    gb_free(&gb);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
