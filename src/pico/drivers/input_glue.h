// PICO-AMIGA: what the input drivers taken from pico-speccy (usbhost/, input/)
// expect from that program's app/Config.h, app/ESPectrum.h and app/Debug.h.
// The names are kept so those files stay close to their originals; the state
// itself lives in src/pico/input.cpp.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "drivers/input/fabutils.h"

namespace Config {
    extern uint8_t  secondJoy;      // 1: USB/NES pad 1 does not drive joystick 1
    extern uint16_t mouse_sens;     // Q8 multiplier on HID counts (256 = 1:1)
    extern uint8_t  mouse_swap;     // 0 off, 1 on, 2 auto (first single press decides)
    extern bool     CursorAsJoy;    // cursor keys + Ctrl drive joystick 1
}

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
