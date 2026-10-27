// pico-amiga — the menu's hooks into the firmware (UiPlatform.h).
#include "UiPlatform.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include "input.h"
#include "machine.h"
#include "ui.h"
#include "UiGfx.h"
#include "app/Config.h"

char shiftSymUS(char c) {
    switch (c) {
        case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case '\\': return '|';
        case ';': return ':'; case '\'': return '"'; case '`': return '~';
        case ',': return '<'; case '.': return '>'; case '/': return '?';
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '('; case '0': return ')';
        default:  return c;   // already shifted, or a non-shiftable char
    }
}

namespace nm {

void uiSleepMs(int ms) { sleep_ms((uint32_t)ms); }

const char* uiMachineName() {
    static char buf[48];
    snprintf(buf, sizeof buf, "A500, %s", machine_rom_name());
    return buf;
}

void uiAmigaReset() { input_request_reset(); }

}  // namespace nm

extern "C" void ui_init(uint16_t *framebuffer) {
    Config::load();
    nm::gfxSetFramebuffer(framebuffer);
}

namespace OSD {

void clickNoPause() {}

void esp_hard_reset() {
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

}  // namespace OSD
