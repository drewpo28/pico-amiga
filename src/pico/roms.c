// Lookup over the built-in ROM table (roms.h); the table itself is generated.
#include <string.h>
#include "roms.h"

const builtin_rom_t *builtin_rom_find(const char *id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < builtin_rom_count; i++)
        if (!strcmp(builtin_roms[i].id, id)) return &builtin_roms[i];
    return NULL;
}

bool builtin_rom_on(const builtin_rom_t *r, const char *machine) {
    const size_t n = strlen(machine);
    for (const char *p = r->machine; p && *p; ) {
        const char *e = strchr(p, ',');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, machine, n)) return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}
