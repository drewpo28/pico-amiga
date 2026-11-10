// Akiko: the CD32's gate array at $B80000 - the CD drive's controller, the
// NVRAM's I2C lines and the chunky-to-planar converter.  After WinUAE's
// akiko.cpp (Toni Wilen), without its threads and sector cache: the platform's
// read callback is asked for a sector when its turn comes (75 or 150 a second).
//
//   $B80000.L  $C0CACAFE
//   $B80004.L  INTREQ (read-only; each bit has its own way of being cleared)
//   $B80008.L  INTENA
//     31 subcode  30 drive took a command (PIO)  29 drive has a reply (PIO)
//     28 reply DMA done  27 command DMA done  26 sector DMA done  25 overflow
//   $B80010.L  sector DMA base (4 KB blocks: bit n of PBX is base + n * $1000)
//   $B80014.L  base of the 256-byte reply ring (+0), the subcode buffers
//              (+$100, two of 128) and the 256-byte command ring (+$200)
//   $B80018.B  read: subcode offset; write: clears the subcode interrupt
//   $B80019/1A read: command / reply ring positions
//   $B8001D.B  write: command ring end (the drive takes bytes up to it)
//   $B8001F.B  write: reply ring end (the drive's reply goes in up to it)
//   $B80020.W  PBX: the blocks the next sectors may go to, highest bit first;
//              a write sets bits and clears interrupt 26
//   $B80024.L  flags: 31 subcode, 30 command DMA, 29 reply DMA, 28 CAS,
//              27 sector DMA (PBX), 26 enable (reading), 25 raw, 24 MSB, 23 NTSC
//   $B80028.B  PIO byte to / from the drive
//   $B80030.B  NVRAM I2C: bit 7 SCL, 6 SDA;  $B80032.B their directions
//   $B80038.L  C2P: eight longs in (32 chunky pixels), eight planar longs out
//
// The drive (a Chinon, by commands of 1-12 bytes + checksum, the first byte's
// low nibble the command, its high one a counter the reply repeats): 1 stop,
// 2 pause, 3 unpause, 4 play / read (start and end MSF; byte 7 bit 7 = data),
// 5 LED, 6 subcode Q, 7 info (door and firmware; the TOC follows a seek to
// before the disc: one entry a sector time, each three times).
//
// A sector arrives in its block as the drive's 2352 raw bytes with the first
// four replaced (0, 0, 0, a 5-bit counter): the sync's rest, the header (MSF
// in BCD and the mode) at +12 and the data at +16.  cd.device checks nothing
// behind the data, so the EDC / ECC bytes are whatever the platform left.
//
// CD audio: a play command over audio tracks is heard - akiko_audio, called by
// Paula for each output sample (31300 Hz), takes the disc's 44100 Hz frames
// from the platform a sector (588 frames) at a time, the nearest one for each
// sample, and adds them at half level to Paula's (clamped).  Both clocks are
// counted from the lines (75 sectors and 31300 samples a second), so the audio's
// own sector follows play_pos; it is put back on it when they part (a pause, a
// snapshot).
#include <string.h>
#include "amiga_int.h"
#include "m68k.h"

#define INT_SUBCODE   0x80000000u
#define INT_DRIVEXMIT 0x40000000u
#define INT_DRIVERECV 0x20000000u
#define INT_RXDMADONE 0x10000000u
#define INT_TXDMADONE 0x08000000u
#define INT_PBX       0x04000000u
#define INT_OVERFLOW  0x02000000u

#define FLAG_SUBCODE  0x80000000u
#define FLAG_TXD      0x40000000u
#define FLAG_RXD      0x20000000u
#define FLAG_CAS      0x10000000u
#define FLAG_PBX      0x08000000u
#define FLAG_ENABLE   0x04000000u

#define CDS_ERROR   0x80
#define CDS_PLAYING 0x08
#define CH_ERR_BADCOMMAND 0x80
#define CH_ERR_CHECKSUM   0x88
#define CH_ERR_NODISK     0xF8

#define LINES_PER_SEC (AMIGA_LINES_LONG * 50)
#define TOC_REPEAT 3

static amiga_cd_t cd;                    // tracks 0: no disc

// ---- the registers and the drive (snapshots) -----------------------------------
static uint32_t intreq, intena, flags, pbx;
static uint32_t addr_data, addr_misc;
static uint8_t  sub_off, txinx, rxinx, txcmp, rxcmp, last_rx;
static uint8_t  result[32], command[32];
static uint8_t  cmd, cmd_len, cmd_active, rx_len, rx_off;
static bool     cmd_checksum, cmd_unknown;
static uint8_t  initialized;             // 0 reset, 1 first status sent, 2 the ROM asked for info
static bool     disk, mediachanged, door = true;
static bool     paused;
static uint8_t  playing;                 // 0, 1 starting, 2 playing
static int8_t   audiotimeout;
static int32_t  toc_counter;             // -1: not sending the TOC
static int32_t  data_offset;             // the read's first sector, -1: none
static uint32_t sector_counter;
static int32_t  current_sector;
static uint8_t  speed;                   // 1 or 2
static uint8_t  seek_delay, tx_delay, rx_delay;
static uint32_t play_pos, play_end;      // CD audio, sectors
static uint16_t tick_acc, play_acc;      // line counters: a sector's time
static uint8_t  led;

// CD audio's read position: not in snapshots (it follows play_pos).
static const uint8_t *a_ptr;             // the sector being played, NULL: silence
static uint32_t a_lsn;
static uint16_t a_frame, a_frac;         // the frame in it, and 313ths of the next
static bool     a_on;

// Activity (amiga_cd_status): not in snapshots.
static uint32_t act_frame, act_lsn, act_sectors;

static uint8_t tobcd(uint32_t v) { return (uint8_t)((v / 10) << 4 | v % 10); }
static uint32_t frombcd(uint8_t v) { return (uint32_t)(v >> 4) * 10 + (v & 15); }
static int32_t msf2lsn(const uint8_t *p) {         // three BCD bytes
    return (int32_t)((frombcd(p[0]) * 60 + frombcd(p[1])) * 75 + frombcd(p[2])) - 150;
}
static void lsn2msf(uint32_t lsn, uint8_t *p) {    // into three BCD bytes
    lsn += 150;
    p[0] = tobcd(lsn / (75 * 60)); p[1] = tobcd(lsn / 75 % 60); p[2] = tobcd(lsn % 75);
}

static void track_of(int n, uint32_t *lsn, uint8_t *ctrl) {
    *lsn = 0; *ctrl = 4;
    if (cd.track) cd.track(cd.ctx, n, lsn, ctrl);
}
// The track a sector is in (1..tracks), 0: none.
static int track_at(uint32_t lsn, uint32_t *start, uint8_t *ctrl) {
    if (lsn >= cd.leadout) return 0;
    for (int n = cd.tracks; n >= 1; n--) {
        track_of(n, start, ctrl);
        if (lsn >= *start) return n;
    }
    return 0;
}
static bool data_sector(uint32_t lsn) {
    uint32_t s; uint8_t c;
    return track_at(lsn, &s, &c) && (c & 0x0C) == 4;
}

static void set_status(uint32_t bits) { intreq |= bits; }

// ---- the NVRAM: a 24C08 on two I2C lines ------------------------------------------
// 1 KB in the platform's buffer (cfg.nvram; none: the lines float high and
// nothing answers).  Bits are taken on SCL's rising edge and put out on its
// falling one; SDA moving while SCL is high is a start or a stop.
static uint8_t  i2c_dir;
static bool     i2c_scl = true, i2c_sda = true;      // as the host drives them
static bool     ee_sda = true;                        // as the chip does
static uint8_t  ee_state, ee_cnt, ee_shift;
static uint16_t ee_addr;
static bool     ee_rw, ee_hostack, nv_dirty;
enum { EE_IDLE, EE_DEV, EE_WADDR, EE_WDATA, EE_READ };

static void ee_lines(bool scl, bool sda) {
    uint8_t *nv = A.cfg.nvram;
    const bool oscl = i2c_scl, osda = i2c_sda;
    i2c_scl = scl; i2c_sda = sda;
    if (!nv) return;
    if (oscl && scl) {
        if (osda && !sda) { ee_state = EE_DEV; ee_cnt = 0; ee_shift = 0; ee_sda = true; }   // start
        else if (!osda && sda) { ee_state = EE_IDLE; ee_sda = true; }                       // stop
        return;
    }
    if (ee_state == EE_IDLE || oscl == scl) return;
    if (scl) {                                           // rising: a bit in
        if (ee_state == EE_READ) {
            if (ee_cnt == 8) ee_hostack = !sda;
        } else if (ee_cnt < 8) {
            ee_shift = (uint8_t)(ee_shift << 1 | (sda ? 1 : 0));
        }
        ee_cnt++;
        return;
    }
    if (ee_state == EE_READ) {                           // falling: the next bit out
        if (ee_cnt == 9) {
            ee_cnt = 0;
            if (!ee_hostack) { ee_state = EE_IDLE; ee_sda = true; return; }
            ee_addr = (uint16_t)((ee_addr + 1) & 1023);
        }
        ee_sda = ee_cnt < 8 ? (nv[ee_addr] >> (7 - ee_cnt)) & 1 : true;
        return;
    }
    if (ee_cnt == 8) {                                   // a byte is in: acknowledge it
        if (ee_state == EE_DEV) {
            if ((ee_shift & 0xF0) != 0xA0) { ee_state = EE_IDLE; ee_sda = true; return; }
            ee_addr = (uint16_t)((ee_addr & 0xFF) | ((ee_shift >> 1) & 3) << 8);
            ee_rw = ee_shift & 1;
        } else if (ee_state == EE_WADDR) {
            ee_addr = (uint16_t)((ee_addr & 0x300) | ee_shift);
        } else {
            if (nv[ee_addr] != ee_shift) { nv[ee_addr] = ee_shift; nv_dirty = true; }
            ee_addr = (uint16_t)((ee_addr & ~15) | ((ee_addr + 1) & 15));   // a 16-byte page
        }
        ee_sda = false;
    } else if (ee_cnt == 9) {
        ee_cnt = 0;
        ee_sda = true;
        if (ee_state == EE_DEV) ee_state = ee_rw ? EE_READ : EE_WADDR;
        else if (ee_state == EE_WADDR) ee_state = EE_WDATA;
        if (ee_state == EE_READ) ee_sda = nv[ee_addr] >> 7;
    }
}

static void nvram_write(int off, uint8_t v) {
    if (off == 2) { i2c_dir = v; return; }
    if (off != 0) return;
    ee_lines((i2c_dir & 0x80) ? (v & 0x80) != 0 : true, (i2c_dir & 0x40) ? (v & 0x40) != 0 : true);
}
static uint8_t nvram_read(int off) {
    if (off == 2) return i2c_dir;
    if (off != 0) return 0;
    return (uint8_t)((i2c_scl ? 0x80 : 0) | (i2c_sda && ee_sda ? 0x40 : 0));
}

bool amiga_cd_nvram_dirty(void) { const bool d = nv_dirty; nv_dirty = false; return d; }

// ---- chunky to planar -----------------------------------------------------------------
static uint32_t c2p_in[8], c2p_out[8];
static int8_t   c2p_rd = -1;
static uint8_t  c2p_wr;

static void c2p_do(void) {
    memset(c2p_out, 0, sizeof c2p_out);
    for (int i = 0; i < 8 * 32; i++)
        if (c2p_in[7 - (i >> 5)] & (1u << (i & 31))) c2p_out[i & 7] |= 1u << (i >> 3);
}
static void c2p_write(int off, uint8_t v) {
    if (off == 3) c2p_in[c2p_wr] = 0;
    c2p_in[c2p_wr] |= (uint32_t)v << (8 * (3 - off));
    if (off == 0) c2p_wr = (c2p_wr + 1) & 7;
    c2p_rd = -1;
}
static uint8_t c2p_read(int off) {
    if (c2p_rd < 0) { c2p_do(); c2p_rd = 0; }
    c2p_wr = 0;
    return (uint8_t)(c2p_out[c2p_rd] >> (8 * (3 - off)));
}

// ---- CD audio ---------------------------------------------------------------------------
AMIGA_HOT void akiko_audio(int32_t *l, int32_t *r) {
    if (playing != 2 || paused || !cd.audio) { a_on = false; return; }
    const uint32_t d = a_lsn > play_pos ? a_lsn - play_pos : play_pos - a_lsn;
    if (!a_on || d > 2) {                                 // (re)start on the drive's sector
        a_on = true;
        a_lsn = play_pos; a_frame = 0; a_frac = 0;
        a_ptr = cd.audio(cd.ctx, a_lsn);
    }
    if (a_ptr) {
        int16_t s[2];
        memcpy(s, a_ptr + 4u * a_frame, 4);
        int32_t x = *l + s[0] / 2, y = *r + s[1] / 2;
        *l = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
        *r = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
    }
    a_frac += 441;                                        // 44100 / 31300 frames a sample
    a_frame += a_frac / 313; a_frac %= 313;
    if (a_frame >= 588) {
        a_frame -= 588;
        a_lsn++;
        a_ptr = a_lsn < play_end ? cd.audio(cd.ctx, a_lsn) : NULL;
    }
}

// ---- the drive ------------------------------------------------------------------------
static void audio_stop(void) {
    a_on = false; a_ptr = NULL;
    audiotimeout = 0;
    paused = false;
    playing = 0;
}

static bool can_return(void) { return rx_len == 0; }

static void start_return(int len) {
    if (!can_return() || len <= 0) return;
    uint8_t sum = 0xFF;
    for (int i = 0; i < len; i++) sum -= result[i];
    result[len] = sum;
    rx_len = (uint8_t)(len + 1);
    rx_off = 0;
    set_status(INT_DRIVERECV);
}

// The reply into the ring, up to the end the ROM set.
static void return_data(void) {
    if (!rx_len || !(flags & FLAG_RXD) || rxinx == rxcmp || rx_delay) return;
    while (rx_off < rx_len) {
        last_rx = result[rx_off];
        m68k_write_memory_8(addr_misc + rxinx, last_rx);
        rxinx++;
        rx_off++;
        if (rxinx == rxcmp) { set_status(INT_RXDMADONE); break; }
    }
    if (rx_off == rx_len) {
        rx_len = rx_off = 0;
        intreq &= ~INT_DRIVERECV;
        set_status(INT_DRIVEXMIT);
    }
}

static int media_status(void) {
    result[0] = 0x0A;
    result[1] = disk ? 0x01 : 0x00;
    return 2;
}

// One TOC entry: the points $A0 (first track), $A1 (last), $A2 (lead-out), then
// the tracks; each goes out TOC_REPEAT times.
static int toc_entry(void) {
    result[0] = 6;
    const int points = cd.tracks ? cd.tracks + 3 : 0;
    if (!points) { result[1] = CDS_ERROR | door; toc_counter = -1; return 15; }
    const int j = toc_counter / TOC_REPEAT;
    uint8_t *d = result + 2;
    memset(d, 0, 13);
    uint32_t lsn; uint8_t ctrl;
    if (j == 0)      { track_of(1, &lsn, &ctrl);         d[3] = 0xA0; d[8] = tobcd(1); }
    else if (j == 1) { track_of(cd.tracks, &lsn, &ctrl); d[3] = 0xA1; d[8] = tobcd(cd.tracks); }
    else if (j == 2) { track_of(cd.tracks, &lsn, &ctrl); d[3] = 0xA2; lsn2msf(cd.leadout, d + 8); }
    else             { track_of(j - 2, &lsn, &ctrl);     d[3] = tobcd((uint32_t)(j - 2)); lsn2msf(lsn, d + 8); }
    d[1] = (uint8_t)(1 | ctrl << 4);
    result[1] = 0x0A;                        // unknown, but a real CD32 sets it
    result[6] = tobcd(99);
    result[7] = tobcd(24 + (uint32_t)toc_counter / 75);
    result[8] = tobcd((uint32_t)toc_counter % 75);
    toc_counter++;
    if (toc_counter / TOC_REPEAT >= points) toc_counter = -1;
    return 15;
}

static bool nodisk(void) {
    if (disk) return false;
    result[1] = CH_ERR_NODISK | door;
    return true;
}

// Subcode Q of the playing position: 11 bytes at d.
static void qcode(uint8_t *d) {
    memset(d, 0, 11);
    if (playing != 2) return;
    uint32_t start; uint8_t ctrl;
    const int t = track_at(play_pos, &start, &ctrl);
    if (!t) return;
    d[1] = (uint8_t)(ctrl << 4 | 1);
    d[2] = tobcd((uint32_t)t);
    d[3] = 1;
    lsn2msf(play_pos - start - 150, d + 4);           // within the track (lsn2msf adds the 150 back)
    lsn2msf(play_pos, d + 8);
}

static int play_notify(int status) {
    result[0] = 4;
    result[1] = (uint8_t)((status < 0 ? CDS_ERROR : status == 0 ? CDS_PLAYING | 2 : 0) | door);
    return 2;
}

// Seek / play audio / read data.
static int command_multi(void) {
    const int32_t seekpos = msf2lsn(command + 1), endpos = msf2lsn(command + 4);
    if (playing) audio_stop();
    paused = false;
    speed = (command[8] & 0x40) ? 2 : 1;
    result[0] = cmd;
    result[1] = 0;
    if (!disk) { result[1] = 1; return 2; }
    if (command[7] & 0x80) {                              // data
        data_offset = seekpos;
        const int32_t far = current_sector > seekpos ? current_sector - seekpos : seekpos - current_sector;
        seek_delay = far < 100 ? 1 : (uint8_t)(far / 1000 + 10 > 100 ? 100 : far / 1000 + 10);
        result[1] |= 0x02;
    } else if (seekpos < 0) {                             // before the disc: the TOC
        toc_counter = 0;
    } else {                                              // audio
        toc_counter = -1;
        result[1] = 0x42;
        playing = 1;
        uint32_t start; uint8_t ctrl;
        uint32_t from = (uint32_t)seekpos;
        int t = track_at(from, &start, &ctrl);
        if (t && (ctrl & 0x0C) == 4) {                    // a data track: the next audio one
            t = track_at(from + 150, &start, &ctrl);
            if (t && (ctrl & 0x0C) == 4 && t < cd.tracks) { track_of(t + 1, &start, &ctrl); from = start; t++; }
            else if (t) from = start;
        }
        if (t && (ctrl & 0x0C) != 4) {
            play_pos = from;
            play_end = endpos > 0 && (uint32_t)endpos < cd.leadout ? (uint32_t)endpos : cd.leadout;
            play_acc = 0;
            audiotimeout = 10;
        } else {
            audiotimeout = -3;                            // it did not start: the next status says so
        }
    }
    return 2;
}

static const int8_t command_lengths[16] = { 1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 2, -1, -1, -1, -1, -1 };

static bool can_send(void) { return initialized && !cmd_active && !rx_len; }

static void add_command_byte(uint8_t b) {
    if (cmd_len >= sizeof command) cmd_len = 0;
    command[cmd_len++] = b;
    cmd = command[0];
    const int len = command_lengths[cmd & 15];
    cmd_checksum = cmd_unknown = false;
    if (len < 0) { cmd_unknown = true; cmd_active = 1; return; }
    if (len + 1 > cmd_len) return;
    uint8_t sum = 0;
    for (int i = 0; i < len + 1; i++) sum += command[i];
    if (sum != 0xFF) cmd_checksum = true;
    cmd_active = 1;
    cmd_len = (uint8_t)len;
}

// The command ring's next byte to the drive.
static void run_command(void) {
    if (!(flags & FLAG_TXD) || (flags & FLAG_ENABLE) || txinx == txcmp || tx_delay || !can_send()) return;
    add_command_byte((uint8_t)m68k_read_memory_8((addr_misc | 0x200) + txinx));
    txinx++;
    if (txinx == txcmp) set_status(INT_TXDMADONE);
}

static void run_command_run(void) {
    int len = 0;
    cmd_len = 0;
    cmd_active = 0;
    memset(result, 0, sizeof result);
    if (cmd_checksum || cmd_unknown) {
        result[0] = (uint8_t)((cmd & 0xF0) | 5);
        result[1] = (uint8_t)((cmd_checksum ? CH_ERR_CHECKSUM : CH_ERR_BADCOMMAND) | door);
        start_return(2);
        return;
    }
    switch (cmd & 15) {
    case 0: result[0] = cmd; len = 1; break;
    case 1:                                               // stop
        audiotimeout = 0;
        result[0] = cmd; len = 2;
        if (!nodisk()) { result[1] = 0; audio_stop(); }
        break;
    case 2:                                               // pause
        audiotimeout = 0;
        toc_counter = -1;
        result[0] = cmd; len = 2;
        if (!nodisk()) { result[1] = (uint8_t)((playing ? CDS_PLAYING : 0) | door); paused = true; }
        break;
    case 3:                                               // unpause
        result[0] = cmd; len = 2;
        if (!nodisk()) { result[1] = (uint8_t)((playing ? CDS_PLAYING : 0) | door); paused = false; }
        break;
    case 4: len = command_multi(); break;
    case 5:                                               // LED
        led = command[1] & 1;
        if (command[1] & 0x80) { result[0] = cmd; result[1] = led; len = 2; }
        break;
    case 6:                                               // subcode Q
        result[0] = cmd;
        qcode(result + 2);
        len = 15;
        break;
    case 7:                                               // info: the door, the firmware
        result[0] = cmd;
        result[1] = door;
        memcpy(result + 2, "CHINON  O-658-2 24", 18);
        initialized = 2;
        len = 20;
        break;
    default: break;
    }
    if (!len) { set_status(INT_DRIVEXMIT); return; }
    start_return(len);
}

// The copper's list mirror (amiga_int.h) follows a DMA write into chip RAM.
static void mirror_sync(uint32_t off, uint32_t n) {
    for (int w = 0; w < COP_MIR_N; w++) {
        const uint32_t lo = cop_mir_lo[w];
        if (lo == COP_MIR_NONE || off + n <= lo || off >= lo + COP_MIR_SIZE) continue;
        const uint32_t a = off > lo ? off : lo, b = off + n < lo + COP_MIR_SIZE ? off + n : lo + COP_MIR_SIZE;
        memcpy(cop_mir[w] + (a - lo), A.cfg.chip_ram + a, b - a);
    }
    if (cop_ra_lo != COP_MIR_NONE && off + n > cop_ra_lo && off < cop_ra_lo + COP_RA_SIZE) cop_ra_drop();
}

// 96 bytes of subchannel with Q in bit 6 of each, from 12 bytes of Q.
static void put_subcode(const uint8_t *q) {
    sub_off = sub_off >= 128 ? 0 : 128;
    const uint32_t base = (addr_misc | 0x100) + sub_off;
    for (int i = 0; i < 96; i++)
        m68k_write_memory_8(base + (uint32_t)i, (q[i >> 3] >> (7 - (i & 7))) & 1 ? 0x40 : 0);
    m68k_write_memory_16(base + 96, 0xFFFF);
    m68k_write_memory_16(base + 98, 0x0000);
    sub_off += 100;
    set_status(INT_SUBCODE);
}
static void q_of(uint32_t lsn, uint8_t *q) {
    memset(q, 0, 12);
    uint32_t start; uint8_t ctrl;
    const int t = track_at(lsn, &start, &ctrl);
    if (!t) return;
    q[0] = (uint8_t)(ctrl << 4 | 1);
    q[1] = tobcd((uint32_t)t);
    q[2] = 1;
    lsn2msf(lsn - start - 150, q + 3);
    lsn2msf(lsn, q + 7);
}

// One sector into the highest block PBX offers.
static void run_read(void) {
    if (!(flags & FLAG_ENABLE) || !pbx || !(flags & FLAG_PBX) || data_offset < 0 || !cd.tracks) return;
    int n = 15;
    while (!(pbx & (1u << n))) n--;
    const uint32_t sector = (uint32_t)data_offset + sector_counter;
    current_sector = (int32_t)sector;
    if (!data_sector(sector)) return;
    const uint32_t off = (addr_data + (uint32_t)n * 4096u) & 0xFFFFFF;
    if (off + 4096u > A.cfg.chip_size) return;            // the CD32 has chip RAM only
    uint8_t *p = A.cfg.chip_ram + off;
    a_on = false; a_ptr = NULL;                           // the platform's buffer is the read's now
    const int mode = cd.read(cd.ctx, sector, p + 16);
    if (mode < 0) return;                                 // it comes round again
    p[0] = p[1] = p[2] = 0;
    p[3] = (uint8_t)(sector_counter & 31);
    memset(p + 4, 0xFF, 7);
    p[11] = 0;
    lsn2msf(sector, p + 12);
    p[15] = (uint8_t)mode;
    memset(p + 0xC00, 0, 146);
    mirror_sync(off, 4096);
    act_frame = A.frame; act_lsn = sector; act_sectors++;
    pbx &= ~(1u << n);
    set_status(INT_PBX);
    if (flags & FLAG_SUBCODE) {
        uint8_t q[12];
        q_of(sector, q);
        put_subcode(q);
    }
    sector_counter++;
}

// What the drive says by itself: the disc, the play's start and end, the TOC.
static void handler(bool framesync) {
    if (rx_len) return;
    if (!initialized) {                                   // the first status, with a disc in
        if (disk) start_return(media_status());
        initialized = 1;
        return;
    }
    if (initialized < 2) return;
    if (mediachanged) {
        start_return(media_status());
        mediachanged = false;
        return;
    }
    if (audiotimeout > 1) audiotimeout--;
    if (audiotimeout == 1) {                              // the play started
        if (playing <= 1) start_return(play_notify(0));
        playing = 2;
        audiotimeout = 0;
    }
    if (audiotimeout == -1) {                             // it ended
        if (playing) { audio_stop(); audiotimeout = -2; }
        else audiotimeout = 0;
    }
    if (audiotimeout == -2 && can_return()) { start_return(play_notify(1)); audiotimeout = 0; }
    if (audiotimeout == -3 && can_return()) { start_return(play_notify(-1)); audiotimeout = 0; }
    if (toc_counter >= 0 && !cmd_active && framesync && can_return()) start_return(toc_entry());
}

static void internal(void) {
    return_data();
    run_command();
    if (cmd_active && !--cmd_active) run_command_run();
}

// Once a line.
void akiko_line(void) {
    bool framesync = false;
    tick_acc += (uint16_t)(75 * (speed ? speed : 1));
    if (tick_acc >= LINES_PER_SEC) {                      // a sector's time
        tick_acc -= LINES_PER_SEC;
        if (seek_delay) seek_delay--; else run_read();
        framesync = true;
    }
    if (tx_delay) tx_delay--;
    if (rx_delay) rx_delay--;
    if (playing == 2 && !paused) {                        // CD audio: 75 sectors a second
        play_acc += 75;
        if (play_acc >= LINES_PER_SEC) {
            play_acc -= LINES_PER_SEC;
            if (flags & FLAG_SUBCODE) {
                uint8_t q[12];
                q_of(play_pos, q);
                put_subcode(q);
            }
            if (++play_pos >= play_end) audiotimeout = -1;
        }
    }
    internal();
    handler(framesync);
    if (intreq & intena) amiga_set_intreq(INTF_PORTS);
}

// ---- the registers ------------------------------------------------------------------
static uint8_t get_long(uint32_t v, int off) { return (uint8_t)(v >> ((3 - off) * 8)); }
static void put_long(uint32_t *p, int off, uint8_t v) {
    *p = (*p & ~(0xFFu << ((3 - off) * 8))) | (uint32_t)v << ((3 - off) * 8);
}

static uint8_t bget(uint32_t a) {
    uint8_t v = 0;
    a &= 0x3F;
    switch (a) {
    case 0x00: return 0xC0;
    case 0x01: case 0x02: return 0xCA;
    case 0x03: return 0xFE;
    case 0x30: case 0x31: case 0x32: case 0x33: return nvram_read((int)a - 0x30);
    case 0x38: case 0x39: case 0x3A: case 0x3B: return c2p_read((int)a - 0x38);
    case 0x04: case 0x05: case 0x06: case 0x07: v = get_long(intreq, (int)a - 0x04); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: v = get_long(intena, (int)a - 0x08); break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: v = get_long(intena, (int)a - 0x0C); break;
    case 0x18: case 0x10: case 0x14: case 0x1C: v = sub_off; break;
    case 0x19: case 0x11: case 0x15: case 0x1D: v = txinx; break;
    case 0x1A: case 0x12: case 0x16: case 0x1E: v = rxinx; break;
    case 0x20: case 0x21: v = get_long(pbx, (int)a - 0x20 + 2); break;
    case 0x24: case 0x25: case 0x26: case 0x27: v = get_long(flags, (int)a - 0x24); break;
    case 0x28:
        if (!(flags & FLAG_RXD) && rx_off < rx_len) {
            last_rx = result[rx_off++];
            if (rx_off == rx_len) { intreq &= ~INT_DRIVERECV; rx_len = rx_off = 0; set_status(INT_DRIVEXMIT); }
        } else {
            intreq &= ~INT_DRIVERECV;
        }
        v = last_rx;
        break;
    default: break;
    }
    internal();
    return v;
}

static void bput(uint32_t a, uint8_t v) {
    uint32_t tmp;
    a &= 0x3F;
    switch (a) {
    case 0x30: case 0x31: case 0x32: case 0x33: nvram_write((int)a - 0x30, v); return;
    case 0x38: case 0x39: case 0x3A: case 0x3B: c2p_write((int)a - 0x38, v); return;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        put_long(&intena, (int)a - 0x08, v);
        intena &= 0xFF000000u;
        break;
    case 0x10: case 0x11: case 0x12: case 0x13:
        put_long(&addr_data, (int)a - 0x10, v);
        addr_data &= 0x00FFF000u;
        break;
    case 0x14: case 0x15: case 0x16: case 0x17:
        put_long(&addr_misc, (int)a - 0x14, v);
        addr_misc &= 0x00FFFC00u;
        break;
    case 0x18: intreq &= ~INT_SUBCODE; break;
    case 0x1D: intreq &= ~INT_TXDMADONE; txcmp = v; tx_delay = 3; break;
    case 0x1F: intreq &= ~INT_RXDMADONE; rxcmp = v; rx_delay = 3; break;
    case 0x20: case 0x21:
        tmp = pbx;
        put_long(&pbx, (int)a - 0x20 + 2, v);
        pbx = (pbx | tmp) & 0xFFFF;
        if (!(flags & FLAG_PBX)) pbx = 0;
        intreq &= ~INT_PBX;
        break;
    case 0x24: case 0x25: case 0x26: case 0x27:
        tmp = flags;
        put_long(&flags, (int)a - 0x24, v);
        if ((flags & FLAG_ENABLE) && !(tmp & FLAG_ENABLE)) { sector_counter = 0; intreq &= ~INT_OVERFLOW; }
        if (!(flags & FLAG_PBX)) pbx = 0;
        flags &= 0xFF800000u;
        break;
    case 0x28:
        if (!(flags & FLAG_TXD)) {
            intreq &= ~INT_DRIVEXMIT;
            if (can_send()) {
                add_command_byte(v);
                if (can_send()) set_status(INT_DRIVEXMIT);
            }
        }
        break;
    default: break;
    }
    internal();
}

// The C2P's read position moves on once per access, whatever its width.
static void c2p_next(uint32_t a) {
    a &= 0x3F;
    if (a >= 0x38 && a < 0x3C) c2p_rd = (c2p_rd + 1) & 7;
}

uint8_t akiko_read(uint32_t a) {
    if (a & 0x8000) return 0;
    const uint8_t v = bget(a);
    c2p_next(a);
    return v;
}
uint16_t akiko_read16(uint32_t a) {
    if (a & 0x8000) return 0;
    uint16_t v = bget(a + 1);
    v |= (uint16_t)(bget(a) << 8);
    c2p_next(a);
    return v;
}
uint32_t akiko_read32(uint32_t a) {
    if (a & 0x8000) return 0;
    uint32_t v = bget(a + 3);
    v |= (uint32_t)bget(a + 2) << 8;
    v |= (uint32_t)bget(a + 1) << 16;
    v |= (uint32_t)bget(a) << 24;
    c2p_next(a);
    return v;
}
void akiko_write(uint32_t a, uint8_t v) {
    if (!(a & 0x8000)) bput(a, v);
}
void akiko_write16(uint32_t a, uint16_t v) {
    if (a & 0x8000) return;
    bput(a + 1, (uint8_t)v);
    bput(a, (uint8_t)(v >> 8));
}
void akiko_write32(uint32_t a, uint32_t v) {
    if (a & 0x8000) return;
    bput(a + 3, (uint8_t)v);
    bput(a + 2, (uint8_t)(v >> 8));
    bput(a + 1, (uint8_t)(v >> 16));
    bput(a, (uint8_t)(v >> 24));
}

// ---- reset, the disc --------------------------------------------------------------------
void akiko_reset(void) {
    audio_stop();
    i2c_scl = i2c_sda = ee_sda = true;
    i2c_dir = 0;
    ee_state = EE_IDLE;
    speed = 1;
    current_sector = -1;
    data_offset = -1;
    sector_counter = 0;
    txinx = rxinx = txcmp = rxcmp = 0;
    last_rx = 0;
    intreq = INT_SUBCODE;
    sub_off = 0xC2;
    intena = 0;
    flags = 0;
    pbx = 0;
    addr_data = addr_misc = 0;
    led = 0;
    rx_len = rx_off = 0;
    cmd_len = cmd_active = 0;
    initialized = 0;
    toc_counter = -1;
    seek_delay = tx_delay = rx_delay = 0;
    tick_acc = play_acc = 0;
    disk = cd.tracks != 0;
    mediachanged = true;
    c2p_rd = -1; c2p_wr = 0;
}

void amiga_set_cd(const amiga_cd_t *d) {
    if (d && d->tracks && d->read) cd = *d; else memset(&cd, 0, sizeof cd);
    audio_stop();
    disk = cd.tracks != 0;
    mediachanged = true;
    toc_counter = -1;
    data_offset = -1;
    act_frame = act_lsn = act_sectors = 0;
}

void amiga_cd_status(amiga_cdst_t *st) {
    st->present = disk;
    st->led = led;
    st->playing = playing == 2 && !paused;
    st->lsn = playing == 2 ? play_pos : act_lsn;
    st->idle = act_sectors ? A.frame - act_frame : 0xFFFFFFFFu;
    st->sectors = act_sectors;
}

// Snapshots: the registers and the drive's protocol state.  The disc is the
// platform's, put back before the load.
void akiko_state(st_t *s) {
    st_u32(s, &intreq); st_u32(s, &intena); st_u32(s, &flags); st_u32(s, &pbx);
    st_u32(s, &addr_data); st_u32(s, &addr_misc);
    st_u8(s, &sub_off); st_u8(s, &txinx); st_u8(s, &rxinx); st_u8(s, &txcmp); st_u8(s, &rxcmp); st_u8(s, &last_rx);
    st_raw(s, result, sizeof result); st_raw(s, command, sizeof command);
    st_u8(s, &cmd); st_u8(s, &cmd_len); st_u8(s, &cmd_active); st_u8(s, &rx_len); st_u8(s, &rx_off);
    st_bool(s, &cmd_checksum); st_bool(s, &cmd_unknown);
    st_u8(s, &initialized); st_bool(s, &mediachanged); st_bool(s, &paused); st_u8(s, &playing);
    st_i8(s, &audiotimeout);
    uint32_t u = (uint32_t)toc_counter; st_u32(s, &u); toc_counter = (int32_t)u;
    u = (uint32_t)data_offset; st_u32(s, &u); data_offset = (int32_t)u;
    st_u32(s, &sector_counter);
    u = (uint32_t)current_sector; st_u32(s, &u); current_sector = (int32_t)u;
    st_u8(s, &speed); st_u8(s, &seek_delay); st_u8(s, &tx_delay); st_u8(s, &rx_delay);
    st_u32(s, &play_pos); st_u32(s, &play_end); st_u16(s, &tick_acc); st_u16(s, &play_acc);
    st_u8(s, &led);
    st_u8(s, &i2c_dir); st_bool(s, &i2c_scl); st_bool(s, &i2c_sda); st_bool(s, &ee_sda);
    st_u8(s, &ee_state); st_u8(s, &ee_cnt); st_u8(s, &ee_shift); st_u16(s, &ee_addr);
    st_bool(s, &ee_rw); st_bool(s, &ee_hostack);
    ST_ARRAY(s, st_u32, c2p_in); ST_ARRAY(s, st_u32, c2p_out);
    st_i8(s, &c2p_rd); st_u8(s, &c2p_wr);
    if (s->mode == ST_LOAD) {
        if (rx_len > sizeof result) rx_len = 0;
        if (rx_off > rx_len) rx_off = rx_len;
        if (cmd_len > sizeof command) cmd_len = 0;
        if (c2p_rd > 7) c2p_rd = -1;
        c2p_wr &= 7;
        ee_addr &= 1023;
        disk = cd.tracks != 0;
        a_on = false; a_ptr = NULL;
    }
}
