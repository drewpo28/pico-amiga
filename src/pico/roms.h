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
    const char    *file;    // its image's name, as WHDLoad's Devs:Kickstarts has it ("kick40068.A1200")
} builtin_rom_t;

// WHDLoad's files (cmake/roms.cmake: ROMS_DIR/WHDLoad/), for the volumes built
// from LHA archives (whdboot.c).
typedef struct {
    const char    *path;    // on the volume: "C/WHDLoad", "Devs/Kickstarts/kick40068.A1200.RTB"
    const uint8_t *data;
    uint32_t       size;
} builtin_whd_t;
extern const builtin_whd_t builtin_whd[];
extern const unsigned builtin_whd_count;

extern const builtin_rom_t builtin_roms[];
extern const unsigned builtin_rom_count;

// The entry with this id, or NULL.
const builtin_rom_t *builtin_rom_find(const char *id);
// Whether the ROM is listed under that Machine menu row.
bool builtin_rom_on(const builtin_rom_t *r, const char *machine);

#ifdef __cplusplus
}
#endif
