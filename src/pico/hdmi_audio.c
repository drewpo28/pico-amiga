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
//   - the sample path: the steady 31.3 kHz output tick (audio_out.c, core 0)
//     upsamples into a queue of 4-sample packets (hdmi_audio_push), and the
//     line IRQ pops them on a per-line credit, bang-bang paced at +-1% around
//     a target depth (HDMI_AU_TARGET)
// The first version read the emulator's ring here, on the video clock. The
// emulator fills that ring a frame at a time, and below 50 fps it ran dry
// every frame: zeros, then a burst, i.e. sound "in waves". The tick holds the
// last sample through an underrun, as PWM/I2S always did.
//
// The packet side runs in the core 1 line IRQ and the push in a core 0 IRQ, so
// tables live in RAM.
#include "hdmi_audio.h"
#include <string.h>
#include "pico.h"
#include "amiga.h"
#include "hardware/sync.h"

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
//
// Character t takes bits 2t and 2t+1 of each subpacket, i.e. bits 2k and 2k+1
// of byte t / 4 (k = t % 4): a byte's even bits spread one per nibble give lane
// 1's bit j for four characters at once, its odd bits lane 2's (by table: the
// line IRQ runs this for every audio packet).
static uint16_t spread_even[256], spread_odd[256];   // .bss: SRAM

static void build_spread(void) {
    for (int x = 0; x < 256; x++) {
        uint16_t e = 0, o = 0;
        for (int k = 0; k < 4; k++) {
            e |= (uint16_t)(((x >> (2 * k)) & 1) << (4 * k));
            o |= (uint16_t)(((x >> (2 * k + 1)) & 1) << (4 * k));
        }
        spread_even[x] = e;
        spread_odd[x] = o;
    }
}

static void __not_in_flash_func(packet_nibbles)(uint8_t nib[32][3], const packet_t *p, uint32_t sync) {
    const uint32_t hdr = p->hdr[0] | p->hdr[1] << 8 | (uint32_t)p->hdr[2] << 16 | (uint32_t)p->hdr[3] << 24;
    for (int b = 0; b < 8; b++) {
        uint32_t e = 0, o = 0;
        for (int j = 0; j < 4; j++) {
            e |= (uint32_t)spread_even[p->sp[j][b]] << j;
            o |= (uint32_t)spread_odd[p->sp[j][b]] << j;
        }
        for (int k = 0; k < 4; k++) {
            const int t = 4 * b + k;
            nib[t][0] = (uint8_t)(sync | ((hdr >> t) & 1) << 2 | (t ? 8u : 0u));
            nib[t][1] = (uint8_t)((e >> (4 * k)) & 15);
            nib[t][2] = (uint8_t)((o >> (4 * k)) & 15);
        }
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
// SPSC queue of 4-sample packets, core 0 tick -> core 1 line IRQ. 128 packets =
// 10.7 ms; the pacing holds it near HDMI_AU_TARGET (5.3 ms), which is the cushion
// for core 0 stalls (pico-speccy: drivers/hdmi/hdmi.c, the same numbers).
#define AQ_LEN         128
#define HDMI_AU_TARGET 64
typedef struct { int16_t l[4], r[4]; } aq_pkt_t;
static aq_pkt_t aq[AQ_LEN];
static volatile uint32_t aq_wr, aq_rd;

static volatile bool active;          // an HDMI back-end is running (hdmi_audio_init)
static volatile bool enabled = true;
// Producer (core 0): linear-interpolating 31.3 -> 48 kHz upsampler, in exact
// steps (pico-speccy's Bresenham): an input interval is UP_IN units, an output
// step UP_OUT. A Q16 step rounded down was +22 ppm (see audio_out.c's period).
#define UP_IN  FS                     // up_pos << 16 stays within 32 bits
#define UP_OUT AMIGA_AUDIO_RATE
static uint32_t up_pos;               // output position between prev (0) and the new input (UP_IN)
static int16_t  up_l, up_r;           // the previous input
static aq_pkt_t up_pkt;
static int      up_n;
// Consumer (core 1).
static uint64_t credit;               // 48 kHz samples owed, Q24 (64-bit: see the cap)
static uint64_t credit_cap;
static uint32_t credit_hi, credit_lo; // per line: nominal +1% / -1%
static int      iec_ct;               // IEC 60958 192-frame block counter
// Health (hdmi_audio_counts): packets dropped on a full queue, lines whose
// credit wanted a packet the queue did not have, the lowest depth seen.
static volatile uint32_t au_full, au_starve, au_min = AQ_LEN;

void hdmi_audio_set_enabled(bool on) { enabled = on; }

void hdmi_audio_counts(uint32_t *full, uint32_t *starve, uint32_t *min_depth) {
    *full = au_full; *starve = au_starve; *min_depth = au_min;
    au_min = AQ_LEN;
}

void __not_in_flash_func(hdmi_audio_push)(int16_t l, int16_t r) {
    if (!active || !enabled) return;
    while (up_pos <= UP_IN) {
        const int32_t f = (int32_t)((up_pos << 16) / UP_IN);
        up_pkt.l[up_n] = (int16_t)(up_l + (((l - up_l) * f) >> 16));
        up_pkt.r[up_n] = (int16_t)(up_r + (((r - up_r) * f) >> 16));
        if (++up_n == 4) {
            up_n = 0;
            const uint32_t w = aq_wr;
            if (w - aq_rd < AQ_LEN) {                // full: the packet is dropped
                aq[w % AQ_LEN] = up_pkt;
                __dmb();
                aq_wr = w + 1;
            } else {
                au_full++;
            }
        }
        up_pos += UP_OUT;
    }
    up_pos -= UP_IN;
    up_l = l;
    up_r = r;
}

// Audio Sample packet: 4 samples, 16-bit in the top of 24, IEC 60958 channel
// status 0x04 (consumer LPCM, copy permitted), rate code 0x02 (48 kHz), 0x02
// (16-bit word) in bytes 0, 3, 4.
static void __not_in_flash_func(audio_packet)(packet_t *p, const aq_pkt_t *q) {
    int ct = iec_ct;
    p->hdr[0] = 0x02;
    p->hdr[1] = 0x0F;                             // layout 0, samples 0-3 present
    p->hdr[2] = (uint8_t)((ct < 4 ? (1 << ct) : 0) << 4);   // B.x: block start flags
    p->hdr[3] = bch3(p->hdr);
    for (int s = 0; s < 4; s++) {
        const int16_t l = q->l[s], r = q->r[s];
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
void hdmi_audio_init(uint32_t pixel_hz, uint32_t h_total, uint32_t v_total) {
    build_spread();
    build_static_packets(pixel_hz);
    // The two rates must straddle the producer's with margin: the tick runs on
    // the 1 MHz timer, the lines on the pixel clock, and they need not agree.
    const uint64_t nom = ((uint64_t)FS * h_total << 24) / pixel_hz;
    credit_hi = (uint32_t)(nom * 101 / 100);
    credit_lo = (uint32_t)(nom * 99 / 100);
    // Must outlast the longest run of lines with no audio packet (blanking lines
    // 1..4 carry ACR and InfoFrames) or the consumer under-delivers for good.
    credit_cap = (uint64_t)credit_hi * v_total / 8 + (8ull << 24);
    credit = 0;
    iec_ct = 0;
    aq_rd = aq_wr;
    active = true;
}

uint32_t hdmi_terc4(int nibble) { return terc4_table[nibble & 15]; }

void hdmi_island_frame(uint32_t pre[10], uint32_t post[2], bool vsync) {
    const uint32_t sync = (vsync ? 0u : 1u) << 1;   // negative polarity; HSYNC active (0) in the pulse
    // Data-island preamble (CTL0 = CTL2 = 1) and the guard bands.
    for (int i = 0; i < 8; i++) pre[i] = SYM(ctrl_sym[sync], CTRL_01, CTRL_01);
    pre[8] = pre[9] = post[0] = post[1] = SYM(terc4_table[0xC | sync], GB_DATA, GB_DATA);
}

static const packet_t *const fixed_pk[HDMI_PK_FIXED] = { &pk_null, &pk_acr, &pk_avi, &pk_vendor, &pk_aif };

void hdmi_audio_fixed_nibbles(int id, bool vsync, uint8_t nib[32][3]) {
    packet_nibbles(nib, fixed_pk[id], (vsync ? 0u : 1u) << 1);
}

int __not_in_flash_func(hdmi_audio_packet)(uint8_t nib[32][3], uint32_t blank, bool vsync) {
    const uint32_t depth = aq_wr - aq_rd;
    credit += depth > HDMI_AU_TARGET ? credit_hi : credit_lo;
    if (credit > credit_cap) credit = credit_cap;
    if (!vsync && blank == 1)                  return HDMI_PK_ACR;
    if (!vsync && blank == 2)                  return HDMI_PK_AVI;
    if (!vsync && blank == 3)                  return HDMI_PK_VENDOR;
    if (!vsync && blank == 4)                  return HDMI_PK_AIF;
    if (!vsync && blank && !(blank & 3))       return HDMI_PK_ACR;
    if (depth < au_min) au_min = depth;
    if (credit < (4ull << 24))                 return HDMI_PK_NULL;
    if (!depth) { au_starve++;                 return HDMI_PK_NULL; }
    credit -= 4ull << 24;                      // credit is spent only on a pop
    packet_t a;
    const uint32_t r = aq_rd;
    audio_packet(&a, &aq[r % AQ_LEN]);
    __dmb();
    aq_rd = r + 1;
    packet_nibbles(nib, &a, (vsync ? 0u : 1u) << 1);
    return -1;
}

void __not_in_flash_func(hdmi_audio_island)(uint32_t *chars, uint32_t blank, bool vsync) {
    hdmi_island_frame(chars, chars + 42, vsync);
    uint8_t nib[32][3];
    const int id = hdmi_audio_packet(nib, blank, vsync);
    if (id >= 0) packet_nibbles(nib, fixed_pk[id], (vsync ? 0u : 1u) << 1);
    for (int t = 0; t < 32; t++)
        chars[10 + t] = SYM(terc4_table[nib[t][0]], terc4_table[nib[t][1]], terc4_table[nib[t][2]]);
}
