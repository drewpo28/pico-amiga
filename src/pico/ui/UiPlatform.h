// pico-amiga — what the menu (from pico-speccy) needs from the rest of the firmware,
// in place of pico-speccy's ESPectrum:: / OSD:: / VIDEO:: calls.
#pragma once
#include <stdint.h>
#include "drivers/input_glue.h"          // fabgl virtual keys, Debug::log
#include "drivers/input/keyboard.h"

namespace nm {
// input.cpp: the VK queue the whole firmware reads (USB/PS/2/pads, or SDL on a PC).
// uiKeyAvailable() also runs the input devices, as Keyboard::virtualKeyAvailable does.
bool             uiKeyAvailable();
bool             uiReadKey(fabgl::VirtualKeyItem* k);
fabgl::Keyboard* uiKeyboard();
// UiPlatform.cpp
void        uiSleepMs(int ms);
const char* uiMachineName();            // the sub-header's "Machine: ..."
void        uiAmigaReset();             // the emulated machine (input.cpp's reset request)
}

namespace OSD {
void clickNoPause();                    // key click (none on pico-amiga yet)
[[noreturn]] void esp_hard_reset();     // reboot the board (the PC build re-execs)
}

inline unsigned debug_sp() { return 0; }

// US-layout shifted form of a symbol/digit (pico-speccy's OSDMain.cpp): map_key()
// gives only unshifted symbol VKs, so the line editor translates Shift+key itself.
char shiftSymUS(char c);

// input.cpp: while a line editor owns the keyboard, Space types instead of
// confirming (kbdExtraMapping does not inject VK_MENU_ENTER for it).
extern volatile bool g_ui_text_entry;
