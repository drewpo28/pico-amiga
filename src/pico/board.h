// Board probes shared by the drivers (board.c).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// pico-speccy's testPins(): the passive pull-down / pull-up levels of pin0 and
// pin1 in bits 4..1 (pin0 PD, pin0 PU, pin1 PD, pin1 PU), bit 0 = "pin0 drives
// pin1" (wired together), bit 5 = that was seen with pin0 driven high.
// Takes ~130 ms; the pins are left deinitialised.
int board_test_pins(uint32_t pin0, uint32_t pin1);

// A free-running cycle counter of the calling core (the M33's DWT CYCCNT; the
// PC host counts nanoseconds), for the core's profiler, and its rate.
uint32_t board_cycles(void);
uint32_t board_cycles_per_us(void);

#ifdef __cplusplus
}
#endif
