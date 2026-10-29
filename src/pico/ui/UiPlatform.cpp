// pico-amiga — the menu's hooks into the firmware (UiPlatform.h).
#include "UiPlatform.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include "input.h"
#include "machine.h"
#include "audio_out.h"
#include "hdmi_audio.h"
extern "C" {
#include "video.h"
}
#include "ui.h"
#include "UiGfx.h"
#include "UiDialog.h"
#include "OSDNewMenu.h"
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
    snprintf(buf, sizeof buf, "%s, %s", machine_name(), machine_rom_name());
    return buf;
}

void uiAmigaReset() { input_request_reset(); }

}  // namespace nm

extern "C" void ui_load_settings(void) { Config::load(); }

extern "C" void ui_init(amiga_row_t *framebuffer) { nm::gfxSetFramebuffer(framebuffer); }

extern "C" int settings_cpu_mhz(void)   { return Config::cpu_mhz; }
extern "C" int settings_vreg_mv(void)   { return Config::vreg_mv; }
// Audio > Output: 3 (HDMI only) means nothing on a VGA display - the DAC then.
extern "C" int settings_audio_out(void) {
    return Config::audio_out == 3 && video_is_vga() ? 0 : Config::audio_out;
}
extern "C" bool settings_dbg_uart(void) { return Config::dbg_uart; }
extern "C" int settings_flash_mhz(void) { return Config::max_flash_freq; }
extern "C" int settings_psram_mhz(void) { return Config::max_psram_freq; }
extern "C" bool settings_hdmi_capture(void) { return Config::hdmi_capture; }

extern "C" int settings_video_mode(void) { return video_is_vga() ? Config::vga_vmode : Config::hdmi_vmode; }

// Booted into a video mode the user has not kept yet (Config::savePendingVideoMode):
// keep it, or go back to the old one and reboot - also when nobody answers, the
// picture may not be there at all.
extern "C" void ui_video_mode_confirm(void) {
    uint8_t h = Config::hdmi_vmode, v = Config::vga_vmode;
    if (!Config::loadPendingVideoMode(h, v)) return;
    sleep_ms(500);                       // let the display lock on
    const bool keep = nm::uiConfirmTimeout("Keep this video mode?", "Video mode", 15);
    Config::clearPendingVideoMode();
    if (keep) return;
    Config::hdmi_vmode = h;
    Config::vga_vmode = v;
    Config::save();
    OSD::esp_hard_reset();
}

extern "C" void settings_apply_video(void) {
    video_set_hpos(Config::hpos);
    amiga_set_fb_x0(Config::hpos ? AMIGA_FB_X0_WIDE : AMIGA_FB_X0_CENTRE);
    amiga_set_vcentre(Config::vcentre);
    amiga_render_async(Config::render_c1);
}

extern "C" void settings_apply_sound(void) {
    audio_set_volume(Config::volume * 16);
    // HDMI carries the sound in Auto (beside the DAC / PWM) and as the only output.
    const int out = settings_audio_out();
    hdmi_audio_set_enabled(out == 0 || out == 3);
}

namespace OSD {

void clickNoPause() {}

void esp_hard_reset() {
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

}  // namespace OSD
