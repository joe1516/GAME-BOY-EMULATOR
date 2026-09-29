/*
 * bus.c - 16-bit memory bus of the Game Boy.
 *
 *   0000-7FFF  Cartridge ROM (banked by the mapper)
 *   8000-9FFF  Video RAM
 *   A000-BFFF  Cartridge RAM (banked, battery backed)
 *   C000-DFFF  Work RAM            E000-FDFF  Echo of C000-DDFF
 *   FE00-FE9F  OAM (sprites)       FEA0-FEFF  Unusable
 *   FF00-FF7F  I/O registers       FF80-FFFE  High RAM
 *   FFFF       Interrupt enable
 */
#include "gb.h"

#include <string.h>

static uint8_t joypad_read(const GB *gb)
{
    uint8_t low = 0x0F;                             /* 0 = pressed */

    if (!(gb->joyp_select & 0x10))                  /* d-pad selected   */
        low &= (uint8_t)~(gb->buttons & 0x0F);
    if (!(gb->joyp_select & 0x20))                  /* buttons selected */
        low &= (uint8_t)~((gb->buttons >> 4) & 0x0F);

    return 0xC0 | (gb->joyp_select & 0x30) | low;
}

static uint8_t io_read(GB *gb, uint16_t addr)
{
    switch (addr) {
        case 0xFF00: return joypad_read(gb);
        case 0xFF01: return gb->sb;
        case 0xFF02: return gb->sc | 0x7E;
        case 0xFF04: return (uint8_t)(gb->div >> 8);
        case 0xFF05: return gb->tima;
        case 0xFF06: return gb->tma;
        case 0xFF07: return gb->tac | 0xF8;
        case 0xFF0F: return gb->iflag | 0xE0;

        case 0xFF40: return gb->lcdc;
        case 0xFF41: {
            uint8_t v = 0x80 | (gb->stat & 0x78);
            if (gb->lcdc & 0x80) {
                v |= gb->ppu_mode;
                if (gb->ly == gb->lyc) v |= 0x04;
            }
            return v;
        }
        case 0xFF42: return gb->scy;
        case 0xFF43: return gb->scx;
        case 0xFF44: return gb->ly;
        case 0xFF45: return gb->lyc;
        case 0xFF46: return gb->dma;
        case 0xFF47: return gb->bgp;
        case 0xFF48: return gb->obp0;
        case 0xFF49: return gb->obp1;
        case 0xFF4A: return gb->wy;
        case 0xFF4B: return gb->wx;

        default:     return gb->io[addr - 0xFF00];  /* sound, wave RAM, ... */
    }
}

static void io_write(GB *gb, uint16_t addr, uint8_t v)
{
    switch (addr) {
    case 0xFF00: gb->joyp_select = v & 0x30; break;

    case 0xFF01: gb->sb = v; break;
    case 0xFF02:
        gb->sc = v;
        if ((v & 0x81) == 0x81) {
            /* Internal-clock transfer. There is no link partner, so the
             * byte is handed to the host (test ROMs print results this way)
             * and the transfer completes immediately. */
            if (gb->serial_cb) gb->serial_cb(gb->sb, gb->serial_user);
            gb->sb = 0xFF;
            gb->sc &= 0x7F;
            gb->iflag |= GB_INT_SERIAL;
        }
        break;

    case 0xFF04: {
        /* Writing DIV resets the internal counter. If the timer's selected
         * bit was high this produces a falling edge and ticks TIMA. */
        static const uint16_t bits[4] = { 1u << 9, 1u << 3, 1u << 5, 1u << 7 };
        if ((gb->tac & 4) && (gb->div & bits[gb->tac & 3])) {
            if (++gb->tima == 0) {
                gb->tima = gb->tma;
                gb->iflag |= GB_INT_TIMER;
            }
        }
        gb->div = 0;
        break;
    }
    case 0xFF05: gb->tima = v; break;
    case 0xFF06: gb->tma = v;  break;
    case 0xFF07: gb->tac = v & 7; break;
    case 0xFF0F: gb->iflag = v & 0x1F; break;

    case 0xFF40: ppu_lcdc_write(gb, v); break;
    case 0xFF41:
        gb->stat = v & 0x78;
        ppu_update_stat(gb);
        break;
    case 0xFF42: gb->scy = v; break;
    case 0xFF43: gb->scx = v; break;
    case 0xFF44: break;                             /* LY is read-only */
    case 0xFF45:
        gb->lyc = v;
        ppu_update_stat(gb);
        break;
    case 0xFF46: {
        /* OAM DMA: copy 160 bytes from (v << 8). Done instantly here;
         * real hardware takes 160 machine cycles. */
        gb->dma = v;
        uint16_t src = (uint16_t)v << 8;
        for (int i = 0; i < 0xA0; i++)
            gb->oam[i] = gb_read(gb, (uint16_t)(src + i));
        break;
    }
    case 0xFF47: gb->bgp = v;  break;
    case 0xFF48: gb->obp0 = v; break;
    case 0xFF49: gb->obp1 = v; break;
    case 0xFF4A: gb->wy = v;   break;
    case 0xFF4B: gb->wx = v;   break;

    default:
        gb->io[addr - 0xFF00] = v;
        break;
    }
}

uint8_t gb_read(GB *gb, uint16_t addr)
{
    if (addr < 0x8000) return cart_read_rom(&gb->cart, addr);
    if (addr < 0xA000) return gb->vram[addr - 0x8000];
    if (addr < 0xC000) return cart_read_ram(&gb->cart, addr);
    if (addr < 0xE000) return gb->wram[addr - 0xC000];
    if (addr < 0xFE00) return gb->wram[addr - 0xE000];      /* echo RAM */
    if (addr < 0xFEA0) return gb->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;                         /* unusable */
    if (addr < 0xFF80) return io_read(gb, addr);
    if (addr < 0xFFFF) return gb->hram[addr - 0xFF80];
    return gb->ie;
}

void gb_write(GB *gb, uint16_t addr, uint8_t value)
{
    if (addr < 0x8000)       cart_write_control(&gb->cart, addr, value);
    else if (addr < 0xA000)  gb->vram[addr - 0x8000] = value;
    else if (addr < 0xC000)  cart_write_ram(&gb->cart, addr, value);
    else if (addr < 0xE000)  gb->wram[addr - 0xC000] = value;
    else if (addr < 0xFE00)  gb->wram[addr - 0xE000] = value;
    else if (addr < 0xFEA0)  gb->oam[addr - 0xFE00] = value;
    else if (addr < 0xFF00)  { /* unusable */ }
    else if (addr < 0xFF80)  io_write(gb, addr, value);
    else if (addr < 0xFFFF)  gb->hram[addr - 0xFF80] = value;
    else                     gb->ie = value;
}
