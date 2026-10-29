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
// (512 KB each), and the area DMS images are unpacked into (the decoder's ~100 KB
// work area, then 880 KB per drive: DF0..DF3 need 3.6 MB, a smaller area serves
// the first drives only).
void machine_init(uint8_t *rom_buf, uint8_t *ext_buf, uint8_t *dms_area, uint32_t dms_size);

// Fill cfg->rom / ext_rom from Config::kick: a built-in ROM (roms.h, read in
// place from flash), or "sd" = Config::kick_file from the card (Machine >
// Custom ROM; with an ext.rom in its folder, that at $E00000: AROS), falling
// back to the first built-in ROM; and the model from Config::machine:
// cfg->chipset, chip_size, slow_size.  The A500 has 512 KB chip RAM at the
// chip_ram buffer and 512 KB slow RAM at slow_ram; the A500+ 1 MB of chip RAM
// and no slow RAM, so slow_ram must be chip_ram + 512 KB (PSRAM: one 1 MB
// area).  False when there is no ROM at all.
bool machine_select_rom(amiga_config_t *cfg);
// The menu changed Config::kick or Config::machine: swap the ROM and the model
// in and hard-reset the Amiga.
bool machine_apply_kickstart(void);
// What is running, for the menu's sub-header ("A500+", "Kickstart 2.04").
const char *machine_name(void);
const char *machine_rom_name(void);

// DF0..DF3 (only DF0 is wired in the core so far).  insert: open the ADF at
// `path` (card path) and put it in the drive; false (and the drive left empty)
// when the file cannot be opened or is not an 880 KB ADF.  A .dms is unpacked
// into the drive's PSRAM slot instead and is always write-protected.  The choice
// is kept in Config (df0, df0_wp) by the caller's save.
bool machine_insert(int drive, const char *path, bool write_protect);
void machine_eject(int drive);
const char *machine_disk(int drive);   // card path, "" when empty
bool machine_disk_ro(int drive);       // the Amiga cannot write the disk in it
bool machine_disk_dms(int drive);      // it is a DMS (unpacked, read-only)
// The disk in `drive` again, write-protected or not (a disk set stays loaded).
bool machine_write_protect(int drive, bool on);
// At boot: put Config::df0 back (or the legacy /amiga/df0.adf); an .m3u there
// brings its disk set back with disk Config::df0_disk in the drive.
void machine_restore_disks(void);

// Disk sets, as RetroArch's .m3u playlists: one ADF per line (relative to the
// list's folder), '#' lines ignored but ours, "#DRIVES:n" (1-4: the drives
// Floppy drives Auto connects for the set). Loading one puts disk `index` in DF0; the
// others wait to be swapped in (machine_set_select), like RetroArch's Disk
// Control. Inserting a single ADF with machine_insert() ends the set.
bool machine_load_set(const char *m3u_path, int index);
int  machine_set_count(void);           // 0 = no set loaded
int  machine_set_index(void);           // the disk now in DF0
const char *machine_set_disk(int i);    // its card path
bool machine_set_select(int i);         // swap disk i into DF0

// Devices > Disks > New blank disk: an empty formatted 880 KB ADF (OFS, not bootable, as a
// quick Format leaves it) named after the volume, next to the loaded disk set
// (or DF0's disk, else in /amiga/disks/). With a set loaded it joins the set
// (machine_set_add); otherwise it goes into DF0. Its path goes to path_out.
bool machine_new_disk(const char *volume, char *path_out, unsigned path_size);
// The ADF itself: an empty formatted volume at `path` (which must not exist).
bool machine_format_adf(const char *path, const char *volume);
// The browser's F1 on a disk image: what is on it, as text for a page - the
// volume and its filesystem, whether it boots, its dates and free space, the
// root directory and the start of s/startup-sequence; a custom (non-DOS)
// format says so. A DMS is unpacked into a spare PSRAM slot first (8 MB boards).
void machine_disk_info(const char *path, char *out, unsigned size);
// Add a disk to the loaded set: to the .m3u on the card and to the swap list.
bool machine_set_add(const char *path);

// Connect the drives Config::drives asks for (0 = Auto: one per disk of a set,
// up to 4; one otherwise) and put the set's disks 2.. into DF1.. . True when
// the count changed: Kickstart only looks for drives at reset.
bool machine_apply_drives(void);
int  machine_drives(void);

#ifdef __cplusplus
}
#endif
