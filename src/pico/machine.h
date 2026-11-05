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
// cfg->chipset, chip_size, slow_size, gayle.  The A500 has 512 KB chip RAM at the
// chip_ram buffer and 512 KB slow RAM at slow_ram; the A500+ and A600 1 MB of chip RAM
// and no slow RAM, so slow_ram must be chip_ram + 512 KB (PSRAM: one 1 MB
// area).  False when there is no ROM at all.
bool machine_select_rom(amiga_config_t *cfg);
// The menu changed Config::kick or Config::machine: swap the ROM and the model
// in and hard-reset the Amiga.
bool machine_apply_kickstart(void);
// Config::machine.  The A600 runs on the A500+'s memory layout (1 MB of chip RAM
// over chip_ram and slow_ram) and adds Gayle.
enum { MACHINE_A500 = 0, MACHINE_A500P = 1, MACHINE_A600 = 2, MACHINE_COUNT = 3 };
const char *machine_model_name(int m);   // "A600"
const char *machine_model_desc(int m);   // "A600 (ECS, 1 MB chip RAM, Gayle)"
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

// ---- snapshots (snapshot.cpp) ----------------------------------------------------------
#include <string>
// What is in the drives, as a snapshot keeps it.
struct MachineDisks {
    std::string path[4];       // card paths, "" = empty
    bool wp[4], manual[4];     // write-protected; DF1..DF3 put there by hand
    std::string set;           // the .m3u disk set loaded, "" = none
    int set_index;             // its disk in DF0
};
void machine_get_disks(MachineDisks &d);
// The A600's IDE hard disk: an HDF on the card (ide.c: a whole drive with its
// RDB, or one bare partition), opened for reading and writing (read-only when
// the file is).  machine_set_hdf: "" removes it; false when the file cannot be
// opened (the drive is then empty).  Config::hdf is the caller's to keep.  The
// core sees it on the A600 only and from the next reset on (Kickstart scans
// the port at reset): the caller resets.
bool machine_set_hdf(const char *path);
const char *machine_hdf(void);          // the HDF's path, or "SD partition 3"; "" = none
// The hard disk as a window of the SD card's own sectors (pico-speccy's raw IDE
// slot): the Amiga's block 0 is card sector `lba`, `len` sectors long (0 = to
// the end; at most 4 GB: amiga_disk_t's byte offsets), no FAT, no file.  A
// partition holding a whole Amiga drive (an HDF written onto it) or one bare
// Amiga partition, as an HDF.  Writable only on request, and never over the FAT
// volume the firmware itself mounted (that one is forced read-only).  `part`
// (1-8, 0 = typed) only names it.  Writes reach FatFs's cache through
// ff_raw_write_notify.  machine_set_hdf("") removes it too.
bool machine_set_hdf_raw(int part, uint32_t lba, uint32_t len, bool writable);
bool machine_hdf_is_raw(void);
bool machine_hdf_ro(void);
// The card's partitions (ide_mbr.h: the four MBR entries, 5-8 the logical ones);
// `files` marks the one FatFs mounted (the card's files).  Returns how many are
// usable, 0 = no MBR (a card formatted without one), -1 = the card cannot be read.
struct MachinePart { uint8_t no, type; uint32_t lba, sectors; bool files; };
int machine_sd_parts(MachinePart out[8]);
// Every drive emptied, then these disks put in (the set loaded first) and the
// drives Config::drives asks for connected; the disks that cannot be opened are
// listed in `missing` (their drives stay empty).
void machine_put_disks(const MachineDisks &d, std::string &missing);
// The CRC-32s (amiga_crc32) of the ROM and extended ROM a Config::kick /
// kick_file pair would run, read without loading them; ext 0 = none.  False:
// no ROM at all.
bool machine_rom_crcs(const std::string &kick, const std::string &file, uint32_t *rom, uint32_t *ext);
// "Kickstart 1.3", or a Custom ROM's file name.
std::string machine_rom_label(const std::string &kick, const std::string &file);
#endif
