#ifndef GAMECOM_H
#define GAMECOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_CPU_HZ            5000000u
#define GC_LCD_NATIVE_CYCLES 66240u
#define GC_LCD_NATIVE_HZ     (5000000.0 / 66240.0)
#define GC_ROM_SIZE          0x200000u
#define GC_SYSTEM_ROM_SIZE 0x40000u
#define GC_VRAM_SIZE    0x4000u
#define GC_PATCH_SIZE   0x10000u
#define GC_RAM_SIZE     0x380u
#define GC_EXT_RAM_SIZE 0x1F00u
#define GC_NVRAM_SIZE   0x2000u /* 8 KiB persistent SRAM image: 0xE000-0xFEFF + 0xFF00-0xFFFF */
#define GC_BOOT_SIZE    0x1000u
#define GC_MAX_BREAKPOINTS 32u

/* PS1 flag bits, as documented for the SM8521. */
enum {
    GC_FLAG_C = 0x80, GC_FLAG_Z = 0x40, GC_FLAG_S = 0x20,
    GC_FLAG_V = 0x10, GC_FLAG_D = 0x08, GC_FLAG_H = 0x04,
    GC_FLAG_B = 0x02, GC_FLAG_I = 0x01
};

typedef struct {
    uint8_t reg_file[0x80];  /* CPU register file and SFRs: 0x0000-0x007F */
    uint8_t pio_output_latch[4]; /* Separate P0-P3 mux/output latches from input readback. */
    uint8_t pio_input_hold[2];   /* Sampled P0/P1 input hold registers across mux transitions. */
    uint8_t ram[0x380];      /* internal RAM: 0x0080-0x03FF */
    uint8_t vram[GC_VRAM_SIZE];
    uint8_t ext_ram[GC_EXT_RAM_SIZE];
    uint8_t ext_io[0x100];
    uint8_t rom[GC_ROM_SIZE];          /* Slot 1 cartridge storage; MMU bank N maps rom[N * 8 KiB] */
    uint8_t rom2[GC_ROM_SIZE];         /* Slot 2 cartridge storage (P3[7:6] = 10). */
    uint8_t system_rom[GC_SYSTEM_ROM_SIZE]; /* External BIOS/kernel ROM, 32 x 8 KiB banks */
    uint8_t boot_rom[GC_BOOT_SIZE];
    uint8_t patch[GC_PATCH_SIZE];
    uint8_t patch_valid[GC_PATCH_SIZE];
    uint8_t mapped_bank[5];
    bool mapped_is_cart[5];
    bool boot_enabled;
    bool cart_present;
    bool cart2_present;
    bool cart_active;
    uint8_t active_slot;  /* 0 = none, 1 = slot 1 (P3=0x40), 2 = slot 2 (P3=0x80). */
    uint8_t input_buttons[3]; /* Active-low MAME IN0/IN1/IN2 button groups. */
    bool touch_active;
    bool touch_strobed;   /* Set when CPU selects/reads the active touch column. */
    uint8_t touch_column; /* 0..12, per MAME's 13-column touch matrix. */
    uint8_t touch_row;    /* 0..9, active touch row within the selected column. */
    bool serial_cts;      /* P3[2] UART CTS input (active-low RS-232, idle high = true). */
    bool serial_dsr;      /* P3[3] UART DSR input (active-low RS-232, idle high = true). */
    bool serial_dtr;      /* P3[0] UART DTR output driven by P3/P3C. */
    bool serial_rts;      /* P3[1] UART RTS output driven by P3/P3C. */
    bool watch_enabled;
    bool watch_hit;
    uint16_t watch_address;
    uint8_t watch_old_value;
    uint8_t watch_new_value;
} GcBus;

typedef struct {
    uint16_t pc;
    uint64_t cycles;
    uint64_t instructions;
    bool halted;
    bool stopped;
    bool faulted;
    uint16_t fault_pc;
    uint8_t fault_opcode;
    char fault_message[96];
} GcCpu;

typedef struct {
    GcBus bus;
    GcCpu cpu;
} GcMachine;

typedef enum {
    GC_STEP_OK = 0,
    GC_STEP_HALTED,
    GC_STEP_STOPPED,
    GC_STEP_UNSUPPORTED,
    GC_STEP_FAULT
} GcStepResult;

typedef enum {
    GC_BUTTON_UP,
    GC_BUTTON_DOWN,
    GC_BUTTON_LEFT,
    GC_BUTTON_RIGHT,
    GC_BUTTON_MENU,
    GC_BUTTON_PAUSE,
    GC_BUTTON_SOUND,
    GC_BUTTON_A,
    GC_BUTTON_B,
    GC_BUTTON_C,
    GC_BUTTON_POWER, /* MAME labels the corresponding key "Reset". */
    GC_BUTTON_D
} GcButton;

/* Allocate GcMachine on the heap: it includes a 2 MiB ROM image. */
void gc_machine_init(GcMachine *machine);
void gc_machine_reset(GcMachine *machine);
uint8_t gc_bus_read(GcMachine *machine, uint16_t address);
uint8_t gc_bus_debug_read(GcMachine *machine, uint16_t address);
void gc_bus_write(GcMachine *machine, uint16_t address, uint8_t value);
uint8_t gc_bus_vram_dma_read(const GcMachine *machine, uint16_t address);
/* Button setters use the active-low IN0/IN1/IN2 layout; touch is a 13x10 grid. */
void gc_input_set_button(GcMachine *machine, GcButton button, bool down);
bool gc_input_set_touch(GcMachine *machine, bool down, unsigned column, unsigned row);
/* Serial handshake lines on P3[3:0] (Compete.com 2-player link & external modem). */
void gc_serial_set_inputs(GcMachine *machine, bool cts, bool dsr);
bool gc_serial_dtr(const GcMachine *machine);
bool gc_serial_rts(const GcMachine *machine);
uint8_t gc_cpu_reg_read(GcMachine *machine, unsigned index);
void gc_cpu_reg_write(GcMachine *machine, unsigned index, uint8_t value);
uint16_t gc_cpu_sp(const GcMachine *machine);
void gc_cpu_set_sp(GcMachine *machine, uint16_t value);
GcStepResult gc_cpu_step(GcMachine *machine);
void gc_cpu_clear_stop(GcMachine *machine);
size_t gc_disassemble(GcMachine *machine, uint16_t address, char *out, size_t out_size);

bool gc_load_intel_hex(GcMachine *machine, const char *path, char *error, size_t error_size);
bool gc_load_binary_patch(GcMachine *machine, const char *path, uint16_t address,
                          char *error, size_t error_size);
bool gc_load_cartridge(GcMachine *machine, const char *path, char *error, size_t error_size);
bool gc_load_cartridge_slot(GcMachine *machine, unsigned slot, const char *path,
                            char *error, size_t error_size);
void gc_unload_cartridge_slot(GcMachine *machine, unsigned slot);
bool gc_load_system_rom(GcMachine *machine, const char *path, char *error, size_t error_size);
bool gc_load_boot_rom(GcMachine *machine, const char *path, char *error, size_t error_size);
void gc_synthesize_boot_rom(GcMachine *machine);
bool gc_load_firmware(GcMachine *machine, const char *internal_path,
                      const char *external_path, char *error, size_t error_size);
bool gc_nvram_load(GcMachine *machine, const char *path, char *error, size_t error_size);
bool gc_nvram_save(const GcMachine *machine, const char *path, char *error, size_t error_size);
void gc_nvram_clear(GcMachine *machine);
void gc_nvram_seed_warm_boot(GcMachine *machine);

#ifdef __cplusplus
}
#endif

#endif
