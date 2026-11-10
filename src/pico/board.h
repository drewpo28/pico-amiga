// Board probes shared by the drivers (board.c).
#pragma once
#include <stdint.h>
#include <stdbool.h>

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

// The XIP cache's hit and access counters since the last call (both cores, flash
// and PSRAM together: the RP2350 has one cache and one counter pair).
void     board_xip_counts(uint32_t *hit, uint32_t *acc);
// PC-sampling profiler of both cores (pico-speccy's PcProf): each core's SysTick
// at 8 kHz counts the interrupted PC in 64-byte buckets (a 2 KB table per core
// from the heap while on).  board_pcprof_dump() prints the hottest as "PROF"
// (core 0) and "PROF1" (core 1) lines and starts over; resolve them with
// tools/pcprof.py against the same ELF.  Call both from core 0.  Core 1 runs its
// own tick when its loop polls board_pcprof_core1_poll() (main.c, video_pio.c).
// Musashi's hot handlers (m68kconf.h): the 68000's and the 68EC020's sets are
// OVERLAY sections in one SRAM window (src/pico/ld/section_extra_post_data.incl);
// this copies the set of cpu (AMIGA_CPU_*) in from flash.  Call with the CPU
// stopped, from core 0, before amiga_reset().
void     board_load_cpu_overlay(int cpu);
// The UART log's "PSRAM" line: what XIP misses cost, measured over 704 KB from
// ram (cached PSRAM; contents kept).  Before the emulation starts.
void     board_psram_bench(uint8_t *ram);
void     board_pcprof_mem(void *psram4k);  // 4 KB of PSRAM for the tables (else the heap)
bool     board_pcprof(bool on);           // false: no heap for the table
void     board_pcprof_dump(void);
extern volatile uint8_t board_pcprof_c1_want;
void     board_pcprof_core1_apply(void);
static inline void board_pcprof_core1_poll(void) {
    static uint8_t have;                  // what this core last applied
    const uint8_t want = board_pcprof_c1_want;
    if (want != have) { have = want; board_pcprof_core1_apply(); }
}

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
