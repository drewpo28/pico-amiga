// PICO-AMIGA: what the input drivers taken from pico-speccy (usbhost/, input/)
// expect from that program's app/Config.h, app/ESPectrum.h and app/Debug.h.
// The names are kept so those files stay close to their originals; Config is
// pico-amiga's (src/pico/app/Config.h), the mouse state lives in input.cpp.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "drivers/input/fabutils.h"
#include "app/Config.h"           // secondJoy, mouse_sens, mouse_swap, CursorAsJoy

namespace Debug {
    inline void log(const char *, ...) {}
}

// The HID mouse, accumulated by hid_app.cpp's mouse_apply().
namespace Mouse {
    extern bool    seen;
    extern bool    buttonL, buttonR, buttonM;
    extern int32_t x, y;            // y up-positive (pico-speccy's Kempston convention)
    extern uint8_t wheel;
}

void joyPushData(fabgl::VirtualKey virtualKey, bool down);
void kbdPushData(fabgl::VirtualKey virtualKey, bool down);
