// Debug > UART log (dbg_uart.h). printf goes through a pico_stdio driver into a
// RAM ring; dbg_uart_pump() feeds the 32-byte UART FIFO from the main loop, so a
// log line never stalls the emulator (a full ring drops text instead).  The
// ring comes from the heap when the console starts: with the log off (the
// default) its 4 KB stay the heap's.
#include "dbg_uart.h"
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#ifdef DBG_UART_TX_PIN
// The UART that has TX on this pin: GP0/12/16/28 -> UART0, GP4/8/20/24 -> UART1.
#define DBG_UART_INST (((DBG_UART_TX_PIN + 4) / 8) % 2 ? uart1 : uart0)
#endif

#define RING 4096u
static char    *ring;
static uint32_t wr, rd;
static bool     on;

bool dbg_uart_active(void) { return on; }

void dbg_uart_pump(void) {
#ifdef DBG_UART_TX_PIN
    while (on && rd != wr && uart_is_writable(DBG_UART_INST))
        uart_get_hw(DBG_UART_INST)->dr = (uint8_t)ring[rd++ % RING];
#endif
}

static void out_chars(const char *s, int n) {
    for (int i = 0; i < n && wr - rd < RING; i++) ring[wr++ % RING] = s[i];
    dbg_uart_pump();
}

static stdio_driver_t drv = {
    .out_chars = out_chars,
    .crlf_enabled = true,
};

void dbg_uart_flush(void) {
#ifdef DBG_UART_TX_PIN
    if (!on) return;
    while (rd != wr) dbg_uart_pump();
    uart_tx_wait_blocking(DBG_UART_INST);
#endif
}

void dbg_uart_reclock(void) {
#ifdef DBG_UART_TX_PIN
    if (on) uart_set_baudrate(DBG_UART_INST, 115200);
#endif
}

bool dbg_uart_start(bool want) {
#ifdef DBG_UART_TX_PIN
    if (!want || on) return on;
    ring = malloc(RING);
    if (!ring) return false;
    uart_init(DBG_UART_INST, 115200);
    gpio_set_function(DBG_UART_TX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&drv, true);
    on = true;
#else
    (void)want;
#endif
    return on;
}
