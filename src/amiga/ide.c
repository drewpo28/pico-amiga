// An ATA hard disk on Gayle's IDE port (the A600's), from an HDF image.
// After WinUAE's ide.cpp: one master drive, PIO, CHS and LBA28, no multiple
// mode, no LBA48.  The data moves a sector at a time through one 512-byte
// buffer (the platform's read/write callbacks per sector: no big buffer in
// SRAM); between sectors the drive is busy for a line or two and interrupts,
// as WinUAE's does (ide_interrupt: 2 lines, ide_fast_interrupt: 1).
//
// The image is either a whole drive (an RDB within its first 16 blocks:
// Kickstart's scsi.device mounts its partitions, a filesystem in it included)
// or one bare partition (WinUAE's partition HDF: a "DOS" boot block at block 0).
// A bare one gets a virtual RDB in front of it, as WinUAE does for a partition
// HDF on an IDE controller: an RDSK block, a PART block (DH0, bootable, the
// filesystem in ROM from the boot block's DOS type, WinUAE's default geometry
// of 32 blocks per track and one surface, 2 reserved blocks), the image from
// block 32 on.  Writes to the virtual blocks are dropped.
//
// The geometry IDENTIFY reports: an RDB's own when it is a valid CHS one,
// otherwise 16 heads x 63 sectors and the cylinders to cover the image (the
// blocks past its end read as zeros).
#include <string.h>
#include "amiga_int.h"

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DSC  0x10
#define ST_DRQ  0x08
#define ST_ERR  0x01
#define ER_ABRT 0x04
#define ER_IDNF 0x10

#define VRDB_BLOCKS 32u            // the virtual RDB's cylinder (1 surface x 32 blocks)

static amiga_disk_t disk;          // size 0: no drive
static uint32_t img_blocks;        // whole 512-byte blocks in the image
static uint32_t voff;              // virtual blocks before the image (0 or VRDB_BLOCKS)
static uint32_t dos_type;          // the bare partition's, for the virtual PART block
static uint16_t cyls_def;
static uint8_t  heads_def, spt_def;

// The task file and the transfer (snapshots).
static uint8_t  r_err, r_feat, r_nsec, r_sec, r_lcyl, r_hcyl, r_select, r_status, r_devcon;
static uint8_t  heads, spt;        // the current translation (INITIALIZE DEVICE PARAMETERS)
static uint8_t  irq_delay;         // lines until the command's interrupt
static bool     irq, intdrq;
static uint8_t  dir;               // 0 read, 1 write
static uint16_t data_pos, data_len;// bytes into buf / bytes in this sector (0: no transfer)
static uint32_t lba_next;          // the next sector of the transfer
static uint16_t left;              // sectors of the transfer still to move after buf's
static uint8_t  buf[512];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
// IDENTIFY's block as a PC holds it (little-endian words, a string's first
// character in a word's high byte): the data port hands bytes over in order,
// as sector data, so the 68000 sees its numbers byte-swapped and scsi.device
// swaps them back (Gayle's wiring; WinUAE's pw / ps without byteswap).
static void put16(uint8_t *p, int word, uint16_t v) { p[word * 2] = (uint8_t)v; p[word * 2 + 1] = (uint8_t)(v >> 8); }
static void puts_ata(uint8_t *p, int word, const char *s, int len) {   // space-padded
    for (int i = 0; i < len; i++) p[(word * 2 + i) ^ 1] = *s ? (uint8_t)*s++ : ' ';
}

static bool present(void) { return disk.size != 0; }
static uint32_t total_blocks(void) { return (uint32_t)cyls_def * heads_def * spt_def; }

// ---- the virtual RDB --------------------------------------------------------------
static void rdb_sum(uint8_t *b) {               // longs 0..63 add up to 0
    put32(b + 8, 0);
    uint32_t s = 0;
    for (int i = 0; i < 64; i++) s += be32(b + 4 * i);
    put32(b + 8, (uint32_t)-s);
}

static void vrdb_block(uint32_t n, uint8_t *b) {
    memset(b, 0, 512);
    const uint32_t cyls = (img_blocks + VRDB_BLOCKS - 1) / VRDB_BLOCKS + 1;
    if (n == 0) {
        memcpy(b, "RDSK", 4);
        put32(b + 4, 64);
        put32(b + 12, 7);                       // host ID
        put32(b + 16, 512);
        put32(b + 20, 0x02);                    // RDBFF_LASTLUN
        put32(b + 24, 0xFFFFFFFF);              // no bad blocks
        put32(b + 28, 1);                       // the partition list: block 1
        put32(b + 32, 0xFFFFFFFF);              // no filesystems (the ROM's)
        put32(b + 36, 0xFFFFFFFF);              // no drive init
        for (int i = 0; i < 6; i++) put32(b + 40 + 4 * i, 0xFFFFFFFF);
        put32(b + 64, cyls);                    // the RDB's geometry: 1 x 32
        put32(b + 68, VRDB_BLOCKS);
        put32(b + 72, 1);
        put32(b + 76, 1);                       // interleave
        put32(b + 80, cyls);                    // park
        put32(b + 96, cyls); put32(b + 100, cyls);   // write precomp, reduced write
        put32(b + 104, 3);                      // step rate
        put32(b + 128, 0); put32(b + 132, VRDB_BLOCKS - 1);  // the RDB's blocks
        put32(b + 136, 1); put32(b + 140, cyls - 1);         // partitionable cylinders
        put32(b + 144, VRDB_BLOCKS);
        put32(b + 152, 1);                      // highest RDB block in use
        memcpy(b + 160, "PICOAMIG", 8);
        memcpy(b + 168, "HDF             ", 16);
        memcpy(b + 184, "1.0 ", 4);
        rdb_sum(b);
    } else if (n == 1) {
        memcpy(b, "PART", 4);
        put32(b + 4, 64);
        put32(b + 12, 7);
        put32(b + 16, 0xFFFFFFFF);              // the last partition
        put32(b + 20, 1);                       // PBFF_BOOTABLE
        b[36] = 3; memcpy(b + 37, "DH0", 3);    // BSTR
        uint8_t *e = b + 128;                   // DosEnvec
        put32(e + 0, 16);                       // de_TableSize
        put32(e + 4, 128);                      // de_SizeBlock (longs)
        put32(e + 12, 1);                       // de_Surfaces
        put32(e + 16, 1);                       // de_SectorPerBlock
        put32(e + 20, VRDB_BLOCKS);             // de_BlocksPerTrack
        put32(e + 24, 2);                       // de_Reserved
        put32(e + 36, 1);                       // de_LowCyl
        put32(e + 40, img_blocks / VRDB_BLOCKS);  // de_HighCyl: whole cylinders of the image
        put32(e + 44, 30);                      // de_NumBuffers
        put32(e + 52, 0x1FE00);                 // de_MaxTransfer
        put32(e + 56, 0x7FFFFFFE);              // de_Mask
        put32(e + 60, 0);                       // de_BootPri
        put32(e + 64, dos_type);                // de_DosType
        rdb_sum(b);
    }
}

// ---- blocks ------------------------------------------------------------------------
static bool read_block(uint32_t lba, uint8_t *b) {
    if (lba < voff) { vrdb_block(lba, b); return true; }
    lba -= voff;
    if (lba >= img_blocks) { memset(b, 0, 512); return true; }
    return disk.read(disk.ctx, lba * 512u, b, 512) == 0;
}

static bool write_block(uint32_t lba, const uint8_t *b) {
    if (lba < voff) return true;
    lba -= voff;
    if (lba >= img_blocks) return true;
    return disk.write && disk.write(disk.ctx, lba * 512u, b, 512) == 0;
}

// ---- insert --------------------------------------------------------------------------
static void chs_cover(uint32_t blocks) {
    heads_def = 16; spt_def = 63;
    uint32_t c = (blocks + 16u * 63 - 1) / (16u * 63);
    cyls_def = (uint16_t)(c < 1 ? 1 : c > 65535 ? 65535 : c);
}

void ide_insert(const amiga_disk_t *d) {
    memset(&disk, 0, sizeof disk);
    img_blocks = voff = dos_type = 0;
    if (!d || d->size < 512 * 2) return;
    disk = *d;
    img_blocks = d->size / 512;
    uint8_t *b = buf;
    bool rdb = false;
    for (uint32_t n = 0; n < 16 && n < img_blocks && !rdb; n++) {
        if (disk.read(disk.ctx, n * 512u, b, 512) != 0) break;
        if (!memcmp(b, "RDSK", 4)) {
            rdb = true;
            const uint32_t c = be32(b + 64), s = be32(b + 68), h = be32(b + 72);
            if (c >= 1 && c <= 65535 && s >= 1 && s <= 255 && h >= 1 && h <= 16) {
                cyls_def = (uint16_t)c; spt_def = (uint8_t)s; heads_def = (uint8_t)h;
            } else {
                chs_cover(img_blocks);
            }
        }
    }
    if (!rdb) {
        if (disk.read(disk.ctx, 0, b, 512) == 0 && !memcmp(b, "DOS", 3) &&
            img_blocks >= VRDB_BLOCKS * 2) {
            voff = VRDB_BLOCKS;
            dos_type = be32(b);
        }
        chs_cover(img_blocks + voff);
    }
    memset(buf, 0, sizeof buf);
}

bool ide_present(void) { return present(); }

// ---- commands ----------------------------------------------------------------------
static void signature(void) {
    r_nsec = 1; r_sec = 1; r_lcyl = r_hcyl = 0;
    r_status = 0;
    r_err = 0x01;                                // diagnostics passed
}

void ide_reset(void) {
    r_feat = r_select = r_devcon = 0;
    signature();
    heads = heads_def; spt = spt_def;
    irq_delay = 0; irq = intdrq = false;
    dir = 0; data_pos = data_len = 0; lba_next = 0; left = 0;
}

static void arm(int lines) {                     // busy, then the interrupt
    r_status = (uint8_t)((r_status | ST_BSY) & ~ST_DRQ);
    irq_delay = (uint8_t)lines;
}

static void fail(uint8_t err) {
    r_err |= err;
    r_status |= ST_ERR;
    intdrq = false;
    data_len = 0;
    arm(2);
}

static bool get_lba(uint32_t *lba) {
    if (r_select & 0x40) {
        *lba = (uint32_t)(r_select & 15) << 24 | (uint32_t)r_hcyl << 16 | (uint32_t)r_lcyl << 8 | r_sec;
    } else {
        const uint32_t c = (uint32_t)r_hcyl << 8 | r_lcyl, h = r_select & 15;
        if (!r_sec || r_sec > spt || h >= heads) return false;
        *lba = (c * heads + h) * spt + r_sec - 1u;
    }
    return *lba < total_blocks();
}

// The task file after a sector: the address of the next one (the last one's
// when the transfer is over), the count down by one.
static void put_lba(uint32_t lba) {
    if (r_select & 0x40) {
        r_select = (uint8_t)((r_select & ~15) | ((lba >> 24) & 15));
        r_hcyl = (uint8_t)(lba >> 16); r_lcyl = (uint8_t)(lba >> 8); r_sec = (uint8_t)lba;
    } else {
        const uint32_t s = lba % spt, t = lba / spt;
        r_sec = (uint8_t)(s + 1);
        r_select = (uint8_t)((r_select & ~15) | (t % heads));
        const uint32_t c = t / heads;
        r_hcyl = (uint8_t)(c >> 8); r_lcyl = (uint8_t)c;
    }
}

// Load the next sector of a read into buf (or open the next one of a write):
// DRQ once the interrupt comes.
static void next_sector(int lines) {
    const uint32_t lba = lba_next++;
    if (dir == 0 && !read_block(lba, buf)) { fail(ER_IDNF); return; }
    data_pos = 0; data_len = 512;
    r_nsec--;
    put_lba(left ? lba + 1 : lba);
    if (dir == 0) { intdrq = true; arm(lines); }
    else { r_status = (uint8_t)((r_status | ST_DRQ) & ~ST_BSY); }
}

static void rw(bool write) {
    uint32_t lba;
    if (!get_lba(&lba)) { fail(ER_IDNF); return; }
    uint32_t n = r_nsec ? r_nsec : 256;
    if (n > total_blocks() - lba) n = total_blocks() - lba;
    dir = write;
    lba_next = lba;
    left = (uint16_t)(n - 1);
    next_sector(1);                              // a write: DRQ at once, no interrupt
}

static void identify(void) {
    memset(buf, 0, 512);
    put16(buf, 0, 1 << 6);                       // fixed drive
    put16(buf, 1, cyls_def);
    put16(buf, 2, 0xC837);
    put16(buf, 3, heads_def);
    put16(buf, 4, (uint16_t)(512 * spt_def));
    put16(buf, 5, 512);
    put16(buf, 6, spt_def);
    puts_ata(buf, 10, "68000", 20);              // serial
    put16(buf, 20, 3); put16(buf, 21, 512 / 512); put16(buf, 22, 4);
    puts_ata(buf, 23, "0.7", 8);                 // firmware
    puts_ata(buf, 27, "PICO-AMIGA HDF", 40);     // model
    put16(buf, 48, 1);
    put16(buf, 49, 1 << 9);                      // LBA
    put16(buf, 51, 0x200); put16(buf, 52, 0x200);
    put16(buf, 53, 1);                           // 54-58 valid
    put16(buf, 54, cyls_def); put16(buf, 55, heads); put16(buf, 56, spt);
    const uint32_t t = total_blocks();
    put16(buf, 57, (uint16_t)t); put16(buf, 58, (uint16_t)(t >> 16));
    put16(buf, 60, (uint16_t)t); put16(buf, 61, (uint16_t)(t >> 16));
    dir = 0; left = 0;
    data_pos = 0; data_len = 512;
    intdrq = true;
    arm(2);
}

static void command(uint8_t cmd) {
    r_status &= (uint8_t)~(ST_DRDY | ST_DRQ | ST_ERR);
    r_err = 0;
    intdrq = false;
    irq_delay = 0;
    data_len = 0;
    switch (cmd) {
    case 0x10: r_sec = 0; r_lcyl = r_hcyl = 0; arm(2); break;     // recalibrate
    case 0xEC: identify(); break;
    case 0x90: signature(); arm(2); break;                        // diagnostics
    case 0x91:                                                    // initialize drive parameters
        spt = r_nsec ? r_nsec : 255;
        heads = (uint8_t)((r_select & 15) + 1);
        arm(2);
        break;
    case 0x20: case 0x21: rw(false); break;
    case 0x30: case 0x31: rw(true); break;
    case 0x40: case 0x41: {                                       // verify
        uint32_t lba;
        if (!get_lba(&lba)) fail(ER_IDNF); else arm(2);
        break;
    }
    case 0x70: case 0xE0: case 0xE1: case 0xE7: arm(2); break;    // seek, standby, idle, flush
    case 0xE5: r_nsec = 0xFF; arm(2); break;                      // check power mode
    case 0xEF:                                                    // set features: write cache only
        if (r_feat == 0x02 || r_feat == 0x82) arm(2); else fail(ER_ABRT);
        break;
    default: fail(ER_ABRT); break;
    }
}

// ---- the registers -------------------------------------------------------------------
// reg: 0-7 the task file, 0x406 / 0x407 the control block (alternate status /
// device control, drive address).
uint8_t ide_read(int reg) {
    if (!present()) return reg == 7 ? 0x7F : 0xFF;
    if (r_select & 0x10) {                       // the slave: none
        if (reg == 7 || reg == 0x406) { irq = false; return 0x01; }
        return 0;
    }
    if ((r_status & ST_BSY) && reg != 0x406) reg = 7;      // busy: every register reads the status
    switch (reg) {
    case 1: return r_err;
    case 2: return r_nsec;
    case 3: return r_sec;
    case 4: return r_lcyl;
    case 5: return r_hcyl;
    case 6: return r_select;
    case 7: irq = false; /* fall through */
    case 0x406: return (uint8_t)(r_status | ST_DRDY | ST_DSC);
    case 0x407: return (uint8_t)~(1 | ((r_select & 15) << 2));
    default: return reg & 0x400 ? 0xFF : 0;
    }
}

void ide_write(int reg, uint8_t v) {
    if (!present()) return;
    switch (reg) {
    case 1: r_feat = v; break;
    case 2: r_nsec = v; break;
    case 3: r_sec = v; break;
    case 4: r_lcyl = v; break;
    case 5: r_hcyl = v; break;
    case 6: r_select = v; break;
    case 7:
        irq = false;
        if (!(r_select & 0x10)) { r_status |= ST_BSY; command(v); }
        break;
    case 0x406:
        if (!(r_devcon & 4) && (v & 4)) { signature(); data_len = 0; irq_delay = 0; }   // SRST
        r_devcon = v;
        break;
    default: break;
    }
}

uint16_t ide_read_data(void) {
    if (!present() || (r_select & 0x10) || !data_len || (r_status & ST_BSY)) return present() ? 0 : 0xFFFF;
    const uint16_t v = (uint16_t)(buf[data_pos] << 8 | buf[data_pos + 1]);
    data_pos += 2;
    if (data_pos >= data_len) {
        data_len = 0;
        r_status &= (uint8_t)~ST_DRQ;
        if (left) { left--; next_sector(2); }
    }
    return v;
}

void ide_write_data(uint16_t v) {
    if (!present() || (r_select & 0x10) || !data_len || dir != 1) return;
    buf[data_pos] = (uint8_t)(v >> 8); buf[data_pos + 1] = (uint8_t)v;
    data_pos += 2;
    if (data_pos < data_len) return;
    data_len = 0;
    if (!write_block(lba_next - 1, buf)) { fail(ER_IDNF); return; }
    if (left) {                                   // the next sector: busy, then DRQ + interrupt
        left--;
        next_sector(2);
        intdrq = true;
        arm(2);
    } else {
        intdrq = false;
        arm(1);
    }
}

// Once a line: the pending interrupt.  True while the drive's INTRQ is up.
bool ide_line(void) {
    if (irq_delay && !--irq_delay) {
        r_status = (uint8_t)(r_status & ~(ST_BSY | ST_DRQ));
        if (intdrq) r_status |= ST_DRQ;
        intdrq = false;
        if (!(r_devcon & 2)) irq = true;
    }
    return irq && !(r_devcon & 2);
}

bool ide_irq(void) { return irq && !(r_devcon & 2); }

// Snapshots: the task file and the transfer.  The drive (the image and its
// geometry) is the platform's, put back before the load as the floppies are.
void ide_state(st_t *s) {
    st_u8(s, &r_err); st_u8(s, &r_feat); st_u8(s, &r_nsec); st_u8(s, &r_sec);
    st_u8(s, &r_lcyl); st_u8(s, &r_hcyl); st_u8(s, &r_select); st_u8(s, &r_status); st_u8(s, &r_devcon);
    st_u8(s, &heads); st_u8(s, &spt); st_u8(s, &irq_delay);
    st_bool(s, &irq); st_bool(s, &intdrq); st_u8(s, &dir);
    st_u16(s, &data_pos); st_u16(s, &data_len); st_u32(s, &lba_next); st_u16(s, &left);
    st_raw(s, buf, sizeof buf);
    if (s->mode == ST_LOAD && (data_pos > 512 || data_len > 512)) data_pos = data_len = 0;
}
