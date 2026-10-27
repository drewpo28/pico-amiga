// DVI/HDMI output through the RP2350 HSTX serializer and its TMDS encoder.
//
// Mode: 720x576 @ 50.06 Hz from a 25.2 MHz pixel clock (832 x 605 total) -
// the hw-proven 576-line timing of pico-speccy's video_mode_table.h [5]/[6],
// chosen because 25.2 MHz is clk_sys/15 at 378 and /20 at 504, the clocks the
// rest of the firmware runs at.  The 288 framebuffer rows (amiga.h) become
// 720x576: each FB row feeds two output lines (two different ones for an
// interlaced row), and each FIFO word carries two output pixels that the
// expander shifts out in turn (a doubled lowres pixel or a hires pair).
//
// Colours are the Amiga's 12-bit 0x0RGB (direct, or via the row palette).  The TMDS lanes take
// their top four bits straight from those nibbles (NBITS = 3), so colour
// conversion costs nothing; the price is a 240/255 peak level.
//
// Scanout runs entirely from a DMA ping-pong on core 1 with one IRQ per
// transfer, after the pico-examples "dvi_out_hstx_encoder" design (BSD-3,
// Raspberry Pi).  The pin/lane map is pico-speccy's (src/drivers/hdmi/
// hdmi_word.h) for its HDMI_BASE_PIN 12 boards.
#include "video.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "amiga.h"

#ifndef HDMI_BASE_PIN
#define HDMI_BASE_PIN 12
#endif

// ---- timing ----------------------------------------------------------------------
#define H_FP       12
#define H_SYNC     64
#define H_BP       36
#define H_ACTIVE   720
#define V_FP       5
#define V_SYNC     5
#define V_BP       19
#define V_ACTIVE   576
#define V_TOTAL    (V_FP + V_SYNC + V_BP + V_ACTIVE)   // 605

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS       (0x2u << 12)
#define HSTX_CMD_NOP        (0xfu << 12)

// Negative sync polarity: the idle level of both syncs is 1.
static uint32_t vblank_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | H_FP, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC, SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (H_BP + H_ACTIVE), SYNC_V1_H1,
    HSTX_CMD_NOP
};
static uint32_t vblank_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | H_FP, SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC, SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (H_BP + H_ACTIVE), SYNC_V0_H1,
    HSTX_CMD_NOP
};
static uint32_t vactive_cmd[] = {
    HSTX_CMD_RAW_REPEAT | H_FP, SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_SYNC, SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_BP, SYNC_V1_H1,
    HSTX_CMD_TMDS | H_ACTIVE
};

// ---- state ------------------------------------------------------------------------
#define DMACH_PING 0
#define DMACH_PONG 1

static const amiga_row_t *fb;
static uint32_t linebuf[2][AMIGA_FB_W];
static volatile uint32_t frames;
static bool     dma_pong;
static uint32_t v_line;              // next output line to post
static bool     cmd_posted;

// One output line of a row (amiga.h formats) as 360 FIFO words, two output
// pixels each: p | p << 16 doubles a lowres pixel, two indices give a hires pair.
static void __not_in_flash_func(expand_line)(uint32_t *dst, const amiga_row_t *row, int sub) {
    switch (row->mode) {
    case AMIGA_ROW_IDX8: {
        const uint8_t *ix = row->px.idx;
        const uint16_t *pal = row->pal;
        for (int x = 0; x < AMIGA_FB_W; x++)
            dst[x] = pal[ix[2 * x] & 63] | ((uint32_t)pal[ix[2 * x + 1] & 63] << 16);
        break;
    }
    case AMIGA_ROW_LACE4: {
        const uint8_t *n = row->px.nib[sub];
        const uint16_t *pal = row->pal + 16 * sub;
        for (int x = 0; x < AMIGA_FB_W; x++)
            dst[x] = pal[n[x] >> 4] | ((uint32_t)pal[n[x] & 15] << 16);
        break;
    }
    default: {
        const uint16_t *src = row->px.rgb;
        for (int x = 0; x < AMIGA_FB_W; x += 4) {
            uint32_t a = src[x], b = src[x + 1], c = src[x + 2], d = src[x + 3];
            dst[x]     = a | (a << 16);
            dst[x + 1] = b | (b << 16);
            dst[x + 2] = c | (c << 16);
            dst[x + 3] = d | (d << 16);
        }
        break;
    }
    }
}

static void __not_in_flash_func(dma_irq)(void) {
    uint ch = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *c = &dma_hw->ch[ch];
    dma_hw->intr = 1u << ch;
    dma_pong = !dma_pong;

    if (v_line >= V_FP && v_line < V_FP + V_SYNC) {
        c->read_addr = (uintptr_t)vblank_vsync_on;
        c->transfer_count = count_of(vblank_vsync_on);
    } else if (v_line < V_FP + V_SYNC + V_BP) {
        c->read_addr = (uintptr_t)vblank_vsync_off;
        c->transfer_count = count_of(vblank_vsync_off);
    } else if (!cmd_posted) {
        c->read_addr = (uintptr_t)vactive_cmd;
        c->transfer_count = count_of(vactive_cmd);
        cmd_posted = true;
        // Prepare the pixels this command list leads into.  The previous line's
        // pixels are being sent from the other buffer meanwhile.
        uint32_t out = v_line - (V_TOTAL - V_ACTIVE);
        expand_line(linebuf[out & 1], &fb[out >> 1], (int)(out & 1));
    } else {
        uint32_t out = v_line - (V_TOTAL - V_ACTIVE);
        c->read_addr = (uintptr_t)linebuf[out & 1];
        c->transfer_count = AMIGA_FB_W;
        cmd_posted = false;
    }
    if (!cmd_posted) {
        if (++v_line == V_TOTAL) { v_line = 0; frames++; }
    }
}

static void hstx_setup(void) {
    const uint32_t sys = clock_get_hz(clk_sys);
    const uint32_t want = 126000000;                 // 5 clk_hstx per 25.2 MHz pixel
    uint32_t div = (sys + want / 2) / want;
    if (div < 1) div = 1;
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys, div);

    // TMDS encoder input: 0x0RGB nibbles. Lane 2 = red (bits 11:8), lane 1 =
    // green (7:4), lane 0 = blue (3:0); each rotated so its nibble sits at 7:4.
    hstx_ctrl_hw->expand_tmds =
        3  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 4  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 0  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 28 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    // Two pixels per FIFO word (the same pixel twice: horizontal doubling).
    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_CLKPHASE_LSB |            // clock starts high (pico-speccy)
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Pin map (offsets from HDMI_BASE_PIN): clock pair at 0/1, data pairs from 2,
    // each pair wired N/P (first pin inverted).  PICO_PC swaps red and green.
#ifdef PICO_PC
    static const int lane_bit[3] = { 2, 6, 4 };
#else
    static const int lane_bit[3] = { 2, 4, 6 };
#endif
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (int lane = 0; lane < 3; lane++) {
        uint32_t sel = (uint32_t)(lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (uint32_t)(lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[lane_bit[lane]]     = sel | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[lane_bit[lane] + 1] = sel;
    }
    for (int i = 0; i < 8; i++) {
        uint pin = (uint)(HDMI_BASE_PIN + i);
        gpio_set_function(pin, GPIO_FUNC_HSTX);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
}

void video_start(const amiga_row_t *framebuffer) {
    fb = framebuffer;
    v_line = 2;                      // the two channels below are pre-loaded with lines 0 and 1
    cmd_posted = false;
    dma_pong = false;
    hstx_setup();

    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_vsync_off,
                          count_of(vblank_vsync_off), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_vsync_off,
                          count_of(vblank_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);
}

uint32_t video_frames(void) { return frames; }
