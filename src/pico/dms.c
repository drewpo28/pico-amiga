// DMS (DiskMasher) archives: a port of xDMS 1.3 (public domain; Andre Rodrigues
// de la Rocha, Heikki Orsila), with its globals and buffers gathered in one work
// area the caller places (the firmware's is in PSRAM).  All the modes are here:
// NOCOMP, SIMPLE (RLE), QUICK, MEDIUM, DEEP (LZ + dynamic Huffman), HEAVY1/2
// (LZH, 4 / 8 KB dictionary), each but HEAVY without its RLE flag followed by RLE.
// The decrunchers keep their state from track to track unless a track's flag
// bit 0 is clear.  Tracks go to their cylinder's place in the ADF (xDMS writes
// them one after another); the banner (-1), FILE_ID.DIZ (80) and the fake boot
// block (a 1 KB track 0) are skipped without unpacking, as xDMS does.
#include "dms.h"
#include <stdbool.h>
#include <string.h>

#define HEADLEN 56
#define THLEN 20
#define TRACK_MAX 32000
#define TRACK_ADF 11264u                     // 2 x 11 x 512

// Heavy (LZH)
#define NC 510
#define NPT 20
#define N1 510
#define OFFSET 253
// Deep (LZHUF)
#define F 60
#define THRESHOLD 2
#define N_CHAR (256 - THRESHOLD + F)
#define T (N_CHAR * 2 - 1)
#define R (T - 1)
#define MAX_FREQ 0x8000

typedef struct {
    uint8_t b1[TRACK_MAX + 64];              // packed track
    uint8_t b2[TRACK_MAX + 512];             // after LZ: a match may run past the end
    uint8_t text[0x4000];                    // the dictionary (Deep and Medium: 16 KB)
    uint8_t d_code[256], d_len[256];
    // bit reader
    const uint8_t *in, *in_end;
    uint32_t bitbuf;
    uint8_t bitcount;
    // decruncher state, kept across tracks
    uint16_t quick_loc, medium_loc, heavy_loc, deep_loc;
    bool deep_fresh;
    // Heavy
    uint16_t left[2 * NC - 1], right[2 * NC - 1 + 9];
    uint8_t c_len[NC], pt_len[NPT];
    uint16_t c_table[4096], pt_table[256];
    uint16_t lastlen, np;
    // make_table
    int16_t mt_c;
    uint16_t mt_n, mt_tblsiz, mt_len, mt_depth, mt_maxdepth, mt_avail, mt_codeword, mt_bit, mt_err;
    uint16_t *mt_tbl;
    const uint8_t *mt_blen;
    // Deep
    uint16_t freq[T + 1], prnt[T + N_CHAR], son[T];
} dms_t;

uint32_t dms_work_size(void) { return (uint32_t)sizeof(dms_t); }

const char *dms_error(int err) {
    switch (err) {
    case DMS_OK:              return "ok";
    case DMS_ERR_READ:        return "read error";
    case DMS_ERR_NOT_DMS:     return "not a DMS archive";
    case DMS_ERR_UNSUPPORTED: return "unsupported DMS (encrypted, HD or FMS)";
    default:                  return "bad DMS data";
    }
}

// ---- CRC, checksum ------------------------------------------------------------------
static uint16_t crc16(const uint8_t *p, uint32_t n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint16_t checksum(const uint8_t *p, uint32_t n) {
    uint16_t s = 0;
    while (n--) s = (uint16_t)(s + *p++);
    return s;
}

// ---- bits -------------------------------------------------------------------------
// Past the packed data the reader feeds zeros (a corrupt track cannot walk off).
static inline uint8_t next_byte(dms_t *d) { return d->in < d->in_end ? *d->in++ : 0; }

static inline uint16_t getbits(dms_t *d, unsigned n) { return (uint16_t)(d->bitbuf >> (d->bitcount - n)); }

static inline void dropbits(dms_t *d, unsigned n) {
    if (n > d->bitcount) n = d->bitcount;               // a bad code length (corrupt track)
    d->bitcount = (uint8_t)(d->bitcount - n);
    d->bitbuf &= (1u << d->bitcount) - 1;
    while (d->bitcount < 16) {
        d->bitbuf = (d->bitbuf << 8) | next_byte(d);
        d->bitcount += 8;
    }
}

static void initbits(dms_t *d, const uint8_t *in, uint32_t len) {
    d->in = in;
    d->in_end = in + len;
    d->bitbuf = 0;
    d->bitcount = 0;
    dropbits(d, 0);
}

static void init_decrunchers(dms_t *d) {
    d->quick_loc = 251;
    d->medium_loc = 0x3fbe;
    d->heavy_loc = 0;
    d->deep_loc = 0x3fc4;
    d->deep_fresh = true;
    memset(d->text, 0, 0x3fc8);
}

// LZHUF's position tables: the upper 6 bits of a position and its code length.
static void init_tables(dms_t *d) {
    static const uint8_t runs[6][2] = { {1, 32}, {3, 16}, {8, 8}, {12, 4}, {24, 2}, {16, 1} };
    static const uint8_t lens[6][2] = { {3, 32}, {4, 48}, {5, 64}, {6, 48}, {7, 48}, {8, 16} };
    int i = 0, code = 0;
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < runs[r][0]; c++, code++)
            for (int k = 0; k < runs[r][1]; k++) d->d_code[i++] = (uint8_t)code;
    i = 0;
    for (int r = 0; r < 6; r++)
        for (int k = 0; k < lens[r][1]; k++) d->d_len[i++] = lens[r][0];
}

// ---- RLE ------------------------------------------------------------------------
static int unpack_rle(const uint8_t *in, const uint8_t *in_end, uint8_t *out, uint32_t size) {
    uint8_t *end = out + size;
    while (out < end) {
        if (in >= in_end) return 1;
        uint8_t a = *in++, b;
        if (a != 0x90) { *out++ = a; continue; }        // 90 00 = a literal 90
        if (in >= in_end) return 1;
        if (!(b = *in++)) { *out++ = a; continue; }
        if (in + (b == 0xff ? 3 : 1) > in_end) return 1;
        a = *in++;                                      // 90 n a, 90 ff hi lo a
        uint32_t n = b;
        if (b == 0xff) { n = (uint32_t)in[0] << 8 | in[1]; in += 2; }
        if (out + n > end) return 1;
        memset(out, a, n);
        out += n;
    }
    return 0;
}

// ---- Quick, Medium ----------------------------------------------------------------
static void unpack_quick(dms_t *d, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t size) {
    uint8_t *end = out + size;
    initbits(d, in, inlen);
    while (out < end) {
        if (getbits(d, 1)) {
            dropbits(d, 1);
            *out++ = d->text[d->quick_loc++ & 0xff] = (uint8_t)getbits(d, 8);
            dropbits(d, 8);
        } else {
            dropbits(d, 1);
            uint16_t j = (uint16_t)(getbits(d, 2) + 2); dropbits(d, 2);
            uint16_t i = (uint16_t)(d->quick_loc - getbits(d, 8) - 1); dropbits(d, 8);
            while (j--) *out++ = d->text[d->quick_loc++ & 0xff] = d->text[i++ & 0xff];
        }
    }
    d->quick_loc = (uint16_t)((d->quick_loc + 5) & 0xff);
}

static void unpack_medium(dms_t *d, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t size) {
    uint8_t *end = out + size;
    initbits(d, in, inlen);
    while (out < end) {
        if (getbits(d, 1)) {
            dropbits(d, 1);
            *out++ = d->text[d->medium_loc++ & 0x3fff] = (uint8_t)getbits(d, 8);
            dropbits(d, 8);
        } else {
            dropbits(d, 1);
            uint16_t c = getbits(d, 8); dropbits(d, 8);
            uint16_t j = (uint16_t)(d->d_code[c] + 3);
            uint8_t u = d->d_len[c];
            c = (uint16_t)(((c << u) | getbits(d, u)) & 0xff); dropbits(d, u);
            u = d->d_len[c];
            c = (uint16_t)((d->d_code[c] << 8) | (((c << u) | getbits(d, u)) & 0xff)); dropbits(d, u);
            uint16_t i = (uint16_t)(d->medium_loc - c - 1);
            while (j--) *out++ = d->text[d->medium_loc++ & 0x3fff] = d->text[i++ & 0x3fff];
        }
    }
    d->medium_loc = (uint16_t)((d->medium_loc + 66) & 0x3fff);
}

// ---- Deep (LZHUF, adaptive Huffman) -------------------------------------------------
static void deep_init(dms_t *d) {
    uint16_t i, j;
    for (i = 0; i < N_CHAR; i++) {
        d->freq[i] = 1;
        d->son[i] = (uint16_t)(i + T);
        d->prnt[i + T] = i;
    }
    for (i = 0, j = N_CHAR; j <= R; i += 2, j++) {
        d->freq[j] = (uint16_t)(d->freq[i] + d->freq[i + 1]);
        d->son[j] = i;
        d->prnt[i] = d->prnt[i + 1] = j;
    }
    d->freq[T] = 0xffff;
    d->prnt[R] = 0;
    d->deep_fresh = false;
}

static void deep_reconst(dms_t *d) {
    uint16_t i, j, k, f, l;
    for (i = 0, j = 0; i < T; i++)
        if (d->son[i] >= T) {
            d->freq[j] = (uint16_t)((d->freq[i] + 1) / 2);
            d->son[j] = d->son[i];
            j++;
        }
    for (i = 0, j = N_CHAR; j < T; i += 2, j++) {
        k = (uint16_t)(i + 1);
        f = d->freq[j] = (uint16_t)(d->freq[i] + d->freq[k]);
        for (k = (uint16_t)(j - 1); f < d->freq[k]; k--) ;
        k++;
        l = (uint16_t)((j - k) * 2);
        memmove(&d->freq[k + 1], &d->freq[k], l);
        d->freq[k] = f;
        memmove(&d->son[k + 1], &d->son[k], l);
        d->son[k] = i;
    }
    for (i = 0; i < T; i++) {
        if ((k = d->son[i]) >= T) d->prnt[k] = i;
        else d->prnt[k] = d->prnt[k + 1] = i;
    }
}

static void deep_update(dms_t *d, uint16_t c) {
    uint16_t i, j, k, l;
    if (d->freq[R] == MAX_FREQ) deep_reconst(d);
    c = d->prnt[c + T];
    do {
        k = ++d->freq[c];
        if (k > d->freq[l = (uint16_t)(c + 1)]) {
            while (k > d->freq[++l]) ;
            l--;
            d->freq[c] = d->freq[l];
            d->freq[l] = k;
            i = d->son[c];
            d->prnt[i] = l;
            if (i < T) d->prnt[i + 1] = l;
            j = d->son[l];
            d->son[l] = i;
            d->prnt[j] = c;
            if (j < T) d->prnt[j + 1] = c;
            d->son[c] = j;
            c = l;
        }
    } while ((c = d->prnt[c]) != 0);
}

static void unpack_deep(dms_t *d, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t size) {
    uint8_t *end = out + size;
    initbits(d, in, inlen);
    if (d->deep_fresh) deep_init(d);
    while (out < end) {
        uint16_t c = d->son[R];
        while (c < T) {
            c = d->son[c + getbits(d, 1)];
            dropbits(d, 1);
        }
        c -= T;
        deep_update(d, c);
        if (c < 256) {
            *out++ = d->text[d->deep_loc++ & 0x3fff] = (uint8_t)c;
        } else {
            uint16_t j = (uint16_t)(c - 255 + THRESHOLD);
            uint16_t p = getbits(d, 8); dropbits(d, 8);
            uint16_t hi = (uint16_t)(d->d_code[p] << 8);
            uint8_t u = d->d_len[p];
            p = (uint16_t)(((p << u) | getbits(d, u)) & 0xff); dropbits(d, u);
            uint16_t i = (uint16_t)(d->deep_loc - (hi | p) - 1);
            while (j--) *out++ = d->text[d->deep_loc++ & 0x3fff] = d->text[i++ & 0x3fff];
        }
    }
    d->deep_loc = (uint16_t)((d->deep_loc + 60) & 0x3fff);
}

// ---- Heavy (LZH, static Huffman per track) -----------------------------------------
// UNIX LHA's make_table (Masaru Oki), as xDMS has it.
static uint16_t mktbl(dms_t *d) {
    uint16_t i = 0;
    if (d->mt_err) return 0;
    if (d->mt_len == d->mt_depth) {
        while (++d->mt_c < d->mt_n)
            if (d->mt_blen[d->mt_c] == d->mt_len) {
                i = d->mt_codeword;
                d->mt_codeword += d->mt_bit;
                if (d->mt_codeword > d->mt_tblsiz) { d->mt_err = 1; return 0; }
                while (i < d->mt_codeword) d->mt_tbl[i++] = (uint16_t)d->mt_c;
                return (uint16_t)d->mt_c;
            }
        d->mt_c = -1;
        d->mt_len++;
        d->mt_bit >>= 1;
    }
    d->mt_depth++;
    if (d->mt_depth < d->mt_maxdepth) {
        mktbl(d);
        mktbl(d);
    } else if (d->mt_depth > 32) {
        d->mt_err = 2;
        return 0;
    } else {
        if ((i = d->mt_avail++) >= 2 * d->mt_n - 1) { d->mt_err = 3; return 0; }
        d->left[i] = mktbl(d);
        d->right[i] = mktbl(d);
        if (d->mt_codeword >= d->mt_tblsiz) { d->mt_err = 4; return 0; }
        if (d->mt_depth == d->mt_maxdepth) d->mt_tbl[d->mt_codeword++] = i;
    }
    d->mt_depth--;
    return i;
}

static int make_table(dms_t *d, uint16_t nchar, const uint8_t *bitlen, uint16_t tablebits, uint16_t *table) {
    d->mt_n = d->mt_avail = nchar;
    d->mt_blen = bitlen;
    d->mt_tbl = table;
    d->mt_tblsiz = (uint16_t)(1u << tablebits);
    d->mt_bit = (uint16_t)(d->mt_tblsiz / 2);
    d->mt_maxdepth = (uint16_t)(tablebits + 1);
    d->mt_depth = d->mt_len = 1;
    d->mt_c = -1;
    d->mt_codeword = 0;
    d->mt_err = 0;
    mktbl(d);
    if (d->mt_err) return d->mt_err;
    mktbl(d);
    if (d->mt_err) return d->mt_err;
    return d->mt_codeword != d->mt_tblsiz ? 5 : 0;
}

static int heavy_tree_c(dms_t *d) {
    uint16_t n = getbits(d, 9); dropbits(d, 9);
    if (n > NC) return 1;
    if (n > 0) {
        for (uint16_t i = 0; i < n; i++) { d->c_len[i] = (uint8_t)getbits(d, 5); dropbits(d, 5); }
        memset(d->c_len + n, 0, NC - n);
        return make_table(d, NC, d->c_len, 12, d->c_table);
    }
    n = getbits(d, 9); dropbits(d, 9);
    memset(d->c_len, 0, NC);
    for (int i = 0; i < 4096; i++) d->c_table[i] = n;
    return 0;
}

static int heavy_tree_p(dms_t *d) {
    uint16_t n = getbits(d, 5); dropbits(d, 5);
    if (n > d->np) return 1;
    if (n > 0) {
        for (uint16_t i = 0; i < n; i++) { d->pt_len[i] = (uint8_t)getbits(d, 4); dropbits(d, 4); }
        memset(d->pt_len + n, 0, d->np - n);
        return make_table(d, d->np, d->pt_len, 8, d->pt_table);
    }
    n = getbits(d, 5); dropbits(d, 5);
    memset(d->pt_len, 0, d->np);
    for (int i = 0; i < 256; i++) d->pt_table[i] = n;
    return 0;
}

static uint16_t heavy_c(dms_t *d) {
    uint16_t j = d->c_table[getbits(d, 12)];
    if (j < N1) {
        dropbits(d, d->c_len[j]);
    } else {
        dropbits(d, 12);
        uint16_t i = getbits(d, 16), m = 0x8000;
        do {
            j = (i & m) ? d->right[j] : d->left[j];
            m >>= 1;
        } while (j >= N1 && m);
        if (j >= N1) return j;                          // corrupt: the caller stops
        dropbits(d, d->c_len[j] - 12u);
    }
    return j;
}

static uint16_t heavy_p(dms_t *d) {
    uint16_t j = d->pt_table[getbits(d, 8)];
    if (j < d->np) {
        dropbits(d, d->pt_len[j]);
    } else {
        dropbits(d, 8);
        uint16_t i = getbits(d, 16), m = 0x8000;
        do {
            j = (i & m) ? d->right[j] : d->left[j];
            m >>= 1;
        } while (j >= d->np && m);
        if (j >= d->np) j = (uint16_t)(d->np - 1);     // corrupt: keep the last length
        dropbits(d, d->pt_len[j] - 8u);
    }
    if (j != d->np - 1) {
        if (j > 0) {
            const uint16_t n = (uint16_t)(j - 1);
            j = (uint16_t)(getbits(d, n) | (1u << n));
            dropbits(d, n);
        }
        d->lastlen = j;
    }
    return d->lastlen;
}

static int unpack_heavy(dms_t *d, const uint8_t *in, uint32_t inlen, uint8_t *out, uint8_t flags, uint32_t size) {
    uint8_t *end = out + size;
    uint16_t mask;
    if (flags & 8) { d->np = 15; mask = 0x1fff; }      // Heavy 2: 8 KB dictionary
    else           { d->np = 14; mask = 0x0fff; }      // Heavy 1: 4 KB
    initbits(d, in, inlen);
    if (flags & 2) {                                    // the track brings new trees
        if (heavy_tree_c(d)) return 1;
        if (heavy_tree_p(d)) return 1;
    }
    while (out < end) {
        uint16_t c = heavy_c(d);
        if (c >= N1) return 1;
        if (c < 256) {
            *out++ = d->text[d->heavy_loc++ & mask] = (uint8_t)c;
        } else {
            uint16_t j = (uint16_t)(c - OFFSET);
            uint16_t i = (uint16_t)(d->heavy_loc - heavy_p(d) - 1);
            while (j--) *out++ = d->text[d->heavy_loc++ & mask] = d->text[i++ & mask];
        }
    }
    return 0;
}

// ---- tracks -----------------------------------------------------------------------
// b1 holds pklen1 packed bytes; the track's `size` bytes go to `out`.
static int unpack_track(dms_t *d, uint32_t pklen1, uint32_t pklen2, uint8_t *out, uint32_t size,
                        uint8_t cmode, uint8_t flags) {
    const uint8_t *b1 = d->b1;
    uint8_t *b2 = d->b2;
    bool rle = true;
    switch (cmode) {
    case 0:
        if (size > pklen1) return 1;
        memcpy(out, b1, size);
        rle = false;
        break;
    case 1:
        if (unpack_rle(b1, b1 + pklen1, out, size)) return 1;
        rle = false;
        break;
    case 2: unpack_quick(d, b1, pklen1, b2, pklen2); break;
    case 3: unpack_medium(d, b1, pklen1, b2, pklen2); break;
    case 4: unpack_deep(d, b1, pklen1, b2, pklen2); break;
    case 5:
    case 6:
        if (unpack_heavy(d, b1, pklen1, b2, cmode == 5 ? (uint8_t)(flags & 7) : (uint8_t)(flags | 8), pklen2))
            return 1;
        rle = flags & 4;
        if (!rle) {
            if (size > pklen2) return 1;
            memcpy(out, b2, size);
        }
        break;
    default:
        return 1;
    }
    if (rle && cmode >= 2 && unpack_rle(b2, b2 + pklen2, out, size)) return 1;
    if (!(flags & 1)) init_decrunchers(d);
    return 0;
}

int dms_unpack(dms_read_fn rd, void *ctx, uint8_t *adf, void *work) {
    dms_t *d = (dms_t *)work;
    uint8_t *h = d->b1;
    if (rd(ctx, h, HEADLEN) != HEADLEN) return DMS_ERR_READ;
    if (memcmp(h, "DMS!", 4) || crc16(h + 4, HEADLEN - 6) != (uint16_t)(h[HEADLEN - 2] << 8 | h[HEADLEN - 1]))
        return DMS_ERR_NOT_DMS;
    const uint16_t geninfo = (uint16_t)(h[10] << 8 | h[11]);
    const uint16_t disktype = (uint16_t)(h[50] << 8 | h[51]);
    if ((geninfo & (2 | 16)) || disktype == 7) return DMS_ERR_UNSUPPORTED;   // encrypted, HD, FMS

    init_tables(d);
    init_decrunchers(d);
    memset(adf, 0, DMS_ADF_SIZE);
    for (;;) {
        uint8_t th[THLEN];
        // The archive ends where the track headers do: some carry text or zeros after.
        if (rd(ctx, th, THLEN) != THLEN || th[0] != 'T' || th[1] != 'R') return DMS_OK;
        if (crc16(th, THLEN - 2) != (uint16_t)(th[THLEN - 2] << 8 | th[THLEN - 1])) return DMS_ERR_DATA;
        const uint16_t number = (uint16_t)(th[2] << 8 | th[3]);
        const uint16_t pklen1 = (uint16_t)(th[6] << 8 | th[7]);
        const uint16_t pklen2 = (uint16_t)(th[8] << 8 | th[9]);
        const uint16_t unpklen = (uint16_t)(th[10] << 8 | th[11]);
        const uint8_t flags = th[12], cmode = th[13];
        const uint16_t usum = (uint16_t)(th[14] << 8 | th[15]);
        const uint16_t dcrc = (uint16_t)(th[16] << 8 | th[17]);
        if (pklen1 > TRACK_MAX || pklen2 > TRACK_MAX || unpklen > TRACK_MAX) return DMS_ERR_DATA;
        if (rd(ctx, d->b1, pklen1) != pklen1) return DMS_ERR_READ;
        if (crc16(d->b1, pklen1) != dcrc) return DMS_ERR_DATA;
        if (number >= 80 || unpklen <= 2048) continue;          // banner, FILE_ID.DIZ, fake boot block
        if (unpklen != TRACK_ADF) return DMS_ERR_UNSUPPORTED;   // not an 880 KB DD disk
        uint8_t *out = adf + number * TRACK_ADF;
        if (unpack_track(d, pklen1, pklen2, out, unpklen, cmode, flags)) return DMS_ERR_DATA;
        if (checksum(out, unpklen) != usum) return DMS_ERR_DATA;
    }
}
