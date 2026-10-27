// Lookup over the built-in ROM table (roms.h); the table itself is generated.
#include <string.h>
#include "roms.h"

const builtin_rom_t *builtin_rom_find(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < builtin_rom_count; i++)
        if (!strcmp(builtin_roms[i].id, id)) return &builtin_roms[i];
    return NULL;
}
