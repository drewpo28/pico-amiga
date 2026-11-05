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
#include "hardware/structs/qmi.h"
#include "hardware/structs/pads_qspi.h"
#include "hardware/sync.h"
#include "hardware/exception.h"
#include "hardware/structs/systick.h"
#include "hardware/regs/xip.h"
#include "hardware/xip_cache.h"
#include "psram.h"
#include <stdio.h>
#include <string.h>
#include "pico/bootrom.h"
#include <stdlib.h>
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

// ---- Bitplane lines from PSRAM (pico-speccy's prev-FB window transport) ---------
// Clean first: the CPU and the blitter wrote the planes through the write-back
// cache.  Sequential uncached words merge into one QMI burst (COOLDOWN).
static void __not_in_flash_func(line_read)(uint8_t *dst, uint32_t off, uint32_t len) {
    const uintptr_t x = PSRAM_BASE - XIP_BASE + off;
    xip_cache_clean_range(x, len);
    const volatile uint32_t *s = (const volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + x);
    uint32_t *d = (uint32_t *)dst;
    for (uint32_t n = len >> 2; n; n--) *d++ = *s++;
}
board_line_read_fn board_line_reader(void) { return line_read; }

// ---- XIP cache counters, PC profiler (pico-speccy's PcProf) --------------------------
void board_xip_counts(uint32_t *hit, uint32_t *acc) {
    volatile uint32_t *h = (volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_HIT_OFFSET);
    volatile uint32_t *a = (volatile uint32_t *)(XIP_CTRL_BASE + XIP_CTR_ACC_OFFSET);
    *hit = *h; *acc = *a;
    *h = 0; *a = 0;                  // any write clears
}

#define PCPROF_HZ    8000u
#define PCPROF_SLOTS 256u            // power of two
#define PCPROF_SHIFT 6               // 64-byte buckets
#define PCPROF_LINES 4               // 14 buckets a line
static uint32_t *volatile s_pkey;    // [PCPROF_SLOTS] keys, then [PCPROF_SLOTS] counts
static volatile uint32_t s_ptotal, s_plost;
static uint32_t s_phz;

void __not_in_flash_func(pcprof_sample)(const uint32_t *frame) {
    uint32_t *const key = s_pkey;
    if (!key) return;
    uint32_t *const cnt = key + PCPROF_SLOTS;
    const uint32_t k = (frame[6] >> PCPROF_SHIFT) + 1u;      // + 1: 0 means empty
    uint32_t h = (k * 2654435761u) >> 24;
    for (uint32_t i = 0; i < 16; i++, h = (h + 1) & (PCPROF_SLOTS - 1)) {
        if (key[h] == k) { cnt[h]++; s_ptotal++; return; }
        if (key[h] == 0) { key[h] = k; cnt[h] = 1; s_ptotal++; return; }
    }
    s_plost++;
}

// The interrupted PC is word 6 of the exception frame, on whichever stack it was.
void __attribute__((naked)) __not_in_flash_func(pcprof_isr)(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "b pcprof_sample \n");
}

static void pcprof_reload(void) {
    s_phz = clock_get_hz(clk_sys);
    systick_hw->csr = 0;
    systick_hw->rvr = s_phz / PCPROF_HZ - 1;
    systick_hw->cvr = 0;
    systick_hw->csr = 0x7;           // processor clock, interrupt, enable
}

bool board_pcprof(bool on) {
    static bool installed;
    systick_hw->csr = 0;
    uint32_t *old = s_pkey;
    s_pkey = NULL;
    free(old);
    if (!on) return true;
    uint32_t *t = (uint32_t *)calloc(2 * PCPROF_SLOTS, sizeof(uint32_t));
    if (!t) return false;
    if (!installed) { exception_set_exclusive_handler(SYSTICK_EXCEPTION, pcprof_isr); installed = true; }
    s_ptotal = s_plost = 0;
    s_pkey = t;
    pcprof_reload();
    printf("PROF: core 0 PC sampling at %u Hz, %u-byte buckets\n", PCPROF_HZ, 1u << PCPROF_SHIFT);
    return true;
}

// The 56 hottest buckets in per mille of the samples, then the table starts over.
void board_pcprof_dump(void) {
    uint32_t *const key = s_pkey;
    if (!key) return;
    uint32_t *const cnt = key + PCPROF_SLOTS;
    systick_hw->csr &= ~0x2u;        // the tick masked while the table is read
    const uint32_t total = s_ptotal, lost = s_plost;
    for (int part = 0; total && part < PCPROF_LINES; part++) {
        char line[240];
        int n = part ? snprintf(line, sizeof line, "PROF+") : snprintf(line, sizeof line, "PROF %u smp:", (unsigned)total);
        for (int k = 0; k < 14 && n < (int)sizeof line - 20; k++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t i = 0; i < PCPROF_SLOTS; i++) if (cnt[i] > best) { best = cnt[i]; bi = i; }
            if (!best) break;
            n += snprintf(line + n, sizeof line - n, " %08X:%u",
                          (unsigned)((key[bi] - 1u) << PCPROF_SHIFT), (unsigned)(best * 1000u / total));
            cnt[bi] = 0;
        }
        if (part == PCPROF_LINES - 1 && lost) snprintf(line + n, sizeof line - n, " lost=%u", (unsigned)lost);
        printf("%s\n", line);
    }
    memset(key, 0, 2 * PCPROF_SLOTS * sizeof(uint32_t));
    s_ptotal = s_plost = 0;
    if (clock_get_hz(clk_sys) != s_phz) pcprof_reload();
    else systick_hw->csr |= 0x2u;
}

// ---- system info (Win+F1) -------------------------------------------------------------
static uint8_t flash_jedec[4];

// Puya flash (ZERO2's PY25Q128HA) ships with SR2.QE = 0, and its 01h command
// writes SR1 only, so boot2's Winbond-style 2-byte status write silently fails:
// quad XIP keeps sampling WP#/HOLD# and the board locks up once overclocked.
// pico-speccy's flash_qe_fix (after fhoedemakers/flash_config), the same
// sequence: inside ONE exit-XIP window (boot2 re-run between commands would
// clear the volatile-WE latch), a volatile SR2 write (50h + 31h); if ignored,
// a one-time non-volatile 06h + 31h; last, the Winbond-style 06h + 01h.
// board_flash_qe(): 0 n/a (not Puya), 1 already set, 2 set volatile, 3 set
// non-volatile 31h, 4 FAILED, 5 set non-volatile 01h, 6 the raw window failed.
static uint8_t flash_qe;

// One CS-framed command in the open window. __no_inline: an inlined copy in a
// flash caller would run from (dead) XIP (a hard fault, traced on ZERO2).
static void __no_inline_not_in_flash_func(qe_cmd_raw)(const uint8_t *tx, uint8_t *rx, size_t n) {
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    size_t txr = n, rxr = n;
    while (txr || rxr) {
        const uint32_t flags = qmi_hw->direct_csr;
        if (txr && !(flags & QMI_DIRECT_CSR_TXFULL_BITS)) { qmi_hw->direct_tx = *tx++; --txr; }
        if (rxr && !(flags & QMI_DIRECT_CSR_RXEMPTY_BITS)) {
            const uint8_t b = (uint8_t)qmi_hw->direct_rx;
            if (rx) *rx++ = b;
            --rxr;
        }
    }
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS0N_BITS);
}

static uint8_t __no_inline_not_in_flash_func(qe_read_reg)(uint8_t cmd) {
    uint8_t tx[2] = { cmd, 0 }, rx[2];
    qe_cmd_raw(tx, rx, 2);
    return rx[1];
}

static void __no_inline_not_in_flash_func(flash_qe_fix)(void) {
    if (flash_jedec[1] != 0x85) return;          // Puya only
    // boot2, to re-enter fast XIP afterwards (crt0 parks it in BOOTRAM).
    uint32_t *boot2 = (uint32_t *)malloc(64 * sizeof(uint32_t));
    if (!boot2) return;
    const volatile uint32_t *b2 = (const volatile uint32_t *)BOOTRAM_BASE;
    for (int i = 0; i < 64; ++i) boot2[i] = b2[i];
    __compiler_memory_barrier();
    rom_connect_internal_flash_fn connect_flash =
        (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn exit_xip =
        (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_flush_cache_fn flush_cache =
        (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    // exit_xip resets the CS1 window; the pads are saved as well.
    const uint32_t m1_timing = qmi_hw->m[1].timing, m1_rcmd = qmi_hw->m[1].rcmd, m1_rfmt = qmi_hw->m[1].rfmt;
    uint32_t pads[count_of(pads_qspi_hw->io)];
    for (size_t i = 0; i < count_of(pads_qspi_hw->io); ++i) pads[i] = pads_qspi_hw->io[i];

    const uint32_t ints = save_and_disable_interrupts();
    connect_flash();
    exit_xip();
    // WP# (SD2) and HOLD# (SD3) pulled high: undriven in serial direct mode, a low
    // WP# would hardware-protect the status registers.
    hw_write_masked(&pads_qspi_hw->io[3], PADS_QSPI_GPIO_QSPI_SD2_PUE_BITS,
                    PADS_QSPI_GPIO_QSPI_SD2_PUE_BITS | PADS_QSPI_GPIO_QSPI_SD2_PDE_BITS);
    hw_write_masked(&pads_qspi_hw->io[4], PADS_QSPI_GPIO_QSPI_SD3_PUE_BITS,
                    PADS_QSPI_GPIO_QSPI_SD3_PUE_BITS | PADS_QSPI_GPIO_QSPI_SD3_PDE_BITS);
    // ---- window open: only the RAM helpers above ----
    uint8_t jtx[4] = { 0x9F, 0, 0, 0 }, jrx[4];
    qe_cmd_raw(jtx, jrx, 4);
    const uint8_t sr2 = qe_read_reg(0x35);
    if (jrx[1] != flash_jedec[1] || jrx[2] != flash_jedec[2] || jrx[3] != flash_jedec[3]) {
        flash_qe = 6;                            // the raw path does not work: write nothing
    } else if (sr2 & 0x02) {
        flash_qe = 1;
    } else {
        uint8_t wr31[2] = { 0x31, (uint8_t)(sr2 | 0x02) };
        const uint8_t c50 = 0x50, c06 = 0x06;
        qe_cmd_raw(&c50, NULL, 1);               // volatile SR write enable
        qe_cmd_raw(wr31, NULL, 2);
        for (volatile int i = 0; i < 2000; ++i) {}
        if (qe_read_reg(0x35) & 0x02) {
            flash_qe = 2;
        } else {
            qe_cmd_raw(&c06, NULL, 1);           // write enable
            if (qe_read_reg(0x05) & 0x02) {      // WEL latched
                qe_cmd_raw(wr31, NULL, 2);
                for (int i = 0; i < 20000; ++i) if (!(qe_read_reg(0x05) & 0x01)) break;
                flash_qe = (qe_read_reg(0x35) & 0x02) ? 3 : 4;
                if (flash_qe == 4) {
                    uint8_t wr01[3] = { 0x01, (uint8_t)(qe_read_reg(0x05) & ~0x03), (uint8_t)(sr2 | 0x02) };
                    qe_cmd_raw(&c06, NULL, 1);
                    qe_cmd_raw(wr01, NULL, 3);
                    for (int i = 0; i < 20000; ++i) if (!(qe_read_reg(0x05) & 0x01)) break;
                    flash_qe = (qe_read_reg(0x35) & 0x02) ? 5 : 4;
                }
            } else {
                flash_qe = 4;
            }
        }
    }
    // ---- close: flush, back to fast XIP through boot2 ----
    flush_cache();
    ((void (*)(void))((intptr_t)boot2 + 1))();
    qmi_hw->m[1].timing = m1_timing;
    qmi_hw->m[1].rcmd = m1_rcmd;
    qmi_hw->m[1].rfmt = m1_rfmt;
    for (size_t i = 0; i < count_of(pads_qspi_hw->io); ++i) pads_qspi_hw->io[i] = pads[i];
    restore_interrupts(ints);
    free(boot2);
}

void board_flash_probe(void) {
    uint8_t tx[4] = { 0x9F };
    flash_do_cmd(tx, flash_jedec, 4);           // [0] is the command's echo slot
    flash_qe_fix();                             // before the overclock: Puya locks up without QE
}
uint8_t board_flash_qe(void) { return flash_qe; }
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
