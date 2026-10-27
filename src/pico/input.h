// Keyboards, mice and pads -> the Amiga (input.cpp).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void input_init(void);               // USB host, PS/2 keyboard, NES pads
void input_poll(void);               // call often (several times per frame)
void input_frame(void);              // once per frame: keys, joystick and mouse into the Amiga
bool input_reset_requested(void);    // Shift+F12 pressed (or the menu asked) since the last call
void input_request_reset(void);      // the menu's "Reset the Amiga"
bool input_keyboard_mounted(void);   // a USB keyboard is enumerated
bool input_key_held(void);           // any keyboard key or modifier down right now

#ifdef __cplusplus
}
#endif
