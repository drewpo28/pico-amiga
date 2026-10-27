#pragma once
// Debug > UART log: a TX-only 115200 8N1 console on the board's DBG_UART_TX_PIN
// (pico-speccy's pins), carrying everything the firmware printf()s. A reboot-class
// setting: on boards where the pin is the PS/2 clock the keyboard moves to
// DBG_UART_KBD_CLOCK_PIN at boot.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the console if `want` and the board has a pin; call after the overclock
// (the baud rate comes from clk_peri). True when it runs.
bool dbg_uart_start(bool want);
bool dbg_uart_active(void);
// Move buffered text into the UART FIFO; call from the main loop (never blocks).
void dbg_uart_pump(void);

// Once a second while the console runs: emulated frame time, FPS and the
// profiler's split, one line ("PERF ..."). Call after every amiga_run_frame().
void perf_log_frame(uint32_t emu_us);

#ifdef __cplusplus
}
#endif
