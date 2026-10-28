// ROM images linked into the firmware (cmake/roms.cmake: DiagROM and the A500
// Kickstarts, from the build machine's ROM folder, never from git).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char    *id;      // what storage.nvs keeps (Config::kick)
    const char    *label;   // what the menu shows
    const char    *machine; // the Machine menu rows it is under ("DiagROM", "A500,A500+")
    const uint8_t *data;    // in flash (XIP); the core reads it in place
    uint32_t       size;    // 256 or 512 KB
} builtin_rom_t;

extern const builtin_rom_t builtin_roms[];
extern const unsigned builtin_rom_count;

// The entry with this id, or NULL.
const builtin_rom_t *builtin_rom_find(const char *id);
// Whether the ROM is listed under that Machine menu row.
bool builtin_rom_on(const builtin_rom_t *r, const char *machine);

#ifdef __cplusplus
}
#endif
