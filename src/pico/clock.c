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

#ifndef FLASH_MAX_MHZ
#define FLASH_MAX_MHZ 66
#endif

static void __not_in_flash_func(flash_timing_for)(int mhz, int *div_out, int *rx_out) {
    const int max_hz = FLASH_MAX_MHZ * 1000000;
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

int clock_overclock(int mhz) {
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(100);                              // regulator settles first
    const int boot_mhz = (int)(clock_get_hz(clk_sys) / 1000000);
    int applied = mhz;
    flash_timings_transition(boot_mhz, applied);
    if (!set_sys_clock_khz((uint32_t)mhz * 1000, false)) {
        applied = 252;                          // fail-safe clock
        flash_timings_transition(boot_mhz, applied);
        set_sys_clock_khz((uint32_t)applied * 1000, true);
    }
    flash_timings(applied);
    return applied;
}
