// The menu and the settings, as the C side of the firmware (main.c) sees them.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once the card is mounted: read the settings (Config::load) and give the menu the
// framebuffer it draws into.
void ui_init(uint16_t *framebuffer);

#ifdef __cplusplus
}
#endif
