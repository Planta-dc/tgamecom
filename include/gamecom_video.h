#ifndef GAMECOM_VIDEO_H
#define GAMECOM_VIDEO_H

#include "gamecom.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_LCD_WIDTH  200u
#define GC_LCD_HEIGHT 160u

typedef enum GcLcdPalette {
    GC_PALETTE_NATURAL = 0,       /* Hardware-calibrated Game.com STN olive (default) */
    GC_PALETTE_GREENISH = 1,      /* Pocket greenish STN */
    GC_PALETTE_GRAYISH = 2,       /* Crisp warm grayish STN */
    GC_PALETTE_HIGH_CONTRAST = 3, /* High-contrast classic green/black */
    GC_PALETTE_COUNT = 4
} GcLcdPalette;

/* Render one 200x160 LCD frame as 0xAARRGGBB pixels using the selected 5-pen
   palette preset (falls back to GC_PALETTE_NATURAL if out of range). */
bool gc_lcd_render_argb_palette(const GcMachine *machine, uint32_t *pixels,
                                size_t stride_pixels, unsigned palette_id);

/* Render one 200x160 LCD frame using the default hardware-calibrated STN palette. */
bool gc_lcd_render_argb(const GcMachine *machine, uint32_t *pixels,
                        size_t stride_pixels);

#ifdef __cplusplus
}
#endif

#endif
