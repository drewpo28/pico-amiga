// An LHA archive turned into a bootable FFS hard disk image (whdboot.c): a
// WHDLoad install (a .slave with its icon) boots into WHDLoad, anything else
// into its program.  A lone program (an Amiga executable, no archive) the same
// way: it is one of the extra files, and the startup-sequence runs it.  Plain C, no allocation, no files: the caller reads the
// archive and the extra files through callbacks and writes the image through
// one (machine.cpp: card files; the bench: stdio).
//
// The volume holds the archive as it is, plus:
//   C/WHDLoad                    (an extra file the caller gives)
//   Devs/Kickstarts/kickN.M(.RTB) the Kickstart images the slave names, when the
//                                caller has them (built-in ROMs + relocation tables)
//   S/Startup-Sequence           CD into the slave's folder, WHDLoad SLAVE=x
//                                PRELOAD plus the icon's tooltypes as options
//   and whatever other extra files the caller adds (the card's /amiga/whdload/
//   folder: S/WHDLoad.prefs, more Kickstarts).  An archive with its own
//   S/Startup-Sequence (a whole system) keeps it.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lha.h"
#include "ffs.h"

#ifdef __cplusplus
extern "C" {
#endif

// A file the volume gets besides the archive's: in memory (data) or read
// through rd (data NULL).
typedef struct {
    const char    *path;       // on the volume: "C/WHDLoad", "Devs/Kickstarts/kick40068.A1200"
    const uint8_t *data;
    uint32_t       size;
    lha_read_fn    rd; void *ctx;
    // Kickstart images and .RTB files only go in when the slave names them:
    // the version.model ("40068.A1200") the slave must mention, "" = always.
    const char    *kick;
} whd_extra_t;

typedef struct {
    // in
    lha_read_fn    rd; void *rctx;          // the archive (rd NULL: none, a lone program: run)
    const char    *run;                     // no archive: the extra file (its volume path, at the root) to run
    const whd_extra_t *extra; int extra_count;
    const char    *volume;                  // the volume's name
    uint32_t       unix_time;               // its date
    uint32_t       free_blocks;             // left free for saves (0: 4096 = 2 MB)
    // out
    char     slave[256];                    // the slave's path on the volume, "" = none
    char     program[256];                  // no slave: the program the boot runs, "" = none
    char     command[384];                  // the startup-sequence's command line
    char     kicks[64];                     // the Kickstarts the slave needs that went in ("40068.A1200 ...")
    char     missing[64];                   // ...and those it names that the caller had none of
    uint32_t base_mem, exp_mem;             // the slave's chip and other memory (0: no slave)
    uint32_t blocks;                        // the image's size in blocks
    bool     own_startup;                   // the archive brought its own S/Startup-Sequence
} whd_job_t;

enum { WHD_OK = 0, WHD_ERR_ARCHIVE, WHD_ERR_ARENA, WHD_ERR_TOO_BIG, WHD_ERR_WRITE, WHD_ERR_EXTRA, WHD_ERR_NOTHING };

// The arena: the decoder's work area, the volume's tree, a staging buffer; 256 KB
// does ~1500 files.
#define WHD_ARENA_MIN (256u * 1024u)

// Pass 1: read the archive, pick what boots, lay the volume out.  Fills the
// job's out fields; the image will be job->blocks * 512 bytes.
int whd_plan(whd_job_t *job, void *arena, uint32_t arena_size);
// Pass 2: write the image (the arena as whd_plan left it).
int whd_write(whd_job_t *job, void *arena, ffs_write_fn wr, void *wctx);

const char *whd_error(int err);

// A Kickstart image's or relocation table's tag from its file name:
// ".../kick40068.A1200" and ".../kick40068.A1200.RTB" -> "40068.A1200" (into out,
// 24 bytes); "" for any other file.
const char *whd_kick_tag(const char *path, char *out);

#ifdef __cplusplus
}
#endif
