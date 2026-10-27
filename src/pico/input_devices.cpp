// The board's input devices behind input.cpp: TinyUSB host (pico-speccy's
// drivers/usbhost: keyboards, mice, HID pads, XInput), the PS/2 keyboard and the
// NES/SNES pads.  On the PC, host/sdl provides the same functions over SDL.
#include "input_devices.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "tusb.h"
#include "audio_out.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"      // pico-speccy's headers
#include "drivers/ps2kbd/ps2kbd_mrmltr.h"
#ifdef USE_NESPAD
#include "drivers/nespad/nespad.h"
#endif
#pragma GCC diagnostic pop

// TinyUSB's TU_ASSERT breakpoint (tusb_config.h): count instead of halting a
// debug session; hid_app.cpp's wedged-endpoint recovery reads the counter.
volatile uint32_t g_tusb_assert_count = 0;
extern "C" void pico_amiga_tusb_assert_hook(void) { g_tusb_assert_count++; }
extern "C" void hid_app_rearm_tick(void);
extern "C" bool usb_keyboard_mounted(void);

static Ps2Kbd_Mrmltr ps2kbd(pio1, KBD_CLOCK_PIN, process_kbd_report);

// ---- NES / SNES pads (pico-speccy main.cpp) ---------------------------------------
#ifdef USE_NESPAD
static uint32_t nespad_prev_state = 0;
static bool nespad_active = false;

static void nespad_tick1(void) {
    if (!nespad_active) return;
    nespad_read();
    uint32_t cur = nespad_state;
    uint32_t prev = nespad_prev_state;
    uint32_t pressed  = cur & ~prev;   // 0->1 transitions
    uint32_t released = ~cur & prev;   // 1->0 transitions
    nespad_prev_state = cur;
    if (!pressed && !released) return;
    static const struct { uint32_t bit; fabgl::VirtualKey menu, dpad; } map[] = {
        { DPAD_SELECT, fabgl::VirtualKey::VK_MENU_BS,    fabgl::VirtualKey::VK_DPAD_SELECT  },
        { DPAD_A,      fabgl::VirtualKey::VK_MENU_ENTER, fabgl::VirtualKey::VK_DPAD_FIRE    },
        { DPAD_B,      fabgl::VirtualKey::VK_MENU_ENTER, fabgl::VirtualKey::VK_DPAD_ALTFIRE },
        { DPAD_START,  fabgl::VirtualKey::VK_MENU_HOME,  fabgl::VirtualKey::VK_DPAD_START   },
        { DPAD_UP,     fabgl::VirtualKey::VK_MENU_UP,    fabgl::VirtualKey::VK_DPAD_UP      },
        { DPAD_DOWN,   fabgl::VirtualKey::VK_MENU_DOWN,  fabgl::VirtualKey::VK_DPAD_DOWN    },
        { DPAD_LEFT,   fabgl::VirtualKey::VK_MENU_LEFT,  fabgl::VirtualKey::VK_DPAD_LEFT    },
        { DPAD_RIGHT,  fabgl::VirtualKey::VK_MENU_RIGHT, fabgl::VirtualKey::VK_DPAD_RIGHT   },
    };
    for (auto &m : map) {
        if (!((pressed | released) & m.bit)) continue;
        const bool down = (pressed & m.bit) != 0;
        joyPushData(m.menu, down);
        if (Config::secondJoy != 1) joyPushData(m.dpad, down);
    }
}

static void nespad_tick2(void) {
    if (!nespad_active) return;
    gamepad2_bits.a = (nespad_state2 & DPAD_A) != 0;
    gamepad2_bits.b = (nespad_state2 & DPAD_B) != 0;
    gamepad2_bits.select = (nespad_state2 & DPAD_SELECT) != 0;
    gamepad2_bits.start = (nespad_state2 & DPAD_START) != 0;
    gamepad2_bits.up = (nespad_state2 & DPAD_UP) != 0;
    gamepad2_bits.down = (nespad_state2 & DPAD_DOWN) != 0;
    gamepad2_bits.left = (nespad_state2 & DPAD_LEFT) != 0;
    gamepad2_bits.right = (nespad_state2 & DPAD_RIGHT) != 0;
}
#endif

// Called often from the main loop and from Keyboard::virtualKeyAvailable().
void repeat_me_for_input() {
    static uint64_t last_input_tick = time_us_64();
    static uint32_t last_rearm_tick = time_us_32();
#ifdef USE_NESPAD
    static bool tick1 = true;
#endif
    const uint64_t tick = time_us_64();
    if (tick >= last_input_tick + 16666) {          // 60 Hz: PS/2 and NES pads
        ps2kbd.tick();
#ifdef USE_NESPAD
        (tick1 ? nespad_tick1 : nespad_tick2)();    // split call for joy1 and 2
        tick1 = !tick1;
#endif
        last_input_tick = tick;
    }
    const uint32_t now = time_us_32();
    if (now - last_rearm_tick > 150000) {
        // Recover HID interfaces whose IN endpoint lost its arm (see hid_app.cpp).
        hid_app_rearm_tick();
        repeat_handler();
        last_rearm_tick = now;
    }
    tuh_task();
}

void input_devices_init(void) {
    const tusb_rhport_init_t host = { TUSB_ROLE_HOST, TUSB_SPEED_AUTO };
    tusb_rhport_init(BOARD_TUH_RHPORT, &host);
#ifdef KBD_ALT_CLOCK_PIN
    // ZERO2: the PCM5122 board's control I2C sits on the PS/2 pins; with the
    // board fitted the keyboard is wired to the alternate pair.
    ps2kbd.init_gpio(audio_out_pcm5122_present() ? KBD_ALT_CLOCK_PIN : KBD_CLOCK_PIN);
#else
    ps2kbd.init_gpio();
#endif
#ifdef USE_NESPAD
    nespad_active = nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);
#endif
}

bool input_devices_keyboard_mounted(void) { return usb_keyboard_mounted(); }
