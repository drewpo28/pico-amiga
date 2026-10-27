// Board probes shared by the drivers.
//
// board_test_pins() is pico-speccy's testPins() (pwm_audio.cpp): it drives and
// pulls two GPIOs to tell how they are wired.  The audio output uses it to tell an
// I2S DAC from an RC-filtered PWM pair, the video output to tell a VGA resistor
// ladder from an HDMI connector on the same pins.
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

// connection is possible 00->00 (external pull down)
static int test_0000_case(uint32_t pin0, uint32_t pin1, int res) {
    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_OUT);
    sleep_ms(33);
    gpio_put(pin0, 1);

    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_down(pin1);
    sleep_ms(33);
    if (gpio_get(pin1)) res |= (1 << 5) | 1;   // 1 -> 1, looks really connected
    gpio_deinit(pin0);
    gpio_deinit(pin1);
    return res;
}

// connection is possible 01->01 (no external pull up/down)
static int test_0101_case(uint32_t pin0, uint32_t pin1, int res) {
    return test_0000_case(pin0, pin1, res);
}

// connection is possible 11->11 (externally pulled up)
static int test_1111_case(uint32_t pin0, uint32_t pin1, int res) {
    gpio_init(pin0);
    gpio_set_dir(pin0, GPIO_OUT);
    sleep_ms(33);
    gpio_put(pin0, 0);

    gpio_init(pin1);
    gpio_set_dir(pin1, GPIO_IN);
    gpio_pull_up(pin1);
    sleep_ms(33);
    if (!gpio_get(pin1)) res |= 1;            // 0 -> 0, looks really connected
    gpio_deinit(pin0);
    gpio_deinit(pin1);
    return res;
}

int board_test_pins(uint32_t pin0, uint32_t pin1) {
    int res = 0;
    if (pin0 == BUTTER_PSRAM_GPIO || pin1 == BUTTER_PSRAM_GPIO) return res;
#if defined(PICO_DEFAULT_LED_PIN) && PICO_DEFAULT_LED_PIN != 255
    if (pin0 == PICO_DEFAULT_LED_PIN || pin1 == PICO_DEFAULT_LED_PIN) return res;
#endif
    if (pin0 == 23 || pin1 == 23) return res;  // SMPS power
    if (pin0 == 24 || pin1 == 24) return res;  // VBUS sense
    // passive pull-down, then pull-up
    gpio_init(pin0); gpio_set_dir(pin0, GPIO_IN); gpio_pull_down(pin0);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN); gpio_pull_down(pin1);
    sleep_ms(33);
    int pin0vPD = gpio_get(pin0), pin1vPD = gpio_get(pin1);
    gpio_deinit(pin0); gpio_deinit(pin1);
    gpio_init(pin0); gpio_set_dir(pin0, GPIO_IN); gpio_pull_up(pin0);
    gpio_init(pin1); gpio_set_dir(pin1, GPIO_IN); gpio_pull_up(pin1);
    sleep_ms(33);
    int pin0vPU = gpio_get(pin0), pin1vPU = gpio_get(pin1);
    gpio_deinit(pin0); gpio_deinit(pin1);

    res = (pin0vPD << 4) | (pin0vPU << 3) | (pin1vPD << 2) | (pin1vPU << 1);
    if (pin0vPD && pin0vPU && pin1vPD && pin1vPU) return test_1111_case(pin0, pin1, res);
    if (pin0vPD && !pin0vPU && pin1vPD && !pin1vPU) return res | (1 << 5) | 1;
    if (!pin0vPD && pin0vPU && !pin1vPD && pin1vPU) return test_0101_case(pin0, pin1, res);
    if (!pin0vPD && !pin0vPU && !pin1vPD && !pin1vPU) return test_0000_case(pin0, pin1, res);
    return res;                                // connection impossible
}


// ---- cycle counter -----------------------------------------------------------------
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

uint32_t __not_in_flash_func(board_cycles)(void) {   // the audio tick IRQ calls it too
    static bool on;
    if (!on) {                       // trace enable, then the counter (per core)
        DEMCR |= 1u << 24;
        DWT_CTRL |= 1u;
        on = true;
    }
    return DWT_CYCCNT;
}

uint32_t board_cycles_per_us(void) { return clock_get_hz(clk_sys) / 1000000u; }
