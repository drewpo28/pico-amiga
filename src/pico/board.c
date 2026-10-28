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
#include "hardware/adc.h"
#include "hardware/flash.h"
#include "hardware/vreg.h"
#include "hardware/structs/sysinfo.h"
#include <malloc.h>
#include <unistd.h>

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
    static bool on[2];
    const uint core = get_core_num();
    if (!on[core]) {                 // trace enable, then the counter: each core has its own
        DEMCR |= 1u << 24;
        DWT_CTRL |= 1u;
        on[core] = true;
    }
    return DWT_CYCCNT;
}

uint32_t board_cycles_per_us(void) { return clock_get_hz(clk_sys) / 1000000u; }

// ---- system info (Win+F1) -------------------------------------------------------------
static uint8_t flash_jedec[4];

void board_flash_probe(void) {
    uint8_t tx[4] = { 0x9F };
    flash_do_cmd(tx, flash_jedec, 4);           // [0] is the command's echo slot
}
const uint8_t *board_flash_jedec(void) { return flash_jedec + 1; }
uint32_t board_flash_size(void) {
    const uint8_t n = flash_jedec[3];
    return n >= 16 && n <= 28 ? 1u << n : 0;
}

char board_chip_package(void) { return (sysinfo_hw->package_sel & 1u) ? 'A' : 'B'; }
uint32_t board_cpu_mhz(void) { return clock_get_hz(clk_sys) / 1000000u; }

int board_vreg_mv(void) {
    static const uint16_t mv[] = { 550, 600, 650, 700, 750, 800, 850, 900, 950, 1000,
                                   1050, 1100, 1150, 1200, 1250, 1300, 1350, 1400, 1500,
                                   1600, 1650, 1700, 1800, 1900, 2000, 2350, 2500, 2650,
                                   2800, 3000, 3150, 3300 };
    const int v = (int)vreg_get_voltage();
    return v >= 0 && v < 32 ? mv[v] : 0;
}

// pico-speccy's chipTempX10(): T = 27 - (V - 0.706) / 0.001721.
int board_temp_x10(void) {
    static bool up;
    if (!up) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        busy_wait_us(500);                      // first-time bias settle
        up = true;
    }
#ifdef ZERO2
    // pico-speccy: the Pi Zero form factor puts HDMI DDC/CEC (pulled up to 3V3)
    // on GPIO44-46, and a high level there leaks into the sensor node (60-90 C
    // low). Held low for the conversions; this firmware never speaks DDC/CEC.
    for (uint p = 44; p <= 46; p++) { gpio_init(p); gpio_set_dir(p, GPIO_OUT); }
    busy_wait_us(15);
#endif
    adc_select_input(board_chip_package() == 'A' ? 4 : 8);
    (void)adc_read();                           // the first conversion is stale
    int raw = 0;
    for (int i = 0; i < 4; i++) raw += adc_read();
#ifdef ZERO2
    for (uint p = 44; p <= 46; p++) gpio_deinit(p);
#endif
    const int uv10 = (raw / 4) * 33000 / 4096;  // volts * 10000
    return 270 - (uv10 - 7060) * 1000 / 1721;
}

uint32_t board_free_heap(void) {
    extern char __HeapLimit;
    const struct mallinfo mi = mallinfo();
    const char *top = (const char *)sbrk(0);
    return (uint32_t)mi.fordblks + (top < &__HeapLimit ? (uint32_t)(&__HeapLimit - top) : 0);
}
