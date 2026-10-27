// Audio output: the core's Paula ring to PWM, I2S or a PCM5122 DAC board.
//
// The output and its choice follow pico-speccy's pwm_audio.cpp (init_sound and
// the sample-clock ISR); the drivers are its drivers/audio (I2S on PIO, PCM5122
// over I2C):
//   - ZERO2 with the PCM5122 board fitted (probed once at boot over I2C, before
//     the PS/2 keyboard takes the same pins): I2S on the board's pins
//   - an I2S DAC on I2S_DATA/BCK/LCK, detected with board_test_pins(): I2S
//   - otherwise 8-bit PWM with first-order error feedback on PWM_PIN0 (right)
//     and PWM_PIN1 (left), an RC filter on the pins as on every Murmulator
//
// The sample clock is a hardware alarm with a RAM-resident ISR (an SDK repeating
// timer runs its alarm pool from flash on every tick) at AMIGA_AUDIO_RATE, with a
// fractional period.  The emulator fills the ring in bursts paced by the HDMI
// frame (50.06 Hz, a hair faster than 31300 frames/s): the reader holds the last
// frame when it catches up, drops single frames while it is more than a few
// frames' worth behind, and jumps when it has lost track entirely.
#include "audio_out.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "amiga.h"
#include "board.h"
#include "hdmi_audio.h"
#include "drivers/audio/audio.h"
#ifdef PCM5122_I2S_DATA
#include "drivers/audio/pcm5122_init.h"
#endif

bool is_i2s_enabled = false;      // drivers/audio/audio.c reads it
volatile int16_t audio_volume_q8 = 256;

void audio_set_volume(int q8) { audio_volume_q8 = (int16_t)(q8 < 0 ? 0 : q8 > 256 ? 256 : q8); }

static i2s_config_t i2s_config = {
    .sample_freq = AMIGA_AUDIO_RATE,
    .channel_count = 2,
    .data_pin = I2S_DATA_PIO,
    .bck_pin = I2S_BCK_PIO,
    .lck_pin = I2S_LCK_PIO,
    .pio = pio1,                  // beside the PS/2 keyboard and the NES pad
    .sm = 0,
    .dma_channel = 0,
    .dma_trans_count = 1,
    .dma_buf = NULL,
    .volume = 0,
    .program_offset = 0,
};

static const char *out_name = "none";

// ---- sample clock and output ---------------------------------------------------------
static const int16_t *ring;
static uint32_t ring_frames;
static uint32_t rd;
static int16_t  last_l, last_r;

static int      au_alarm = -1;
static uint32_t au_target;        // next alarm, microseconds (the 32-bit timer)
static uint32_t au_frac;          // its fraction, 1/256 us
static uint32_t au_period_q8;     // 1/AMIGA_AUDIO_RATE in 1/256 us

static inline void __not_in_flash_func(output)(int32_t sL, int32_t sR) {
    const int32_t vol = audio_volume_q8;
    sL = (sL * vol) >> 8;
    sR = (sR * vol) >> 8;
    hdmi_audio_push((int16_t)sL, (int16_t)sR);    // the HDMI islands take the same stream
    if (is_i2s_enabled) {
        if (!pio_sm_is_tx_fifo_full(i2s_config.pio, i2s_config.sm))
            pio_sm_put(i2s_config.pio, i2s_config.sm, ((uint32_t)(uint16_t)sR << 16) | (uint16_t)sL);
        return;
    }
    // 8-bit PWM with first-order error feedback (dither) on each channel.
    static int16_t err_L = 0, err_R = 0;
    int32_t xL = sL + 0x8000 + err_L;
    if (xL < 0) xL = 0; else if (xL > 0xFFFF) xL = 0xFFFF;
    uint16_t outL = (uint16_t)xL >> 8;
    err_L = (int16_t)(xL - ((int32_t)outL << 8));
    int32_t xR = sR + 0x8000 + err_R;
    if (xR < 0) xR = 0; else if (xR > 0xFFFF) xR = 0xFFFF;
    uint16_t outR = (uint16_t)xR >> 8;
    err_R = (int16_t)(xR - ((int32_t)outR << 8));
    pwm_set_gpio_level(PWM_PIN0, outR);
    pwm_set_gpio_level(PWM_PIN1, outL);
}

static void __not_in_flash_func(au_alarm_isr)(void) {
    const uint32_t bit = 1u << au_alarm;
    timer_hw->intr = bit;
    hw_clear_bits(&timer_hw->intf, bit);
    au_frac += au_period_q8;
    au_target += au_frac >> 8;
    au_frac &= 0xFF;
    const uint32_t target = au_target;
    timer_hw->alarm[au_alarm] = target;

    const uint32_t wp = amiga_audio_write_pos();
    uint32_t lag = wp - rd;
    if (lag > ring_frames / 2) { rd = wp - ring_frames / 4; lag = ring_frames / 4; }   // lost track
    else if (lag > 3 * (AMIGA_AUDIO_RATE / 50)) { rd++; lag--; }                        // drifting ahead
    if (lag) {
        const uint32_t i = (rd & (ring_frames - 1)) * 2;
        last_l = ring[i];
        last_r = ring[i + 1];
        rd++;
    }
    output(last_l, last_r);

    // An alarm fires only on an exact match, so a target already behind the
    // counter (a long IRQ-off stretch) would wait for the 32-bit wrap: force the
    // interrupt instead, and the ticks catch up back to back.
    if ((int32_t)(target - timer_hw->timerawl) <= 0) hw_set_bits(&timer_hw->intf, bit);
}

static void pwm_pin_init(uint pin) {
    pwm_config config = pwm_get_default_config();
    gpio_set_function(pin, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_wrap(&config, (1 << 8) - 1);
    pwm_init(pwm_gpio_to_slice_num(pin), &config, true);
    pwm_set_gpio_level(pin, 1 << 7);
}

bool audio_out_pcm5122_present(void) {
#ifdef PCM5122_I2S_DATA
    return pcm5122_present(PCM5122_I2C_SDA, PCM5122_I2C_SCL);
#else
    return false;
#endif
}

void audio_out_start(const int16_t *audio_ring, uint32_t frames, int mode) {
    ring = audio_ring;
    ring_frames = frames;

#ifdef PCM5122_I2S_DATA
    if (mode != 1 && audio_out_pcm5122_present() && pcm5122_init(PCM5122_I2C_SDA, PCM5122_I2C_SCL)) {
        i2s_config.data_pin = PCM5122_I2S_DATA;
        i2s_config.bck_pin = PCM5122_I2S_BCK;
        i2s_config.lck_pin = PCM5122_I2S_LCK;
        is_i2s_enabled = true;
        out_name = "PCM5122";
    } else
#endif
    if (mode == 2) {
        is_i2s_enabled = I2S_BCK_PIO != I2S_LCK_PIO;      // forced, unless the board has none
        if (is_i2s_enabled) out_name = "I2S";
    } else if (mode == 0 &&
               I2S_BCK_PIO != I2S_LCK_PIO && I2S_LCK_PIO != I2S_DATA_PIO && I2S_BCK_PIO != I2S_DATA_PIO) {
        // Drain residual charge before probing: after a warm restart the PWM
        // board's RC filter caps can hold the pins high, so testPins reads 1 even
        // under pull-down and would falsely select I2S.
        gpio_init(I2S_DATA_PIO); gpio_set_dir(I2S_DATA_PIO, GPIO_OUT); gpio_put(I2S_DATA_PIO, 0);
        gpio_init(I2S_BCK_PIO);  gpio_set_dir(I2S_BCK_PIO, GPIO_OUT);  gpio_put(I2S_BCK_PIO, 0);
        sleep_ms(50);
        gpio_deinit(I2S_DATA_PIO);
        gpio_deinit(I2S_BCK_PIO);
        int code = board_test_pins(I2S_DATA_PIO, I2S_BCK_PIO);
        // A pin reading HIGH under pull-down is back-fed by external circuitry,
        // not a floating I2S input: never I2S.
        if (code & 0b10100) code = 0;
        is_i2s_enabled = code != 0;
        if (is_i2s_enabled) out_name = "I2S";
    }

    if (is_i2s_enabled) {
        // Above GPIO31 (the _WS modules' audio on 40-42) I2S needs a block whose
        // window can move to gpio_base 16: pio1 is pinned to 0 by the keyboard.
        if (i2s_config.data_pin >= 32 || i2s_config.bck_pin >= 32 || i2s_config.lck_pin >= 32)
            i2s_config.pio = pio2;
        i2s_volume(&i2s_config, 0);
        i2s_init(&i2s_config);
    } else {
        pwm_pin_init(PWM_PIN0);
        pwm_pin_init(PWM_PIN1);
        out_name = "PWM";
    }

    rd = amiga_audio_write_pos();
    au_alarm = hardware_alarm_claim_unused(true);
    const uint irq = hardware_alarm_get_irq_num(au_alarm);
    irq_set_exclusive_handler(irq, au_alarm_isr);
    irq_set_enabled(irq, true);
    au_period_q8 = (uint32_t)((1000000ull << 8) / AMIGA_AUDIO_RATE);
    au_target = timer_hw->timerawl + (au_period_q8 >> 8);
    au_frac = 0;
    const uint32_t bit = 1u << au_alarm;
    timer_hw->intr = bit;
    hw_set_bits(&timer_hw->inte, bit);
    timer_hw->alarm[au_alarm] = au_target;
}

const char *audio_out_name(void) { return out_name; }
