#include "gamecom.h"
#include "gamecom_demo.h"
#include "gamecom_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_PROGRAM_CAPACITY ((200u * 40u / 2u) * 8u + 1u)
#define DEMO_START 0x1020u

static uint8_t get_pixel(const GcMachine *m, unsigned x, unsigned y)
{
    uint16_t address = (uint16_t)(0xA000u + x * 40u + (y >> 2));
    uint8_t packed = m->bus.vram[address - 0xA000u];
    return (uint8_t)((packed >> (6u - 2u * (y & 3u))) & 3u);
}

static void print_screen(const GcMachine *m)
{
    static const char shade[] = " .o#";
    /* A 2x4 downsample keeps the full 200x160 image legible in a terminal. */
    for (unsigned y = 0; y < 160u; y += 4u) {
        for (unsigned x = 0; x < 200u; x += 2u) {
            uint8_t a = get_pixel(m, x, y);
            uint8_t b = get_pixel(m, x + 1u, y);
            putchar(shade[(a + b + 1u) / 2u]);
        }
        putchar('\n');
    }
}

static bool save_program(const char *path, const uint8_t *program, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return false; }
    bool ok = fwrite(program, 1, length, file) == length;
    if (fclose(file) != 0) ok = false;
    if (!ok) fprintf(stderr, "Could not write complete demo ROM to %s.\n", path);
    return ok;
}

static bool save_ppm(const char *path, const GcMachine *machine)
{
    uint32_t pixels[GC_LCD_WIDTH * GC_LCD_HEIGHT];
    if (!gc_lcd_render_argb(machine, pixels, GC_LCD_WIDTH)) return false;
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); return false; }
    bool ok = fprintf(file, "P6\n%u %u\n255\n", GC_LCD_WIDTH, GC_LCD_HEIGHT) > 0;
    for (unsigned y = 0; y < GC_LCD_HEIGHT && ok; ++y) {
        for (unsigned x = 0; x < GC_LCD_WIDTH; ++x) {
            uint32_t argb = pixels[y * GC_LCD_WIDTH + x];
            uint8_t rgb[3] = { (uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb };
            if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) { ok = false; break; }
        }
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) fprintf(stderr, "Could not write complete PPM image to %s.\n", path);
    return ok;
}

int main(int argc, char **argv)
{
    const char *export_path = NULL;
    const char *image_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) export_path = argv[++i];
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image_path = argv[++i];
        else {
            fprintf(stderr, "Usage: %s [--export raw-program.bin] [--image screen.ppm]\n", argv[0]);
            return 2;
        }
    }
    GcMachine *machine = (GcMachine *)calloc(1, sizeof(*machine));
    uint8_t *program = (uint8_t *)malloc(DEMO_PROGRAM_CAPACITY);
    if (!machine || !program) {
        fputs("Out of memory.\n", stderr);
        free(program); free(machine);
        return 1;
    }
    gc_machine_init(machine);
    size_t program_size = gc_build_vram_demo(program, DEMO_PROGRAM_CAPACITY);
    if (program_size == 0 || program_size > GC_PATCH_SIZE - DEMO_START) {
        fputs("Could not build demo program.\n", stderr);
        free(program); free(machine);
        return 1;
    }
    if (export_path && !save_program(export_path, program, program_size)) {
        free(program); free(machine);
        return 1;
    }
    memcpy(machine->bus.patch + DEMO_START, program, program_size);
    memset(machine->bus.patch_valid + DEMO_START, 1, program_size);
    free(program);

    const uint64_t instruction_limit = 20000u;
    GcStepResult result = GC_STEP_OK;
    while (machine->cpu.instructions < instruction_limit && result == GC_STEP_OK)
        result = gc_cpu_step(machine);

    if (result != GC_STEP_HALTED || machine->cpu.faulted) {
        fprintf(stderr, "Demo did not finish: PC=%04X, result=%d, %s\n",
                machine->cpu.pc, (int)result, machine->cpu.fault_message);
        free(machine);
        return 1;
    }

    if (image_path && !save_ppm(image_path, machine)) {
        free(machine);
        return 1;
    }
    printf("SM8521 homebrew VRAM demo ran in the emulator: %llu instructions, %llu cycles.\n",
           (unsigned long long)machine->cpu.instructions,
           (unsigned long long)machine->cpu.cycles);
    printf("Program image: %zu bytes at 0x%04X-0x%04X%s.\n", program_size, DEMO_START,
           (unsigned)(DEMO_START + program_size - 1u),
           (DEMO_START + program_size - 1u < 0xA000u) ? " (inside ROM address window)" : "");
    puts("The screen below is read back from emulated VRAM (200x160, downsampled):");
    print_screen(machine);
    puts("Done. The same built-in program can be opened in the optional SDL2 viewer with --demo.");
    free(machine);
    return 0;
}
