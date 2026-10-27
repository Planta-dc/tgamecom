/* Clean-room Sharp SM8521 / Tiger Game.com 200x160 STN LCD frame renderer. */
#include "gamecom_video.h"

#include <stddef.h>
#include <stdint.h>

/* Five RGB888 LCD pens ordered from pen 0 (darkest / black) to pen 4 (lightest / white). */
static const uint32_t k_lcd_palettes[GC_PALETTE_COUNT][5] = {
    /* GC_PALETTE_NATURAL: hardware-calibrated Game.com STN olive */
    { 0xFF424B3Bu, 0xFF737D5Eu, 0xFFA7AF86u, 0xFFD4D8BAu, 0xFFE7E8D6u },
    /* GC_PALETTE_GREENISH: pocket greenish STN */
    { 0xFF4C4D3Cu, 0xFF7A7A5Fu, 0xFFA4A48Au, 0xFFC9C8B6u, 0xFFE1E0D4u },
    /* GC_PALETTE_GRAYISH: crisp warm grayish STN */
    { 0xFF5A5141u, 0xFF8C8068u, 0xFFB9AC90u, 0xFFDED4C0u, 0xFFEDE7D8u },
    /* GC_PALETTE_HIGH_CONTRAST: high-contrast green/black */
    { 0xFF000000u, 0xFF0F4F2Fu, 0xFF6F8F4Fu, 0xFF8FCF8Fu, 0xFFDFFF8Fu }
};

/* SM8521 LCDC 2-bit-to-5-shade gradation tables selected by LCC[5:4]:
   dot 00 -> lightest (4), dot 11 -> darkest (0), middle dots pick 3/2 or 3/1 or 2/1. */
static const uint8_t k_gradation_map[4][4] = {
    { 4u, 3u, 2u, 0u },
    { 4u, 3u, 1u, 0u },
    { 4u, 3u, 1u, 0u },
    { 4u, 2u, 1u, 0u }
};

bool gc_lcd_render_argb_palette(const GcMachine *machine, uint32_t *pixels,
                                size_t stride_pixels, unsigned palette_id)
{
    if (!machine || !pixels || stride_pixels < GC_LCD_WIDTH) return false;
    if (palette_id >= GC_PALETTE_COUNT) palette_id = GC_PALETTE_NATURAL;

    const uint32_t *pens = k_lcd_palettes[palette_id];
    const uint8_t lcc = machine->bus.reg_file[0x30];
    const size_t page_base = (lcc & 0x40u) ? 0x2000u : 0u;
    const bool display_enabled = (lcc & 0x80u) != 0u;
    const bool normal_black = (lcc & 0x01u) != 0u;
    const unsigned gradation = (lcc >> 4) & 3u;

    for (unsigned y = 0; y < GC_LCD_HEIGHT; ++y) {
        uint32_t *row = pixels + (size_t)y * stride_pixels;
        for (unsigned x = 0; x < GC_LCD_WIDTH; ++x) {
            unsigned pen = 0u;
            if (display_enabled) {
                /* Each VRAM byte stores four vertical 2-bit dots across a 40-byte
                   column inside the selected 8 KiB page; LCC[0] mirrors the 5-pen
                   gradation ladder around pen 2 (4 - pen). */
                const size_t address = page_base + (size_t)x * 40u + (y >> 2);
                const uint8_t packed = machine->bus.vram[address];
                const unsigned dot = (packed >> (6u - 2u * (y & 3u))) & 3u;
                pen = k_gradation_map[gradation][dot];
                if (normal_black) pen = 4u - pen;
            }
            row[x] = pens[pen];
        }
    }
    return true;
}

bool gc_lcd_render_argb(const GcMachine *machine, uint32_t *pixels,
                        size_t stride_pixels)
{
    return gc_lcd_render_argb_palette(machine, pixels, stride_pixels, GC_PALETTE_NATURAL);
}
