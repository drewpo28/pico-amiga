// HDMI audio: data-island packets for the HDMI back-ends.
//
// The packet encoding (TERC4, BCH ECC, parity, InfoFrames, IEC 60958 channel
// status) and the packet schedule are pico-speccy's drivers/hdmi/hdmi.c (itself
// after PICO-BK's libdvi audio fork, hw-confirmed), down to the lessons recorded
// there:
//   - one island on every line, audio packets on the vsync lines too (an
//     HDMI->VGA converter goes silent without them)
//   - ACR on blanking line 1 and every 4th blanking line after it (strict sinks
//     need a dense ACR stream to lock their audio PLL)
//   - AVI, Vendor-Specific (keeps the sink in HDMI mode) and Audio InfoFrames
//     once a frame on blanking lines 2..4
//   - audio paced by a per-line credit of 48 kHz samples, packets of 4
// What differs is the sample source: the core's 31.3 kHz Paula ring, resampled
// here to 48 kHz (linear interpolation) with a slow rate trim that keeps the
// reader a steady distance behind the emulator.
//
// Everything here runs in the core 1 line IRQ, so tables live in RAM.
#include "hdmi_audio.h"
#include <string.h>
#include "pico.h"
#include "amiga.h"
#include "audio_out.h"

#define FS 48000u

// ---- TMDS symbols (HDMI 1.4b section 5.4) ----------------------------------------
#define CTRL_00 0x354u
#define CTRL_01 0x0abu
#define CTRL_10 0x154u
#define CTRL_11 0x2abu
#define GB_DATA 0x133u                // data-island guard band, lanes 1 and 2
#define GB_V0   0x2ccu                // video guard band, lanes 0 and 2
#define GB_V1   0x133u                // ...lane 1
#define SYM(l0, l1, l2) ((uint32_t)(l0) | (uint32_t)(l1) << 10 | (uint32_t)(l2) << 20)

static const uint32_t ctrl_sym[4] = { CTRL_00, CTRL_01, CTRL_10, CTRL_11 };   // [vs << 1 | hs]

const uint32_t __not_in_flash("hdmi_audio") hdmi_video_lead[HDMI_VLEAD_PX] = {
    // video preamble: CTL0 = 1 (lane 1 = 01), CTL2 = 0; lane 0 keeps the syncs idle
    SYM(CTRL_11, CTRL_01, CTRL_00), SYM(CTRL_11, CTRL_01, CTRL_00),
    SYM(CTRL_11, CTRL_01, CTRL_00), SYM(CTRL_11, CTRL_01, CTRL_00),
    SYM(CTRL_11, CTRL_01, CTRL_00), SYM(CTRL_11, CTRL_01, CTRL_00),
    SYM(CTRL_11, CTRL_01, CTRL_00), SYM(CTRL_11, CTRL_01, CTRL_00),
    SYM(GB_V0, GB_V1, GB_V0), SYM(GB_V0, GB_V1, GB_V0),
};

// TERC4 encoding table: 4-bit value -> 10-bit codeword (HDMI 1.4b Table 5-4)
static const uint16_t __not_in_flash("hdmi_audio") terc4_table[16] = {
    0b1010011100, 0b1001100011, 0b1011100100, 0b1011100010,
    0b0101110001, 0b0100011110, 0b0110001110, 0b0100111100,
    0b1011001100, 0b0100111001, 0b0110011100, 0b1011000110,
    0b1010001110, 0b1001110001, 0b0101100011, 0b1011000011
};

// BCH(64,56)/(32,24) ECC table, polynomial x^8+x^4+x^3+x^2+1 (pico-speccy)
static const uint8_t __not_in_flash("hdmi_audio") bch_table[256] = {
    0x00, 0xd9, 0xb5, 0x6c, 0x6d, 0xb4, 0xd8, 0x01, 0xda, 0x03, 0x6f, 0xb6, 0xb7, 0x6e, 0x02, 0xdb,
    0xb3, 0x6a, 0x06, 0xdf, 0xde, 0x07, 0x6b, 0xb2, 0x69, 0xb0, 0xdc, 0x05, 0x04, 0xdd, 0xb1, 0x68,
    0x61, 0xb8, 0xd4, 0x0d, 0x0c, 0xd5, 0xb9, 0x60, 0xbb, 0x62, 0x0e, 0xd7, 0xd6, 0x0f, 0x63, 0xba,
    0xd2, 0x0b, 0x67, 0xbe, 0xbf, 0x66, 0x0a, 0xd3, 0x08, 0xd1, 0xbd, 0x64, 0x65, 0xbc, 0xd0, 0x09,
    0xc2, 0x1b, 0x77, 0xae, 0xaf, 0x76, 0x1a, 0xc3, 0x18, 0xc1, 0xad, 0x74, 0x75, 0xac, 0xc0, 0x19,
    0x71, 0xa8, 0xc4, 0x1d, 0x1c, 0xc5, 0xa9, 0x70, 0xab, 0x72, 0x1e, 0xc7, 0xc6, 0x1f, 0x73, 0xaa,
    0xa3, 0x7a, 0x16, 0xcf, 0xce, 0x17, 0x7b, 0xa2, 0x79, 0xa0, 0xcc, 0x15, 0x14, 0xcd, 0xa1, 0x78,
    0x10, 0xc9, 0xa5, 0x7c, 0x7d, 0xa4, 0xc8, 0x11, 0xca, 0x13, 0x7f, 0xa6, 0xa7, 0x7e, 0x12, 0xcb,
    0x83, 0x5a, 0x36, 0xef, 0xee, 0x37, 0x5b, 0x82, 0x59, 0x80, 0xec, 0x35, 0x34, 0xed, 0x81, 0x58,
    0x30, 0xe9, 0x85, 0x5c, 0x5d, 0x84, 0xe8, 0x31, 0xea, 0x33, 0x5f, 0x86, 0x87, 0x5e, 0x32, 0xeb,
    0xe2, 0x3b, 0x57, 0x8e, 0x8f, 0x56, 0x3a, 0xe3, 0x38, 0xe1, 0x8d, 0x54, 0x55, 0x8c, 0xe0, 0x39,
    0x51, 0x88, 0xe4, 0x3d, 0x3c, 0xe5, 0x89, 0x50, 0x8b, 0x52, 0x3e, 0xe7, 0xe6, 0x3f, 0x53, 0x8a,
    0x41, 0x98, 0xf4, 0x2d, 0x2c, 0xf5, 0x99, 0x40, 0x9b, 0x42, 0x2e, 0xf7, 0xf6, 0x2f, 0x43, 0x9a,
    0xf2, 0x2b, 0x47, 0x9e, 0x9f, 0x46, 0x2a, 0xf3, 0x28, 0xf1, 0x9d, 0x44, 0x45, 0x9c, 0xf0, 0x29,
    0x20, 0xf9, 0x95, 0x4c, 0x4d, 0x94, 0xf8, 0x21, 0xfa, 0x23, 0x4f, 0x96, 0x97, 0x4e, 0x22, 0xfb,
    0x93, 0x4a, 0x26, 0xff, 0xfe, 0x27, 0x4b, 0x92, 0x49, 0x90, 0xfc, 0x25, 0x24, 0xfd, 0x91, 0x48,
};

static inline uint8_t bch3(const uint8_t *p) {
    uint8_t v = bch_table[p[0]];
    v = bch_table[p[1] ^ v];
    return bch_table[p[2] ^ v];
}
static inline uint8_t bch7(const uint8_t *p) {
    uint8_t v = bch_table[p[0]];
    for (int i = 1; i < 7; i++) v = bch_table[p[i] ^ v];
    return v;
}
// No __builtin_popcount: on the M33 it is a libgcc call, i.e. flash, from an IRQ.
static inline uint8_t parity8(uint8_t x) { x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; return x & 1; }

// ---- packets ---------------------------------------------------------------------
typedef struct { uint8_t hdr[4]; uint8_t sp[4][8]; } packet_t;

static packet_t pk_null, pk_acr, pk_avi, pk_vendor, pk_aif;

// A packet as the island's 32 packet characters, TERC4 nibbles per lane
// (HDMI 1.4b 5.2.3.4):
//   lane 0: D0 = HSYNC, D1 = VSYNC, D2 = header bit t, D3 = 0 on the first character
//   lane 1: D0..D3 = bit 2t of subpackets 0..3
//   lane 2: D0..D3 = bit 2t+1 of subpackets 0..3
static void __not_in_flash_func(packet_nibbles)(uint8_t nib[32][3], const packet_t *p, uint32_t sync) {
    const uint32_t hdr = p->hdr[0] | p->hdr[1] << 8 | (uint32_t)p->hdr[2] << 16 | (uint32_t)p->hdr[3] << 24;
    uint64_t sp[4];
    for (int j = 0; j < 4; j++) {
        uint64_t v = 0;
        for (int b = 7; b >= 0; b--) v = v << 8 | p->sp[j][b];
        sp[j] = v;
    }
    for (int t = 0; t < 32; t++) {
        uint32_t d1 = 0, d2 = 0;
        for (int j = 0; j < 4; j++) {
            d1 |= (uint32_t)((sp[j] >> (2 * t)) & 1) << j;
            d2 |= (uint32_t)((sp[j] >> (2 * t + 1)) & 1) << j;
        }
        nib[t][0] = (uint8_t)(sync | ((hdr >> t) & 1) << 2 | (t ? 8u : 0u));
        nib[t][1] = (uint8_t)d1;
        nib[t][2] = (uint8_t)d2;
    }
}

// InfoFrame checksum over header + payload, stored in PB0, then the ECC.
static void if_finish(packet_t *p) {
    int s = p->hdr[0] + p->hdr[1] + p->hdr[2];
    int n = p->hdr[2] + 1;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 7 && n; i++, n--) s += p->sp[j][i];
    p->sp[0][0] = (uint8_t)(-s);
    p->hdr[3] = bch3(p->hdr);
    for (int j = 0; j < 4; j++) p->sp[j][7] = bch7(p->sp[j]);
}

static void build_static_packets(uint32_t pixel_hz) {
    memset(&pk_null, 0, sizeof pk_null);

    // Audio Clock Regeneration: N = 6144 for 48 kHz, CTS = f_TMDS * N / (128 fs).
    const uint32_t n = 6144, cts = (uint32_t)((uint64_t)pixel_hz * n / (128u * FS));
    memset(&pk_acr, 0, sizeof pk_acr);
    pk_acr.hdr[0] = 0x01;
    pk_acr.hdr[3] = bch3(pk_acr.hdr);
    pk_acr.sp[0][1] = (cts >> 16) & 0xFF;
    pk_acr.sp[0][2] = (cts >> 8) & 0xFF;
    pk_acr.sp[0][3] = cts & 0xFF;
    pk_acr.sp[0][4] = (n >> 16) & 0xFF;
    pk_acr.sp[0][5] = (n >> 8) & 0xFF;
    pk_acr.sp[0][6] = n & 0xFF;
    pk_acr.sp[0][7] = bch7(pk_acr.sp[0]);
    for (int s = 1; s < 4; s++) memcpy(pk_acr.sp[s], pk_acr.sp[0], 8);

    // AVI: RGB, picture aspect 4:3 (the doubled 288-line PAL picture), VIC 0
    // (832 x 605 at 25.2 MHz is not a CEA timing).
    memset(&pk_avi, 0, sizeof pk_avi);
    pk_avi.hdr[0] = 0x82; pk_avi.hdr[1] = 0x02; pk_avi.hdr[2] = 0x0D;
    pk_avi.sp[0][2] = 0x18;           // M = 01 (4:3), R = 1000 (same as picture)
    if_finish(&pk_avi);

    // Vendor-Specific: IEEE OUI 0x000C03 (HDMI Licensing), LSB first.
    memset(&pk_vendor, 0, sizeof pk_vendor);
    pk_vendor.hdr[0] = 0x81; pk_vendor.hdr[1] = 0x01; pk_vendor.hdr[2] = 0x05;
    pk_vendor.sp[0][1] = 0x03; pk_vendor.sp[0][2] = 0x0C;
    if_finish(&pk_vendor);

    // Audio InfoFrame: 2 channels, rate and size "refer to stream header".
    memset(&pk_aif, 0, sizeof pk_aif);
    pk_aif.hdr[0] = 0x84; pk_aif.hdr[1] = 0x01; pk_aif.hdr[2] = 0x0A;
    pk_aif.sp[0][1] = 0x01;           // CC = 1: 2 channels
    if_finish(&pk_aif);
}

// ---- samples ---------------------------------------------------------------------
static const int16_t *ring;
static uint32_t ring_mask;
static uint32_t rd_frame, rd_frac;    // read position: frame index + 16-bit fraction
static uint32_t step_nom;             // source frames per output sample, Q16
static int32_t  lag_avg;              // Q8, smoothed write - read distance
static uint32_t lag_target;
static uint32_t credit;               // 48 kHz samples owed, Q24
static uint32_t credit_line;          // ...accrued per line
static int      iec_ct;               // IEC 60958 192-frame block counter
static volatile bool enabled = true;

void hdmi_audio_set_enabled(bool on) { enabled = on; }

static void __not_in_flash_func(next_sample)(int16_t *l, int16_t *r) {
    const uint32_t wp = amiga_audio_write_pos();
    uint32_t lag = wp - rd_frame;
    if (lag > ring_mask / 2 || lag < 2) {        // lost track, or ran dry: re-centre
        if (lag > ring_mask / 2) rd_frame = wp - lag_target;
        else if (lag < 2) { *l = *r = 0; return; }
        lag = wp - rd_frame;
        lag_avg = (int32_t)lag << 8;
    }
    lag_avg += (((int32_t)lag << 8) - lag_avg) >> 10;
    // Trim the rate by up to +-0.4% to hold the smoothed distance at the target.
    int32_t err = (lag_avg >> 8) - (int32_t)lag_target;
    if (err > 256) err = 256; else if (err < -256) err = -256;
    const uint32_t step = step_nom + (uint32_t)((int32_t)step_nom * err / 65536);

    const uint32_t i0 = (rd_frame & ring_mask) * 2, i1 = ((rd_frame + 1) & ring_mask) * 2;
    const int32_t f = (int32_t)rd_frac;
    const int32_t vol = audio_volume_q8;  // audio_out.c: the one volume for every output
    *l = (int16_t)(((ring[i0]     + (((ring[i1]     - ring[i0])     * f) >> 16)) * vol) >> 8);
    *r = (int16_t)(((ring[i0 + 1] + (((ring[i1 + 1] - ring[i0 + 1]) * f) >> 16)) * vol) >> 8);
    rd_frac += step;
    rd_frame += rd_frac >> 16;
    rd_frac &= 0xFFFF;
}

// Audio Sample packet: 4 samples, 16-bit in the top of 24, IEC 60958 channel
// status 0x04 (consumer LPCM, copy permitted), rate code 0x02 (48 kHz), 0x02
// (16-bit word) in bytes 0, 3, 4.
static void __not_in_flash_func(audio_packet)(packet_t *p) {
    int ct = iec_ct;
    p->hdr[0] = 0x02;
    p->hdr[1] = 0x0F;                             // layout 0, samples 0-3 present
    p->hdr[2] = (uint8_t)((ct < 4 ? (1 << ct) : 0) << 4);   // B.x: block start flags
    p->hdr[3] = bch3(p->hdr);
    for (int s = 0; s < 4; s++) {
        int16_t l, r;
        next_sample(&l, &r);
        const int f = ct ? 192 - ct : 0;
        const int cs = (f == 2) || (f >= 24 && f < 32 && ((0x02 >> (f - 24)) & 1)) || (f == 33);
        uint8_t *d = p->sp[s];
        d[0] = 0;
        d[1] = l & 0xFF;
        d[2] = (l >> 8) & 0xFF;
        d[3] = 0;
        d[4] = r & 0xFF;
        d[5] = (r >> 8) & 0xFF;
        const uint8_t pl = parity8(d[1]) ^ parity8(d[2]);
        const uint8_t pr = parity8(d[4]) ^ parity8(d[5]);
        d[6] = (uint8_t)(((pl << 3) | (pr << 7)) ^ (cs ? 0xCC : 0x00));
        d[7] = bch7(d);
        if (--ct < 0) ct = 191;
    }
    iec_ct = ct;
}

// ---- API -------------------------------------------------------------------------
void hdmi_audio_init(const int16_t *audio_ring, uint32_t frames, uint32_t pixel_hz, uint32_t h_total,
                     uint32_t v_total) {
    ring = audio_ring;
    ring_mask = frames - 1;
    build_static_packets(pixel_hz);
    // The emulator runs one Amiga frame (2 samples a line) per output frame.
    const uint64_t src_milli_hz = (uint64_t)2 * AMIGA_LINES_LONG * pixel_hz * 1000u / ((uint64_t)h_total * v_total);
    step_nom = (uint32_t)((src_milli_hz << 16) / (FS * 1000u));
    credit_line = (uint32_t)(((uint64_t)FS * h_total << 24) / pixel_hz);
    lag_target = 2 * AMIGA_LINES_LONG + AMIGA_LINES_LONG;   // ~1.5 Amiga frames of samples
    rd_frame = amiga_audio_write_pos() - lag_target;
    rd_frac = 0;
    lag_avg = (int32_t)lag_target << 8;
    credit = 0;
    iec_ct = 0;
}

uint32_t hdmi_terc4(int nibble) { return terc4_table[nibble & 15]; }

void hdmi_island_frame(uint32_t pre[10], uint32_t post[2], bool vsync) {
    const uint32_t sync = (vsync ? 0u : 1u) << 1;   // negative polarity; HSYNC active (0) in the pulse
    // Data-island preamble (CTL0 = CTL2 = 1) and the guard bands.
    for (int i = 0; i < 8; i++) pre[i] = SYM(ctrl_sym[sync], CTRL_01, CTRL_01);
    pre[8] = pre[9] = post[0] = post[1] = SYM(terc4_table[0xC | sync], GB_DATA, GB_DATA);
}

void __not_in_flash_func(hdmi_audio_packet)(uint8_t nib[32][3], uint32_t blank, bool vsync) {
    const uint32_t sync = (vsync ? 0u : 1u) << 1;
    credit += credit_line;
    if (credit > (8u << 24)) credit = 8u << 24;
    const packet_t *p = &pk_null;
    packet_t a;
    if (!vsync && blank == 1)                  p = &pk_acr;
    else if (!vsync && blank == 2)             p = &pk_avi;
    else if (!vsync && blank == 3)             p = &pk_vendor;
    else if (!vsync && blank == 4)             p = &pk_aif;
    else if (!vsync && blank && !(blank & 3))  p = &pk_acr;
    else if (credit >= (4u << 24) && ring && enabled) {
        credit -= 4u << 24;
        audio_packet(&a);
        p = &a;
    }
    packet_nibbles(nib, p, sync);
}

void __not_in_flash_func(hdmi_audio_island)(uint32_t *chars, uint32_t blank, bool vsync) {
    hdmi_island_frame(chars, chars + 42, vsync);
    uint8_t nib[32][3];
    hdmi_audio_packet(nib, blank, vsync);
    for (int t = 0; t < 32; t++)
        chars[10 + t] = SYM(terc4_table[nib[t][0]], terc4_table[nib[t][1]], terc4_table[nib[t][2]]);
}
