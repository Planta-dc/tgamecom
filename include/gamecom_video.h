#ifndef GAMECOM_VIDEO_H
#define GAMECOM_VIDEO_H

#include "gamecom.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_LCD_WIDTH  200u
#define GC_LCD_HEIGHT 160u

/* Render one LCD frame as 0xAARRGGBB pixels. stride_pixels is the distance
   between output rows. This uses the MAME-reference VRAM/page/palette mapping;
   scanline fetch races and LCD analog response are not yet modeled. */
bool gc_lcd_render_argb(const GcMachine *machine, uint32_t *pixels,
                        size_t stride_pixels);

#ifdef __cplusplus
}
#endif

#endif
