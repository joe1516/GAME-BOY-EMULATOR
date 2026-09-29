#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void put16(uint8_t *rom, unsigned offset, uint16_t value)
{
    rom[offset] = (uint8_t)value;
    rom[offset + 1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *rom, unsigned offset, uint32_t value)
{
    rom[offset] = (uint8_t)value;
    rom[offset + 1] = (uint8_t)(value >> 8);
    rom[offset + 2] = (uint8_t)(value >> 16);
    rom[offset + 3] = (uint8_t)(value >> 24);
}

int main(void)
{
    uint8_t rom[0x200] = {0};

    /*
     * ARM branch target = (address + 8) + offset * 4.
     * From 0x00 to 0x0C: (0x0C - 0x08) / 4 = 1  ->  0xEA000001.
     * (The old value 0xEA000002 really lands on 0x10 on hardware.)
     */
    put32(rom, 0x00, 0xEA000001);

    memcpy(&rom[0xA0], "THUMB TEST", 10);
    memcpy(&rom[0xAC], "THMB", 4);

    /* ---------------- ARM ---------------- */
    put32(rom, 0x0C, 0xE3A00005); /* MOV R0,#5                        */
    put32(rom, 0x10, 0xE1A04100); /* MOV R4,R0,LSL #2      -> R4 = 20 */
    put32(rom, 0x14, 0xEB000000); /* BL  0x1C  (0x14+8+0)  -> LR=0x18 */
    put32(rom, 0x18, 0xE3A05001); /* MOV R5,#1   (skipped by BL)      */
    put32(rom, 0x1C, 0xE3A03408); /* MOV R3,#0x08000000               */
    put32(rom, 0x20, 0xE3833031); /* ORR R3,R3,#0x31  -> 0x08000031   */
    put32(rom, 0x24, 0xE12FFF13); /* BX  R3   -> THUMB at 0x30        */

    /* ---------------- THUMB ---------------- */
    put16(rom, 0x30, 0x210A);     /* MOV R1,#10                       */
    put16(rom, 0x32, 0x3105);     /* ADD R1,#5             -> 15      */
    put16(rom, 0x34, 0x3903);     /* SUB R1,#3             -> 12      */
    put16(rom, 0x36, 0x290C);     /* CMP R1,#12            -> Z=1,C=1 (later MOV clears Z) */
    put16(rom, 0x38, 0xD000);     /* BEQ 0x3C  (0x38+4+0)             */
    put16(rom, 0x3A, 0x2263);     /* MOV R2,#0x63 (skipped by BEQ)    */
    put16(rom, 0x3C, 0x22AA);     /* MOV R2,#0xAA                     */
    put16(rom, 0x3E, 0xE7FE);     /* B .   (idle loop)                */

    FILE *file = fopen("test.gba", "wb");
    if (!file) {
        perror("Could not create test.gba");
        return 1;
    }

    if (fwrite(rom, 1, sizeof(rom), file) != sizeof(rom)) {
        perror("Could not write test.gba");
        fclose(file);
        return 1;
    }

    fclose(file);
    printf("Created test.gba successfully (%zu bytes).\n", sizeof(rom));
    printf("Expected final state: R0=5 R1=12 R2=0xAA R3=0x08000031 R4=20\n"
           "                      R5=0 R14=0x08000018 PC=0x0800003E THUMB N=0 Z=0 C=1 V=0\n");
    return 0;
}
