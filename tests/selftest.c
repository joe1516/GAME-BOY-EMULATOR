/*
 * selftest.c - Unit and integration tests for the emulator core.
 *
 * Each test builds a tiny ROM in memory (code placed at 0x0100), steps the
 * machine, and checks registers, flags, memory and pixels.
 *
 * Build and run with `make test`. For a much deeper CPU check, run
 * Blargg's cpu_instrs ROMs through `gbcli --serial` (see the README).
 */
#include "gb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("    FAIL line %d: %s\n", __LINE__, #cond);               \
        }                                                                    \
    } while (0)

#define RUN(test)                                                            \
    do {                                                                     \
        int before = failures;                                               \
        test();                                                              \
        printf("[%s] %s\n", failures == before ? " ok " : "FAIL", #test);    \
    } while (0)

static GB gb;

/* Boot a machine whose cartridge is a 32 KB ROM with `code` at 0x0100. */
static void boot(const uint8_t *code, size_t n)
{
    static uint8_t rom[0x8000];
    char err[64];

    memset(rom, 0, sizeof rom);
    memcpy(&rom[0x100], code, n);

    gb_free(&gb);
    memset(&gb, 0, sizeof gb);
    if (!cart_load_memory(&gb.cart, rom, sizeof rom, err, sizeof err)) {
        printf("cannot build test cart: %s\n", err);
        exit(2);
    }
    gb_reset(&gb);
}

static void steps(int n)
{
    while (n-- > 0) gb_step(&gb);
}

/* ------------------------------------------------------------------ */
/* CPU                                                                */
/* ------------------------------------------------------------------ */

static void test_add_flags(void)
{
    const uint8_t code[] = { 0x3E, 0x3A,        /* LD A,0x3A   */
                             0xC6, 0xC6 };      /* ADD A,0xC6  */
    boot(code, sizeof code);
    steps(2);
    CHECK(gb.cpu.a == 0x00);
    CHECK(gb.cpu.f == (FLAG_Z | FLAG_H | FLAG_C));
}

static void test_sub_and_cp_flags(void)
{
    const uint8_t code[] = { 0x3E, 0x10,        /* LD A,0x10 */
                             0xD6, 0x01,        /* SUB 1     */
                             0xFE, 0x0F };      /* CP 0x0F   */
    boot(code, sizeof code);
    steps(2);
    CHECK(gb.cpu.a == 0x0F);
    CHECK(gb.cpu.f == (FLAG_N | FLAG_H));
    steps(1);
    CHECK(gb.cpu.a == 0x0F);                    /* CP must not change A */
    CHECK(gb.cpu.f == (FLAG_Z | FLAG_N));
}

static void test_daa(void)
{
    const uint8_t code[] = { 0x3E, 0x45,        /* LD A,0x45  */
                             0xC6, 0x38,        /* ADD A,0x38 */
                             0x27 };            /* DAA        */
    boot(code, sizeof code);
    steps(3);
    CHECK(gb.cpu.a == 0x83);                    /* 45 + 38 = 83 in BCD */
}

static void test_push_pop_af_masks_flags(void)
{
    const uint8_t code[] = { 0x01, 0xFF, 0x12,  /* LD BC,0x12FF */
                             0xC5,              /* PUSH BC      */
                             0xF1 };            /* POP AF       */
    boot(code, sizeof code);
    steps(3);
    CHECK(gb.cpu.a == 0x12);
    CHECK(gb.cpu.f == 0xF0);                    /* low nibble of F is always 0 */
}

static void test_call_and_ret(void)
{
    const uint8_t code[] = { 0xCD, 0x06, 0x01,  /* 0100 CALL 0x0106 */
                             0x04,              /* 0103 INC B       */
                             0x18, 0xFE,        /* 0104 JR $        */
                             0x0E, 0x07,        /* 0106 LD C,7      */
                             0xC9 };            /* 0108 RET         */
    boot(code, sizeof code);
    steps(4);
    CHECK(gb.cpu.c == 7);
    CHECK(gb.cpu.b == 1);
    CHECK(gb.cpu.sp == 0xFFFE);
}

static void test_relative_jump_backwards(void)
{
    const uint8_t code[] = { 0x3E, 0x03,        /* 0100 LD A,3     */
                             0x3D,              /* 0102 DEC A      */
                             0x20, 0xFD,        /* 0103 JR NZ,0102 */
                             0x04 };            /* 0105 INC B      */
    boot(code, sizeof code);
    steps(1 + 3 * 2 + 1);
    CHECK(gb.cpu.a == 0);
    CHECK(gb.cpu.b == 1);
}

static void test_cb_instructions(void)
{
    const uint8_t code[] = { 0x3E, 0xF0,        /* LD A,0xF0  */
                             0xCB, 0x37,        /* SWAP A     */
                             0xCB, 0x7F,        /* BIT 7,A    */
                             0xCB, 0xFF,        /* SET 7,A    */
                             0xCB, 0x87 };      /* RES 0,A    */
    boot(code, sizeof code);
    steps(2);
    CHECK(gb.cpu.a == 0x0F);
    steps(1);
    CHECK(gb.cpu.f & FLAG_Z);                   /* bit 7 of 0x0F is clear */
    steps(2);
    CHECK(gb.cpu.a == 0x8E);
}

static void test_add_sp_signed(void)
{
    const uint8_t code[] = { 0x31, 0xF8, 0xFF,  /* LD SP,0xFFF8 */
                             0xE8, 0x08 };      /* ADD SP,+8    */
    boot(code, sizeof code);
    steps(2);
    CHECK(gb.cpu.sp == 0x0000);
    CHECK(gb.cpu.f == (FLAG_H | FLAG_C));
}

static void test_illegal_opcode_locks_cpu(void)
{
    const uint8_t code[] = { 0xD3 };
    boot(code, sizeof code);
    steps(1);
    CHECK(gb.cpu.locked);
    CHECK(gb.cpu.locked_opcode == 0xD3);
}

/* ------------------------------------------------------------------ */
/* Interrupts / HALT                                                  */
/* ------------------------------------------------------------------ */

static void test_interrupt_dispatch_and_ei_delay(void)
{
    const uint8_t code[] = { 0xFB,              /* 0100 EI  */
                             0x00,              /* 0101 NOP */
                             0x00 };            /* 0102 NOP */
    boot(code, sizeof code);
    gb.ie = GB_INT_VBLANK;
    gb.iflag = GB_INT_VBLANK;

    steps(1);                                   /* EI               */
    CHECK(!gb.cpu.ime);
    steps(1);                                   /* NOP: still runs before the interrupt */
    CHECK(gb.cpu.pc == 0x0102);
    CHECK(gb.cpu.ime);
    steps(1);                                   /* interrupt taken  */
    CHECK(gb.cpu.pc == 0x0040);
    CHECK(!gb.cpu.ime);
    CHECK(!(gb.iflag & GB_INT_VBLANK));
    CHECK(gb_read(&gb, gb.cpu.sp) == 0x02 && gb_read(&gb, (uint16_t)(gb.cpu.sp + 1)) == 0x01);
}

static void test_interrupt_priority(void)
{
    const uint8_t code[] = { 0xFB, 0x00, 0x00 };
    boot(code, sizeof code);
    gb.ie = 0x1F;
    gb.iflag = GB_INT_TIMER | GB_INT_STAT;      /* STAT (bit 1) beats Timer (bit 2) */
    steps(3);
    CHECK(gb.cpu.pc == 0x0048);
}

static void test_halt_wakes_on_interrupt(void)
{
    const uint8_t code[] = { 0x76,              /* HALT  */
                             0x04 };            /* INC B */
    boot(code, sizeof code);
    steps(3);
    CHECK(gb.cpu.halted);
    CHECK(gb.cpu.b == 0);
    gb.ie = GB_INT_TIMER;
    gb.iflag = GB_INT_TIMER;                    /* IME is off: wake without jumping */
    steps(1);
    CHECK(!gb.cpu.halted);
    CHECK(gb.cpu.b == 1);
    CHECK(gb.cpu.pc == 0x0102);
}

/* ------------------------------------------------------------------ */
/* Timer, joypad, DMA, memory                                         */
/* ------------------------------------------------------------------ */

static void test_timer_overflow_reloads_and_interrupts(void)
{
    boot((const uint8_t[]){ 0x00 }, 1);         /* the ROM is all NOPs */
    gb.div = 0;
    gb_write(&gb, 0xFF06, 0xAB);                /* TMA         */
    gb_write(&gb, 0xFF05, 0xFF);                /* TIMA        */
    gb_write(&gb, 0xFF07, 0x05);                /* on, 16 cycles per tick */
    gb.iflag = 0;
    steps(4);                                   /* 4 NOPs = 16 cycles */
    CHECK(gb.tima == 0xAB);
    CHECK(gb.iflag & GB_INT_TIMER);
}

static void test_div_resets_on_write(void)
{
    boot((const uint8_t[]){ 0x00 }, 1);
    gb.div = 0x3400;
    CHECK(gb_read(&gb, 0xFF04) == 0x34);
    gb_write(&gb, 0xFF04, 0x99);
    CHECK(gb_read(&gb, 0xFF04) == 0x00);
}

static void test_joypad_matrix(void)
{
    boot((const uint8_t[]){ 0x00 }, 1);
    gb_write(&gb, 0xFF00, 0x20);                /* select D-pad */
    CHECK((gb_read(&gb, 0xFF00) & 0x0F) == 0x0F);
    gb.iflag = 0;
    gb_set_button(&gb, GB_BTN_RIGHT, true);
    CHECK((gb_read(&gb, 0xFF00) & 0x0F) == 0x0E);
    CHECK(gb.iflag & GB_INT_JOYPAD);
    gb_set_button(&gb, GB_BTN_RIGHT, false);
    CHECK((gb_read(&gb, 0xFF00) & 0x0F) == 0x0F);

    gb_write(&gb, 0xFF00, 0x10);                /* select buttons */
    gb_set_button(&gb, GB_BTN_START, true);
    CHECK((gb_read(&gb, 0xFF00) & 0x0F) == 0x07);
    gb_write(&gb, 0xFF00, 0x20);                /* D-pad again: START must be hidden */
    CHECK((gb_read(&gb, 0xFF00) & 0x0F) == 0x0F);
}

static void test_oam_dma(void)
{
    boot((const uint8_t[]){ 0x00 }, 1);
    for (int i = 0; i < 0xA0; i++) gb_write(&gb, (uint16_t)(0xC000 + i), (uint8_t)(i ^ 0x5A));
    gb_write(&gb, 0xFF46, 0xC0);
    CHECK(gb.oam[0] == 0x5A);
    CHECK(gb.oam[0x9F] == (uint8_t)(0x9F ^ 0x5A));
}

static void test_echo_ram_and_unusable_area(void)
{
    boot((const uint8_t[]){ 0x00 }, 1);
    gb_write(&gb, 0xC123, 0x77);
    CHECK(gb_read(&gb, 0xE123) == 0x77);
    gb_write(&gb, 0xFEA5, 0x12);
    CHECK(gb_read(&gb, 0xFEA5) == 0xFF);
}

static uint8_t serial_got;

static void serial_capture(uint8_t byte, void *user)
{
    (void)user;
    serial_got = byte;
}

static void test_serial_callback(void)
{
    serial_got = 0;
    boot((const uint8_t[]){ 0x00 }, 1);
    gb.serial_cb = serial_capture;
    gb_write(&gb, 0xFF01, 'X');
    gb_write(&gb, 0xFF02, 0x81);
    CHECK(serial_got == 'X');
    CHECK(!(gb.sc & 0x80));
}

/* ------------------------------------------------------------------ */
/* Cartridge mappers                                                  */
/* ------------------------------------------------------------------ */

/* ROM whose every bank starts with its own bank number. */
static uint8_t *make_banked_rom(unsigned banks, uint8_t type, uint8_t rom_code, uint8_t ram_code)
{
    uint8_t *rom = calloc(banks, 0x4000);
    for (unsigned b = 0; b < banks; b++) rom[b * 0x4000] = (uint8_t)b;
    rom[0x147] = type;
    rom[0x148] = rom_code;
    rom[0x149] = ram_code;
    return rom;
}

static void test_mbc1_banking_and_ram(void)
{
    Cart c;
    char err[64];
    uint8_t *rom = make_banked_rom(64, 0x03, 5, 3);        /* MBC1+RAM+BATTERY, 1 MB, 32 KB RAM */
    CHECK(cart_load_memory(&c, rom, 64u * 0x4000, err, sizeof err));
    free(rom);

    CHECK(cart_read_rom(&c, 0x4000) == 1);                 /* default bank 1 */
    cart_write_control(&c, 0x2000, 5);
    CHECK(cart_read_rom(&c, 0x4000) == 5);
    cart_write_control(&c, 0x2000, 0);                     /* bank 0 maps to 1 */
    CHECK(cart_read_rom(&c, 0x4000) == 1);
    cart_write_control(&c, 0x4000, 1);                     /* upper bits: bank 0x21 */
    CHECK(cart_read_rom(&c, 0x4000) == 33);
    cart_write_control(&c, 0x2000, 3);
    CHECK(cart_read_rom(&c, 0x4000) == 35);

    CHECK(cart_read_ram(&c, 0xA000) == 0xFF);              /* RAM disabled */
    cart_write_control(&c, 0x0000, 0x0A);
    cart_write_control(&c, 0x4000, 0);
    cart_write_ram(&c, 0xA000, 0x42);
    CHECK(cart_read_ram(&c, 0xA000) == 0x42);
    cart_write_control(&c, 0x0000, 0x00);
    CHECK(cart_read_ram(&c, 0xA000) == 0xFF);

    cart_free(&c);
}

static void test_mbc3_and_mbc5_banking(void)
{
    Cart c;
    char err[64];

    uint8_t *rom = make_banked_rom(128, 0x13, 6, 0);       /* MBC3, 2 MB */
    CHECK(cart_load_memory(&c, rom, 128u * 0x4000, err, sizeof err));
    free(rom);
    cart_write_control(&c, 0x2000, 0x7F);
    CHECK(cart_read_rom(&c, 0x4000) == 0x7F);
    cart_write_control(&c, 0x2000, 0);
    CHECK(cart_read_rom(&c, 0x4000) == 1);                 /* bank 0 -> 1 */
    cart_free(&c);

    rom = make_banked_rom(128, 0x19, 6, 0);                /* MBC5, 2 MB */
    CHECK(cart_load_memory(&c, rom, 128u * 0x4000, err, sizeof err));
    free(rom);
    cart_write_control(&c, 0x2000, 0x40);
    CHECK(cart_read_rom(&c, 0x4000) == 0x40);
    cart_write_control(&c, 0x2000, 0);
    CHECK(cart_read_rom(&c, 0x4000) == 0);                 /* MBC5 can map bank 0 */
    cart_free(&c);
}

static void test_unsupported_cartridge_is_rejected(void)
{
    Cart c;
    char err[64];
    uint8_t *rom = make_banked_rom(2, 0x05, 0, 0);         /* MBC2: not supported */
    CHECK(!cart_load_memory(&c, rom, 2u * 0x4000, err, sizeof err));
    CHECK(strstr(err, "0x05") != NULL);
    free(rom);
}

static void test_battery_save_roundtrip(void)
{
    Cart a, b;
    char err[64];
    uint8_t *rom = make_banked_rom(4, 0x03, 1, 2);         /* MBC1+RAM+BATTERY, 8 KB */
    CHECK(cart_load_memory(&a, rom, 4u * 0x4000, err, sizeof err));
    CHECK(cart_load_memory(&b, rom, 4u * 0x4000, err, sizeof err));
    free(rom);

    cart_write_control(&a, 0x0000, 0x0A);
    cart_write_ram(&a, 0xA010, 0xC3);
    CHECK(cart_write_save(&a, "selftest.sav"));
    CHECK(cart_load_save(&b, "selftest.sav"));
    cart_write_control(&b, 0x0000, 0x0A);
    CHECK(cart_read_ram(&b, 0xA010) == 0xC3);
    remove("selftest.sav");

    cart_free(&a);
    cart_free(&b);
}

/* ------------------------------------------------------------------ */
/* PPU                                                                */
/* ------------------------------------------------------------------ */

#define DARKEST  0xFF081820u
#define LIGHTEST 0xFFE0F8D0u

static void test_ppu_background_tile(void)
{
    boot((const uint8_t[]){ 0x18, 0xFE }, 2);              /* JR $ (spin) */
    for (int i = 0; i < 16; i++) gb.vram[16 + i] = 0xFF;   /* tile 1: solid colour 3 */
    gb.vram[0x1800] = 1;                                   /* map (0,0) = tile 1 */
    gb.bgp = 0xE4;
    gb_write(&gb, 0xFF40, 0x91);
    gb.window_line = 0;
    gb_run_frame(&gb);
    CHECK(gb.framebuffer[0] == DARKEST);
    CHECK(gb.framebuffer[7] == DARKEST);
    CHECK(gb.framebuffer[8] == LIGHTEST);                  /* next tile is blank */
    CHECK(gb.framebuffer[7 * GB_SCREEN_W] == DARKEST);
    CHECK(gb.framebuffer[8 * GB_SCREEN_W] == LIGHTEST);
}

static void test_ppu_scroll(void)
{
    boot((const uint8_t[]){ 0x18, 0xFE }, 2);
    for (int i = 0; i < 16; i++) gb.vram[16 + i] = 0xFF;
    gb.vram[0x1800] = 1;
    gb.bgp = 0xE4;
    gb.scx = 4;                                            /* shifts the dark tile 4 px left */
    gb_run_frame(&gb);
    CHECK(gb.framebuffer[3] == DARKEST);
    CHECK(gb.framebuffer[4] == LIGHTEST);
}

static void test_ppu_sprite_and_priority(void)
{
    boot((const uint8_t[]){ 0x18, 0xFE }, 2);
    for (int i = 0; i < 16; i++) gb.vram[2 * 16 + i] = 0xFF;   /* tile 2: solid 3 */
    for (int i = 0; i < 16; i++) gb.vram[16 + i] = 0xFF;       /* tile 1: solid 3 */
    gb.vram[0x1800 + 1] = 1;                                   /* BG tile 1 at map (1,0) */
    gb.oam[0] = 16; gb.oam[1] = 8; gb.oam[2] = 2; gb.oam[3] = 0;       /* sprite at (0,0) */
    gb.oam[4] = 16; gb.oam[5] = 16; gb.oam[6] = 2; gb.oam[7] = 0x80;   /* behind BG at (8,0) */
    gb.bgp = 0xE4;
    gb.obp0 = 0xE4;
    gb_write(&gb, 0xFF40, 0x93);                               /* BG + sprites on */
    gb_run_frame(&gb);
    CHECK(gb.framebuffer[0] == DARKEST);                       /* visible sprite */
    CHECK(gb.framebuffer[8] == DARKEST);                       /* BG colour 3 under the low-priority sprite */
    CHECK(gb.framebuffer[16] == LIGHTEST);                     /* nothing there */
}

static void test_ppu_vblank_interrupt_and_ly(void)
{
    boot((const uint8_t[]){ 0x18, 0xFE }, 2);
    gb.iflag = 0;
    gb_run_frame(&gb);
    CHECK(gb.iflag & GB_INT_VBLANK);
    CHECK(gb.ly == 144);
    CHECK((gb_read(&gb, 0xFF41) & 3) == 1);                    /* STAT mode = VBlank */
}

static void test_ppu_lcd_off_resets_ly(void)
{
    boot((const uint8_t[]){ 0x18, 0xFE }, 2);
    gb_run_frame(&gb);
    gb_write(&gb, 0xFF40, 0x00);
    CHECK(gb.ly == 0);
    gb_run_frame(&gb);                                         /* must not hang with the LCD off */
    CHECK(gb.ly == 0);
}

/* ------------------------------------------------------------------ */
/* Integration: the demo ROM reacts to the joypad                     */
/* ------------------------------------------------------------------ */

static void test_demo_rom_scrolls_and_reads_joypad(void)
{
    char err[64];
    gb_free(&gb);
    memset(&gb, 0, sizeof gb);
    if (!cart_load_file(&gb.cart, "roms/demo.gb", err, sizeof err)) {
        printf("    (skipped: roms/demo.gb missing, run `make demo`)\n");
        return;
    }
    gb_reset(&gb);
    for (int i = 0; i < 10; i++) gb_run_frame(&gb);            /* let it initialise */

    uint8_t before = gb.scx;
    gb_run_frame(&gb);
    CHECK((uint8_t)(gb.scx - before) == 1);                    /* auto-scroll: +1 per frame */

    gb_set_button(&gb, GB_BTN_RIGHT, true);
    before = gb.scx;
    gb_run_frame(&gb);
    CHECK((uint8_t)(gb.scx - before) == 2);                    /* Right: +2 per frame */

    gb_set_button(&gb, GB_BTN_RIGHT, false);
    gb_set_button(&gb, GB_BTN_LEFT, true);
    before = gb.scx;
    gb_run_frame(&gb);
    CHECK((uint8_t)(gb.scx - before) == (uint8_t)-1);          /* Left: +1 - 2 = -1 */
}

int main(void)
{
    printf("Game Boy emulator self-test\n\n");

    RUN(test_add_flags);
    RUN(test_sub_and_cp_flags);
    RUN(test_daa);
    RUN(test_push_pop_af_masks_flags);
    RUN(test_call_and_ret);
    RUN(test_relative_jump_backwards);
    RUN(test_cb_instructions);
    RUN(test_add_sp_signed);
    RUN(test_illegal_opcode_locks_cpu);

    RUN(test_interrupt_dispatch_and_ei_delay);
    RUN(test_interrupt_priority);
    RUN(test_halt_wakes_on_interrupt);

    RUN(test_timer_overflow_reloads_and_interrupts);
    RUN(test_div_resets_on_write);
    RUN(test_joypad_matrix);
    RUN(test_oam_dma);
    RUN(test_echo_ram_and_unusable_area);
    RUN(test_serial_callback);

    RUN(test_mbc1_banking_and_ram);
    RUN(test_mbc3_and_mbc5_banking);
    RUN(test_unsupported_cartridge_is_rejected);
    RUN(test_battery_save_roundtrip);

    RUN(test_ppu_background_tile);
    RUN(test_ppu_scroll);
    RUN(test_ppu_sprite_and_priority);
    RUN(test_ppu_vblank_interrupt_and_ly);
    RUN(test_ppu_lcd_off_resets_ly);

    RUN(test_demo_rom_scrolls_and_reads_joypad);

    gb_free(&gb);
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
