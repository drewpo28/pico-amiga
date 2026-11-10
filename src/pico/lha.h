// LHA / LZH archives (lha.c): the members' headers and their data unpacked.
// Plain C, no allocation: the caller gives the work area (lha_work_size(), the
// firmware puts it in PSRAM) and reads the archive through callbacks.
// Methods: -lh0- (stored), -lh4- .. -lh7- (static Huffman, 4 to 64 KB window),
// -lhd- (a directory); header levels 0, 1 and 2 (Amiga LhA writes 0 and 1, with
// the file comment after a NUL in the name).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LHA_OK = 0,
    LHA_END,                 // no more members
    LHA_ERR_READ,            // the callback failed / the archive ended early
    LHA_ERR_HEADER,          // not an LHA header
    LHA_ERR_METHOD,          // a method we do not unpack (-lh1-, -lzs-, ...)
    LHA_ERR_DATA,            // bad Huffman tables or a CRC mismatch
    LHA_ERR_WRITE,           // the output callback failed
};

// Read `len` bytes at `off` of the archive; returns how many it read.
typedef uint32_t (*lha_read_fn)(void *ctx, uint32_t off, void *dst, uint32_t len);
// Take the next `len` unpacked bytes; nonzero stops the unpacking (LHA_ERR_WRITE),
// LHA_STOP stops it without an error (lha_unpack returns LHA_OK).
typedef int (*lha_write_fn)(void *ctx, const uint8_t *src, uint32_t len);
#define LHA_STOP 0x5354

typedef struct {
    char     path[256];      // '/'-separated, as stored (a directory's has no trailing '/')
    char     comment[80];    // the Amiga file comment, "" if none
    char     method[6];      // "-lh5-"
    bool     dir;            // -lhd-: a directory entry
    uint8_t  attr;           // the attribute byte (Amiga LhA: the protection bits)
    char     os;             // the packer's system ('A' Amiga, 'M' MS-DOS, ...; 0 level 0)
    uint32_t size;           // unpacked
    uint32_t packed;         // the data's bytes in the archive
    uint32_t data_off;       // where the data starts in the archive
    uint32_t next_off;       // the next header
    uint32_t unix_time;      // seconds since 1970 (local time for levels 0/1)
    uint16_t crc;            // CRC-16 of the unpacked data
} lha_entry_t;

uint32_t lha_work_size(void);

// The member whose header is at `off` (0: the first).  LHA_END after the last.
int lha_header(lha_read_fn rd, void *ctx, uint32_t off, lha_entry_t *e);

// Unpack a member's data through `wr`, checking its CRC.  `work`:
// lha_work_size() bytes, 4-byte aligned.
int lha_unpack(lha_read_fn rd, void *rctx, const lha_entry_t *e, lha_write_fn wr, void *wctx, void *work);

const char *lha_error(int err);

#ifdef __cplusplus
}
#endif
