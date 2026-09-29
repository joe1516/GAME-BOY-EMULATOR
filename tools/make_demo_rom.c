/*
 * make_demo_rom.c - Writes a tiny homebrew Game Boy ROM (roms/demo.gb).
 *
 * The ROM turns the LCD on, uploads four tiles, fills the background map
 * with vertical stripes, and scrolls it every frame. The D-pad changes the
 * scrolling, so it exercises the CPU, VRAM, LCD registers, VBlank timing and
 * the joypad without needing any commercial ROM.
 *
 * Usage: make_demo_rom [output.gb]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t rom[0x8000];
static unsigned pos;

static void db(uint8_t v) { rom[pos++] = v; }

/* JR-style relative jump back to `target` (opcode already chosen). */
static void jr(uint8_t opcode, unsigned target)
{
    int offset = (int)target - (int)(pos + 2);
    db(opcode);
    db((uint8_t)(int8_t)offset);
}

/* One D-pad block: BIT n,B ; JR NZ,+len ; LDH A,(reg) ; step... ; LDH (reg),A */
static void dpad_block(uint8_t bit_opcode, uint8_t reg, uint8_t step_opcode, int repeats)
{
    db(0xCB); db(bit_opcode);
    db(0x20); db((uint8_t)(4 + repeats));
    db(0xF0); db(reg);
    for (int i = 0; i < repeats; i++) db(step_opcode);
    db(0xE0); db(reg);
}

int main(int argc, char **argv)
{
    const char *out_path = argc > 1 ? argv[1] : "demo.gb";

    memset(rom, 0xFF, sizeof rom);

    /* Entry point: NOP ; JP 0x0150 */
    rom[0x100] = 0x00; rom[0x101] = 0xC3; rom[0x102] = 0x50; rom[0x103] = 0x01;

    /* Nintendo logo (checked by real hardware's boot ROM). */
    static const uint8_t logo[48] = {
        0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
        0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
        0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
    };
    memcpy(&rom[0x104], logo, sizeof logo);

    memcpy(&rom[0x134], "DEMO", 4);
    for (int i = 0x138; i < 0x143; i++) rom[i] = 0;
    rom[0x143] = 0x00;          /* DMG only        */
    rom[0x147] = 0x00;          /* ROM only        */
    rom[0x148] = 0x00;          /* 32 KB           */
    rom[0x149] = 0x00;          /* no RAM          */
    rom[0x14A] = 0x01;          /* non-Japan       */
    rom[0x14B] = 0x00;          /* no licensee     */

    /* ---- Tile data lives at 0x0400: 4 tiles x 16 bytes ---- */
    static const uint8_t tiles[64] = {
        /* tile 0: blank (colour 0) */
        0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        /* tile 1: checkerboard, colour 3 */
        0xAA,0xAA, 0x55,0x55, 0xAA,0xAA, 0x55,0x55, 0xAA,0xAA, 0x55,0x55, 0xAA,0xAA, 0x55,0x55,
        /* tile 2: solid colour 2 */
        0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF,
        /* tile 3: box, colour-3 border and colour-1 inside */
        0xFF,0xFF, 0xFF,0x81, 0xFF,0x81, 0xFF,0x81, 0xFF,0x81, 0xFF,0x81, 0xFF,0x81, 0xFF,0xFF
    };
    memcpy(&rom[0x400], tiles, sizeof tiles);

    /* ---- Program ---- */
    pos = 0x150;

    db(0xF3);                                   /* DI                       */
    db(0x31); db(0xFE); db(0xFF);               /* LD SP,0xFFFE             */

    unsigned wait1 = pos;                       /* wait for VBlank (LY == 144) */
    db(0xF0); db(0x44);                         /* LDH A,(LY)               */
    db(0xFE); db(0x90);                         /* CP 144                   */
    jr(0x20, wait1);                            /* JR NZ,wait1              */

    db(0xAF);                                   /* XOR A                    */
    db(0xE0); db(0x40);                         /* LDH (LCDC),A  LCD off    */

    db(0x21); db(0x00); db(0x80);               /* LD HL,0x8000             */
    db(0x11); db(0x00); db(0x04);               /* LD DE,0x0400 (tile data) */
    db(0x01); db(0x40); db(0x00);               /* LD BC,64                 */
    unsigned copy = pos;
    db(0x1A);                                   /* LD A,(DE)                */
    db(0x22);                                   /* LD (HL+),A               */
    db(0x13);                                   /* INC DE                   */
    db(0x0B);                                   /* DEC BC                   */
    db(0x78);                                   /* LD A,B                   */
    db(0xB1);                                   /* OR C                     */
    jr(0x20, copy);                             /* JR NZ,copy               */

    db(0x21); db(0x00); db(0x98);               /* LD HL,0x9800 (BG map)    */
    db(0x01); db(0x00); db(0x04);               /* LD BC,0x0400             */
    unsigned fill = pos;
    db(0x7D);                                   /* LD A,L                   */
    db(0xE6); db(0x03);                         /* AND 3   -> tile index    */
    db(0x22);                                   /* LD (HL+),A               */
    db(0x0B);                                   /* DEC BC                   */
    db(0x78);                                   /* LD A,B                   */
    db(0xB1);                                   /* OR C                     */
    jr(0x20, fill);                             /* JR NZ,fill               */

    db(0x3E); db(0xE4);                         /* LD A,0xE4                */
    db(0xE0); db(0x47);                         /* LDH (BGP),A              */
    db(0x3E); db(0x91);                         /* LD A,0x91                */
    db(0xE0); db(0x40);                         /* LDH (LCDC),A  LCD on     */

    unsigned frame = pos;
    unsigned wait_vb = pos;                     /* wait for LY == 144       */
    db(0xF0); db(0x44);
    db(0xFE); db(0x90);
    jr(0x20, wait_vb);

    db(0xF0); db(0x43); db(0x3C); db(0xE0); db(0x43);   /* SCX++ (auto-scroll) */

    db(0x3E); db(0x20);                         /* LD A,0x20  select D-pad  */
    db(0xE0); db(0x00);                         /* LDH (P1),A               */
    db(0xF0); db(0x00);                         /* LDH A,(P1)  (read twice  */
    db(0xF0); db(0x00);                         /*              to settle)  */
    db(0x47);                                   /* LD B,A                   */

    dpad_block(0x40, 0x43, 0x3C, 1);            /* Right: SCX++             */
    dpad_block(0x48, 0x43, 0x3D, 2);            /* Left : SCX -= 2          */
    dpad_block(0x50, 0x42, 0x3D, 1);            /* Up   : SCY--             */
    dpad_block(0x58, 0x42, 0x3C, 1);            /* Down : SCY++             */

    unsigned wait_end = pos;                    /* wait until LY leaves 144 */
    db(0xF0); db(0x44);
    db(0xFE); db(0x90);
    jr(0x28, wait_end);                         /* JR Z,wait_end            */

    jr(0x18, frame);                            /* JR frame                 */

    /* ---- Checksums ---- */
    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - rom[i] - 1);
    rom[0x14D] = x;

    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof rom; i++)
        if (i != 0x14E && i != 0x14F) sum += rom[i];
    rom[0x14E] = (uint8_t)(sum >> 8);
    rom[0x14F] = (uint8_t)sum;

    FILE *fp = fopen(out_path, "wb");
    if (!fp) { perror(out_path); return 1; }
    if (fwrite(rom, 1, sizeof rom, fp) != sizeof rom) { perror("write"); fclose(fp); return 1; }
    fclose(fp);

    printf("Wrote %s (%zu bytes, program ends at 0x%04X)\n", out_path, sizeof rom, pos);
    return 0;
}
