// What input.cpp needs from the platform: the board's devices (input_devices.cpp)
// or, on the PC, SDL (host/sdl).  Both feed process_kbd_report() and the VK queue.
#pragma once
#include <stdbool.h>
#include "tusb.h"                       // hid_keyboard_report_t, HID_KEY_*, KEYBOARD_MODIFIER_*
#include "drivers/input_glue.h"

// Pad state shared with hid_app.cpp / joy_hid.cpp (they diff against it).
struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};
extern input_bits_t gamepad1_bits, gamepad2_bits;

// input.cpp
void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report);

// The platform
void input_devices_init(void);
bool input_devices_keyboard_mounted(void);
void repeat_me_for_input();             // called often; also by fabgl::Keyboard
