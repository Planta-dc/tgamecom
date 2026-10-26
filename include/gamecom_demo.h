#ifndef GAMECOM_DEMO_H
#define GAMECOM_DEMO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a small SM8521 machine-code program that paints a demo screen in VRAM.
   Returns its byte length, or zero if the supplied output buffer is too small. */
size_t gc_build_vram_demo(uint8_t *output, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
