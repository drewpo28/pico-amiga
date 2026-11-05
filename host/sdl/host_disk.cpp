// The raw SD card (disk_read / disk_write / disk_ioctl, drive 0) for what reads the
// card sector by sector - the A600's hard disk as an SD partition (machine.cpp) -
// NOT the firmware's own filesystem, which is a host directory (host_fatfs.cpp).
// pico-speccy's host_disk.cpp (its pico-speccy-host branch), image mode only:
// --sd card.img gives a card image (an MBR with partitions, as the board's card);
// without it there is no raw card.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "host_posix.h"

extern "C" {
#include "ff.h"
#include "diskio.h"
}

const char *host_sd_path = nullptr;
static int s_fd = -1;
static bool s_ro = false;
static std::mutex s_mx;

static bool ensure() {
    if (!host_sd_path) return false;
    if (s_fd < 0) {
        s_fd = open(host_sd_path, O_RDWR | O_BINARY);
        if (s_fd < 0) { s_fd = open(host_sd_path, O_RDONLY | O_BINARY); s_ro = s_fd >= 0; }
        if (s_fd < 0) fprintf(stderr, "host: no SD image '%s'\n", host_sd_path);
    }
    return s_fd >= 0;
}

extern "C" {
DSTATUS disk_initialize(BYTE pdrv) {
    std::lock_guard<std::mutex> lk(s_mx);
    if (pdrv != 0 || !ensure()) return STA_NOINIT | STA_NODISK;
    return s_ro ? STA_PROTECT : 0;
}
DSTATUS disk_status(BYTE pdrv) {
    std::lock_guard<std::mutex> lk(s_mx);
    if (pdrv != 0 || !ensure()) return STA_NOINIT | STA_NODISK;
    return s_ro ? STA_PROTECT : 0;
}
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    std::lock_guard<std::mutex> lk(s_mx);
    if (pdrv != 0 || !ensure()) return RES_NOTRDY;
    const ssize_t n = pread(s_fd, buff, (size_t)count * 512, (off_t)sector * 512);
    if (n < 0) return RES_ERROR;
    if ((size_t)n < (size_t)count * 512) memset(buff + n, 0, (size_t)count * 512 - n);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    std::lock_guard<std::mutex> lk(s_mx);
    if (pdrv != 0 || !ensure()) return RES_NOTRDY;
    if (s_ro) return RES_WRPRT;
    const ssize_t n = pwrite(s_fd, buff, (size_t)count * 512, (off_t)sector * 512);
    return n == (ssize_t)count * 512 ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    std::lock_guard<std::mutex> lk(s_mx);
    if (pdrv != 0 || !ensure()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: fsync(s_fd); return RES_OK;
    case GET_SECTOR_COUNT: { struct stat st; fstat(s_fd, &st); *(LBA_t *)buff = (LBA_t)(st.st_size / 512); return RES_OK; }
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
}
