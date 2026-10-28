// VGA output through one PIO state machine (`out pins, 8`) on the pins the HDMI
// connector uses on the same boards (VGA_BASE_PIN), after pico-speccy's
// drivers/vga-nextgen/vga.c:
//   - the pin byte is 2 bits per channel (R 5:4, G 3:2, B 1:0), HSYNC bit 6,
//     VSYNC bit 7, both active low
//   - two DMA channels: the data channel streams a line template into the PIO
//     and chains to a control channel, which loads the next template's address
//     and chains back; the control channel's IRQ (one per line) renders the line
//     after next into the template not being streamed
//   - timing: pico-speccy's PIO VGA twin of its 720x576@50 mode ([5]): 27 MHz,
//     880 x 614 (HS 48, BP 96, active 720, FP 16; VSYNC lines 580-586), 49.97 Hz
//
// The Amiga framebuffer is 0x0RGB; the 2 bits per channel come from a Bayer 2x2
// ordered dither (vga.c's vga_bayer4: 13 levels a channel), looked up as one
// 16-bit output-pixel pair per colour and line parity (2 x 4096 x 2 B = 16 KB).
// Each framebuffer pixel is two output pixels, each row two lines.
#include "video.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "amiga.h"

#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN 6
#endif

#define PIXEL_HZ   27000000u
#define H_SYNC     48
#define H_BP       96
#define H_ACTIVE   (AMIGA_FB_W * 2)        // 720
#define H_FP       16
#define H_TOTAL    (H_SYNC + H_BP + H_ACTIVE + H_FP)   // 880
#define V_ACTIVE   (AMIGA_FB_H * 2)        // 576
#define V_TOTAL    614
#define VS_FIRST   580
#define VS_LAST    586

#define SYNC_IDLE  0xC0u                   // both syncs high (inactive)
#define BIT_HS     0x40u
#define BIT_VS     0x80u

#define VPIO       pio0

static const amiga_row_t *fb;
static uint16_t line_rgb[AMIGA_OUT_W];     // the output line being rendered
static uint16_t pair[2][4096];             // [line parity][0x0RGB] -> two pin bytes
static uint8_t  tmpl[4][H_TOTAL] __attribute__((aligned(4)));   // blank, vsync, active x2
static uint8_t *tmpl_ptr[4];
static int      dma_data, dma_ctrl;
static uint32_t line;                      // line whose template the ctrl channel just loaded
static volatile uint32_t frames;
static volatile uint32_t lines;            // line advances (line = lines mod V_TOTAL)

// vga.c's vga_bayer4 on a 4-bit channel: level 0..12, low 2-bit value plus how
// many of the four Bayer positions take the next value up.
static void bayer4(uint16_t c, uint8_t p[4]) {
    uint8_t lo[3], hi[3], sub[3];
    for (int k = 0; k < 3; k++) {
        const uint8_t v8 = (uint8_t)(((c >> (8 - 4 * k)) & 0xF) * 17);
        const uint8_t level = v8 / 21;
        lo[k] = level >> 2;
        sub[k] = level & 3;
        hi[k] = lo[k] + (lo[k] < 3 && sub[k]);
    }
#define PX(t) (uint8_t)(((sub[0] >= (t) ? hi[0] : lo[0]) << 4) | ((sub[1] >= (t) ? hi[1] : lo[1]) << 2) | \
                        (sub[2] >= (t) ? hi[2] : lo[2]))
    p[0] = PX(1);                          // x0,y0
    p[1] = PX(3);                          // x1,y0
    p[2] = PX(4);                          // x0,y1 (never above lo)
    p[3] = PX(2);                          // x1,y1
#undef PX
}

static void build_tables(void) {
    for (int c = 0; c < 4096; c++) {
        uint8_t p[4];
        bayer4((uint16_t)c, p);
        pair[0][c] = (uint16_t)((SYNC_IDLE | p[0]) | ((SYNC_IDLE | p[1]) << 8));
        pair[1][c] = (uint16_t)((SYNC_IDLE | p[2]) | ((SYNC_IDLE | p[3]) << 8));
    }
    for (int t = 0; t < 4; t++) {
        const uint8_t idle = (t == 1) ? (SYNC_IDLE & ~BIT_VS) : SYNC_IDLE;
        memset(tmpl[t], idle, H_TOTAL);
        memset(tmpl[t], idle & ~BIT_HS, H_SYNC);
        tmpl_ptr[t] = tmpl[t];
    }
}

// Fill template `t` with the picture for output line `l`.
static void __not_in_flash_func(render)(int t, uint32_t l) {
    const amiga_row_t *row = &fb[l >> 1];
    const uint16_t *pal = pair[l & 1];
    uint16_t *dst = (uint16_t *)(tmpl[t] + H_SYNC + H_BP);
    if (row->mode == AMIGA_ROW_DIRECT) {
        const uint16_t *src = row->px.rgb;
        for (int x = 0; x < AMIGA_FB_W; x += 2) {
            dst[x]     = pal[src[x] & 0xFFF];
            dst[x + 1] = pal[src[x + 1] & 0xFFF];
        }
        return;
    }
    // Hires / interlace: one pin byte per output pixel, from the 2x2 Bayer
    // pair of that pixel's colour (even columns take its low byte, odd its high).
    amiga_fb_line(row, (int)(l & 1), line_rgb);
    uint8_t *d8 = tmpl[t] + H_SYNC + H_BP;
    for (int x = 0; x < AMIGA_OUT_W; x += 2) {
        d8[x]     = (uint8_t)pal[line_rgb[x] & 0xFFF];
        d8[x + 1] = (uint8_t)(pal[line_rgb[x + 1] & 0xFFF] >> 8);
    }
}

static void __not_in_flash_func(dma_irq)(void) {
    dma_hw->ints0 = 1u << dma_ctrl;
    // The ctrl channel has just started line `line`; queue line + 1.
    lines++;
    if (++line == V_TOTAL) { line = 0; frames++; }
    const uint32_t next = line + 1 == V_TOTAL ? 0 : line + 1;
    int t;
    if (next < V_ACTIVE) {
        t = 2 + (next & 1);
        render(t, next);
    } else {
        t = (next >= VS_FIRST && next <= VS_LAST) ? 1 : 0;
    }
    dma_channel_set_read_addr(dma_ctrl, &tmpl_ptr[t], false);
}

void vga_start(const amiga_row_t *framebuffer) {
    fb = framebuffer;
    build_tables();
    render(2, 0);

#if VGA_BASE_PIN >= 32
    pio_set_gpio_base(VPIO, 16);           // ZERO2: display on GPIO 32-39
#endif
    static const uint16_t prog_instr[] = { 0x6008 };   // out pins, 8
    static const struct pio_program prog = { .instructions = prog_instr, .length = 1, .origin = -1 };
    const uint offset = pio_add_program(VPIO, &prog);
    const uint sm = (uint)pio_claim_unused_sm(VPIO, true);
    for (int i = 0; i < 8; i++) {
        pio_gpio_init(VPIO, VGA_BASE_PIN + i);
        gpio_set_slew_rate(VGA_BASE_PIN + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(VPIO, sm, VGA_BASE_PIN, 8, true);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)PIXEL_HZ);
    pio_sm_init(VPIO, sm, offset, &c);
    pio_sm_set_enabled(VPIO, sm, true);

    dma_data = dma_claim_unused_channel(true);
    dma_ctrl = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(dma_data);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(VPIO, sm, true));
    channel_config_set_chain_to(&d, dma_ctrl);
    dma_channel_configure(dma_data, &d, &VPIO->txf[sm], tmpl[2], H_TOTAL / 4, false);

    dma_channel_config k = dma_channel_get_default_config(dma_ctrl);
    channel_config_set_transfer_data_size(&k, DMA_SIZE_32);
    channel_config_set_read_increment(&k, false);
    channel_config_set_write_increment(&k, false);
    channel_config_set_chain_to(&k, dma_data);
    // Line 0 streams first (started below); the ctrl channel then loads line 1.
    render(3, 1);
    dma_channel_configure(dma_ctrl, &k, &dma_hw->ch[dma_data].read_addr, &tmpl_ptr[3], 1, false);
    line = 0;

    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    dma_channel_set_irq0_enabled(dma_ctrl, true);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_start_channel_mask(1u << dma_data);
}

uint32_t vga_frames(void) { return frames; }
uint32_t vga_lines(void) { return lines; }          // the picture is lines 0..V_ACTIVE-1
uint32_t vga_frame_lines(void) { return V_TOTAL; }
