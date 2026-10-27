// The firmware's driver APIs on a PC: video.h, audio_out.h, input_devices.h,
// psram.h, clock.h.  The SDL side (window, events, the audio device) is
// host_main.cpp; this file is what src/pico/main.c and input.cpp call.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "pico_host.h"
#include "host_sdl.h"
extern "C" {
#include "amiga.h"
#include "video.h"
#include "audio_out.h"
#include "psram.h"
#include "clock.h"
}
#include "input_devices.h"

// board.h: the profiler's clock (nanoseconds on a PC).
extern "C" uint32_t board_cycles(void) {
    return (uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" uint32_t board_cycles_per_us(void) { return 1000; }

void __wfi(void) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

// ---- psram.h / clock.h: host_main.cpp maps the window at PSRAM_BASE -------------
extern "C" uint32_t psram_init(unsigned cs_pin, int max_mhz) { (void)cs_pin; (void)max_mhz; return HOST_PSRAM_SIZE; }
extern "C" uint32_t psram_size(void) { return HOST_PSRAM_SIZE; }
extern "C" int clock_overclock(int mhz, int flash_mhz) { (void)flash_mhz; return mhz; }

// ---- video.h ----------------------------------------------------------------------
// One output frame every 832 x 605 pixels at 25.2 MHz, as the HDMI back-ends run;
// the main thread presents host_fb whenever it changes.
const uint16_t *volatile host_fb;
static std::atomic<uint32_t> s_frames{0};

extern "C" void video_select(void) {}
extern "C" bool video_is_vga(void) { return false; }
extern "C" void video_start(const uint16_t *framebuffer) {
    host_fb = framebuffer;
    std::thread([] {
        using clk = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(1000000000ull * 832 * 605 / 25200000);
        auto next = clk::now() + period;
        for (;;) {
            std::this_thread::sleep_until(next);
            next += period;
            s_frames++;
        }
    }).detach();
}
extern "C" uint32_t video_frames(void) { return s_frames; }

// ---- audio_out.h: an SDL device pulling the Paula ring ------------------------------
// The same reader as audio_out.c: hold on underrun, drop single frames while more
// than three frames' worth behind, re-centre when lost.
static const int16_t *s_ring;
static uint32_t s_ring_frames, s_rd;

static void audio_cb(void *, Uint8 *stream, int len) {
    int16_t *out = (int16_t *)stream;
    static int16_t last_l, last_r;
    for (int n = len / 4; n--; ) {
        const uint32_t wp = amiga_audio_write_pos();
        uint32_t lag = wp - s_rd;
        if (lag > s_ring_frames / 2) { s_rd = wp - s_ring_frames / 4; lag = s_ring_frames / 4; }
        else if (lag > 3 * (AMIGA_AUDIO_RATE / 50)) { s_rd++; lag--; }
        if (lag) {
            const uint32_t i = (s_rd & (s_ring_frames - 1)) * 2;
            last_l = s_ring[i];
            last_r = s_ring[i + 1];
            s_rd++;
        }
        *out++ = (int16_t)((last_l * audio_volume_q8) >> 8);
        *out++ = (int16_t)((last_r * audio_volume_q8) >> 8);
    }
}

extern "C" bool audio_out_pcm5122_present(void) { return false; }
volatile int16_t audio_volume_q8 = 256;
extern "C" void audio_set_volume(int q8) { audio_volume_q8 = (int16_t)(q8 < 0 ? 0 : q8 > 256 ? 256 : q8); }
extern "C" void hdmi_audio_set_enabled(bool) {}
extern "C" void sdcard_reclock(void) {}

extern "C" void audio_out_start(const int16_t *ring, uint32_t frames, int) {
    s_ring = ring;
    s_ring_frames = frames;
    s_rd = amiga_audio_write_pos();
    SDL_AudioSpec want = {}, have;
    want.freq = AMIGA_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "host: no audio (%s)\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
}
extern "C" const char *audio_out_name(void) { return "SDL"; }

// ---- input_devices.h: SDL keyboard reports and mouse --------------------------------
static std::mutex s_kmx;
static std::vector<hid_keyboard_report_t> s_reports;
static hid_keyboard_report_t s_cur = {}, s_prev = {};
static std::atomic<int32_t> s_mdx{0}, s_mdy{0};
static std::atomic<bool> s_ml{false}, s_mr{false};

void host_key(SDL_Scancode sc, bool down) {
    uint8_t bit = 0;
    switch (sc) {
    case SDL_SCANCODE_LCTRL:  bit = KEYBOARD_MODIFIER_LEFTCTRL; break;
    case SDL_SCANCODE_LSHIFT: bit = KEYBOARD_MODIFIER_LEFTSHIFT; break;
    case SDL_SCANCODE_LALT:   bit = KEYBOARD_MODIFIER_LEFTALT; break;
    case SDL_SCANCODE_LGUI:   bit = KEYBOARD_MODIFIER_LEFTGUI; break;
    case SDL_SCANCODE_RCTRL:  bit = KEYBOARD_MODIFIER_RIGHTCTRL; break;
    case SDL_SCANCODE_RSHIFT: bit = KEYBOARD_MODIFIER_RIGHTSHIFT; break;
    case SDL_SCANCODE_RALT:   bit = KEYBOARD_MODIFIER_RIGHTALT; break;
    case SDL_SCANCODE_RGUI:   bit = KEYBOARD_MODIFIER_RIGHTGUI; break;
    default: break;
    }
    // SDL scancodes are USB HID usage ids: the report is what a USB keyboard sends.
    if (bit) {
        if (down) s_cur.modifier |= bit; else s_cur.modifier &= (uint8_t)~bit;
    } else {
        if (sc <= 0 || sc > 0xFF) return;
        const uint8_t kc = (uint8_t)sc;
        if (down) {
            for (uint8_t k : s_cur.keycode) if (k == kc) return;
            for (uint8_t &k : s_cur.keycode) if (!k) { k = kc; break; }
        } else {
            for (uint8_t &k : s_cur.keycode) if (k == kc) k = 0;
        }
    }
    std::lock_guard<std::mutex> lk(s_kmx);
    s_reports.push_back(s_cur);
}

void host_mouse(int dx, int dy, bool l, bool r) {
    s_mdx += dx;
    s_mdy += dy;
    s_ml = l;
    s_mr = r;
}

void input_devices_init(void) {}
bool input_devices_keyboard_mounted(void) { return true; }

// On core0, like the board's: hand the queued reports to the firmware's funnel.
// main.c spins on input_poll() while it waits for the next output frame; the short
// sleep keeps that from eating a whole PC core.
void repeat_me_for_input() {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    std::vector<hid_keyboard_report_t> q;
    {
        std::lock_guard<std::mutex> lk(s_kmx);
        q.swap(s_reports);
    }
    for (const auto &r : q) {
        process_kbd_report(&r, &s_prev);
        s_prev = r;
    }
    static uint64_t t_rep = 0;
    const uint64_t now = time_us_64();
    if (now - t_rep > 150000) { repeat_handler(); t_rep = now; }
    // hid_app.cpp's mouse_apply at 1:1: HID y is down-positive, Mouse::y up-positive.
    const int32_t dx = s_mdx.exchange(0), dy = s_mdy.exchange(0);
    if (dx || dy || s_ml != Mouse::buttonL || s_mr != Mouse::buttonR) {
        Mouse::seen = true;
        Mouse::x += dx;
        Mouse::y -= dy;
        Mouse::buttonL = s_ml;
        Mouse::buttonR = s_mr;
    }
}
