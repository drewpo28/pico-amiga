#pragma once
#include <stdbool.h>

void usb_input_init(void);
void usb_input_poll(void);              // call often (at least once per frame)
bool usb_input_reset_requested(void);   // F12 pressed since the last call
bool usb_input_keyboard_mounted(void);  // a boot-protocol keyboard is enumerated
bool usb_input_key_held(void);          // any key or modifier down right now
