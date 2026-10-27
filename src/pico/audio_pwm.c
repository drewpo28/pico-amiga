// Stereo PWM audio from the core's Paula ring.
//
// A repeating timer at ~31.25 kHz takes the next stereo frame from the ring
// the emulator fills (amiga_audio_write_pos) and sets two PWM levels.  The
// emulator produces 31300 frames/s paced by the 50.06 Hz HDMI frame, so the
// reader drifts slowly: it holds the last frame on underrun and skips ahead
// when it falls more than half a ring behind.  An RC filter on the pins is
// expected (as on every Murmulator audio output).
#include "audio_pwm.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "amiga.h"

#define PWM_BITS 10
#define PWM_TOP  ((1u << PWM_BITS) - 1)

static const int16_t *ring;
static uint32_t ring_frames;
static uint32_t rd;
static int pin_l = -1, pin_r = -1;
static repeating_timer_t timer;

static void set_level(int pin, int16_t s) {
    if (pin < 0) return;
    uint32_t v = (uint32_t)((int32_t)s + 32768) >> (16 - PWM_BITS);
    pwm_set_gpio_level((uint)pin, (uint16_t)v);
}

static bool __not_in_flash_func(tick)(repeating_timer_t *t) {
    (void)t;
    uint32_t wp = amiga_audio_write_pos();
    uint32_t lag = wp - rd;
    if (lag > ring_frames / 2) rd = wp - ring_frames / 4;     // fell behind: resync
    if (rd != wp) {
        uint32_t i = (rd & (ring_frames - 1)) * 2;
        set_level(pin_l, ring[i]);
        set_level(pin_r, ring[i + 1]);
        rd++;
    }
    return true;
}

static void pin_init(int pin) {
    if (pin < 0) return;
    gpio_set_function((uint)pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num((uint)pin);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, PWM_TOP);
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level((uint)pin, PWM_TOP / 2);
}

void audio_pwm_start(int left_pin, int right_pin, const int16_t *audio_ring, uint32_t frames) {
    ring = audio_ring;
    ring_frames = frames;
    pin_l = left_pin;
    pin_r = right_pin;
    pin_init(pin_l);
    if (pin_r != pin_l) pin_init(pin_r);
    rd = amiga_audio_write_pos();
    add_repeating_timer_us(-32, tick, NULL, &timer);
}
