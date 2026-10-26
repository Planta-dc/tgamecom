#include "gamecom_demo.h"
#include "gamecom.h"

#include <string.h>

#define DEMO_VRAM_BYTES (200u * 40u)
#define DEMO_CODE_BYTES ((DEMO_VRAM_BYTES / 2u) * 8u + 1u)
#define DEMO_W 200u
#define DEMO_H 160u

static uint8_t image[DEMO_VRAM_BYTES];

/* Five columns, seven rows, one bit per lit pixel. */
static const char *glyph(char c)
{
    switch (c) {
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'C': return "01111" "10000" "10000" "10000" "10000" "10000" "01111";
    case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
    case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case 'G': return "01111" "10000" "10000" "10111" "10001" "10001" "01111";
    case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
    case 'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
    case '.': return "00000" "00000" "00000" "00000" "00000" "00110" "00110";
    case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    default:  return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    }
}

static void pixel(unsigned x, unsigned y, uint8_t shade)
{
    if (x >= DEMO_W || y >= DEMO_H) return;
    unsigned byte_index = x * 40u + (y >> 2);
    unsigned shift = 6u - 2u * (y & 3u);
    image[byte_index] = (uint8_t)((image[byte_index] & (uint8_t)~(3u << shift)) |
                                  ((shade & 3u) << shift));
}

static void rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint8_t shade)
{
    for (unsigned x = x0; x <= x1; ++x) {
        pixel(x, y0, shade); pixel(x, y1, shade);
    }
    for (unsigned y = y0; y <= y1; ++y) {
        pixel(x0, y, shade); pixel(x1, y, shade);
    }
}

static void draw_text(const char *text, unsigned x, unsigned y, unsigned scale, uint8_t shade)
{
    for (; *text; ++text, x += 6u * scale) {
        const char *bits = glyph(*text);
        for (unsigned gy = 0; gy < 7; ++gy) {
            for (unsigned gx = 0; gx < 5; ++gx) {
                if (bits[gy * 5u + gx] == '1') {
                    for (unsigned sy = 0; sy < scale; ++sy)
                        for (unsigned sx = 0; sx < scale; ++sx)
                            pixel(x + gx * scale + sx, y + gy * scale + sy, shade);
                }
            }
        }
    }
}

static void make_demo_image(void)
{
    memset(image, 0, sizeof(image));
    rect(4, 14, 195, 145, 1);
    rect(8, 18, 191, 141, 2);
    rect(12, 22, 187, 137, 1);
    /* A few dim stars keep the background from competing with the text. */
    static const uint8_t stars[][2] = {
        {24, 34}, {38, 119}, {165, 37}, {176, 119}, {48, 126}, {153, 31}
    };
    for (size_t i = 0; i < sizeof(stars) / sizeof(stars[0]); ++i)
        pixel(stars[i][0], stars[i][1], 1);
    draw_text("GAME.COM", 30, 48, 3, 3);
    for (unsigned x = 55; x < 145; x += 6) pixel(x, 82, 2);
    draw_text("CPU DEMO", 53, 96, 2, 3);
}

size_t gc_build_vram_demo(uint8_t *output, size_t capacity)
{
    if (!output || capacity < DEMO_CODE_BYTES) return 0;
    make_demo_image();
    size_t p = 0;
    for (unsigned i = 0; i < DEMO_VRAM_BYTES; i += 2u) {
        uint16_t address = (uint16_t)(0xA000u + i);
        uint16_t word = (uint16_t)(((uint16_t)image[i] << 8) | image[i + 1u]);
        /* MOVW RR00,#word ; MOVW @$address,RR00. This actual SM8521 machine
           code paints two VRAM bytes per eight instruction bytes. The image
           program remains inside the documented 0x1000-0x9FFF ROM window. */
        output[p++] = 0x4B;
        output[p++] = 0x00;
        output[p++] = (uint8_t)(word >> 8);
        output[p++] = (uint8_t)word;
        output[p++] = 0x3B;
        output[p++] = 0x80;
        output[p++] = (uint8_t)(address >> 8);
        output[p++] = (uint8_t)address;
    }
    output[p++] = 0xF1; /* HALT after the image has been drawn. */
    return p;
}
