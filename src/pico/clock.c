// System clock bring-up: core voltage, overclock and the QMI flash timing.
//
// Ported from pico-speccy (src/main.cpp: flash_timing_for / flash_timings /
// flash_timings_transition and the boot sequence in main()).  The important
// part is the TRANSITION timing: set_sys_clock_khz() runs from flash while the
// PLL moves, so the M0 timing in force must read correctly at both clocks.
// Take the steady-state divider for the higher clock and double it, keeping
// RXDELAY (an absolute delay); restore the steady-state timing afterwards.
#include "clock.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"

// The flash SCK limit (Options > Flash frequency); 66 MHz until clock_overclock().
static int flash_max_mhz = 66;

static void __not_in_flash_func(flash_timing_for)(int mhz, int *div_out, int *rx_out) {
    const int max_hz = flash_max_mhz * 1000000;
    const int clock_hz = mhz * 1000000;
    int divisor = (clock_hz + max_hz - 1) / max_hz;
    if (divisor == 1 && clock_hz > 100000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) rxdelay += 1;
    *div_out = divisor;
    *rx_out = rxdelay;
}

static inline void flash_timing_write(int divisor, int rxdelay) {
    qmi_hw->m[0].timing = 0x60007000 |
                          rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                          divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

static void __not_in_flash_func(flash_timings)(int mhz) {
    int d, r;
    flash_timing_for(mhz, &d, &r);
    flash_timing_write(d, r);
}

static void __not_in_flash_func(flash_timings_transition)(int from_mhz, int to_mhz) {
    int d, r;
    flash_timing_for(from_mhz > to_mhz ? from_mhz : to_mhz, &d, &r);
    d *= 2;
    if (d > (int)(QMI_M0_TIMING_CLKDIV_BITS >> QMI_M0_TIMING_CLKDIV_LSB))
        d = QMI_M0_TIMING_CLKDIV_BITS >> QMI_M0_TIMING_CLKDIV_LSB;
    flash_timing_write(d, r);
}

// mV -> the SDK's step (the menu's list; anything else: 1.60 V, the tested one).
static enum vreg_voltage vreg_for(int mv) {
    static const struct { uint16_t mv; enum vreg_voltage v; } k[] = {
        { 1150, VREG_VOLTAGE_1_15 }, { 1200, VREG_VOLTAGE_1_20 }, { 1250, VREG_VOLTAGE_1_25 },
        { 1300, VREG_VOLTAGE_1_30 }, { 1350, VREG_VOLTAGE_1_35 }, { 1400, VREG_VOLTAGE_1_40 },
        { 1500, VREG_VOLTAGE_1_50 }, { 1600, VREG_VOLTAGE_1_60 }, { 1650, VREG_VOLTAGE_1_65 },
        { 1700, VREG_VOLTAGE_1_70 }, { 1800, VREG_VOLTAGE_1_80 },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (k[i].mv == mv) return k[i].v;
    return VREG_VOLTAGE_1_60;
}

int clock_overclock(int mhz, int flash_mhz, int vreg_mv) {
    vreg_disable_voltage_limit();
    vreg_set_voltage(vreg_for(vreg_mv));
    sleep_ms(100);                              // regulator settles first
    const int boot_mhz = (int)(clock_get_hz(clk_sys) / 1000000);
    int applied = mhz;
    flash_timings_transition(boot_mhz, applied);
    if (!set_sys_clock_khz((uint32_t)mhz * 1000, false)) {
        applied = 252;                          // fail-safe clock
        flash_timings_transition(boot_mhz, applied);
        set_sys_clock_khz((uint32_t)applied * 1000, true);
    }
    if (flash_mhz >= 33 && flash_mhz <= 166) flash_max_mhz = flash_mhz;
    flash_timings(applied);
    return applied;
}
