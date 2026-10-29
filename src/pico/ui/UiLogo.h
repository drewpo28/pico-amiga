// pico-amiga - Help > About's logo (UiLogo.c, tools/gen_logo.py): framebuffer
// pixels in 0x0RGB, drawn as they are (no palette slots, unlike pico-speccy's).
#pragma once

#include <stdint.h>

#define UI_LOGO_W 128
#define UI_LOGO_H 128

#ifdef __cplusplus
extern "C" {
#endif
extern const uint16_t ui_logo128[UI_LOGO_W * UI_LOGO_H];
#ifdef __cplusplus
}
#endif
