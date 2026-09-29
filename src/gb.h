/*
 * gb.h - Core data structures and public API of the Game Boy (DMG) emulator.
 *
 * Hardware emulated: Sharp LR35902 CPU, memory bus, cartridge mappers
 * (ROM only, MBC1, MBC3 without RTC, MBC5), timer, joypad, serial port
 * (output only), and a scanline PPU (background, window, sprites).
 * Not emulated: sound (APU), Game Boy Color, link cable, MBC2, MBC3 RTC.
 */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define GB_SCREEN_W          160
#define GB_SCREEN_H          144
#define GB_CLOCK_HZ          4194304
#define GB_CYCLES_PER_FRAME  70224      /* 154 lines * 456 dots */

/* Joypad buttons: bit positions in GB.buttons (1 = pressed). */
enum {
    GB_BTN_RIGHT = 0, GB_BTN_LEFT = 1, GB_BTN_UP = 2, GB_BTN_DOWN = 3,
    GB_BTN_A = 4, GB_BTN_B = 5, GB_BTN_SELECT = 6, GB_BTN_START = 7
};

/* Interrupt bits (IE / IF registers). */
enum {
    GB_INT_VBLANK = 1 << 0, GB_INT_STAT = 1 << 1, GB_INT_TIMER = 1 << 2,
    GB_INT_SERIAL = 1 << 3, GB_INT_JOYPAD = 1 << 4
};

/* ------------------------------------------------------------------ */
/* Cartridge                                                          */
/* ------------------------------------------------------------------ */

typedef enum { MBC_NONE, MBC_1, MBC_3, MBC_5 } MapperType;

typedef struct {
    uint8_t   *rom;
    size_t     rom_size;
    unsigned   rom_banks;       /* number of 16 KB banks (power of two) */
    uint8_t   *ram;
    size_t     ram_size;
    MapperType mapper;
    bool       battery;
    bool       ram_enabled;
    unsigned   rom_bank;        /* MBC1: 5-bit reg, MBC3: 7-bit, MBC5: 9-bit */
    unsigned   ram_bank;        /* MBC1: 2-bit reg, MBC3/5: RAM bank        */
    unsigned   mode;            /* MBC1 banking mode                        */
    uint8_t    type_byte;       /* header byte 0x147                        */
    char       title[17];
} Cart;

/* Both return false and fill `err` on failure. */
bool cart_load_file(Cart *cart, const char *path, char *err, size_t errlen);
bool cart_load_memory(Cart *cart, const uint8_t *data, size_t size,
                      char *err, size_t errlen);
void cart_free(Cart *cart);
bool cart_load_save(Cart *cart, const char *path);          /* .sav -> RAM */
bool cart_write_save(const Cart *cart, const char *path);   /* RAM -> .sav */

uint8_t cart_read_rom(const Cart *cart, uint16_t addr);
uint8_t cart_read_ram(const Cart *cart, uint16_t addr);
void    cart_write_ram(Cart *cart, uint16_t addr, uint8_t value);
void    cart_write_control(Cart *cart, uint16_t addr, uint8_t value);

/* ------------------------------------------------------------------ */
/* CPU                                                                */
/* ------------------------------------------------------------------ */

#define FLAG_Z 0x80
#define FLAG_N 0x40
#define FLAG_H 0x20
#define FLAG_C 0x10

typedef struct {
    uint8_t  a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    bool     ime;               /* interrupt master enable                  */
    uint8_t  ei_delay;          /* EI takes effect after the next opcode    */
    bool     halted;
    bool     halt_bug;          /* next opcode fetch does not advance PC    */
    bool     locked;            /* executed an illegal opcode               */
    uint8_t  locked_opcode;
} CPU;

/* ------------------------------------------------------------------ */
/* Whole machine                                                      */
/* ------------------------------------------------------------------ */

typedef void (*SerialCallback)(uint8_t byte, void *user);

typedef struct GB {
    CPU  cpu;
    Cart cart;

    uint8_t vram[0x2000];
    uint8_t wram[0x2000];
    uint8_t oam[0xA0];
    uint8_t hram[0x80];         /* 0xFF80-0xFFFE (+ unused slot)            */
    uint8_t io[0x80];           /* registers not handled by a named field   */
    uint8_t ie, iflag;

    /* Timer */
    uint16_t div;               /* internal 16-bit counter, DIV = div >> 8  */
    uint8_t  tima, tma, tac;

    /* Joypad */
    uint8_t buttons;            /* 1 = pressed, see GB_BTN_*                */
    uint8_t joyp_select;        /* bits 4-5 of last write to 0xFF00         */

    /* Serial */
    uint8_t        sb, sc;
    SerialCallback serial_cb;
    void          *serial_user;

    /* PPU */
    uint8_t  lcdc, stat, scy, scx, ly, lyc, dma, bgp, obp0, obp1, wy, wx;
    uint8_t  ppu_mode;          /* 0 HBlank, 1 VBlank, 2 OAM scan, 3 draw   */
    int      ppu_dots;
    int      window_line;
    bool     stat_line;
    bool     frame_ready;
    uint32_t framebuffer[GB_SCREEN_W * GB_SCREEN_H];   /* 0xAARRGGBB        */

    uint64_t cycles;            /* total T-cycles executed                  */
} GB;

/* gb.c */
void gb_init(GB *gb);                       /* zero everything (no cart)   */
bool gb_load_rom(GB *gb, const char *path, char *err, size_t errlen);
void gb_reset(GB *gb);                      /* power-on state, keep cart   */
void gb_free(GB *gb);
int  gb_step(GB *gb);                       /* one instruction; returns T-cycles */
void gb_run_frame(GB *gb);                  /* run until next frame is ready */
void gb_set_button(GB *gb, int button, bool pressed);

/* bus.c */
uint8_t gb_read(GB *gb, uint16_t addr);
void    gb_write(GB *gb, uint16_t addr, uint8_t value);

/* cpu.c */
void cpu_reset(CPU *cpu);
int  cpu_step(GB *gb);                      /* interrupt or instruction; T-cycles */

/* ppu.c */
void ppu_reset(GB *gb);
void ppu_step(GB *gb, int cycles);
void ppu_lcdc_write(GB *gb, uint8_t value);
void ppu_update_stat(GB *gb);

#endif /* GB_H */
