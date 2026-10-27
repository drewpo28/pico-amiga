// Shared between the SDL front end (host_main.cpp) and the drivers (host_drivers.cpp).
#pragma once
#include <SDL.h>
#include <stdint.h>
#include "amiga.h"

#define HOST_PSRAM_SIZE (8u * 1024 * 1024)

extern const amiga_row_t *volatile host_fb;          // the firmware's framebuffer, once video_start ran
void host_key(SDL_Scancode sc, bool down);         // -> HID reports for core0
void host_mouse(int dx, int dy, bool l, bool r);   // -> Mouse:: on core0
