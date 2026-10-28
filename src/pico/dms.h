// DMS (DiskMasher) disk images, unpacked to an ADF in memory (dms.c).
// Plain C, no allocation: the caller gives the work area (dms_work_size(), the
// firmware puts it in PSRAM) and reads the archive through a callback.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMS_ADF_SIZE 901120u                 // 80 cylinders x 2 heads x 11 x 512

enum {
    DMS_OK = 0,
    DMS_ERR_READ,            // the callback failed / the file ended mid-track
    DMS_ERR_NOT_DMS,         // no "DMS!" or a bad header CRC
    DMS_ERR_UNSUPPORTED,     // encrypted, HD, FMS archive, unknown mode
    DMS_ERR_DATA,            // a track's CRC, checksum or packed data is bad
};

// Read the next `len` bytes of the archive; returns how many it read (fewer at
// the end of the file).
typedef uint32_t (*dms_read_fn)(void *ctx, void *dst, uint32_t len);

uint32_t dms_work_size(void);

// Unpack the whole archive into `adf` (DMS_ADF_SIZE bytes; cylinders the archive
// leaves out stay zero).  `work`: dms_work_size() bytes, 4-byte aligned.
int dms_unpack(dms_read_fn rd, void *ctx, uint8_t *adf, void *work);

const char *dms_error(int err);

#ifdef __cplusplus
}
#endif
