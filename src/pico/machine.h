// The machine around the core: which Kickstart it runs and what is in its drives
// (machine.cpp). Shared by main.c (boot) and the menu (Machine, Disks).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "amiga.h"

#ifdef __cplusplus
extern "C" {
#endif

// The PSRAM buffers a Kickstart / extended ROM read from the card goes into
// (512 KB each).
void machine_init(uint8_t *rom_buf, uint8_t *ext_buf);

// Fill cfg->rom / ext_rom from Config::kick: a built-in ROM (roms.h, read in
// place from flash), or "sd" = /amiga/kick.rom (+ /amiga/ext.rom) from the card,
// falling back to the first built-in ROM.  False when there is no ROM at all.
bool machine_select_rom(amiga_config_t *cfg);
// The menu changed Config::kick: swap the ROM in and hard-reset the Amiga.
bool machine_apply_kickstart(void);
// What is running, for the menu's sub-header ("Kickstart 1.3").
const char *machine_rom_name(void);

// DF0..DF3 (only DF0 is wired in the core so far).  insert: open the ADF at
// `path` (card path) and put it in the drive; false (and the drive left empty)
// when the file cannot be opened or is not an 880 KB ADF.  The choice is kept in
// Config (df0, df0_wp) by the caller's save.
bool machine_insert(int drive, const char *path, bool write_protect);
void machine_eject(int drive);
const char *machine_disk(int drive);   // card path, "" when empty
// At boot: put Config::df0 back (or the legacy /amiga/df0.adf).
void machine_restore_disks(void);

#ifdef __cplusplus
}
#endif
