#include "gamecom.h"
#include "gamecom_video.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define STRIDE (GC_LCD_WIDTH + 3u)

static int expect_pixel(const uint32_t *pixels, unsigned x, unsigned y,
                        uint32_t expected, const char *label)
{
    const uint32_t actual = pixels[y * STRIDE + x];
    if (actual == expected) return 0;
    fprintf(stderr, "%s: expected %08X, got %08X\n", label,
            (unsigned)expected, (unsigned)actual);
    return 1;
}

int main(void)
{
    GcMachine *machine = (GcMachine *)calloc(1, sizeof(*machine));
    uint32_t *pixels = (uint32_t *)malloc(sizeof(uint32_t) * STRIDE * GC_LCD_HEIGHT);
    if (!machine || !pixels) {
        fputs("Could not allocate LCD test state.\n", stderr);
        free(machine);
        free(pixels);
        return 1;
    }
    gc_machine_init(machine);

    for (size_t i = 0; i < STRIDE * GC_LCD_HEIGHT; ++i) pixels[i] = 0xDEADBEEFu;
    machine->bus.reg_file[0x30] = 0xB0; /* enabled, page 0, gradation 3 */
    machine->bus.vram[0] = 0x1Bu; /* top-to-bottom source dots: 0, 1, 2, 3 */
    if (!gc_lcd_render_argb(machine, pixels, STRIDE)) {
        fputs("LCD renderer rejected valid output buffer.\n", stderr);
        free(pixels);
        free(machine);
        return 1;
    }
    int failures = 0;
    /* Default hardware-calibrated Natural STN palette: E7E8D6 / D4D8BA / A7AF86 / 737D5E / 424B3B */
    failures += expect_pixel(pixels, 0, 0, 0xFFE7E8D6u, "gradation 3 dot 0 -> pen 4");
    failures += expect_pixel(pixels, 0, 1, 0xFFA7AF86u, "gradation 3 dot 1 -> pen 2");
    failures += expect_pixel(pixels, 0, 2, 0xFF737D5Eu, "gradation 3 dot 2 -> pen 1");
    failures += expect_pixel(pixels, 0, 3, 0xFF424B3Bu, "gradation 3 dot 3 -> pen 0");
    if (pixels[GC_LCD_WIDTH] != 0xDEADBEEFu) {
        fputs("LCD renderer overwrote row padding.\n", stderr);
        ++failures;
    }

    machine->bus.reg_file[0x30] = 0xB1; /* gradation 3 + LCC[0] normal-black inversion */
    if (!gc_lcd_render_argb(machine, pixels, STRIDE)) ++failures;
    failures += expect_pixel(pixels, 0, 0, 0xFF424B3Bu, "inverted dot 0 -> pen 0");
    failures += expect_pixel(pixels, 0, 1, 0xFFA7AF86u, "inverted dot 1 -> pen 2 unchanged");
    failures += expect_pixel(pixels, 0, 2, 0xFFD4D8BAu, "inverted dot 2 -> pen 3");
    failures += expect_pixel(pixels, 0, 3, 0xFFE7E8D6u, "inverted dot 3 -> pen 4");

    machine->bus.reg_file[0x30] = 0x80; /* gradation 0, page 0 */
    if (!gc_lcd_render_argb(machine, pixels, STRIDE)) ++failures;
    failures += expect_pixel(pixels, 0, 1, 0xFFD4D8BAu, "gradation 0 dot 1 -> pen 3");

    /* Alternate palettes (Greenish / Grayish / High-Contrast) */
    if (!gc_lcd_render_argb_palette(machine, pixels, STRIDE, GC_PALETTE_GREENISH)) ++failures;
    failures += expect_pixel(pixels, 0, 0, 0xFFE1E0D4u, "greenish pen 4");
    if (!gc_lcd_render_argb_palette(machine, pixels, STRIDE, GC_PALETTE_GRAYISH)) ++failures;
    failures += expect_pixel(pixels, 0, 0, 0xFFEDE7D8u, "grayish pen 4");
    if (!gc_lcd_render_argb_palette(machine, pixels, STRIDE, GC_PALETTE_HIGH_CONTRAST)) ++failures;
    failures += expect_pixel(pixels, 0, 0, 0xFFDFFF8Fu, "high-contrast pen 4");

    machine->bus.reg_file[0x30] = 0xF0; /* page 1 */
    machine->bus.vram[0x2000] = 0x00;
    if (!gc_lcd_render_argb(machine, pixels, STRIDE)) ++failures;
    failures += expect_pixel(pixels, 0, 0, 0xFFE7E8D6u, "page 1 selection");

    machine->bus.reg_file[0x30] = 0x70; /* LCD disabled */
    if (!gc_lcd_render_argb(machine, pixels, STRIDE)) ++failures;
    failures += expect_pixel(pixels, 0, 0, 0xFF424B3Bu, "display disabled");

    if (gc_lcd_render_argb(machine, pixels, GC_LCD_WIDTH - 1u)) {
        fputs("LCD renderer accepted a too-short row stride.\n", stderr);
        ++failures;
    }
    free(pixels);
    free(machine);
    if (failures) return 1;
    puts("LCD frame renderer tests passed (packing, gradation, palettes, page and enable).");
    return 0;
}
