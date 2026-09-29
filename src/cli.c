/*
 * cli.c - Headless runner (no window needed).
 *
 * Useful for running test ROMs (Blargg's tests print their result over the
 * serial port), tracing instructions, and saving a screenshot.
 *
 *   gbcli game.gb [--frames N] [--cycles N] [--serial] [--trace N]
 *                 [--screenshot out.ppm]
 *
 * Exit status: 0 = ran fine / test passed, 1 = error or test failed,
 *              2 = a test ROM never finished.
 */
#include "gb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char   text[8192];
    size_t len;
    bool   echo;
} SerialLog;

static void on_serial(uint8_t byte, void *user)
{
    SerialLog *log = user;
    if (log->len < sizeof log->text - 1) {
        log->text[log->len++] = (char)byte;
        log->text[log->len] = '\0';
    }
    if (log->echo) {
        putchar(byte);
        fflush(stdout);
    }
}

static void usage(const char *argv0)
{
    printf("Usage: %s game.gb [options]\n"
           "  --frames N          run N frames (default 600)\n"
           "  --cycles N          run N T-cycles instead of frames\n"
           "  --serial            print serial output; stop on Passed/Failed\n"
           "  --trace N           print the first N instructions (gameboy-doctor format)\n"
           "  --screenshot FILE   write the final screen as a PPM image\n",
           argv0);
}

static bool save_ppm(const GB *gb, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    fprintf(fp, "P6\n%d %d\n255\n", GB_SCREEN_W, GB_SCREEN_H);
    for (int i = 0; i < GB_SCREEN_W * GB_SCREEN_H; i++) {
        uint32_t px = gb->framebuffer[i];
        uint8_t rgb[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    return true;
}

static void print_trace(GB *gb)
{
    const CPU *c = &gb->cpu;
    printf("A:%02X F:%02X B:%02X C:%02X D:%02X E:%02X H:%02X L:%02X "
           "SP:%04X PC:%04X PCMEM:%02X,%02X,%02X,%02X\n",
           c->a, c->f, c->b, c->c, c->d, c->e, c->h, c->l, c->sp, c->pc,
           gb_read(gb, c->pc), gb_read(gb, (uint16_t)(c->pc + 1)),
           gb_read(gb, (uint16_t)(c->pc + 2)), gb_read(gb, (uint16_t)(c->pc + 3)));
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *shot_path = NULL;
    long frames = 600, max_cycles = 0, trace = 0;
    bool serial = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)          frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc)     max_cycles = atol(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc)      trace = atol(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--serial"))                     serial = true;
        else if (argv[i][0] != '-' && !rom_path)                   rom_path = argv[i];
        else { usage(argv[0]); return 1; }
    }
    if (!rom_path) { usage(argv[0]); return 1; }

    static GB gb;                               /* large: keep off the stack */
    static SerialLog log;
    char err[128];

    gb_init(&gb);
    if (!gb_load_rom(&gb, rom_path, err, sizeof err)) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    log.echo = serial;
    gb.serial_cb = on_serial;
    gb.serial_user = &log;

    if (!serial || trace)
        fprintf(stderr, "Loaded '%s' (%zu KB, mapper %d)\n", gb.cart.title,
                gb.cart.rom_size / 1024, (int)gb.cart.mapper);

    int status = 0;
    long done_frames = 0;
    uint64_t start = gb.cycles;
    bool finished = false;

    if (max_cycles > 0 || trace > 0) {
        /* Instruction-level loop (needed for --trace). */
        long limit = max_cycles > 0 ? max_cycles : (long)frames * GB_CYCLES_PER_FRAME;
        while ((long)(gb.cycles - start) < limit && !gb.cpu.locked) {
            if (trace > 0) { print_trace(&gb); trace--; }
            gb_step(&gb);
            if (serial && (strstr(log.text, "Passed") || strstr(log.text, "Failed"))) {
                finished = true;
                break;
            }
        }
    } else {
        while (done_frames < frames && !gb.cpu.locked) {
            gb_run_frame(&gb);
            done_frames++;
            if (serial && (strstr(log.text, "Passed") || strstr(log.text, "Failed"))) {
                finished = true;
                break;
            }
        }
    }

    if (gb.cpu.locked) {
        fprintf(stderr, "\nCPU locked up: illegal opcode 0x%02X near PC=0x%04X\n",
                gb.cpu.locked_opcode, gb.cpu.pc);
        status = 1;
    }

    if (serial) {
        printf("\n");
        if (strstr(log.text, "Failed"))      status = 1;
        else if (strstr(log.text, "Passed")) status = status ? status : 0;
        else if (!finished)                  status = status ? status : 2;
    }

    if (shot_path) {
        if (save_ppm(&gb, shot_path)) fprintf(stderr, "Screenshot saved to %s\n", shot_path);
        else { fprintf(stderr, "Could not write %s\n", shot_path); status = 1; }
    }

    gb_free(&gb);
    return status;
}
