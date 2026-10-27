// Host stand-in for TinyUSB: no USB host stack on a PC.  What input.cpp needs of
// it: the HID boot keyboard report and the HID_KEY_* usage ids (generated from
// TinyUSB's hid.h at configure time, so the PC and the board share them).
#pragma once
#include <stdint.h>
#include "host_hid_keys.h"
static inline void tuh_task(void) {}
typedef struct { uint8_t modifier; uint8_t reserved; uint8_t keycode[6]; } hid_keyboard_report_t;
typedef enum {
    KEYBOARD_MODIFIER_LEFTCTRL = 1 << 0, KEYBOARD_MODIFIER_LEFTSHIFT = 1 << 1,
    KEYBOARD_MODIFIER_LEFTALT = 1 << 2, KEYBOARD_MODIFIER_LEFTGUI = 1 << 3,
    KEYBOARD_MODIFIER_RIGHTCTRL = 1 << 4, KEYBOARD_MODIFIER_RIGHTSHIFT = 1 << 5,
    KEYBOARD_MODIFIER_RIGHTALT = 1 << 6, KEYBOARD_MODIFIER_RIGHTGUI = 1 << 7,
} hid_keyboard_modifier_bm_t;
