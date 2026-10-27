#pragma once
#include <stdbool.h>

void usb_input_init(void);
void usb_input_poll(void);              // call often (at least once per frame)
bool usb_input_reset_requested(void);   // F12 pressed since the last call
