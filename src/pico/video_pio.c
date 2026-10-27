// DVI/HDMI output through a PIO TMDS serializer, for boards whose HDMI pins are
// not the HSTX ones (PICO_DV: clock on GPIO 6/7, data on 8-13).
//
// Same mode as video_hstx.c - 720x576 @ 50.06 Hz, 25.2 MHz pixel, 832 x 605 -
// so the PIO runs at 252 MHz: clk_sys / 1.5 at 378, / 2 at 504 (pico-speccy
// runs its PIO HDMI from the same half-integer dividers).
//
// Every output pixel is two 32-bit words of pin patterns (tmds_pio.h), i.e.
// 1664 words per line and ~200 MB/s of DMA.  Each FB row is TMDS-encoded once
// by core 1's main loop, one row ahead of the beam (it is shown on two output
// lines, 66 us), from lookup tables of balanced character pairs; the DMA
// ping-pong + IRQ structure is the one in video_hstx.c, each line being two
// transfers: blanking (fp + sync + bp) and the active part.
#include "video.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "amiga.h"
#include "tmds_pio.h"
#include "tmds.pio.h"

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN 6
#endif
#define PIN_CLK   HDMI_BASE_PIN
#define PIN_DATA  (HDMI_BASE_PIN + 2)

#define H_FP       12
#define H_SYNC     64
#define H_BP       36
#define H_ACTIVE   720
#define H_BLANK    (H_FP + H_SYNC + H_BP)
#define V_FP       5
#define V_SYNC     5
#define V_BP       19
#define V_ACTIVE   576
#define V_TOTAL    (V_FP + V_SYNC + V_BP + V_ACTIVE)

#define VPIO       pio0
#define VSM        0
#define DMACH_PING 0
#define DMACH_PONG 1

static const uint16_t *fb;
static uint32_t tab_r[16][4];
static uint32_t tab_gb[256][4];
static uint32_t blank_line[2][H_BLANK * 2];        // [vsync active?]
static uint32_t blank_active[2][H_ACTIVE * 2];     // the active part of a vblank line
static uint32_t rowbuf[2][AMIGA_FB_W * 4];

static volatile uint32_t frames;
static volatile int      shown_row = -1;           // row whose pixels were last posted
static bool     dma_pong, part_b;
static uint32_t v_line;

static void fill_ctrl(uint32_t *dst, int px, int vs, int hs) {
    uint32_t w[2];
    tmds_ctrl_words(vs, hs, w);
    for (int i = 0; i < px; i++) { dst[2 * i] = w[0]; dst[2 * i + 1] = w[1]; }
}

static void build_static(void) {
    tmds_build_tables(tab_r, tab_gb);
    for (int vs = 0; vs < 2; vs++) {
        int vlev = vs ? 0 : 1;                         // negative polarity: active = 0
        fill_ctrl(blank_line[vs], H_FP, vlev, 1);
        fill_ctrl(blank_line[vs] + 2 * H_FP, H_SYNC, vlev, 0);
        fill_ctrl(blank_line[vs] + 2 * (H_FP + H_SYNC), H_BP, vlev, 1);
        fill_ctrl(blank_active[vs], H_ACTIVE, vlev, 1);
    }
}

static void __not_in_flash_func(dma_irq)(void) {
    uint ch = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *c = &dma_hw->ch[ch];
    dma_hw->intr = 1u << ch;
    dma_pong = !dma_pong;

    const bool vsync = v_line >= V_FP && v_line < V_FP + V_SYNC;
    const bool active = v_line >= V_TOTAL - V_ACTIVE;
    if (!part_b) {
        c->read_addr = (uintptr_t)blank_line[vsync];
        c->transfer_count = H_BLANK * 2;
        part_b = true;
    } else {
        if (active) {
            int row = (int)(v_line - (V_TOTAL - V_ACTIVE)) >> 1;
            c->read_addr = (uintptr_t)rowbuf[row & 1];
            shown_row = row;
        } else {
            c->read_addr = (uintptr_t)blank_active[vsync];
        }
        c->transfer_count = H_ACTIVE * 2;
        part_b = false;
        if (++v_line == V_TOTAL) { v_line = 0; frames++; shown_row = -1; }
    }
}

static void __no_inline_not_in_flash_func(encode_loop)(void) {
    int done = -2;
    for (;;) {
        // Keep one row ahead of the beam; in the vertical blank prepare row 0.
        int want = shown_row + 1;
        if (want >= AMIGA_FB_H) want = 0;
        if (want != done) {
            tmds_encode_row(rowbuf[want & 1], fb + want * AMIGA_FB_W, AMIGA_FB_W, tab_r, tab_gb);
            done = want;
        } else {
            __wfi();
        }
    }
}

void video_start(const uint16_t *framebuffer) {
    fb = framebuffer;
    build_static();
    tmds_encode_row(rowbuf[0], fb, AMIGA_FB_W, tab_r, tab_gb);

    uint offset = pio_add_program(VPIO, &tmds_program);
    pio_sm_config c = tmds_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, PIN_CLK);
    sm_config_set_out_pins(&c, PIN_DATA, 6);
    sm_config_set_out_shift(&c, true, true, 30);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / 252000000.0f);
    for (int i = 0; i < 8; i++) {
        uint pin = (uint)(HDMI_BASE_PIN + i);
        pio_gpio_init(VPIO, pin);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(VPIO, VSM, HDMI_BASE_PIN, 8, true);
    pio_sm_init(VPIO, VSM, offset, &c);

    v_line = 1;            // the two pre-loaded transfers below are line 0's parts
    part_b = false;
    dma_pong = false;
    dma_channel_config d = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&d, DMACH_PONG);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, VSM, true));
    dma_channel_configure(DMACH_PING, &d, &VPIO->txf[VSM], blank_line[0], H_BLANK * 2, false);
    d = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&d, DMACH_PING);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, VSM, true));
    dma_channel_configure(DMACH_PONG, &d, &VPIO->txf[VSM], blank_active[0], H_ACTIVE * 2, false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);
    pio_sm_set_enabled(VPIO, VSM, true);

    encode_loop();         // never returns: core 1 encodes rows from here on
}

uint32_t video_frames(void) { return frames; }
