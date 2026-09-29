/* gb.c - Machine glue: power-on state, main stepping loop, timer, joypad. */
#include "gb.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Timer                                                              */
/* ------------------------------------------------------------------ */

/*
 * DIV is the upper byte of a free-running 16-bit counter. TIMA increments
 * on the falling edge of one bit of that counter, chosen by TAC bits 0-1.
 */
static void timer_step(GB *gb, int cycles)
{
    static const uint16_t bits[4] = { 1u << 9, 1u << 3, 1u << 5, 1u << 7 };

    for (int i = 0; i < cycles; i++) {
        uint16_t old = gb->div++;

        if (gb->tac & 4) {
            uint16_t mask = bits[gb->tac & 3];
            if ((old & mask) && !(gb->div & mask)) {
                if (++gb->tima == 0) {
                    gb->tima = gb->tma;
                    gb->iflag |= GB_INT_TIMER;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void gb_init(GB *gb)
{
    memset(gb, 0, sizeof *gb);
    gb_reset(gb);
}

void gb_reset(GB *gb)
{
    /* Keep the cartridge (and its battery RAM) and host callbacks. */
    Cart           cart = gb->cart;
    SerialCallback cb   = gb->serial_cb;
    void          *user = gb->serial_user;

    memset(gb, 0, sizeof *gb);

    gb->cart = cart;
    gb->cart.rom_bank = 1;
    gb->cart.ram_bank = 0;
    gb->cart.mode = 0;
    gb->cart.ram_enabled = false;
    gb->serial_cb = cb;
    gb->serial_user = user;

    /* State the DMG boot ROM leaves behind, so games can start at 0x0100
     * without needing a copy of the (copyrighted) boot ROM. */
    memset(gb->io, 0xFF, sizeof gb->io);
    gb->io[0x10] = 0x80; gb->io[0x11] = 0xBF; gb->io[0x12] = 0xF3;
    gb->io[0x14] = 0xBF; gb->io[0x16] = 0x3F; gb->io[0x19] = 0xBF;
    gb->io[0x1A] = 0x7F; gb->io[0x1B] = 0xFF; gb->io[0x1C] = 0x9F;
    gb->io[0x1E] = 0xBF; gb->io[0x20] = 0xFF; gb->io[0x23] = 0xBF;
    gb->io[0x24] = 0x77; gb->io[0x25] = 0xF3; gb->io[0x26] = 0xF1;

    gb->div = 0xABCC;
    gb->iflag = 0x01;
    gb->joyp_select = 0x30;
    gb->sc = 0x7E;
    gb->bgp = 0xFC;
    gb->obp0 = 0xFF;
    gb->obp1 = 0xFF;

    cpu_reset(&gb->cpu);
    ppu_reset(gb);
}

bool gb_load_rom(GB *gb, const char *path, char *err, size_t errlen)
{
    Cart cart;
    if (!cart_load_file(&cart, path, err, errlen)) return false;

    cart_free(&gb->cart);
    gb->cart = cart;
    gb_reset(gb);
    return true;
}

void gb_free(GB *gb)
{
    cart_free(&gb->cart);
}

/* ------------------------------------------------------------------ */
/* Running                                                            */
/* ------------------------------------------------------------------ */

int gb_step(GB *gb)
{
    int cycles = cpu_step(gb);
    timer_step(gb, cycles);
    ppu_step(gb, cycles);
    gb->cycles += (uint64_t)cycles;
    return cycles;
}

void gb_run_frame(GB *gb)
{
    uint64_t start = gb->cycles;
    gb->frame_ready = false;

    while (!gb->frame_ready) {
        gb_step(gb);
        if (gb->cpu.locked) break;

        uint64_t ran = gb->cycles - start;
        /* With the LCD off no VBlank happens; still emit one frame's worth. */
        if (!(gb->lcdc & 0x80) && ran >= GB_CYCLES_PER_FRAME) break;
        if (ran >= 2 * GB_CYCLES_PER_FRAME) break;      /* safety net */
    }
}

void gb_set_button(GB *gb, int button, bool pressed)
{
    uint8_t bit = (uint8_t)(1u << button);
    bool was = (gb->buttons & bit) != 0;

    if (pressed) {
        gb->buttons |= bit;
        if (!was) {
            /* Joypad interrupt: a selected input line goes high -> low. */
            bool is_dpad = button <= GB_BTN_DOWN;
            bool selected = is_dpad ? !(gb->joyp_select & 0x10)
                                    : !(gb->joyp_select & 0x20);
            if (selected) gb->iflag |= GB_INT_JOYPAD;
        }
    } else {
        gb->buttons &= (uint8_t)~bit;
    }
}
