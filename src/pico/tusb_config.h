// TinyUSB configuration: host mode on the native USB port, HID only.
// Sizes follow pico-speccy's tusb_config.h, where they were tuned on hardware.
#pragma once

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (the pico-sdk does this)
#endif

#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_HOST
#define BOARD_TUH_RHPORT            0
#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED

// tuh_task() only runs between emulated frames (~20 ms).  The default 16-entry
// event queue can overflow meanwhile; a dropped xfer-complete event leaves that
// endpoint busy for ever (a dead keyboard).
#define CFG_TUH_TASK_QUEUE_SZ       64
// A device whose configuration descriptor does not fit is not enumerated at all.
#define CFG_TUH_ENUMERATION_BUFSIZE 1024
#define CFG_TUH_HUB                 1
#define CFG_TUH_DEVICE_MAX          6
#define CFG_TUH_HID                 8   // composite keyboards/receivers use several interfaces
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64
