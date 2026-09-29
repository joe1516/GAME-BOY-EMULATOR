/*
 * ppu.c - Pixel Processing Unit (scanline based).
 *
 * Each of the 154 lines lasts 456 dots:
 *   lines 0-143:  mode 2 (OAM scan, 80) -> mode 3 (drawing, 172) -> mode 0 (HBlank, 204)
 *   lines 144-153: mode 1 (VBlank)
 *
 * A whole line is rendered at the end of mode 3. This is not cycle-exact
 * (no pixel FIFO), but it is enough for the vast majority of DMG games.
 */
#include "gb.h"

#include <string.h>

/* Classic green-tinted DMG shades, lightest to darkest (0xAARRGGBB). */
static const uint32_t SHADES[4] = {
    0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820
};

/* ------------------------------------------------------------------ */
/* STAT interrupt line                                                */
/* ------------------------------------------------------------------ */

/*
 * All STAT sources are ORed onto one line; the interrupt fires only on a
 * rising edge of that line ("STAT blocking").
 */
void ppu_update_stat(GB *gb)
{
    if (!(gb->lcdc & 0x80)) { gb->stat_line = false; return; }

    bool line = ((gb->stat & 0x40) && gb->ly == gb->lyc)
             || ((gb->stat & 0x08) && gb->ppu_mode == 0)
             || ((gb->stat & 0x10) && gb->ppu_mode == 1)
             || ((gb->stat & 0x20) && gb->ppu_mode == 2);

    if (line && !gb->stat_line) gb->iflag |= GB_INT_STAT;
    gb->stat_line = line;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

/* Colour index (0-3) of pixel (px,py) of a 256x256 background/window map. */
static uint8_t map_pixel(const GB *gb, uint16_t map_offset, int px, int py)
{
    uint8_t tile = gb->vram[map_offset + (py >> 3) * 32 + (px >> 3)];
    uint16_t addr;

    if (gb->lcdc & 0x10) addr = (uint16_t)(tile * 16);                      /* 0x8000 mode */
    else                 addr = (uint16_t)(0x1000 + (int8_t)tile * 16);     /* 0x8800 mode */

    addr = (uint16_t)(addr + (py & 7) * 2);
    uint8_t lo = gb->vram[addr], hi = gb->vram[addr + 1];
    int bit = 7 - (px & 7);
    return (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

static void render_scanline(GB *gb)
{
    int y = gb->ly;
    uint32_t *line = &gb->framebuffer[y * GB_SCREEN_W];
    uint8_t bg_index[GB_SCREEN_W];              /* remembered for sprite priority */

    bool bg_on = gb->lcdc & 0x01;

    /* ---- Background ---- */
    if (!bg_on) {
        for (int x = 0; x < GB_SCREEN_W; x++) {
            bg_index[x] = 0;
            line[x] = SHADES[0];
        }
    } else {
        uint16_t map = (gb->lcdc & 0x08) ? 0x1C00 : 0x1800;
        int py = (y + gb->scy) & 0xFF;
        for (int x = 0; x < GB_SCREEN_W; x++) {
            int px = (x + gb->scx) & 0xFF;
            uint8_t ci = map_pixel(gb, map, px, py);
            bg_index[x] = ci;
            line[x] = SHADES[(gb->bgp >> (ci * 2)) & 3];
        }
    }

    /* ---- Window ---- */
    if (bg_on && (gb->lcdc & 0x20) && y >= gb->wy && gb->wx <= 166) {
        uint16_t map = (gb->lcdc & 0x40) ? 0x1C00 : 0x1800;
        int wx0 = (int)gb->wx - 7;
        for (int x = wx0 < 0 ? 0 : wx0; x < GB_SCREEN_W; x++) {
            uint8_t ci = map_pixel(gb, map, x - wx0, gb->window_line);
            bg_index[x] = ci;
            line[x] = SHADES[(gb->bgp >> (ci * 2)) & 3];
        }
        gb->window_line++;
    }

    /* ---- Sprites ---- */
    if (gb->lcdc & 0x02) {
        int height = (gb->lcdc & 0x04) ? 16 : 8;
        int found[10], n = 0;

        /* OAM scan: the first 10 sprites (in OAM order) touching this line. */
        for (int i = 0; i < 40 && n < 10; i++) {
            int sy = gb->oam[i * 4] - 16;
            if (y >= sy && y < sy + height) found[n++] = i;
        }

        /* Lower X wins; ties go to the lower OAM index (stable sort). */
        for (int a = 1; a < n; a++) {
            int t = found[a], b = a - 1;
            while (b >= 0 && gb->oam[found[b] * 4 + 1] > gb->oam[t * 4 + 1]) {
                found[b + 1] = found[b];
                b--;
            }
            found[b + 1] = t;
        }

        /* Draw lowest priority first so higher priority overwrites it. */
        for (int k = n - 1; k >= 0; k--) {
            const uint8_t *s = &gb->oam[found[k] * 4];
            int sy = s[0] - 16, sx = s[1] - 8;
            uint8_t tile = s[2], attr = s[3];

            int row = y - sy;
            if (attr & 0x40) row = height - 1 - row;            /* Y flip */
            if (height == 16) tile &= 0xFE;

            uint16_t addr = (uint16_t)(tile * 16 + row * 2);
            uint8_t lo = gb->vram[addr], hi = gb->vram[addr + 1];
            uint8_t pal = (attr & 0x10) ? gb->obp1 : gb->obp0;

            for (int px = 0; px < 8; px++) {
                int x = sx + px;
                if (x < 0 || x >= GB_SCREEN_W) continue;

                int bit = (attr & 0x20) ? px : 7 - px;          /* X flip */
                uint8_t ci = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
                if (ci == 0) continue;                          /* transparent */
                if ((attr & 0x80) && bg_index[x] != 0) continue; /* behind BG */

                line[x] = SHADES[(pal >> (ci * 2)) & 3];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */

void ppu_reset(GB *gb)
{
    gb->lcdc = 0x91;                            /* LCD on, BG on, 0x8000 tiles */
    gb->stat = 0;
    gb->ly = 0;
    gb->ppu_mode = 2;
    gb->ppu_dots = 0;
    gb->window_line = 0;
    gb->stat_line = false;
    gb->frame_ready = false;
    for (int i = 0; i < GB_SCREEN_W * GB_SCREEN_H; i++)
        gb->framebuffer[i] = SHADES[0];
}

void ppu_lcdc_write(GB *gb, uint8_t value)
{
    bool was_on = gb->lcdc & 0x80;
    bool now_on = value & 0x80;
    gb->lcdc = value;

    if (was_on && !now_on) {
        /* LCD switched off: LY resets, PPU idles, screen goes blank. */
        gb->ly = 0;
        gb->ppu_mode = 0;
        gb->ppu_dots = 0;
        gb->window_line = 0;
        gb->stat_line = false;
        for (int i = 0; i < GB_SCREEN_W * GB_SCREEN_H; i++)
            gb->framebuffer[i] = SHADES[0];
    } else if (!was_on && now_on) {
        gb->ly = 0;
        gb->ppu_mode = 2;
        gb->ppu_dots = 0;
        gb->window_line = 0;
        ppu_update_stat(gb);
    }
}

void ppu_step(GB *gb, int cycles)
{
    if (!(gb->lcdc & 0x80)) return;

    gb->ppu_dots += cycles;

    for (;;) {
        switch (gb->ppu_mode) {
        case 2:                                                 /* OAM scan */
            if (gb->ppu_dots < 80) return;
            gb->ppu_dots -= 80;
            gb->ppu_mode = 3;
            break;

        case 3:                                                 /* drawing  */
            if (gb->ppu_dots < 172) return;
            gb->ppu_dots -= 172;
            gb->ppu_mode = 0;
            render_scanline(gb);
            break;

        case 0:                                                 /* HBlank   */
            if (gb->ppu_dots < 204) return;
            gb->ppu_dots -= 204;
            gb->ly++;
            if (gb->ly == GB_SCREEN_H) {
                gb->ppu_mode = 1;
                gb->iflag |= GB_INT_VBLANK;
                gb->frame_ready = true;
            } else {
                gb->ppu_mode = 2;
            }
            break;

        default:                                                /* VBlank   */
            if (gb->ppu_dots < 456) return;
            gb->ppu_dots -= 456;
            gb->ly++;
            if (gb->ly > 153) {
                gb->ly = 0;
                gb->window_line = 0;
                gb->ppu_mode = 2;
            }
            break;
        }
        ppu_update_stat(gb);
    }
}
