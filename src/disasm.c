#include "gamecom.h"

#include <stdio.h>

size_t gc_disassemble(GcMachine *machine, uint16_t address, char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    uint8_t op = gc_bus_debug_read(machine, address);
    uint8_t x, y;
    size_t len = 1;
    const char *name = NULL;
    char temp[96];

    switch (op) {
    case 0x00:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1)); len = 2;
        (void)snprintf(temp, sizeof(temp), "CLR RF[%02X]", x); name = temp; break;
    case 0x08:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1)); len = 2;
        (void)snprintf(temp, sizeof(temp), "INC RF[%02X]", x); name = temp; break;
    case 0x09:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1)); len = 2;
        (void)snprintf(temp, sizeof(temp), "DEC RF[%02X]", x); name = temp; break;
    case 0x2E:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1)); len = 2;
        (void)snprintf(temp, sizeof(temp), "MOV PS0,#%02X", x); name = temp; break;
    case 0x40: case 0x41: case 0x42: case 0x48:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1));
        y = gc_bus_debug_read(machine, (uint16_t)(address + 2)); len = 3;
        (void)snprintf(temp, sizeof(temp), "%s RF[%02X],RF[%02X]",
                       op == 0x40 ? "CMP" : op == 0x41 ? "ADD" : op == 0x42 ? "SUB" : "MOV", y, x);
        name = temp; break;
    case 0x3B: {
        uint8_t mode = gc_bus_debug_read(machine, (uint16_t)(address + 1));
        static const uint8_t pair_base[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };
        unsigned source = pair_base[(mode >> 3) & 7u];
        if ((mode & 0xC0u) == 0x80u) {
            uint16_t target = (uint16_t)(((uint16_t)gc_bus_debug_read(machine, (uint16_t)(address + 2)) << 8) |
                                         gc_bus_debug_read(machine, (uint16_t)(address + 3)));
            len = 4;
            if (mode & 7u)
                (void)snprintf(temp, sizeof(temp), "MOVW @$%04X(RR%02X),RR%02X", target,
                               pair_base[mode & 7u], source);
            else
                (void)snprintf(temp, sizeof(temp), "MOVW @$%04X,RR%02X", target, source);
        } else {
            len = 2;
            (void)snprintf(temp, sizeof(temp), "MOVW indirect(mode=%02X),RR%02X", mode, source);
        }
        name = temp; break;
    }
    case 0x39: {
        uint8_t mode = gc_bus_debug_read(machine, (uint16_t)(address + 1));
        static const uint8_t pair_base[8] = { 0, 8, 2, 10, 4, 12, 6, 14 };
        unsigned source = pair_base[(mode >> 3) & 7u];
        if ((mode & 0xC0u) == 0x80u) {
            uint16_t target = (uint16_t)(((uint16_t)gc_bus_debug_read(machine, (uint16_t)(address + 2)) << 8) |
                                         gc_bus_debug_read(machine, (uint16_t)(address + 3)));
            len = 4;
            if (mode & 7u)
                (void)snprintf(temp, sizeof(temp), "MOV @$%04X(RR%02X),RF[%02X]", target,
                               pair_base[mode & 7u], source);
            else
                (void)snprintf(temp, sizeof(temp), "MOV @$%04X,RF[%02X]", target, source);
        } else {
            len = 2;
            (void)snprintf(temp, sizeof(temp), "MOV indirect(mode=%02X),RF[%02X]", mode, source);
        }
        name = temp; break;
    }
    case 0x49: {
        uint16_t target = (uint16_t)(((uint16_t)gc_bus_debug_read(machine, (uint16_t)(address + 1)) << 8) |
                                     gc_bus_debug_read(machine, (uint16_t)(address + 2)));
        len = 3; (void)snprintf(temp, sizeof(temp), "CALL %04X", target); name = temp; break;
    }
    case 0x4B: {
        uint8_t pair = gc_bus_debug_read(machine, (uint16_t)(address + 1));
        uint16_t value = (uint16_t)(((uint16_t)gc_bus_debug_read(machine, (uint16_t)(address + 2)) << 8) |
                                    gc_bus_debug_read(machine, (uint16_t)(address + 3)));
        len = 4;
        (void)snprintf(temp, sizeof(temp), "MOVW RR%02X,#%04X", pair, value);
        name = temp; break;
    }
    case 0x50: case 0x51: case 0x52: case 0x58:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1));
        y = gc_bus_debug_read(machine, (uint16_t)(address + 2)); len = 3;
        (void)snprintf(temp, sizeof(temp), "%s RF[%02X],#%02X",
                       op == 0x50 ? "CMP" : op == 0x51 ? "ADD" : op == 0x52 ? "SUB" : "MOV", y, x);
        name = temp; break;
    case 0x2D: case 0x3D:
        x = gc_bus_debug_read(machine, (uint16_t)(address + 1)); len = 2;
        (void)snprintf(temp, sizeof(temp), "DM #$%02X", x); name = temp; break;
    case 0xF0: name = "STOP"; break;
    case 0xF1: name = "HALT"; break;
    case 0xF8: name = "RET"; break;
    case 0xF9: name = "IRET"; break;
    case 0xFA: name = "CLRC"; break;
    case 0xFB: name = "COMC"; break;
    case 0xFC: name = "SETC"; break;
    case 0xFD: name = "EI"; break;
    case 0xFE: name = "DI"; break;
    case 0xFF: name = "NOP"; break;
    default:
        (void)snprintf(temp, sizeof(temp), ".DB $%02X ; unsupported", op); name = temp; break;
    }

    (void)snprintf(out, out_size, "%s", name);
    return len;
}
