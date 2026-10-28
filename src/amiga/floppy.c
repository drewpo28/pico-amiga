// Floppy drives and disk DMA.
//
// ADF images are MFM-encoded one track at a time into a 6334-word buffer, the
// length of a real DD track at 300 rpm / 500 kbit/s: 11 sectors of 544 words
// (AAAA AAAA 4489 4489, info, label, two checksums, 512 data bytes split into
// odd/even halves) followed by a gap.  A read DMA copies DSKLEN words out of
// that ring starting at the current rotational position (after the next sync
// word when ADKCON WORDSYNC is set), exactly as the head would see them, so
// trackdisk.device's normal "read a whole revolution and find the sectors"
// logic works unchanged.  Write DMA decodes the MFM back into sectors.
//
// The data moves at DMA start; DSKBLK is raised after the transfer time scaled
// by FLOPPY_TURBO (1 = real speed, 32 us per word), and while a DMA runs the
// index pulses come FLOPPY_TURBO times as often too: a drive that spins faster,
// so a transfer still spans as many revolutions as on hardware.  Barbarian
// (Palace) starts a two-revolution read on an index pulse and stops it on the
// third, its DSKBLK handler only a fallback; with the index at real speed the
// read ended first, the loader returned with its index handler still on the
// level 6 vector and the next pulse, in the OS's context, never cleared EXTER.
//
// A disk put into a drive that holds one (or was emptied less than SWAP_LINES
// ago) goes in only after the drive has been empty for SWAP_LINES: Kickstart
// polls /CHNG now and then, and a swap from the menu (no emulated time between
// eject and insert) was not noticed - Dune II waited for disk 5 forever with
// it in DF0. KS 1.3 needs about half a second; two are given.
//
// DF0..DF(n-1) are connected (floppy_set_drives). Kickstart finds the external
// ones by their ID, shifted out on /RDY while the motor is off: a 3.5" DD drive
// answers all ones (/RDY low), an empty port all zeros (every line high).
#include <string.h>
#include "amiga_int.h"

#ifndef FLOPPY_TURBO
#define FLOPPY_TURBO 4
#endif

#define NDRIVES       4
#define SECTORS       11
#define SECTOR_WORDS  544
#define TRACK_WORDS   6334
#define CYLINDERS     80
#define SWAP_LINES    (2 * 50 * AMIGA_LINES_LONG)   // a swapped disk stays out 2 s

typedef struct {
    amiga_disk_t disk;
    bool     inserted;
    bool     motor;
    bool     changed;       // /CHNG latched low until a step with a disk in
    uint8_t  cyl;
    bool     pending;       // `next` goes in once the drive was empty SWAP_LINES
    amiga_disk_t next;
    uint32_t empty_lines;   // lines since the drive became empty (saturating)
} drive_t;

static drive_t  drv[NDRIVES];
static int      ndrives = 1;             // connected drives, DF0 first
static uint8_t  prb_last = 0xFF;
static int      side;                    // 0 = lower head
static int      sel = -1;                // lowest selected drive or -1

static uint16_t track[TRACK_WORDS];
static int      trk_drive = -1, trk_num = -1;
static uint32_t rot_pos;                 // word under the head

static uint32_t dskpt;
static uint16_t dsklen, dsklen_prev, dsksync = 0x4489;
static bool     dma_on, dma_write;
static int      dma_drive;
static uint32_t dma_lines_left;
static uint32_t dma_end_frame;           // A.frame when the last DMA ended (amiga_floppy_status)
static int      last_drive = -1;
static uint32_t index_lines;

// ---- MFM --------------------------------------------------------------------
// Data bits sit in the 0x5555 positions; a clock bit is 1 between two zeros.
static int mfm_prev;   // last data bit emitted
static int wpos;

static void put_raw(uint16_t w) {
    track[wpos++] = w;
    mfm_prev = w & 1;
}

static void put_data16(uint16_t d) {
    uint16_t w = d & 0x5555;
    uint16_t clk = (uint16_t)(~((w << 1) | (w >> 1) | (mfm_prev << 15)) & 0xAAAA);
    track[wpos++] = w | clk;
    mfm_prev = w & 1;
}

static void put_data32(uint32_t d) {
    put_data16((uint16_t)(d >> 16));
    put_data16((uint16_t)d);
}

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void encode_track(int d, int tnum) {
    uint8_t sec[512];
    wpos = 0; mfm_prev = 0;
    for (int s = 0; s < SECTORS; s++) {
        uint32_t off = ((uint32_t)tnum * SECTORS + (uint32_t)s) * 512u;
        if (!drv[d].disk.read || off + 512 > drv[d].disk.size ||
            drv[d].disk.read(drv[d].disk.ctx, off, sec, 512))
            memset(sec, 0, sizeof sec);

        put_data16(0); put_data16(0);         // AAAA AAAA
        put_raw(0x4489); put_raw(0x4489);
        uint32_t info = 0xFF000000u | ((uint32_t)tnum << 16) | ((uint32_t)s << 8) | (uint32_t)(SECTORS - s);
        uint32_t io = (info >> 1) & 0x55555555u, ie = info & 0x55555555u;
        put_data32(io); put_data32(ie);
        for (int i = 0; i < 8; i++) put_data32(0);                 // label (16 bytes, odd + even)
        uint32_t hsum = io ^ ie;
        put_data32((hsum >> 1) & 0x55555555u); put_data32(hsum & 0x55555555u);
        uint32_t dsum = 0;
        for (int i = 0; i < 128; i++) {
            uint32_t v = rd32(sec + i * 4);
            dsum ^= ((v >> 1) & 0x55555555u) ^ (v & 0x55555555u);
        }
        dsum &= 0x55555555u;
        put_data32((dsum >> 1) & 0x55555555u); put_data32(dsum & 0x55555555u);
        for (int i = 0; i < 128; i++) put_data32((rd32(sec + i * 4) >> 1) & 0x55555555u);
        for (int i = 0; i < 128; i++) put_data32(rd32(sec + i * 4) & 0x55555555u);
    }
    while (wpos < TRACK_WORDS) put_data16(0);                      // gap
    trk_drive = d; trk_num = tnum;
}

static void ensure_track(int d) {
    int tnum = drv[d].cyl * 2 + side;
    if (trk_drive != d || trk_num != tnum) encode_track(d, tnum);
}

// Decode a write-DMA buffer (chip RAM words) back into sectors.
static uint32_t chip32(uint32_t a) {
    return ((uint32_t)chip_rw(a) << 16) | chip_rw(a + 2);
}

static void decode_write(int d, uint32_t ptr, uint32_t words) {
    if (!drv[d].disk.write) return;
    uint32_t end = ptr + words * 2;
    for (uint32_t a = ptr; a + SECTOR_WORDS * 2 <= end + 4; a += 2) {
        if (chip_rw(a) != 0x4489) continue;
        while (chip_rw(a) == 0x4489 && a < end) a += 2;
        uint32_t info = ((chip32(a) & 0x55555555u) << 1) | (chip32(a + 4) & 0x55555555u);
        int tnum = (info >> 16) & 0xFF, s = (info >> 8) & 0xFF;
        if ((info >> 24) != 0xFF || s >= SECTORS || tnum >= CYLINDERS * 2) continue;
        uint32_t data = a + 8 + 32 + 8 + 8;
        if (data + 1024 > end) break;
        uint8_t sec[512];
        for (int i = 0; i < 128; i++) {
            uint32_t v = ((chip32(data + i * 4) & 0x55555555u) << 1) |
                         (chip32(data + 512 + i * 4) & 0x55555555u);
            sec[i * 4] = (uint8_t)(v >> 24); sec[i * 4 + 1] = (uint8_t)(v >> 16);
            sec[i * 4 + 2] = (uint8_t)(v >> 8); sec[i * 4 + 3] = (uint8_t)v;
        }
        drv[d].disk.write(drv[d].disk.ctx, ((uint32_t)tnum * SECTORS + (uint32_t)s) * 512u, sec, 512);
        a = data + 1024 - 2;
    }
    trk_drive = -1;   // re-encode from the image next time
}

static void put_in(int d, const amiga_disk_t *disk);

// ---- DMA ----------------------------------------------------------------------
static void dma_start_(void);
static void dma_start(void) {
    const int prof = prof_switch(AMIGA_PROF_DISK);
    dma_start_();
    prof_switch(prof);
}

static void dma_start_(void) {
    uint32_t words = dsklen & 0x3FFF;
    dma_drive = sel;
    if (sel >= 0) last_drive = sel;
    dma_write = (dsklen & 0x4000) != 0;
    dma_on = true;
    dma_lines_left = words / (2 * FLOPPY_TURBO) + 1;
    if (sel < 0 || !drv[sel].inserted || !drv[sel].motor) {
        return;   // nothing spinning: the transfer never completes, as on hardware
    }
    if (dma_write) return;              // data decoded at completion
    ensure_track(sel);
    uint32_t pos = rot_pos % TRACK_WORDS;
    if (A.adkcon & 0x0400) {            // WORDSYNC: start after the next sync word
        uint32_t i;
        for (i = 0; i < TRACK_WORDS; i++)
            if (track[(pos + i) % TRACK_WORDS] == dsksync) break;
        if (i == TRACK_WORDS) { return; }   // no sync on this track: hang like a real drive
        pos = (pos + i + 1) % TRACK_WORDS;
        amiga_set_intreq(INTF_DSKSYNC);
    }
    for (uint32_t n = 0; n < words; n++) {
        chip_ww(dskpt + n * 2, track[pos]);
        if (++pos == TRACK_WORDS) pos = 0;
    }
    rot_pos = pos;
}

static void dma_finish(void) {
    uint32_t words = dsklen & 0x3FFF;
    if (dma_write && dma_drive >= 0) decode_write(dma_drive, dskpt, words);
    dskpt += words * 2;
    dma_on = false;
    dma_end_frame = A.frame;
    amiga_set_intreq(INTF_DSKBLK);
}

void floppy_write(uint32_t reg, uint16_t v) {
    switch (reg) {
    case R_DSKPTH: dskpt = (dskpt & 0x0000FFFF) | ((uint32_t)(v & 0x1F) << 16); break;
    case R_DSKPTL: dskpt = (dskpt & 0xFFFF0000) | (v & 0xFFFE); break;
    case R_DSKSYNC: dsksync = v; break;
    case R_DSKLEN:
        dsklen_prev = dsklen;
        dsklen = v;
        if (!(v & 0x8000)) { if (dma_on) dma_end_frame = A.frame; dma_on = false; break; }
        if ((dsklen_prev & 0x8000) && (A.dmacon & (DMAF_MASTER | DMAF_DISK)) == (DMAF_MASTER | DMAF_DISK))
            dma_start();
        break;
    default: break;
    }
}

uint16_t floppy_read(uint32_t reg) {
    if (reg == R_DSKBYTR)
        return (uint16_t)((dma_on ? 0x4000 : 0) | (dma_write ? 0x2000 : 0));
    return 0;
}

void floppy_line(void) {
    for (int d = 0; d < NDRIVES; d++) {
        drive_t *dv = &drv[d];
        if (dv->inserted || dv->empty_lines >= SWAP_LINES) continue;
        if (++dv->empty_lines == SWAP_LINES && dv->pending) put_in(d, &dv->next);
    }
    // 32 us per word, 64 us per line.  Not while a read DMA is running: its words
    // were handed over at the start and rot_pos is already at their end, where the
    // head is when the transfer completes.  Turning on meanwhile (the wait is
    // FLOPPY_TURBO-scaled, the turning was not) carried the head past the gap:
    // Hero Quest's loader reads a sector header, then "the sectors up to the gap",
    // then "the next sector" - expected to be sector 0 - and kept getting 2.
    if (!(dma_on && !dma_write)) rot_pos += 2;
    if (dma_on && dma_drive >= 0 && drv[dma_drive].inserted && drv[dma_drive].motor) {
        if (dma_lines_left && --dma_lines_left == 0) dma_finish();
    }
    // One index pulse per revolution on CIA-B /FLAG while a selected drive spins
    // (FLOPPY_TURBO times as often during a transfer, as the transfer itself).
    if (sel >= 0 && drv[sel].motor && drv[sel].inserted) {
        index_lines += dma_on ? 2 * FLOPPY_TURBO : 2;
        if (index_lines >= TRACK_WORDS) { index_lines -= TRACK_WORDS; cia_flag(1); }
    }
}

// ---- drive mechanics (CIA-B port B) --------------------------------------------
void floppy_cia_prb(uint8_t prb) {
    uint8_t fall = prb_last & ~prb;
    for (int d = 0; d < NDRIVES; d++) {
        uint8_t sbit = (uint8_t)(0x08 << d);
        if (fall & sbit) drv[d].motor = !(prb & 0x80);   // /MTR latched on select
    }
    side = (prb & 0x04) ? 0 : 1;
    sel = -1;
    for (int d = 0; d < NDRIVES; d++)
        if (!(prb & (0x08 << d))) { sel = d; break; }
    if ((fall & 0x01) && sel >= 0) {               // /STEP pulse
        drive_t *dv = &drv[sel];
        if (prb & 0x02) { if (dv->cyl > 0) dv->cyl--; }            // DIR=1: outward
        else            { if (dv->cyl < CYLINDERS + 3) dv->cyl++; } // inward
        if (dv->inserted) dv->changed = false;
    }
    prb_last = prb;
}

void floppy_status(amiga_floppy_t *st) {
    const int d = sel >= 0 ? sel : last_drive;
    st->drive = (int8_t)d;
    st->cyl = d >= 0 ? drv[d].cyl : 0;
    st->side = (uint8_t)side;
    st->sector = (uint8_t)(rot_pos % TRACK_WORDS / SECTOR_WORDS);   // 11: the gap
    st->motor = d >= 0 && drv[d].motor;
    st->dma = dma_on ? (dma_write ? 2 : 1) : 0;
    st->wrote = dma_write;
    st->idle = dma_on ? 0 : A.frame - dma_end_frame + 1;
}

void floppy_set_drives(int n) {
    ndrives = n < 1 ? 1 : n > NDRIVES ? NDRIVES : n;
}

uint8_t floppy_cia_pra(void) {
    uint8_t v = 0x3C;                          // /RDY /TK0 /WPRO /CHNG all high
    if (sel < 0 || sel >= ndrives) return v;   // nothing on that select line
    const drive_t *dv = &drv[sel];
    if (!dv->motor || dv->inserted) v &= ~0x20;   // motor off: the ID bit (1); on: ready
    if (dv->cyl == 0)              v &= ~0x10;
    if (dv->inserted && !dv->disk.write) v &= ~0x08;
    if (dv->changed)               v &= ~0x04;
    return v;
}

static void put_in(int d, const amiga_disk_t *disk) {
    drv[d].disk = *disk;
    drv[d].inserted = true;
    drv[d].changed = true;
    drv[d].pending = false;
    if (trk_drive == d) trk_drive = -1;
}

void floppy_eject(int d) {
    if (d < 0 || d >= NDRIVES) return;
    if (drv[d].inserted) drv[d].empty_lines = 0;
    drv[d].inserted = false;
    drv[d].pending = false;
    drv[d].changed = true;
    if (trk_drive == d) trk_drive = -1;
}

void floppy_insert(int d, const amiga_disk_t *disk) {
    if (d < 0 || d >= NDRIVES || !disk) return;
    if (drv[d].inserted) floppy_eject(d);
    if (drv[d].empty_lines >= SWAP_LINES) { put_in(d, disk); return; }
    drv[d].next = *disk;                      // a swap: out first, long enough to be seen
    drv[d].pending = true;
}

void floppy_reset(void) {
    for (int d = 0; d < NDRIVES; d++) {
        drv[d].motor = false;
        drv[d].cyl = 0;
        drv[d].changed = true;
        // Kickstart starts from scratch: a disk on its way in is in, an empty
        // drive has been empty long enough.
        if (drv[d].pending) put_in(d, &drv[d].next);
        if (!drv[d].inserted) drv[d].empty_lines = SWAP_LINES;
    }
    prb_last = 0xFF; sel = -1; side = 0;
    dma_on = false; dsklen = dsklen_prev = 0; dsksync = 0x4489;
    dma_end_frame = A.frame - 1000;             // no recent activity
    trk_drive = -1;
}
