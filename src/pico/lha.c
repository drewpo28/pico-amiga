// LHA / LZH archives (lha.h): headers of levels 0-2 and the static-Huffman
// methods -lh4- .. -lh7- (LZSS with a 4 / 8 / 32 / 64 KB window, the codes in
// blocks: a code-length tree, the literal / length tree, the distance tree), after
// the format of Haruyasu Yoshizaki's LHa and Okumura's ar002.
#include <string.h>
#include "lha.h"

#define NC      510          // 256 literals + lengths 3..256
#define NT      19           // code-length codes
#define NPT     0x80         // the larger of NT and the distance codes
#define CBIT    9
#define TBIT    5
#define CTBITS  12           // c_table's direct bits
#define PTBITS  8            // pt_table's direct bits
#define DICMAX  (64u * 1024u)
#define INBUF   4096u

typedef struct {
    uint8_t  dict[DICMAX];
    uint8_t  in[INBUF];
    uint16_t c_table[1u << CTBITS];
    uint16_t pt_table[1u << PTBITS];
    uint16_t left[2 * NC - 1], right[2 * NC - 1];
    uint8_t  c_len[NC], pt_len[NPT];
    // the bit reader
    lha_read_fn rd; void *rctx;
    uint32_t in_off, in_left;    // the archive's next byte, packed bytes still unread
    uint32_t in_pos, in_len;     // into in[]
    uint32_t bb; int bc;         // bits, top-aligned; how many are valid
    bool     bad;
} work_t;

uint32_t lha_work_size(void) { return (uint32_t)sizeof(work_t); }

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- CRC-16 (ARC: polynomial $A001, reflected, from 0) -------------------------------
static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (uint16_t)(crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1);
    }
    return crc;
}

// ---- headers ----------------------------------------------------------------------
// MS-DOS time (levels 0/1) to seconds since 1970, the fields taken as they are.
static uint32_t dos_to_unix(uint32_t t) {
    static const uint16_t mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    const unsigned y = 1980 + (t >> 25), m = (t >> 21) & 15, d = (t >> 16) & 31;
    const unsigned hh = (t >> 11) & 31, mm = (t >> 5) & 63, ss = (t & 31) * 2;
    if (m < 1 || m > 12 || d < 1) return 0;
    uint32_t days = (y - 1970) * 365 + (y - 1969) / 4 + mdays[m - 1] + d - 1;
    if (m > 2 && y % 4 == 0) days++;
    return days * 86400u + hh * 3600u + mm * 60u + ss;
}

// A stored name into e->path: '\' and $FF are separators, a NUL ends the name
// and starts the Amiga comment.
static void set_name(lha_entry_t *e, const uint8_t *s, unsigned n, bool append) {
    unsigned o = append ? (unsigned)strlen(e->path) : 0;
    unsigned i = 0;
    for (; i < n && s[i]; i++) {
        uint8_t c = s[i];
        if (c == '\\' || c == 0xFF) c = '/';
        if (o + 1 < sizeof e->path) e->path[o++] = (char)c;
    }
    e->path[o] = 0;
    if (i < n && !append) {                                   // "name\0comment"
        unsigned k = 0;
        for (i++; i < n && s[i] && k + 1 < sizeof e->comment; i++) e->comment[k++] = (char)s[i];
        e->comment[k] = 0;
    }
}

int lha_header(lha_read_fn rd, void *ctx, uint32_t off, lha_entry_t *e) {
    uint8_t h[264];                                           // a level 0/1 header is at most 257 bytes
    memset(e, 0, sizeof *e);
    uint32_t n = rd(ctx, off, h, 24);
    if (n == 0 || h[0] == 0) return LHA_END;                  // the end mark (or the file's end)
    if (n < 22) return LHA_ERR_READ;
    if (h[2] != '-' || h[6] != '-') return LHA_ERR_HEADER;
    memcpy(e->method, h + 2, 5);
    e->dir = !memcmp(e->method, "-lhd-", 5);
    e->packed = le32(h + 7);
    e->size = le32(h + 11);
    e->attr = h[19];
    const uint8_t level = h[20];
    uint32_t pos;                                             // the first extended header
    uint32_t next = 0;                                        // its size (0: none)
    char dirname[256] = "";
    if (level == 0 || level == 1) {
        const uint32_t hsize = (uint32_t)h[0] + 2;
        if (rd(ctx, off, h, hsize) != hsize) return LHA_ERR_READ;
        const unsigned nl = h[21];
        if (22u + nl + 2 > hsize) return LHA_ERR_HEADER;
        set_name(e, h + 22, nl, false);
        e->crc = le16(h + 22 + nl);
        e->unix_time = dos_to_unix(le32(h + 15));
        pos = off + hsize;
        if (level == 1) { next = le16(h + hsize - 2); e->os = (char)h[22 + nl + 2]; }
    } else if (level == 2) {
        const uint32_t total = le16(h);
        e->unix_time = le32(h + 15);
        e->crc = le16(h + 21);
        e->os = (char)h[23];
        next = le16(h + 24);
        pos = off + 26;
        e->next_off = off + total;
    } else {
        return LHA_ERR_HEADER;
    }
    // Extended headers: type, data, the next one's size.
    uint32_t ext_total = 0;
    while (next) {
        if (next < 3) return LHA_ERR_HEADER;
        // the type and up to 260 bytes of data; the next header's size at the end
        const uint32_t take = next - 2 < sizeof h ? next - 2 : sizeof h;
        uint8_t nx[2];
        if (rd(ctx, pos, h, take) != take || rd(ctx, pos + next - 2, nx, 2) != 2) return LHA_ERR_READ;
        const unsigned dl = take - 1;
        switch (h[0]) {
        case 0x01: set_name(e, h + 1, dl, false); break;     // the file name
        case 0x02: {                                          // the directory
            unsigned k = 0;
            for (unsigned i = 0; i < dl && k + 1 < sizeof dirname; i++)
                dirname[k++] = (char)(h[1 + i] == 0xFF || h[1 + i] == '\\' ? '/' : h[1 + i]);
            dirname[k] = 0;
            break;
        }
        case 0x3F: case 0x71: {                               // a comment
            unsigned k = 0;
            for (unsigned i = 0; i < dl && k + 1 < sizeof e->comment; i++) e->comment[k++] = (char)h[1 + i];
            e->comment[k] = 0;
            break;
        }
        case 0x54: if (dl >= 4) e->unix_time = le32(h + 1); break;
        default: break;
        }
        pos += next;
        ext_total += next;
        next = le16(nx);
    }
    if (level == 1) {                                         // the skip size counts the extended headers
        if (e->packed < ext_total) return LHA_ERR_HEADER;
        e->packed -= ext_total;
    }
    if (dirname[0]) {                                         // the directory goes in front of the name
        char name[256];
        strcpy(name, e->path);
        size_t dl = strlen(dirname);
        while (dl && dirname[dl - 1] == '/') dirname[--dl] = 0;
        strcpy(e->path, dirname);
        if (name[0]) { strcat(e->path, "/"); set_name(e, (const uint8_t *)name, (unsigned)strlen(name), true); }
    }
    size_t pl = strlen(e->path);
    while (pl && e->path[pl - 1] == '/') { e->path[--pl] = 0; e->dir = true; }
    e->data_off = pos;
    if (level != 2) e->next_off = pos + e->packed;
    else e->data_off = e->next_off, e->next_off += e->packed;
    return LHA_OK;
}

// ---- the bit reader ----------------------------------------------------------------
static uint8_t next_byte(work_t *w) {
    if (w->in_pos == w->in_len) {
        if (!w->in_left) return 0;                            // past the data: zeros (the last code's padding)
        uint32_t n = w->in_left < INBUF ? w->in_left : INBUF;
        if (w->rd(w->rctx, w->in_off, w->in, n) != n) { w->bad = true; w->in_left = 0; return 0; }
        w->in_off += n; w->in_left -= n;
        w->in_pos = 0; w->in_len = n;
    }
    return w->in[w->in_pos++];
}

static void fill(work_t *w, int n) {
    w->bb <<= n;
    w->bc -= n;
    while (w->bc <= 24) { w->bb |= (uint32_t)next_byte(w) << (24 - w->bc); w->bc += 8; }
}

static unsigned peek16(const work_t *w) { return w->bb >> 16; }

static unsigned getbits(work_t *w, int n) {
    if (!n) return 0;
    const unsigned v = w->bb >> (32 - n);
    fill(w, n);
    return v;
}

// ---- Huffman tables (ar002's make_table) ---------------------------------------------
static bool make_table(work_t *w, int nchar, const uint8_t *bitlen, int tablebits, uint16_t *table) {
    uint32_t count[17] = { 0 }, weight[17], start[18];
    for (int i = 0; i < nchar; i++) {
        if (bitlen[i] > 16) return false;
        count[bitlen[i]]++;
    }
    start[1] = 0;
    for (int i = 1; i <= 16; i++) start[i + 1] = start[i] + (count[i] << (16 - i));
    if (start[17] != 0x10000u) return false;
    const int jut = 16 - tablebits;
    for (int i = 1; i <= tablebits; i++) { start[i] >>= jut; weight[i] = 1u << (tablebits - i); }
    for (int i = tablebits + 1; i <= 16; i++) weight[i] = 1u << (16 - i);
    uint32_t i = start[tablebits + 1] >> jut;
    if (i != 0x10000u >> jut) {
        const uint32_t k = 1u << tablebits;
        while (i != k) table[i++] = 0;
    }
    unsigned avail = (unsigned)nchar;
    const uint32_t mask = 1u << (15 - tablebits);
    for (int ch = 0; ch < nchar; ch++) {
        const int len = bitlen[ch];
        if (!len) continue;
        const uint32_t nextcode = start[len] + weight[len];
        if (len <= tablebits) {
            if (nextcode > (1u << tablebits)) return false;
            for (uint32_t j = start[len]; j < nextcode; j++) table[j] = (uint16_t)ch;
        } else {
            uint32_t k = start[len];
            uint16_t *p = &table[k >> jut];
            for (int n = len - tablebits; n; n--) {
                if (*p == 0) {
                    if (avail >= 2 * NC - 1) return false;
                    w->right[avail] = w->left[avail] = 0;
                    *p = (uint16_t)avail++;
                }
                p = (k & mask) ? &w->right[*p] : &w->left[*p];
                k <<= 1;
            }
            *p = (uint16_t)ch;
        }
        start[len] = nextcode;
    }
    return true;
}

static bool read_pt_len(work_t *w, int nn, int nbit, int special) {
    const int n = (int)getbits(w, nbit);
    if (n == 0) {
        const unsigned c = getbits(w, nbit);
        memset(w->pt_len, 0, (size_t)nn);
        for (int i = 0; i < 256; i++) w->pt_table[i] = (uint16_t)c;
        return c < (unsigned)nn;
    }
    if (n > nn) return false;
    int i = 0;
    while (i < n) {
        int c = (int)(peek16(w) >> 13);
        if (c == 7) {
            unsigned m = 1u << 12;
            while (peek16(w) & m) { m >>= 1; c++; if (c > 16) return false; }
        }
        fill(w, c < 7 ? 3 : c - 3);
        w->pt_len[i++] = (uint8_t)c;
        if (i == special) {
            int z = (int)getbits(w, 2);
            while (z-- > 0 && i < nn) w->pt_len[i++] = 0;
        }
    }
    while (i < nn) w->pt_len[i++] = 0;
    return make_table(w, nn, w->pt_len, PTBITS, w->pt_table);
}

static bool read_c_len(work_t *w) {
    const int n = (int)getbits(w, CBIT);
    if (n == 0) {
        const unsigned c = getbits(w, CBIT);
        memset(w->c_len, 0, NC);
        for (unsigned i = 0; i < (1u << CTBITS); i++) w->c_table[i] = (uint16_t)c;
        return c < NC;
    }
    if (n > NC) return false;
    int i = 0;
    while (i < n) {
        unsigned c = w->pt_table[peek16(w) >> 8];
        if (c >= NT) {
            unsigned m = 1u << 7;
            do { c = (peek16(w) & m) ? w->right[c] : w->left[c]; m >>= 1; } while (c >= NT && m);
            if (c >= NT) return false;
        }
        fill(w, w->pt_len[c]);
        if (c <= 2) {
            int z = c == 0 ? 1 : c == 1 ? (int)getbits(w, 4) + 3 : (int)getbits(w, CBIT) + 20;
            while (z-- > 0 && i < NC) w->c_len[i++] = 0;
        } else {
            w->c_len[i++] = (uint8_t)(c - 2);
        }
    }
    while (i < NC) w->c_len[i++] = 0;
    return make_table(w, NC, w->c_len, CTBITS, w->c_table);
}

// ---- unpacking ---------------------------------------------------------------------
typedef struct {
    lha_write_fn wr; void *ctx;
    uint16_t crc;
    int err;
} out_t;

static bool emit(out_t *o, const uint8_t *p, uint32_t n) {
    if (!n || o->err) return !o->err;
    o->crc = crc16(o->crc, p, n);
    const int r = o->wr(o->ctx, p, n);
    if (r) o->err = r == LHA_STOP ? LHA_STOP : LHA_ERR_WRITE;
    return !o->err;
}

static int unpack_huff(work_t *w, const lha_entry_t *e, out_t *o, int dicbit) {
    const int np = dicbit + 1, pbit = dicbit >= 15 ? 5 : 4;
    const uint32_t dmask = (1u << dicbit) - 1;
    uint32_t pos = 0, flushed = 0;                            // bytes out, bytes handed to emit
    uint32_t left = e->size;
    unsigned blocksize = 0;
    fill(w, 0);
    while (left) {
        if (blocksize == 0) {
            blocksize = getbits(w, 16);
            if (!read_pt_len(w, NT, TBIT, 3) || !read_c_len(w) || !read_pt_len(w, np, pbit, -1))
                return LHA_ERR_DATA;
            if (w->bad) return LHA_ERR_READ;
            if (!blocksize) return LHA_ERR_DATA;
        }
        blocksize--;
        unsigned c = w->c_table[peek16(w) >> 4];
        if (c >= NC) {
            unsigned m = 1u << 3;
            do { c = (peek16(w) & m) ? w->right[c] : w->left[c]; m >>= 1; } while (c >= NC && m);
            if (c >= NC) return LHA_ERR_DATA;
        }
        fill(w, w->c_len[c]);
        if (c < 256) {
            w->dict[pos & dmask] = (uint8_t)c;
            pos++; left--;
            if ((pos & dmask) == 0) { if (!emit(o, w->dict + (flushed & dmask), pos - flushed)) break; flushed = pos; }
            continue;
        }
        uint32_t len = c - 256 + 3;
        unsigned p = w->pt_table[peek16(w) >> 8];
        if (p >= (unsigned)np) {
            unsigned m = 1u << 7;
            do { p = (peek16(w) & m) ? w->right[p] : w->left[p]; m >>= 1; } while (p >= (unsigned)np && m);
            if (p >= (unsigned)np) return LHA_ERR_DATA;
        }
        fill(w, w->pt_len[p]);
        uint32_t dist = p ? (1u << (p - 1)) + getbits(w, (int)p - 1) : 0;
        uint32_t from = pos - dist - 1;
        if (len > left) len = left;
        while (len--) {
            w->dict[pos & dmask] = w->dict[from++ & dmask];
            pos++; left--;
            if ((pos & dmask) == 0) { if (!emit(o, w->dict + (flushed & dmask), pos - flushed)) break; flushed = pos; }
        }
        if (o->err) break;
    }
    if (!o->err && pos != flushed) emit(o, w->dict + (flushed & dmask), pos - flushed);
    if (w->bad) return LHA_ERR_READ;
    return LHA_OK;
}

int lha_unpack(lha_read_fn rd, void *rctx, const lha_entry_t *e, lha_write_fn wr, void *wctx, void *work) {
    work_t *w = (work_t *)work;
    out_t o = { wr, wctx, 0, 0 };
    w->rd = rd; w->rctx = rctx;
    w->in_off = e->data_off; w->in_left = e->packed;
    w->in_pos = w->in_len = 0;
    w->bb = 0; w->bc = 0; w->bad = false;
    int r;
    if (e->dir) return LHA_OK;
    if (!memcmp(e->method, "-lh0-", 5) || !memcmp(e->method, "-lz4-", 5)) {
        if (e->packed != e->size) return LHA_ERR_DATA;
        uint32_t left = e->size, off = e->data_off;
        r = LHA_OK;
        while (left && !o.err) {
            const uint32_t n = left < INBUF ? left : INBUF;
            if (rd(rctx, off, w->in, n) != n) { r = LHA_ERR_READ; break; }
            emit(&o, w->in, n);
            off += n; left -= n;
        }
    } else if (!memcmp(e->method, "-lh4-", 5)) r = unpack_huff(w, e, &o, 12);
    else if (!memcmp(e->method, "-lh5-", 5)) r = unpack_huff(w, e, &o, 13);
    else if (!memcmp(e->method, "-lh6-", 5)) r = unpack_huff(w, e, &o, 15);
    else if (!memcmp(e->method, "-lh7-", 5)) r = unpack_huff(w, e, &o, 16);
    else return LHA_ERR_METHOD;
    if (o.err == LHA_STOP) return LHA_OK;
    if (o.err) return o.err;
    if (r != LHA_OK) return r;
    return o.crc == e->crc ? LHA_OK : LHA_ERR_DATA;
}

const char *lha_error(int err) {
    switch (err) {
    case LHA_OK: return "ok";
    case LHA_END: return "end of archive";
    case LHA_ERR_READ: return "read error";
    case LHA_ERR_HEADER: return "not an LHA archive";
    case LHA_ERR_METHOD: return "unsupported method";
    case LHA_ERR_DATA: return "bad data (CRC)";
    case LHA_ERR_WRITE: return "write error";
    default: return "?";
    }
}
