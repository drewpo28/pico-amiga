// QSPI ("butter") PSRAM on XIP chip-select 1, memory-mapped at 0x11000000.
//
// Ported from pico-speccy (src/main.cpp: psram_init / psram_detect /
// psram_retiming / butter_psram_probe), where it is hardware-proven on the
// APS6404 parts of MURM2, PICO_PC and Pico Plus 2 boards.  The comments that
// matter are kept: the direct-mode window must not touch flash.
#include "psram.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#define MB (1u << 20)

// Hard ceiling on the PSRAM SCK, whatever the menu says: 135 MHz is the highest
// hw-proven clock (540 / 4); 168 MHz (504 / 3) failed on pico-speccy.
#define PSRAM_QMI_SCK_HW_MAX_MHZ 135

static uint32_t psram_bytes;

static void __not_in_flash_func(psram_retiming)(uint32_t clock_hz, int max_mhz) {
    if (max_mhz > PSRAM_QMI_SCK_HW_MAX_MHZ) max_mhz = PSRAM_QMI_SCK_HW_MAX_MHZ;
    if (max_mhz < 33) max_mhz = 33;
    const int max_psram_freq = max_mhz * 1000000;
    int divisor = (int)((clock_hz + max_psram_freq - 1) / max_psram_freq);
    if (divisor == 1 && clock_hz > 100000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) rxdelay += 1;
    const int clock_period_fs = (int)(1000000000000000ll / clock_hz);
    // MAX_SELECT is a 6-bit field (units of 64 sys clocks, 8 us tCEM): 504 MHz
    // gives exactly 63; anything faster overflowed into SELECT_HOLD.
    int max_select = (125 * 1000000) / clock_period_fs;
    if (max_select > 63) max_select = 63;
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;
    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                          divisor << QMI_M1_TIMING_CLKDIV_LSB;
}

// Exit QPI (in case of a warm reboot), then Read ID: APS6404-compatible chips
// answer KGD 0x5D.  Runs entirely from RAM: in direct mode any flash access
// deadlocks the QMI.
static bool __no_inline_not_in_flash_func(psram_detect)(void) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                            (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | 0xF5;
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
        (void)qmi_hw->direct_rx;
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (volatile int d = 0; d < 64; ++d) {}

        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        uint8_t mfid = 0, kgd = 0;
        for (int i = 0; i < 6; ++i) {
            qmi_hw->direct_tx = (i == 0) ? 0x9F : 0xFF;
            while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {}
            while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
            uint8_t b = (uint8_t)qmi_hw->direct_rx;
            if (i == 4) mfid = b;
            else if (i == 5) kgd = b;
        }
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (volatile int d = 0; d < 64; ++d) {}
        if (kgd == 0x5D || mfid == 0x5D) return true;
    }
    return false;
}

static uint32_t __not_in_flash_func(psram_probe_size)(void) {
    volatile uint8_t *p = (volatile uint8_t *)PSRAM_BASE;
    for (uint32_t i = 8 * MB; i < 16 * MB; i += 4096) p[i] = 16;
    for (uint32_t i = 4 * MB; i < 8 * MB; i += 4096) p[i] = 8;
    for (uint32_t i = 1 * MB; i < 4 * MB; i += 4096) p[i] = 4;
    for (uint32_t i = 0; i < 1 * MB; i += 4096) p[i] = 1;
    uint32_t res = p[16 * MB - 4096];
    for (uint32_t i = 15 * MB; i < 16 * MB; i += 4096)
        if (res != p[i]) return 0;
    // A floating bus can read a consistent garbage value; trust only our markers.
    if (res != 1 && res != 4 && res != 8 && res != 16) return 0;
    return res * MB;
}

uint32_t __no_inline_not_in_flash_func(psram_init)(unsigned cs_pin, int max_mhz) {
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    // Direct-mode window: IRQs off, RAM code only (USB/flash handlers would stall).
    const uint32_t ints = save_and_disable_interrupts();
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    if (!psram_detect()) {
        qmi_hw->direct_csr = 0;
        restore_interrupts(ints);
        psram_bytes = 0;
        return 0;
    }
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35;          // enter QPI
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
    qmi_hw->direct_csr = 0;
    restore_interrupts(ints);

    psram_retiming(clock_get_hz(clk_sys), max_mhz);
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6                                << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    psram_bytes = psram_probe_size();
    return psram_bytes;
}

uint32_t psram_size(void) { return psram_bytes; }
