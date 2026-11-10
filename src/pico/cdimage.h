// A CD image for the CD32's drive (akiko.c): an .iso (one MODE1/2048 track) or
// a .cue sheet with its track files - BINARY files of 2048, 2336 or 2352 bytes
// a sector, one for the disc or one a track.  Plain C, no allocation, no files:
// the caller reads the sheet and the track files through callbacks (machine.cpp:
// card files; the bench: stdio).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CD_MAX_TRACKS 99

typedef struct {
    uint8_t  file;        // which FILE of the sheet (0..)
    uint8_t  mode;        // 0 audio, 1 MODE1, 2 MODE2
    uint16_t secsize;     // bytes a sector in the file: 2048, 2336, 2352
    uint32_t lsn;         // INDEX 01 on the disc
    uint32_t foff;        // ...and in the file, bytes
    uint32_t sectors;     // from INDEX 01 to the track's end
} cd_track_t;

typedef struct {
    cd_track_t t[CD_MAX_TRACKS];
    int      tracks;
    uint32_t leadout;     // the sector after the last track
} cd_toc_t;

// A sheet's FILE line: the caller notes the name (as written, relative to the
// sheet) under `index` and returns the file's size in bytes, 0 = not there.
typedef uint32_t (*cd_file_fn)(void *ctx, int index, const char *name);
// `len` bytes of file `index` from `off` into dst; false = failed.
typedef bool (*cd_read_fn)(void *ctx, int index, uint32_t off, void *dst, uint32_t len);

// The sheet's text (len bytes) into a TOC.  False: no track, a missing file, or
// a line it cannot use (a WAVE / MP3 file, a mode it does not know).
bool cd_parse_cue(const char *text, uint32_t len, cd_toc_t *toc, cd_file_fn file, void *ctx);
// An .iso of `size` bytes: one data track, file 0.
bool cd_from_iso(uint32_t size, cd_toc_t *toc);

// Where a data sector is: its file, the byte offset of its stored bytes there,
// how many a sector (2048 / 2336 / 2352), its mode, and how many sectors of the
// track follow from it (itself included) at that spacing.  False: in no data track.
bool cd_locate(const cd_toc_t *toc, uint32_t lsn, int *file, uint32_t *off, uint32_t *secsize, int *mode, uint32_t *run);
// A sector as stored (secsize bytes at src) into Akiko's 2336 bytes at dst.
void cd_unpack_sector(const uint8_t *src, uint32_t secsize, uint8_t *dst);

// A sector for Akiko (amiga_cd_t.read): the 2336 bytes after its header at dst;
// returns its mode (1, 2), -1 when it is in no data track or cannot be read.
int cd_read_sector(const cd_toc_t *toc, uint32_t lsn, uint8_t *dst, cd_read_fn rd, void *ctx);

#ifdef __cplusplus
}
#endif
