#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================
 *             MINIMUM GBA EMULATOR
 * ============================================================
 *
 * Target CPU: ARM7TDMI
 * Architecture: Game Boy Advance
 *
 * Current features:
 *   - GBA memory map
 *   - Cartridge ROM loading
 *   - ARM mode
 *   - Basic ARM instructions
 *   - THUMB mode
 *   - Basic THUMB instructions
 *   - ARM <-> THUMB switching with BX
 *   - Branching (B, BL, BX) with correct pipeline offsets
 *   - All 16 ARM data-processing ops + barrel shifter
 *   - All 15 ARM condition codes
 *   - CPSR flags N, Z, C, V
 *
 * This is an educational emulator foundation.
 * ============================================================
 */


/* ============================================================
 * GBA MEMORY MAP
 * ============================================================ */

#define BIOS_START      0x00000000
#define BIOS_SIZE       0x00004000      /* 16 KB */

#define EWRAM_START     0x02000000
#define EWRAM_SIZE      0x00040000      /* 256 KB */

#define IWRAM_START     0x03000000
#define IWRAM_SIZE      0x00008000      /* 32 KB */

#define IO_START        0x04000000
#define IO_SIZE         0x00000400

#define PAL_START       0x05000000
#define PAL_SIZE        0x00000400

#define VRAM_START      0x06000000
#define VRAM_SIZE       0x00018000      /* 96 KB */

#define OAM_START       0x07000000
#define OAM_SIZE        0x00000400

#define ROM_START       0x08000000


/* ============================================================
 * MEMORY
 * ============================================================ */

uint8_t bios[BIOS_SIZE];

uint8_t ewram[EWRAM_SIZE];
uint8_t iwram[IWRAM_SIZE];

uint8_t io[IO_SIZE];
uint8_t palette[PAL_SIZE];
uint8_t vram[VRAM_SIZE];
uint8_t oam[OAM_SIZE];

uint8_t *rom = NULL;
size_t rom_size = 0;


/* ============================================================
 * CPU
 * ============================================================ */

typedef struct
{
    uint32_t r[16];

    /*
     * R13 = SP
     * R14 = LR
     * R15 = PC
     */

    uint32_t cpsr;

} CPU;


/* CPSR flags */

#define FLAG_N  (1u << 31)
#define FLAG_Z  (1u << 30)
#define FLAG_C  (1u << 29)
#define FLAG_V  (1u << 28)

/* CPSR T bit */

#define FLAG_T  (1u << 5)


/* ============================================================
 * MEMORY ACCESS
 * ============================================================ */

uint8_t memory_read8(uint32_t address)
{
    /* BIOS */

    if (address < BIOS_START + BIOS_SIZE)
    {
        return bios[address - BIOS_START];
    }


    /* EWRAM */

    if (address >= EWRAM_START &&
        address < EWRAM_START + EWRAM_SIZE)
    {
        return ewram[address - EWRAM_START];
    }


    /* IWRAM */

    if (address >= IWRAM_START &&
        address < IWRAM_START + IWRAM_SIZE)
    {
        return iwram[address - IWRAM_START];
    }


    /* I/O */

    if (address >= IO_START &&
        address < IO_START + IO_SIZE)
    {
        return io[address - IO_START];
    }


    /* Palette */

    if (address >= PAL_START &&
        address < PAL_START + PAL_SIZE)
    {
        return palette[address - PAL_START];
    }


    /* VRAM */

    if (address >= VRAM_START &&
        address < VRAM_START + VRAM_SIZE)
    {
        return vram[address - VRAM_START];
    }


    /* OAM */

    if (address >= OAM_START &&
        address < OAM_START + OAM_SIZE)
    {
        return oam[address - OAM_START];
    }


    /* Cartridge ROM */

    if (address >= ROM_START)
    {
        size_t offset = address - ROM_START;

        if (offset < rom_size)
        {
            return rom[offset];
        }

        /*
         * ROM is mirrored on real hardware.
         * For now return 0 for addresses outside
         * our loaded ROM.
         */
        return 0;
    }


    return 0;
}


/* ------------------------------------------------------------
 * 16-bit read
 * ------------------------------------------------------------ */

uint16_t memory_read16(uint32_t address)
{
    uint16_t value;

    value  = memory_read8(address);
    value |= (uint16_t)memory_read8(address + 1) << 8;

    return value;
}


/* ------------------------------------------------------------
 * 32-bit read
 * ------------------------------------------------------------ */

uint32_t memory_read32(uint32_t address)
{
    uint32_t value;

    value  = memory_read8(address);
    value |= (uint32_t)memory_read8(address + 1) << 8;
    value |= (uint32_t)memory_read8(address + 2) << 16;
    value |= (uint32_t)memory_read8(address + 3) << 24;

    return value;
}


/* ------------------------------------------------------------
 * 8-bit write
 * ------------------------------------------------------------ */

void memory_write8(uint32_t address, uint8_t value)
{
    if (address >= EWRAM_START &&
        address < EWRAM_START + EWRAM_SIZE)
    {
        ewram[address - EWRAM_START] = value;
        return;
    }


    if (address >= IWRAM_START &&
        address < IWRAM_START + IWRAM_SIZE)
    {
        iwram[address - IWRAM_START] = value;
        return;
    }


    if (address >= IO_START &&
        address < IO_START + IO_SIZE)
    {
        io[address - IO_START] = value;
        return;
    }


    if (address >= PAL_START &&
        address < PAL_START + PAL_SIZE)
    {
        palette[address - PAL_START] = value;
        return;
    }


    if (address >= VRAM_START &&
        address < VRAM_START + VRAM_SIZE)
    {
        vram[address - VRAM_START] = value;
        return;
    }


    if (address >= OAM_START &&
        address < OAM_START + OAM_SIZE)
    {
        oam[address - OAM_START] = value;
        return;
    }
}


/* ------------------------------------------------------------
 * 16-bit write
 * ------------------------------------------------------------ */

void memory_write16(uint32_t address, uint16_t value)
{
    memory_write8(address, value & 0xFF);
    memory_write8(address + 1, (value >> 8) & 0xFF);
}


/* ------------------------------------------------------------
 * 32-bit write
 * ------------------------------------------------------------ */

void memory_write32(uint32_t address, uint32_t value)
{
    memory_write8(address, value & 0xFF);
    memory_write8(address + 1, (value >> 8) & 0xFF);
    memory_write8(address + 2, (value >> 16) & 0xFF);
    memory_write8(address + 3, (value >> 24) & 0xFF);
}


/* ============================================================
 * PIPELINE NOTE
 * ============================================================
 *
 * cpu_step() fetches at address A, then advances R15 by the
 * instruction size (A+4 ARM, A+2 THUMB) BEFORE executing.
 *
 * Real ARM7TDMI hardware has a 3-stage pipeline, so when an
 * instruction executes, reading R15 gives:
 *
 *      ARM   : A + 8
 *      THUMB : A + 4
 *
 * and PC-relative branches are relative to that value.
 * So relative to our already-advanced R15 we must add another
 * 4 (ARM) or 2 (THUMB). The helpers below do this.
 * ============================================================ */

static uint32_t arm_reg(const CPU *cpu, unsigned n)
{
    return (n == 15) ? cpu->r[15] + 4 : cpu->r[n];
}

static uint32_t thumb_reg(const CPU *cpu, unsigned n)
{
    return (n == 15) ? cpu->r[15] + 2 : cpu->r[n];
}


/* ============================================================
 * CPU FLAGS
 * ============================================================ */

static void set_flag(CPU *cpu, uint32_t flag, int on)
{
    if (on)
        cpu->cpsr |= flag;
    else
        cpu->cpsr &= ~flag;
}

void update_nz(CPU *cpu, uint32_t result)
{
    set_flag(cpu, FLAG_N, (result & 0x80000000u) != 0);
    set_flag(cpu, FLAG_Z, result == 0);
}

/*
 * a + b + carry_in, returning carry-out and signed overflow.
 * Subtraction a - b is add_with_carry(a, ~b, 1):
 *   C = 1 means "no borrow".
 */
static uint32_t add_with_carry(uint32_t a, uint32_t b, uint32_t carry_in,
                               int *carry_out, int *overflow)
{
    uint64_t wide = (uint64_t)a + (uint64_t)b + carry_in;
    uint32_t result = (uint32_t)wide;

    *carry_out = (wide >> 32) & 1;
    *overflow  = (((a ^ result) & (b ^ result)) >> 31) & 1;

    return result;
}


/* ============================================================
 * ARM CONDITION CODES
 * ============================================================ */

int condition_passed(CPU *cpu, uint8_t condition)
{
    int n = (cpu->cpsr & FLAG_N) != 0;
    int z = (cpu->cpsr & FLAG_Z) != 0;
    int c = (cpu->cpsr & FLAG_C) != 0;
    int v = (cpu->cpsr & FLAG_V) != 0;

    switch (condition)
    {
        case 0x0: return z;                 /* EQ */
        case 0x1: return !z;                /* NE */
        case 0x2: return c;                 /* CS/HS */
        case 0x3: return !c;                /* CC/LO */
        case 0x4: return n;                 /* MI */
        case 0x5: return !n;                /* PL */
        case 0x6: return v;                 /* VS */
        case 0x7: return !v;                /* VC */
        case 0x8: return c && !z;           /* HI */
        case 0x9: return !c || z;           /* LS */
        case 0xA: return n == v;            /* GE */
        case 0xB: return n != v;            /* LT */
        case 0xC: return !z && (n == v);    /* GT */
        case 0xD: return z || (n != v);     /* LE */
        case 0xE: return 1;                 /* AL */
        default:  return 0;                 /* NV / reserved on ARMv4 */
    }
}


/* ============================================================
 * ARM BARREL SHIFTER (data-processing operand 2)
 * ============================================================ */

static uint32_t arm_operand2(CPU *cpu, uint32_t instruction, int *carry_out)
{
    int old_c = (cpu->cpsr & FLAG_C) != 0;

    *carry_out = old_c;

    /* ---- Immediate: imm8 rotated right by 2 * rotate ---- */

    if ((instruction >> 25) & 1)
    {
        uint32_t imm8 = instruction & 0xFF;
        unsigned rotate = ((instruction >> 8) & 0xF) * 2;

        if (rotate == 0)
            return imm8;

        uint32_t value = (imm8 >> rotate) | (imm8 << (32 - rotate));

        *carry_out = (value >> 31) & 1;

        return value;
    }

    /* ---- Register, shifted by immediate or by register ---- */

    uint32_t rm_index = instruction & 0xF;
    uint32_t type = (instruction >> 5) & 3;
    int reg_shift = (instruction >> 4) & 1;
    uint32_t amount;
    uint32_t value;

    if (reg_shift)
    {
        /* Shift by register: PC reads as A + 12 in this form. */
        uint32_t rs = (instruction >> 8) & 0xF;

        value = (rm_index == 15) ? cpu->r[15] + 8 : cpu->r[rm_index];
        amount = cpu->r[rs] & 0xFF;

        if (amount == 0)
            return value;           /* no shift, carry unchanged */
    }
    else
    {
        value = arm_reg(cpu, rm_index);
        amount = (instruction >> 7) & 0x1F;
    }

    switch (type)
    {
        case 0: /* LSL */
            if (amount == 0)
                return value;
            if (amount < 32)
            {
                *carry_out = (value >> (32 - amount)) & 1;
                return value << amount;
            }
            *carry_out = (amount == 32) ? (value & 1) : 0;
            return 0;

        case 1: /* LSR */
            if (!reg_shift && amount == 0)
                amount = 32;        /* LSR #0 encodes LSR #32 */
            if (amount < 32)
            {
                *carry_out = (value >> (amount - 1)) & 1;
                return value >> amount;
            }
            *carry_out = (amount == 32) ? ((value >> 31) & 1) : 0;
            return 0;

        case 2: /* ASR */
            if (!reg_shift && amount == 0)
                amount = 32;        /* ASR #0 encodes ASR #32 */
            if (amount < 32)
            {
                *carry_out = (value >> (amount - 1)) & 1;
                return (uint32_t)((int32_t)value >> amount);
            }
            *carry_out = (value >> 31) & 1;
            return (value & 0x80000000u) ? 0xFFFFFFFFu : 0;

        default: /* ROR / RRX */
            if (!reg_shift && amount == 0)
            {
                /* RRX: rotate right 1 through carry */
                *carry_out = value & 1;
                return (value >> 1) | ((uint32_t)old_c << 31);
            }
            amount &= 31;
            if (amount == 0)
            {
                *carry_out = (value >> 31) & 1;
                return value;
            }
            *carry_out = (value >> (amount - 1)) & 1;
            return (value >> amount) | (value << (32 - amount));
    }
}


/* ============================================================
 * ARM MODE
 * ============================================================ */

static const char *arm_op_names[16] = {
    "AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
    "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"
};

void execute_arm(CPU *cpu, uint32_t instruction)
{
    uint8_t condition = (instruction >> 28) & 0xF;

    if (!condition_passed(cpu, condition))
    {
        printf("ARM condition failed\n");
        return;
    }


    /* --------------------------------------------------------
     * BX Rm  (ARM <-> THUMB switching)
     * -------------------------------------------------------- */

    if ((instruction & 0x0FFFFFF0) == 0x012FFF10)
    {
        uint32_t target = arm_reg(cpu, instruction & 0xF);

        if (target & 1)
        {
            cpu->cpsr |= FLAG_T;
            cpu->r[15] = target & ~1u;
            printf("ARM BX -> THUMB, PC = 0x%08X\n", cpu->r[15]);
        }
        else
        {
            cpu->cpsr &= ~FLAG_T;
            cpu->r[15] = target & ~3u;
            printf("ARM BX -> ARM, PC = 0x%08X\n", cpu->r[15]);
        }

        return;
    }


    /* --------------------------------------------------------
     * B / BL   (bits 27:25 = 101, bit 24 = link)
     * -------------------------------------------------------- */

    if (((instruction >> 25) & 7) == 5)
    {
        int32_t offset = (int32_t)(instruction << 8) >> 6;  /* sign-extend, *4 */
        int link = (instruction >> 24) & 1;

        if (link)
            cpu->r[14] = cpu->r[15];    /* address of next instruction */

        /* Target = (A + 8) + offset; R15 is already A + 4. */
        cpu->r[15] = cpu->r[15] + 4 + offset;

        printf("ARM %s -> PC = 0x%08X\n", link ? "BL" : "B", cpu->r[15]);

        return;
    }


    /* --------------------------------------------------------
     * Only data processing is implemented past this point
     * (bits 27:26 = 00). Multiply, halfword transfers, SWP,
     * MRS/MSR, LDR/STR, LDM/STM, SWI are still TODO.
     * -------------------------------------------------------- */

    if (((instruction >> 26) & 3) != 0)
    {
        printf("Unimplemented ARM instruction: 0x%08X\n", instruction);
        return;
    }

    int I = (instruction >> 25) & 1;

    if (!I && (instruction & 0x90) == 0x90)
    {
        /* Multiply / swap / halfword transfer encodings */
        printf("Unimplemented ARM instruction: 0x%08X\n", instruction);
        return;
    }

    uint8_t opcode = (instruction >> 21) & 0xF;
    uint8_t S      = (instruction >> 20) & 1;
    uint8_t rn     = (instruction >> 16) & 0xF;
    uint8_t rd     = (instruction >> 12) & 0xF;

    if (opcode >= 0x8 && opcode <= 0xB && !S)
    {
        /* TST/TEQ/CMP/CMN without S are MRS/MSR */
        printf("Unimplemented ARM instruction (PSR transfer): 0x%08X\n",
               instruction);
        return;
    }

    int shifter_carry;
    uint32_t op2 = arm_operand2(cpu, instruction, &shifter_carry);

    /* Rn as first operand: PC reads as A + 8 (A + 12 with reg shift). */
    uint32_t op1;

    if (rn == 15 && !I && ((instruction >> 4) & 1))
        op1 = cpu->r[15] + 8;
    else
        op1 = arm_reg(cpu, rn);

    uint32_t result = 0;
    int carry = shifter_carry;      /* logical ops: C = shifter carry */
    int overflow = (cpu->cpsr & FLAG_V) != 0;
    int arithmetic = 0;
    int write_result = 1;
    int c_in = (cpu->cpsr & FLAG_C) != 0;

    switch (opcode)
    {
        case 0x0: result = op1 & op2;  break;                     /* AND */
        case 0x1: result = op1 ^ op2;  break;                     /* EOR */
        case 0x2: result = add_with_carry(op1, ~op2, 1, &carry, &overflow);
                  arithmetic = 1; break;                          /* SUB */
        case 0x3: result = add_with_carry(op2, ~op1, 1, &carry, &overflow);
                  arithmetic = 1; break;                          /* RSB */
        case 0x4: result = add_with_carry(op1, op2, 0, &carry, &overflow);
                  arithmetic = 1; break;                          /* ADD */
        case 0x5: result = add_with_carry(op1, op2, c_in, &carry, &overflow);
                  arithmetic = 1; break;                          /* ADC */
        case 0x6: result = add_with_carry(op1, ~op2, c_in, &carry, &overflow);
                  arithmetic = 1; break;                          /* SBC */
        case 0x7: result = add_with_carry(op2, ~op1, c_in, &carry, &overflow);
                  arithmetic = 1; break;                          /* RSC */
        case 0x8: result = op1 & op2;  write_result = 0; break;   /* TST */
        case 0x9: result = op1 ^ op2;  write_result = 0; break;   /* TEQ */
        case 0xA: result = add_with_carry(op1, ~op2, 1, &carry, &overflow);
                  arithmetic = 1; write_result = 0; break;        /* CMP */
        case 0xB: result = add_with_carry(op1, op2, 0, &carry, &overflow);
                  arithmetic = 1; write_result = 0; break;        /* CMN */
        case 0xC: result = op1 | op2;  break;                     /* ORR */
        case 0xD: result = op2;        break;                     /* MOV */
        case 0xE: result = op1 & ~op2; break;                     /* BIC */
        case 0xF: result = ~op2;       break;                     /* MVN */
    }

    if (write_result)
    {
        if (rd == 15)
            cpu->r[15] = result & ~3u;      /* ARM-mode branch */
        else
            cpu->r[rd] = result;
    }

    /* Rd == 15 with S copies SPSR -> CPSR; not implemented yet. */
    if (S && rd != 15)
    {
        update_nz(cpu, result);
        set_flag(cpu, FLAG_C, carry);

        if (arithmetic)
            set_flag(cpu, FLAG_V, overflow);
    }

    if (write_result)
        printf("ARM %s%s R%d = 0x%08X\n",
               arm_op_names[opcode], S ? "S" : "", rd, result);
    else
        printf("ARM %s R%d, operand -> 0x%08X\n",
               arm_op_names[opcode], rn, result);
}


/* ============================================================
 * THUMB MODE
 * ============================================================ */

/* Shared helper for THUMB ADD/SUB/CMP with full flags. */

static uint32_t thumb_addsub_flags(CPU *cpu, uint32_t a, uint32_t b, int sub)
{
    int carry, overflow;
    uint32_t result = sub ? add_with_carry(a, ~b, 1, &carry, &overflow)
                          : add_with_carry(a, b, 0, &carry, &overflow);

    update_nz(cpu, result);
    set_flag(cpu, FLAG_C, carry);
    set_flag(cpu, FLAG_V, overflow);

    return result;
}

void execute_thumb(CPU *cpu, uint16_t instruction)
{
    /* --------------------------------------------------------
     * BX Rm      010001 11 0 Rm(4) 000
     * -------------------------------------------------------- */

    if ((instruction & 0xFF87) == 0x4700)
    {
        uint32_t target = thumb_reg(cpu, (instruction >> 3) & 0xF);

        if (target & 1)
        {
            cpu->cpsr |= FLAG_T;
            cpu->r[15] = target & ~1u;
            printf("THUMB BX -> THUMB, PC = 0x%08X\n", cpu->r[15]);
        }
        else
        {
            cpu->cpsr &= ~FLAG_T;
            cpu->r[15] = target & ~3u;
            printf("THUMB BX -> ARM, PC = 0x%08X\n", cpu->r[15]);
        }

        return;
    }


    /* --------------------------------------------------------
     * MOV Rd,#imm8     00100 Rd Imm8   (sets N, Z)
     * -------------------------------------------------------- */

    if ((instruction & 0xF800) == 0x2000)
    {
        uint8_t rd  = (instruction >> 8) & 7;
        uint8_t imm = instruction & 0xFF;

        cpu->r[rd] = imm;
        update_nz(cpu, cpu->r[rd]);

        printf("THUMB MOV R%d, #%u\n", rd, imm);
        return;
    }


    /* --------------------------------------------------------
     * CMP Rd,#imm8     00101 Rd Imm8
     * -------------------------------------------------------- */

    if ((instruction & 0xF800) == 0x2800)
    {
        uint8_t rd  = (instruction >> 8) & 7;
        uint8_t imm = instruction & 0xFF;

        thumb_addsub_flags(cpu, cpu->r[rd], imm, 1);

        printf("THUMB CMP R%d, #%u\n", rd, imm);
        return;
    }


    /* --------------------------------------------------------
     * ADD Rd,#imm8     00110 Rd Imm8
     * -------------------------------------------------------- */

    if ((instruction & 0xF800) == 0x3000)
    {
        uint8_t rd  = (instruction >> 8) & 7;
        uint8_t imm = instruction & 0xFF;

        cpu->r[rd] = thumb_addsub_flags(cpu, cpu->r[rd], imm, 0);

        printf("THUMB ADD R%d, #%u\n", rd, imm);
        return;
    }


    /* --------------------------------------------------------
     * SUB Rd,#imm8     00111 Rd Imm8
     * -------------------------------------------------------- */

    if ((instruction & 0xF800) == 0x3800)
    {
        uint8_t rd  = (instruction >> 8) & 7;
        uint8_t imm = instruction & 0xFF;

        cpu->r[rd] = thumb_addsub_flags(cpu, cpu->r[rd], imm, 1);

        printf("THUMB SUB R%d, #%u\n", rd, imm);
        return;
    }


    /* --------------------------------------------------------
     * Unconditional branch     11100 offset11
     * -------------------------------------------------------- */

    if ((instruction & 0xF800) == 0xE000)
    {
        int32_t offset = (int32_t)((uint32_t)(instruction & 0x7FF) << 21) >> 20;

        /* Target = (A + 4) + offset; R15 is already A + 2. */
        cpu->r[15] = cpu->r[15] + 2 + offset;

        printf("THUMB B -> PC = 0x%08X\n", cpu->r[15]);
        return;
    }


    /* --------------------------------------------------------
     * Conditional branch     1101 cond offset8
     * (cond 0xE = undefined, 0xF = SWI: not handled)
     * -------------------------------------------------------- */

    if ((instruction & 0xF000) == 0xD000)
    {
        uint8_t cond = (instruction >> 8) & 0xF;

        if (cond < 0xE)
        {
            int32_t offset = (int32_t)(int8_t)(instruction & 0xFF) * 2;

            if (condition_passed(cpu, cond))
            {
                cpu->r[15] = cpu->r[15] + 2 + offset;
                printf("THUMB B<cond %X> taken -> PC = 0x%08X\n",
                       cond, cpu->r[15]);
            }
            else
            {
                printf("THUMB B<cond %X> not taken\n", cond);
            }

            return;
        }
    }


    printf("Unknown THUMB instruction: 0x%04X\n", instruction);
}


/* ============================================================
 * CPU STEP
 * ============================================================ */

void cpu_step(CPU *cpu)
{
    /*
     * THUMB mode?
     */

    if (cpu->cpsr & FLAG_T)
    {
        uint32_t pc =
            cpu->r[15];

        uint16_t instruction =
            memory_read16(pc);


        /*
         * THUMB instructions are 16-bit.
         */

        cpu->r[15] += 2;


        printf(
            "\nPC = 0x%08X | THUMB = 0x%04X\n",
            pc,
            instruction
        );


        execute_thumb(cpu, instruction);
    }
    else
    {
        uint32_t pc =
            cpu->r[15];

        uint32_t instruction =
            memory_read32(pc);


        /*
         * ARM instructions are 32-bit.
         */

        cpu->r[15] += 4;


        printf(
            "\nPC = 0x%08X | ARM = 0x%08X\n",
            pc,
            instruction
        );


        execute_arm(cpu, instruction);
    }
}


/* ============================================================
 * ROM LOADER
 * ============================================================ */

int load_rom(const char *filename)
{
    FILE *file =
        fopen(filename, "rb");


    if (!file)
    {
        printf(
            "ERROR: Could not open ROM: %s\n",
            filename
        );

        return 0;
    }


    /* Find size */

    fseek(file, 0, SEEK_END);

    long size =
        ftell(file);

    fseek(file, 0, SEEK_SET);


    if (size <= 0)
    {
        printf("ERROR: Invalid ROM size\n");

        fclose(file);

        return 0;
    }


    rom_size =
        (size_t)size;


    rom =
        malloc(rom_size);


    if (!rom)
    {
        printf(
            "ERROR: Could not allocate ROM memory\n"
        );

        fclose(file);

        return 0;
    }


    size_t read =
        fread(
            rom,
            1,
            rom_size,
            file
        );


    fclose(file);


    if (read != rom_size)
    {
        printf(
            "ERROR: Could not read entire ROM\n"
        );

        free(rom);

        rom = NULL;

        rom_size = 0;

        return 0;
    }


    printf(
        "ROM loaded successfully\n"
    );

    printf(
        "ROM size: %zu bytes\n",
        rom_size
    );


    /*
     * Display some GBA header information.
     */

    if (rom_size >= 0xC0)
    {
        char title[13];

        memcpy(
            title,
            &rom[0xA0],
            12
        );

        title[12] = '\0';


        printf(
            "Game title: %s\n",
            title
        );


        printf(
            "Game code: %c%c%c%c\n",
            rom[0xAC],
            rom[0xAD],
            rom[0xAE],
            rom[0xAF]
        );
    }


    return 1;
}


/* ============================================================
 * CPU INITIALIZATION
 * ============================================================ */

void cpu_init(CPU *cpu)
{
    memset(
        cpu,
        0,
        sizeof(CPU)
    );


    /*
     * Start in ARM mode.
     */

    cpu->cpsr = 0;


    /*
     * Initial stack location.
     *
     * Simplified for our emulator.
     */

    cpu->r[13] =
        IWRAM_START + IWRAM_SIZE - 4;


    /*
     * Start execution from
     * cartridge ROM.
     */

    cpu->r[15] =
        ROM_START;


    printf(
        "ARM7TDMI CPU initialized\n"
    );

    printf(
        "SP = 0x%08X\n",
        cpu->r[13]
    );

    printf(
        "PC = 0x%08X\n",
        cpu->r[15]
    );
}


/* ============================================================
 * CPU DEBUG
 * ============================================================ */

void cpu_dump(CPU *cpu)
{
    printf(
        "\n========== CPU STATE ==========\n"
    );


    for (int i = 0; i < 16; i++)
    {
        printf(
            "R%-2d = 0x%08X\n",
            i,
            cpu->r[i]
        );
    }


    printf(
        "CPSR = 0x%08X\n",
        cpu->cpsr
    );


    printf(
        "Mode = %s\n",
        (cpu->cpsr & FLAG_T)
            ? "THUMB"
            : "ARM"
    );


    printf(
        "N=%d Z=%d C=%d V=%d\n",
        (cpu->cpsr & FLAG_N) != 0,
        (cpu->cpsr & FLAG_Z) != 0,
        (cpu->cpsr & FLAG_C) != 0,
        (cpu->cpsr & FLAG_V) != 0
    );


    printf(
        "===============================\n"
    );
}


/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char *argv[])
{
    printf(
        "=====================================\n"
    );

    printf(
        "       MINIMUM GBA EMULATOR\n"
    );

    printf(
        "       ARM7TDMI / C\n"
    );

    printf(
        "=====================================\n\n"
    );


    /*
     * A ROM filename is required.
     */

    if (argc < 2)
    {
        printf(
            "Usage:\n"
        );

        printf(
            "  %s game.gba\n",
            argv[0]
        );

        return 1;
    }


    /*
     * Load cartridge.
     */

    if (!load_rom(argv[1]))
    {
        return 1;
    }


    /*
     * Initialize CPU.
     */

    CPU cpu;

    cpu_init(&cpu);


    /*
     * Run a limited number of instructions.
     *
     * We intentionally limit execution while
     * developing the emulator so that a bad
     * instruction cannot create an endless loop.
     */

    const int MAX_INSTRUCTIONS = 100;


    printf(
        "\nStarting emulation...\n"
    );


    int executed = 0;

    for (int i = 0;
         i < MAX_INSTRUCTIONS;
         i++)
    {
        uint32_t pc_before = cpu.r[15];

        cpu_step(&cpu);
        executed++;

        /*
         * A branch-to-self (B .) is the classic "program finished"
         * idle loop. Stop instead of spinning until the cap.
         */
        if (cpu.r[15] == pc_before)
        {
            printf("\nBranch-to-self detected at 0x%08X, halting.\n",
                   pc_before);
            break;
        }
    }


    /*
     * Show final CPU state.
     */

    cpu_dump(&cpu);


    /*
     * Free ROM.
     */

    free(rom);

    rom = NULL;


    printf(
        "\nEmulation stopped after %d instructions.\n",
        executed
    );


    return 0;
}