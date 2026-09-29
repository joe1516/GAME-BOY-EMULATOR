/*
 * cpu.c - Sharp LR35902 (the Game Boy's Z80-like CPU).
 *
 * Opcodes are decoded from their bit fields rather than with a 256-entry
 * table. An opcode byte is split as:
 *
 *      x = bits 7-6    y = bits 5-3    z = bits 2-0
 *      p = y >> 1      q = y & 1
 *
 * and the 3-bit register field maps to: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A.
 *
 * Timing is per instruction (T-cycles, 4 per machine cycle), which is
 * accurate enough for games and for Blargg's cpu_instrs tests.
 */
#include "gb.h"

/* ------------------------------------------------------------------ */
/* Register helpers                                                   */
/* ------------------------------------------------------------------ */

static inline uint16_t get_bc(const CPU *c) { return (uint16_t)((c->b << 8) | c->c); }
static inline uint16_t get_de(const CPU *c) { return (uint16_t)((c->d << 8) | c->e); }
static inline uint16_t get_hl(const CPU *c) { return (uint16_t)((c->h << 8) | c->l); }
static inline uint16_t get_af(const CPU *c) { return (uint16_t)((c->a << 8) | c->f); }

static inline void set_bc(CPU *c, uint16_t v) { c->b = v >> 8; c->c = (uint8_t)v; }
static inline void set_de(CPU *c, uint16_t v) { c->d = v >> 8; c->e = (uint8_t)v; }
static inline void set_hl(CPU *c, uint16_t v) { c->h = v >> 8; c->l = (uint8_t)v; }
static inline void set_af(CPU *c, uint16_t v) { c->a = v >> 8; c->f = v & 0xF0; }

/* 16-bit register pairs as encoded in opcodes: BC, DE, HL, SP. */
static uint16_t get_rp(const CPU *c, int p)
{
    switch (p) {
        case 0: return get_bc(c);
        case 1: return get_de(c);
        case 2: return get_hl(c);
        default: return c->sp;
    }
}

static void set_rp(CPU *c, int p, uint16_t v)
{
    switch (p) {
        case 0: set_bc(c, v); break;
        case 1: set_de(c, v); break;
        case 2: set_hl(c, v); break;
        default: c->sp = v;   break;
    }
}

/* PUSH/POP use AF instead of SP. */
static uint16_t get_rp2(const CPU *c, int p)
{
    return p == 3 ? get_af(c) : get_rp(c, p);
}

static void set_rp2(CPU *c, int p, uint16_t v)
{
    if (p == 3) set_af(c, v);
    else        set_rp(c, p, v);
}

/* 8-bit operand fields (index 6 is the byte at (HL)). */
static uint8_t get_r(GB *gb, int i)
{
    CPU *c = &gb->cpu;
    switch (i) {
        case 0: return c->b;
        case 1: return c->c;
        case 2: return c->d;
        case 3: return c->e;
        case 4: return c->h;
        case 5: return c->l;
        case 6: return gb_read(gb, get_hl(c));
        default: return c->a;
    }
}

static void set_r(GB *gb, int i, uint8_t v)
{
    CPU *c = &gb->cpu;
    switch (i) {
        case 0: c->b = v; break;
        case 1: c->c = v; break;
        case 2: c->d = v; break;
        case 3: c->e = v; break;
        case 4: c->h = v; break;
        case 5: c->l = v; break;
        case 6: gb_write(gb, get_hl(c), v); break;
        default: c->a = v; break;
    }
}

/* ------------------------------------------------------------------ */
/* Memory / stack helpers                                             */
/* ------------------------------------------------------------------ */

static uint8_t fetch8(GB *gb)
{
    CPU *c = &gb->cpu;
    uint8_t v = gb_read(gb, c->pc);
    if (c->halt_bug) c->halt_bug = false;       /* HALT bug: PC fails to advance once */
    else             c->pc++;
    return v;
}

static uint16_t fetch16(GB *gb)
{
    uint8_t lo = fetch8(gb);
    uint8_t hi = fetch8(gb);
    return (uint16_t)(lo | (hi << 8));
}

static void push16(GB *gb, uint16_t v)
{
    CPU *c = &gb->cpu;
    gb_write(gb, --c->sp, (uint8_t)(v >> 8));
    gb_write(gb, --c->sp, (uint8_t)v);
}

static uint16_t pop16(GB *gb)
{
    CPU *c = &gb->cpu;
    uint8_t lo = gb_read(gb, c->sp++);
    uint8_t hi = gb_read(gb, c->sp++);
    return (uint16_t)(lo | (hi << 8));
}

/* ------------------------------------------------------------------ */
/* ALU                                                                */
/* ------------------------------------------------------------------ */

static inline uint8_t zflag(unsigned v) { return (v & 0xFF) ? 0 : FLAG_Z; }

/* Operations selected by the y field: ADD ADC SUB SBC AND XOR OR CP. */
static void alu(CPU *c, int op, uint8_t v)
{
    unsigned a = c->a, r, carry = (c->f & FLAG_C) ? 1 : 0;

    switch (op) {
    case 0: /* ADD */
        r = a + v;
        c->f = zflag(r) | (((a & 0xF) + (v & 0xF)) > 0xF ? FLAG_H : 0)
                        | (r > 0xFF ? FLAG_C : 0);
        c->a = (uint8_t)r;
        break;
    case 1: /* ADC */
        r = a + v + carry;
        c->f = zflag(r) | (((a & 0xF) + (v & 0xF) + carry) > 0xF ? FLAG_H : 0)
                        | (r > 0xFF ? FLAG_C : 0);
        c->a = (uint8_t)r;
        break;
    case 2: /* SUB */
    case 7: /* CP  (SUB without storing the result) */
        r = a - v;
        c->f = FLAG_N | zflag(r) | ((a & 0xF) < (v & 0xF) ? FLAG_H : 0)
                                 | (a < v ? FLAG_C : 0);
        if (op == 2) c->a = (uint8_t)r;
        break;
    case 3: /* SBC */
        r = a - v - carry;
        c->f = FLAG_N | zflag(r) | ((a & 0xF) < ((v & 0xF) + carry) ? FLAG_H : 0)
                                 | (a < (unsigned)v + carry ? FLAG_C : 0);
        c->a = (uint8_t)r;
        break;
    case 4: /* AND */
        c->a &= v;
        c->f = FLAG_H | zflag(c->a);
        break;
    case 5: /* XOR */
        c->a ^= v;
        c->f = zflag(c->a);
        break;
    default: /* OR */
        c->a |= v;
        c->f = zflag(c->a);
        break;
    }
}

/* Rotates and shifts (CB prefix, y = 0..7): RLC RRC RL RR SLA SRA SWAP SRL. */
static uint8_t rot_shift(CPU *c, int op, uint8_t v)
{
    uint8_t r, carry;
    uint8_t old_c = (c->f & FLAG_C) ? 1 : 0;

    switch (op) {
        case 0:  carry = v >> 7; r = (uint8_t)((v << 1) | carry);         break; /* RLC  */
        case 1:  carry = v & 1;  r = (uint8_t)((v >> 1) | (carry << 7));  break; /* RRC  */
        case 2:  carry = v >> 7; r = (uint8_t)((v << 1) | old_c);         break; /* RL   */
        case 3:  carry = v & 1;  r = (uint8_t)((v >> 1) | (old_c << 7));  break; /* RR   */
        case 4:  carry = v >> 7; r = (uint8_t)(v << 1);                   break; /* SLA  */
        case 5:  carry = v & 1;  r = (uint8_t)((v >> 1) | (v & 0x80));    break; /* SRA  */
        case 6:  carry = 0;      r = (uint8_t)((v << 4) | (v >> 4));      break; /* SWAP */
        default: carry = v & 1;  r = v >> 1;                              break; /* SRL  */
    }
    c->f = zflag(r) | (carry ? FLAG_C : 0);
    return r;
}

static void daa(CPU *c)
{
    unsigned a = c->a;

    if (!(c->f & FLAG_N)) {
        if ((c->f & FLAG_C) || a > 0x99) { a += 0x60; c->f |= FLAG_C; }
        if ((c->f & FLAG_H) || (a & 0x0F) > 0x09) a += 0x06;
    } else {
        if (c->f & FLAG_C) a -= 0x60;
        if (c->f & FLAG_H) a -= 0x06;
    }
    c->a = (uint8_t)a;
    c->f = (uint8_t)((c->f & (FLAG_N | FLAG_C)) | zflag(a));
}

/* SP + signed 8-bit offset; used by ADD SP,e and LD HL,SP+e. */
static uint16_t sp_plus_e(CPU *c, int8_t e)
{
    uint16_t sp = c->sp;
    c->f = (uint8_t)((((sp & 0xF) + ((uint8_t)e & 0xF)) > 0xF ? FLAG_H : 0)
                   | (((sp & 0xFF) + (uint8_t)e) > 0xFF ? FLAG_C : 0));
    return (uint16_t)(sp + e);
}

static bool condition(const CPU *c, int cc)
{
    switch (cc) {
        case 0:  return !(c->f & FLAG_Z);       /* NZ */
        case 1:  return  (c->f & FLAG_Z);       /* Z  */
        case 2:  return !(c->f & FLAG_C);       /* NC */
        default: return  (c->f & FLAG_C);       /* C  */
    }
}

/* ------------------------------------------------------------------ */
/* Instruction execution                                              */
/* ------------------------------------------------------------------ */

static int exec_cb(GB *gb)
{
    CPU *c = &gb->cpu;
    uint8_t op = fetch8(gb);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = get_r(gb, z);
    bool mem = (z == 6);

    switch (x) {
    case 0:                                             /* rotate / shift / swap */
        set_r(gb, z, rot_shift(c, y, v));
        return mem ? 16 : 8;
    case 1:                                             /* BIT y, r */
        c->f = (uint8_t)((c->f & FLAG_C) | FLAG_H | (((v >> y) & 1) ? 0 : FLAG_Z));
        return mem ? 12 : 8;
    case 2:                                             /* RES y, r */
        set_r(gb, z, (uint8_t)(v & ~(1u << y)));
        return mem ? 16 : 8;
    default:                                            /* SET y, r */
        set_r(gb, z, (uint8_t)(v | (1u << y)));
        return mem ? 16 : 8;
    }
}

static int illegal(CPU *c, uint8_t op)
{
    c->locked = true;
    c->locked_opcode = op;
    return 4;
}

static int exec(GB *gb)
{
    CPU *c = &gb->cpu;
    uint8_t op = fetch8(gb);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    switch (x) {

    /* ---------------------------------------------------------- */
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4;                                       /* NOP */
            case 1: {                                               /* LD (a16),SP */
                uint16_t a = fetch16(gb);
                gb_write(gb, a, (uint8_t)c->sp);
                gb_write(gb, (uint16_t)(a + 1), (uint8_t)(c->sp >> 8));
                return 20;
            }
            case 2: fetch8(gb); return 4;                           /* STOP */
            case 3: { int8_t e = (int8_t)fetch8(gb); c->pc = (uint16_t)(c->pc + e); return 12; }
            default: {                                              /* JR cc,e */
                int8_t e = (int8_t)fetch8(gb);
                if (condition(c, y - 4)) { c->pc = (uint16_t)(c->pc + e); return 12; }
                return 8;
            }
            }

        case 1:
            if (!q) { set_rp(c, p, fetch16(gb)); return 12; }       /* LD rr,nn */
            else {                                                  /* ADD HL,rr */
                uint16_t hl = get_hl(c), v = get_rp(c, p);
                unsigned r = hl + v;
                c->f = (uint8_t)((c->f & FLAG_Z)
                     | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FLAG_H : 0)
                     | (r > 0xFFFF ? FLAG_C : 0));
                set_hl(c, (uint16_t)r);
                return 8;
            }

        case 2: {                                                   /* LD (rr),A / LD A,(rr) */
            uint16_t hl = get_hl(c), addr;
            switch (p) {
                case 0:  addr = get_bc(c); break;
                case 1:  addr = get_de(c); break;
                case 2:  addr = hl; set_hl(c, (uint16_t)(hl + 1)); break;   /* HL+ */
                default: addr = hl; set_hl(c, (uint16_t)(hl - 1)); break;   /* HL- */
            }
            if (!q) gb_write(gb, addr, c->a);
            else    c->a = gb_read(gb, addr);
            return 8;
        }

        case 3: {                                                   /* INC rr / DEC rr */
            uint16_t v = get_rp(c, p);
            set_rp(c, p, (uint16_t)(q ? v - 1 : v + 1));
            return 8;
        }

        case 4: {                                                   /* INC r */
            uint8_t v = get_r(gb, y), r = (uint8_t)(v + 1);
            set_r(gb, y, r);
            c->f = (uint8_t)((c->f & FLAG_C) | zflag(r) | ((v & 0xF) == 0xF ? FLAG_H : 0));
            return y == 6 ? 12 : 4;
        }

        case 5: {                                                   /* DEC r */
            uint8_t v = get_r(gb, y), r = (uint8_t)(v - 1);
            set_r(gb, y, r);
            c->f = (uint8_t)((c->f & FLAG_C) | FLAG_N | zflag(r) | ((v & 0xF) == 0 ? FLAG_H : 0));
            return y == 6 ? 12 : 4;
        }

        case 6:                                                     /* LD r,n */
            set_r(gb, y, fetch8(gb));
            return y == 6 ? 12 : 8;

        default:
            switch (y) {
            case 0: case 1: case 2: case 3: {                       /* RLCA RRCA RLA RRA */
                c->a = rot_shift(c, y, c->a);
                c->f &= (uint8_t)~FLAG_Z;
                return 4;
            }
            case 4: daa(c); return 4;                               /* DAA */
            case 5: c->a = (uint8_t)~c->a;                          /* CPL */
                    c->f |= FLAG_N | FLAG_H; return 4;
            case 6: c->f = (uint8_t)((c->f & FLAG_Z) | FLAG_C); return 4;   /* SCF */
            default: c->f = (uint8_t)((c->f & FLAG_Z) | ((c->f & FLAG_C) ? 0 : FLAG_C));
                     return 4;                                      /* CCF */
            }
        }

    /* ---------------------------------------------------------- */
    case 1:                                                         /* LD r,r' and HALT */
        if (op == 0x76) {
            /* HALT with IME off and an interrupt already pending does not
             * halt; instead the next byte is fetched twice (the HALT bug). */
            if (!c->ime && (gb->ie & gb->iflag & 0x1F)) c->halt_bug = true;
            else                                        c->halted = true;
            return 4;
        }
        set_r(gb, y, get_r(gb, z));
        return (y == 6 || z == 6) ? 8 : 4;

    /* ---------------------------------------------------------- */
    case 2:                                                         /* ALU A,r */
        alu(c, y, get_r(gb, z));
        return z == 6 ? 8 : 4;

    /* ---------------------------------------------------------- */
    default:
        switch (z) {
        case 0:
            switch (y) {
            case 0: case 1: case 2: case 3:                         /* RET cc */
                if (condition(c, y)) { c->pc = pop16(gb); return 20; }
                return 8;
            case 4: gb_write(gb, (uint16_t)(0xFF00 | fetch8(gb)), c->a); return 12;   /* LDH (a8),A */
            case 5: { int8_t e = (int8_t)fetch8(gb); c->sp = sp_plus_e(c, e); return 16; } /* ADD SP,e */
            case 6: c->a = gb_read(gb, (uint16_t)(0xFF00 | fetch8(gb))); return 12;   /* LDH A,(a8) */
            default: { int8_t e = (int8_t)fetch8(gb); set_hl(c, sp_plus_e(c, e)); return 12; } /* LD HL,SP+e */
            }

        case 1:
            if (!q) { set_rp2(c, p, pop16(gb)); return 12; }        /* POP rr */
            switch (p) {
                case 0: c->pc = pop16(gb); return 16;                            /* RET  */
                case 1: c->pc = pop16(gb); c->ime = true; c->ei_delay = 0; return 16; /* RETI */
                case 2: c->pc = get_hl(c); return 4;                             /* JP HL */
                default: c->sp = get_hl(c); return 8;                            /* LD SP,HL */
            }

        case 2:
            switch (y) {
            case 0: case 1: case 2: case 3: {                       /* JP cc,nn */
                uint16_t a = fetch16(gb);
                if (condition(c, y)) { c->pc = a; return 16; }
                return 12;
            }
            case 4: gb_write(gb, (uint16_t)(0xFF00 | c->c), c->a); return 8;        /* LD (C),A */
            case 5: { uint16_t a = fetch16(gb); gb_write(gb, a, c->a); return 16; } /* LD (a16),A */
            case 6: c->a = gb_read(gb, (uint16_t)(0xFF00 | c->c)); return 8;        /* LD A,(C) */
            default: { uint16_t a = fetch16(gb); c->a = gb_read(gb, a); return 16; } /* LD A,(a16) */
            }

        case 3:
            switch (y) {
                case 0: c->pc = fetch16(gb); return 16;                     /* JP nn */
                case 1: return exec_cb(gb);                                 /* CB prefix */
                case 6: c->ime = false; c->ei_delay = 0; return 4;          /* DI */
                case 7: c->ei_delay = 2; return 4;                          /* EI */
                default: return illegal(c, op);
            }

        case 4:
            if (y < 4) {                                                    /* CALL cc,nn */
                uint16_t a = fetch16(gb);
                if (condition(c, y)) { push16(gb, c->pc); c->pc = a; return 24; }
                return 12;
            }
            return illegal(c, op);

        case 5:
            if (!q) { push16(gb, get_rp2(c, p)); return 16; }               /* PUSH rr */
            if (p == 0) {                                                   /* CALL nn */
                uint16_t a = fetch16(gb);
                push16(gb, c->pc);
                c->pc = a;
                return 24;
            }
            return illegal(c, op);

        case 6:                                                             /* ALU A,n */
            alu(c, y, fetch8(gb));
            return 8;

        default:                                                            /* RST y*8 */
            push16(gb, c->pc);
            c->pc = (uint16_t)(y * 8);
            return 16;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void cpu_reset(CPU *cpu)
{
    /* Register values the DMG boot ROM hands over to the cartridge. */
    *cpu = (CPU){0};
    set_af(cpu, 0x01B0);
    set_bc(cpu, 0x0013);
    set_de(cpu, 0x00D8);
    set_hl(cpu, 0x014D);
    cpu->sp = 0xFFFE;
    cpu->pc = 0x0100;
}

int cpu_step(GB *gb)
{
    CPU *c = &gb->cpu;

    if (c->locked) return 4;

    uint8_t pending = gb->ie & gb->iflag & 0x1F;

    if (pending) {
        c->halted = false;                      /* any pending interrupt wakes HALT */
        if (c->ime) {
            int bit = 0;
            while (!(pending & (1u << bit))) bit++;     /* lowest bit = highest priority */

            c->ime = false;
            c->ei_delay = 0;
            gb->iflag &= (uint8_t)~(1u << bit);
            push16(gb, c->pc);
            c->pc = (uint16_t)(0x40 + bit * 8);
            return 20;
        }
    }

    if (c->halted) return 4;

    int cycles = exec(gb);

    /* EI enables interrupts only after the instruction that follows it. */
    if (c->ei_delay && --c->ei_delay == 0) c->ime = true;

    return cycles;
}
