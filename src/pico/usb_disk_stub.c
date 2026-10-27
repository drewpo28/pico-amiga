// The SD driver (src/pico/drivers/sdcard, from pico-speccy) routes FatFs
// physical drive 1 to a USB mass-storage stick.  There is no MSC host here
// yet, so drive 1 simply reports "no medium".
#include "ff.h"
#include "diskio.h"

DSTATUS usb_disk_status(void) { return STA_NOINIT | STA_NODISK; }
DSTATUS usb_disk_initialize(void) { return STA_NOINIT | STA_NODISK; }
DRESULT usb_disk_read(BYTE *buff, LBA_t sector, UINT count) { (void)buff; (void)sector; (void)count; return RES_NOTRDY; }
DRESULT usb_disk_write(const BYTE *buff, LBA_t sector, UINT count) { (void)buff; (void)sector; (void)count; return RES_NOTRDY; }
DRESULT usb_disk_ioctl(BYTE cmd, void *buff) { (void)cmd; (void)buff; return RES_NOTRDY; }
