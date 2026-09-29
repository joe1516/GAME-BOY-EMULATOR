/* cart.c - Cartridge loading, header parsing, and MBC1/MBC3/MBC5 mappers. */
#include "gb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t len, const char *fmt, ...)
{
    if (!err || !len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

static unsigned next_pow2(unsigned v)
{
    unsigned p = 2;                     /* smallest cart has 2 banks */
    while (p < v) p <<= 1;
    return p;
}

bool cart_load_memory(Cart *cart, const uint8_t *data, size_t size,
                      char *err, size_t errlen)
{
    memset(cart, 0, sizeof *cart);

    if (size < 0x150) {
        set_err(err, errlen, "file too small to be a Game Boy ROM (%zu bytes)", size);
        return false;
    }

    uint8_t type = data[0x147];
    switch (type) {
        case 0x00: case 0x08: case 0x09:            cart->mapper = MBC_NONE; break;
        case 0x01: case 0x02: case 0x03:            cart->mapper = MBC_1;    break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
                                                    cart->mapper = MBC_3;    break;
        case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E:            cart->mapper = MBC_5;    break;
        default:
            set_err(err, errlen, "unsupported cartridge type 0x%02X", type);
            return false;
    }
    cart->type_byte = type;
    cart->battery = (type == 0x03 || type == 0x09 || type == 0x0F ||
                     type == 0x10 || type == 0x13 || type == 0x1B ||
                     type == 0x1E);

    /* ROM: pad up to a power-of-two number of banks so bank masking works. */
    unsigned banks = next_pow2((unsigned)((size + 0x3FFF) / 0x4000));
    cart->rom_banks = banks;
    cart->rom_size = (size_t)banks * 0x4000;
    cart->rom = malloc(cart->rom_size);
    if (!cart->rom) {
        set_err(err, errlen, "out of memory");
        return false;
    }
    memset(cart->rom, 0xFF, cart->rom_size);
    memcpy(cart->rom, data, size);

    /* RAM size from header byte 0x149. */
    static const size_t ram_sizes[] = { 0, 2048, 8192, 32768, 131072, 65536 };
    uint8_t ram_code = data[0x149];
    cart->ram_size = ram_code < 6 ? ram_sizes[ram_code] : 0;
    if (cart->ram_size) {
        cart->ram = calloc(1, cart->ram_size);
        if (!cart->ram) {
            free(cart->rom);
            memset(cart, 0, sizeof *cart);
            set_err(err, errlen, "out of memory");
            return false;
        }
    }

    cart->rom_bank = 1;

    memcpy(cart->title, &data[0x134], 16);
    cart->title[16] = '\0';
    for (int i = 0; i < 16; i++) {
        unsigned char ch = (unsigned char)cart->title[i];
        if (ch != 0 && (ch < 0x20 || ch > 0x7E)) cart->title[i] = '?';
    }
    return true;
}

bool cart_load_file(Cart *cart, const char *path, char *err, size_t errlen)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        set_err(err, errlen, "cannot open '%s'", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        fclose(fp);
        set_err(err, errlen, "invalid ROM size (%ld bytes)", size);
        return false;
    }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fclose(fp);
        set_err(err, errlen, "out of memory");
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(buf);
        set_err(err, errlen, "could not read the whole file");
        return false;
    }
    bool ok = cart_load_memory(cart, buf, (size_t)size, err, errlen);
    free(buf);
    return ok;
}

void cart_free(Cart *cart)
{
    free(cart->rom);
    free(cart->ram);
    memset(cart, 0, sizeof *cart);
}

/* ------------------------------------------------------------------ */
/* Battery saves                                                      */
/* ------------------------------------------------------------------ */

bool cart_load_save(Cart *cart, const char *path)
{
    if (!cart->battery || !cart->ram) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    size_t got = fread(cart->ram, 1, cart->ram_size, fp);
    fclose(fp);
    return got > 0;
}

bool cart_write_save(const Cart *cart, const char *path)
{
    if (!cart->battery || !cart->ram) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t put = fwrite(cart->ram, 1, cart->ram_size, fp);
    fclose(fp);
    return put == cart->ram_size;
}

/* ------------------------------------------------------------------ */
/* Mapper logic                                                       */
/* ------------------------------------------------------------------ */

uint8_t cart_read_rom(const Cart *c, uint16_t addr)
{
    if (!c->rom) return 0xFF;

    unsigned bank;
    if (addr < 0x4000) {
        bank = 0;
        /* MBC1 advanced mode also remaps bank 0 on large carts. */
        if (c->mapper == MBC_1 && c->mode) bank = c->ram_bank << 5;
    } else {
        switch (c->mapper) {
            case MBC_1: bank = (c->ram_bank << 5) | c->rom_bank; break;
            case MBC_3:
            case MBC_5: bank = c->rom_bank;                      break;
            default:    bank = 1;                                break;
        }
    }
    bank &= c->rom_banks - 1;
    return c->rom[(size_t)bank * 0x4000 + (addr & 0x3FFF)];
}

static bool ram_offset(const Cart *c, uint16_t addr, size_t *out)
{
    if (!c->ram_enabled || !c->ram_size) return false;

    unsigned bank = 0;
    switch (c->mapper) {
        case MBC_1: bank = c->mode ? c->ram_bank : 0; break;
        case MBC_3:
            if (c->ram_bank > 3) return false;      /* RTC registers: not emulated */
            bank = c->ram_bank;
            break;
        case MBC_5: bank = c->ram_bank; break;
        default:    bank = 0; break;
    }
    *out = ((size_t)bank * 0x2000 + (addr - 0xA000)) % c->ram_size;
    return true;
}

uint8_t cart_read_ram(const Cart *c, uint16_t addr)
{
    size_t off;
    return ram_offset(c, addr, &off) ? c->ram[off] : 0xFF;
}

void cart_write_ram(Cart *c, uint16_t addr, uint8_t value)
{
    size_t off;
    if (ram_offset(c, addr, &off)) c->ram[off] = value;
}

void cart_write_control(Cart *c, uint16_t addr, uint8_t v)
{
    switch (c->mapper) {
    case MBC_1:
        if (addr < 0x2000)      c->ram_enabled = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) { c->rom_bank = v & 0x1F; if (!c->rom_bank) c->rom_bank = 1; }
        else if (addr < 0x6000) c->ram_bank = v & 0x03;
        else                    c->mode = v & 1;
        break;

    case MBC_3:
        if (addr < 0x2000)      c->ram_enabled = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) { c->rom_bank = v & 0x7F; if (!c->rom_bank) c->rom_bank = 1; }
        else if (addr < 0x6000) c->ram_bank = v & 0x0F;   /* 0-3 RAM, 8-C RTC */
        break;

    case MBC_5:
        if (addr < 0x2000)      c->ram_enabled = (v & 0x0F) == 0x0A;
        else if (addr < 0x3000) c->rom_bank = (c->rom_bank & 0x100) | v;
        else if (addr < 0x4000) c->rom_bank = (c->rom_bank & 0x0FF) | ((unsigned)(v & 1) << 8);
        else if (addr < 0x6000) c->ram_bank = v & 0x0F;
        break;

    default:
        break;
    }
}
