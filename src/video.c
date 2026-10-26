// license:BSD-3-Clause
// Adapted from MAME tiger/gamecom_v.cpp and gamecom.cpp; copyright-holders: Wilbert Pol, Robbbert.
#include "gamecom_video.h"

#include <stddef.h>
#include <stdint.h>

static const uint32_t lcd_palette[5] = {
    0xFF000000u, /* black */
    0xFF0F4F2Fu, /* LCD pen 1 */
    0xFF6F8F4Fu, /* LCD pen 2 */
    0xFF8FCF8Fu, /* LCD pen 3 */
    0xFFDFFF8Fu  /* LCD pen 4 */
};

/* MAME's LCDC palette tables: each table maps packed two-bit source values
   0..3 to one of the five physical LCD palette pens. */
static const uint8_t palette_map[4][4] = {
    { 4, 3, 2, 0 },
    { 4, 3, 1, 0 },
    { 4, 3, 1, 0 },
    { 4, 2, 1, 0 }
};

bool gc_lcd_render_argb(const GcMachine *machine, uint32_t *pixels,
                        size_t stride_pixels)
{
    if (!machine || !pixels || stride_pixels < GC_LCD_WIDTH) return false;

    const uint8_t lcdc = machine->bus.reg_file[0x30];
    const size_t page_base = (lcdc & 0x40u) ? 0x2000u : 0u;
    const bool display_enabled = (lcdc & 0x80u) != 0;
    const unsigned palette = (lcdc >> 4) & 3u;

    for (unsigned y = 0; y < GC_LCD_HEIGHT; ++y) {
        uint32_t *row = pixels + (size_t)y * stride_pixels;
        for (unsigned x = 0; x < GC_LCD_WIDTH; ++x) {
            uint32_t color = lcd_palette[0];
            if (display_enabled) {
                /* Each VRAM byte stores four vertical 2-bit dots. The 200
                   horizontal columns each occupy 40 bytes, as in MAME's
                   gamecom_scanline() implementation. */
                const size_t address = page_base + (size_t)x * 40u + (y >> 2);
                const uint8_t packed = machine->bus.vram[address];
                const unsigned dot = (packed >> (6u - 2u * (y & 3u))) & 3u;
                color = lcd_palette[palette_map[palette][dot]];
            }
            row[x] = color;
        }
    }
    return true;
}
