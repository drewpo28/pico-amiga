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

// System info (Win+F1).  The flash's JEDEC ID is read once, by
// board_flash_probe() first thing in main(): the command leaves XIP and boot2
// restores it, which would undo the overclocked flash timing and the PSRAM's.
void     board_flash_probe(void);
const uint8_t *board_flash_jedec(void);   // manufacturer, type, capacity (2^n bytes)
uint8_t  board_flash_qe(void);            // Puya QE fix: 0 n/a, 1 was set, 2/3/5 set, 4 failed, 6 no raw path
uint32_t board_flash_size(void);          // bytes, from the ID; 0 = unknown
char     board_chip_package(void);        // 'A' (QFN60) or 'B' (QFN80)
uint32_t board_cpu_mhz(void);
int      board_vreg_mv(void);             // core voltage, mV
int      board_temp_x10(void);            // the chip's sensor (uncalibrated), 0.1 C
uint32_t board_free_heap(void);           // malloc's free blocks + the heap not yet taken, bytes

#ifdef __cplusplus
}
#endif
