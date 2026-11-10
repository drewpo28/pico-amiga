// An FFS (DOS\1) volume written from scratch (ffs.h), after the block layouts of
// the ADF FAQ (Laurent Clevy): the root block in the middle of the partition,
// the bitmap right after it, a header block per directory and file with the
// 72-entry hash tables and chains, extension blocks (T_LIST) for files of more
// than 72 blocks, and the data in plain 512-byte blocks (FFS has no data headers).
//
// The allocator takes blocks from the start of the partition up to the root, then
// from after the bitmap on; a file's data is one contiguous run in either half,
// so its bytes are written in order through one staging buffer.
#include <string.h>
#include "ffs.h"

#define BS        512u
#define HT        72u                 // hash table / block table entries
#define RESERVED  2u                  // the boot blocks
#define BM_BITS   (127u * 32u)        // blocks one bitmap block covers
#define BM_MAX    25u                 // the root's bitmap pointers (no bm_ext)
#define AMIGA_EPOCH 252460800u        // 1978-01-01 in Unix seconds

typedef struct {
    char     name[FFS_NAME_MAX + 1];
    uint8_t  dir, protect;
    int32_t  parent;                  // -1: the root
    uint32_t size, time, comment;     // comment: offset into the arena's pool, 0 = none
    uint32_t hdr, first, chain;       // header block, first data block, the next in its hash chain
} node_t;

struct ffs {
    char     volume[FFS_NAME_MAX + 1];
    uint32_t time;
    uint8_t *arena; uint32_t arena_size;
    uint32_t pool;                    // the comment pool grows down from the arena's end
    node_t  *nodes; int count, cap;
    uint32_t blocks, root, nbm;       // the layout
    uint32_t cur_a, cur_b, b0;        // the allocator's two halves
    // the file being written
    ffs_write_fn wr; void *ctx;
    uint8_t *buf; uint32_t buf_size, buf_len;
    uint32_t pos, left;               // byte offset of buf[0] in the image, bytes still due
};

static void put32(uint8_t *b, uint32_t off, uint32_t v) {
    b[off] = (uint8_t)(v >> 24); b[off + 1] = (uint8_t)(v >> 16); b[off + 2] = (uint8_t)(v >> 8); b[off + 3] = (uint8_t)v;
}
static uint32_t be32(const uint8_t *b, uint32_t off) {
    return (uint32_t)b[off] << 24 | (uint32_t)b[off + 1] << 16 | (uint32_t)b[off + 2] << 8 | b[off + 3];
}
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static bool name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (upper((uint8_t)*a) != upper((uint8_t)*b)) return false;
    return *a == *b;
}

// AmigaDOS's hash (the non-international one, as DOS\1 uses).
static uint32_t hash(const char *s) {
    uint32_t h = (uint32_t)strlen(s);
    for (; *s; s++) h = (h * 13 + (uint32_t)upper((uint8_t)*s)) & 0x7FF;
    return h % HT;
}

static void put_date(uint8_t *b, uint32_t off, uint32_t t) {
    t = t > AMIGA_EPOCH ? t - AMIGA_EPOCH : 0;
    put32(b, off, t / 86400);
    put32(b, off + 4, t % 86400 / 60);
    put32(b, off + 8, t % 60 * 50);
}

static void put_bstr(uint8_t *b, uint32_t off, const char *s, uint32_t max) {
    uint32_t n = (uint32_t)strlen(s);
    if (n > max) n = max;
    b[off] = (uint8_t)n;
    memcpy(b + off + 1, s, n);
}

// Header-type blocks: the longs add up to 0, the checksum at byte 20.
static void sum_header(uint8_t *b) {
    put32(b, 20, 0);
    uint32_t s = 0;
    for (uint32_t i = 0; i < BS; i += 4) s += be32(b, i);
    put32(b, 20, (uint32_t)-s);
}

ffs_t *ffs_begin(void *arena, uint32_t size, const char *volume, uint32_t unix_time) {
    if (size < sizeof(ffs_t) + 64 * sizeof(node_t)) return NULL;
    ffs_t *f = (ffs_t *)arena;
    memset(f, 0, sizeof *f);
    strncpy(f->volume, volume, FFS_NAME_MAX);
    f->time = unix_time;
    f->arena = (uint8_t *)arena;
    f->arena_size = size;
    f->pool = size;
    f->nodes = (node_t *)(f->arena + ((sizeof *f + 7) & ~7u));
    f->cap = (int)((size - ((sizeof *f + 7) & ~7u)) / sizeof(node_t));
    return f;
}

static int child(ffs_t *f, int parent, const char *name) {
    for (int i = 0; i < f->count; i++)
        if (f->nodes[i].parent == parent && name_eq(f->nodes[i].name, name)) return i;
    return -1;
}

static int new_node(ffs_t *f, int parent, const char *name, bool dir, uint32_t t) {
    // the pool and the node array share the arena
    const uint32_t end = (uint32_t)((uint8_t *)&f->nodes[f->count + 1] - f->arena);
    if (f->count >= f->cap || end > f->pool) return -1;
    node_t *n = &f->nodes[f->count];
    memset(n, 0, sizeof *n);
    strncpy(n->name, name, FFS_NAME_MAX);
    n->dir = dir;
    n->parent = parent;
    n->time = t;
    return f->count++;
}

int ffs_add(ffs_t *f, const char *path, bool dir, uint32_t size, uint8_t protect,
            const char *comment, uint32_t unix_time) {
    int parent = -1;
    char part[FFS_NAME_MAX + 1];
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return -1;
    for (;;) {
        const char *e = strchr(p, '/');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        const size_t n = len > FFS_NAME_MAX ? FFS_NAME_MAX : len;
        memcpy(part, p, n);
        part[n] = 0;
        const bool last = !e || !e[1];
        const int c = child(f, parent, part);
        if (!last) {
            if (c >= 0 && !f->nodes[c].dir) return -2;          // a file in the way
            parent = c >= 0 ? c : new_node(f, parent, part, true, unix_time);
            if (parent < 0) return -1;
            p = e + 1;
            continue;
        }
        if (c >= 0) return dir && f->nodes[c].dir ? c : -2;
        const int id = new_node(f, parent, part, dir, unix_time);
        if (id < 0) return -1;
        node_t *nd = &f->nodes[id];
        nd->size = dir ? 0 : size;
        nd->protect = protect;
        if (comment && *comment) {
            uint32_t cl = (uint32_t)strlen(comment);
            if (cl > 79) cl = 79;
            const uint32_t need = cl + 1;
            if (f->pool - need >= (uint32_t)((uint8_t *)&f->nodes[f->count] - f->arena)) {
                f->pool -= need;
                memcpy(f->arena + f->pool, comment, cl);
                f->arena[f->pool + cl] = 0;
                nd->comment = f->pool;
            }
        }
        return id;
    }
}

int ffs_find(ffs_t *f, const char *path) {
    int cur = -1;
    char part[FFS_NAME_MAX + 1];
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *e = strchr(p, '/');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        const size_t n = len > FFS_NAME_MAX ? FFS_NAME_MAX : len;
        memcpy(part, p, n);
        part[n] = 0;
        cur = child(f, cur, part);
        if (cur < 0 || !e) return cur;
        p = e + 1;
    }
    return cur;
}

uint32_t ffs_file_size(ffs_t *f, int id) { return id >= 0 && id < f->count ? f->nodes[id].size : 0; }
int ffs_count(ffs_t *f) { return f->count; }

// ---- layout --------------------------------------------------------------------------
static uint32_t data_blocks(const node_t *n) { return (n->size + BS - 1) / BS; }
static uint32_t ext_blocks(const node_t *n) { const uint32_t d = data_blocks(n); return d > HT ? (d - 1) / HT : 0; }

// A run of n blocks: below the root while it fits, else after the bitmap.
static uint32_t alloc(ffs_t *f, uint32_t n) {
    if (f->cur_a + n <= f->root) { const uint32_t b = f->cur_a; f->cur_a += n; return b; }
    if (f->cur_b + n <= f->blocks) { const uint32_t b = f->cur_b; f->cur_b += n; return b; }
    return 0;
}

static bool place(ffs_t *f, uint32_t blocks) {
    f->blocks = blocks;
    f->root = (blocks - 1 + RESERVED) / 2;
    f->nbm = (blocks - RESERVED + BM_BITS - 1) / BM_BITS;
    if (f->nbm > BM_MAX) return false;
    f->cur_a = RESERVED;
    f->b0 = f->cur_b = f->root + 1 + f->nbm;
    for (int i = 0; i < f->count; i++) {
        node_t *n = &f->nodes[i];
        if (!(n->hdr = alloc(f, 1))) return false;
        n->first = 0;
        // the data and its extension blocks right after it: one run, never split
        // across the root (ffs_write_meta numbers them first + data blocks + i)
        if (!n->dir && n->size && !(n->first = alloc(f, data_blocks(n) + ext_blocks(n)))) return false;
    }
    return true;
}

uint32_t ffs_layout(ffs_t *f, uint32_t free_blocks) {
    uint32_t used = 0, run = 0;
    for (int i = 0; i < f->count; i++) {
        const node_t *n = &f->nodes[i];
        used += 1 + data_blocks(n) + ext_blocks(n);
        if (data_blocks(n) + ext_blocks(n) > run) run = data_blocks(n) + ext_blocks(n);
    }
    // the boot blocks, the root, the bitmap, the largest run's worth lost below the root
    uint32_t blocks = RESERVED + 1 + used + run + free_blocks;
    blocks += (blocks + BM_BITS - 1) / BM_BITS;
    blocks = (blocks + 31) & ~31u;
    for (int tries = 0; tries < 64; tries++, blocks += 32 * 16)
        if (place(f, blocks)) return blocks;
    return 0;
}

// ---- the blocks ----------------------------------------------------------------------
static int wblock(ffs_write_fn wr, void *ctx, uint32_t blk, const uint8_t *b) {
    return wr(ctx, blk * BS, b, BS);
}

// The hash table of directory `dir` (-1 the root) into b + 24, the chains into the nodes.
static void hash_table(ffs_t *f, int dir, uint8_t *b) {
    uint32_t head[HT];
    memset(head, 0, sizeof head);
    for (int i = f->count - 1; i >= 0; i--) {        // chains in creation order
        node_t *n = &f->nodes[i];
        if (n->parent != dir) continue;
        const uint32_t h = hash(n->name);
        n->chain = head[h];
        head[h] = n->hdr;
    }
    for (uint32_t i = 0; i < HT; i++) put32(b, 24 + 4 * i, head[i]);
}

static uint32_t parent_block(const ffs_t *f, const node_t *n) {
    return n->parent < 0 ? f->root : f->nodes[n->parent].hdr;
}

int ffs_write_meta(ffs_t *f, ffs_write_fn wr, void *ctx) {
    uint8_t b[BS];
    // boot blocks
    memset(b, 0, BS);
    memcpy(b, "DOS\1", 4);
    if (wblock(wr, ctx, 0, b)) return -1;
    memset(b, 0, BS);
    if (wblock(wr, ctx, 1, b)) return -1;
    // the hash chains first: every header names the next one in its chain
    for (int d = -1; d < f->count; d++)
        if (d < 0 || f->nodes[d].dir) hash_table(f, d, b);
    // root
    memset(b, 0, BS);
    put32(b, 0, 2);
    put32(b, 12, HT);
    hash_table(f, -1, b);
    put32(b, 0x138, 0xFFFFFFFF);                      // bm_flag: the bitmap is valid
    for (uint32_t i = 0; i < f->nbm; i++) put32(b, 0x13C + 4 * i, f->root + 1 + i);
    put_date(b, 0x1A4, f->time);
    put_bstr(b, 0x1B0, f->volume, FFS_NAME_MAX);
    put_date(b, 0x1D8, f->time);
    put_date(b, 0x1E4, f->time);
    put32(b, 0x1FC, 1);                               // ST_ROOT
    sum_header(b);
    if (wblock(wr, ctx, f->root, b)) return -1;
    // bitmap: a set bit is a free block, from block 2 on
    for (uint32_t i = 0; i < f->nbm; i++) {
        memset(b, 0, BS);
        for (uint32_t k = 0; k < BM_BITS; k++) {
            const uint32_t blk = RESERVED + i * BM_BITS + k;
            const bool used = blk < f->cur_a || (blk >= f->root && blk < f->b0) || (blk >= f->b0 && blk < f->cur_b);
            if (blk < f->blocks && !used) b[4 + (k / 32) * 4 + 3 - (k % 32) / 8] |= (uint8_t)(1u << (k % 8));
        }
        uint32_t s = 0;
        for (uint32_t k = 4; k < BS; k += 4) s += be32(b, k);
        put32(b, 0, (uint32_t)-s);
        if (wblock(wr, ctx, f->root + 1 + i, b)) return -1;
    }
    // directories and file headers (with their extension blocks)
    for (int i = 0; i < f->count; i++) {
        const node_t *n = &f->nodes[i];
        memset(b, 0, BS);
        put32(b, 0, 2);
        put32(b, 4, n->hdr);
        if (n->dir) {
            hash_table(f, i, b);
        } else {
            const uint32_t nd = data_blocks(n);
            const uint32_t k = nd < HT ? nd : HT;
            put32(b, 8, k);
            put32(b, 16, n->first);
            for (uint32_t j = 0; j < k; j++) put32(b, 0x18 + 4 * (HT - 1 - j), n->first + j);
            put32(b, 0x144, n->size);
            if (ext_blocks(n)) put32(b, 0x1F8, n->first + nd);
        }
        put32(b, 0x140, n->protect);
        if (n->comment) put_bstr(b, 0x148, (const char *)f->arena + n->comment, 79);
        put_date(b, 0x1A4, n->time);
        put_bstr(b, 0x1B0, n->name, FFS_NAME_MAX);
        put32(b, 0x1F0, n->chain);
        put32(b, 0x1F4, parent_block(f, n));
        put32(b, 0x1FC, n->dir ? 2 : 0xFFFFFFFD);     // ST_USERDIR / ST_FILE
        sum_header(b);
        if (wblock(wr, ctx, n->hdr, b)) return -1;
        // extension blocks: data blocks 72.. in 72s
        const uint32_t ne = n->dir ? 0 : ext_blocks(n), nd = data_blocks(n);
        for (uint32_t e = 0; e < ne; e++) {
            const uint32_t blk = n->first + nd + e, from = (e + 1) * HT;
            const uint32_t k = nd - from < HT ? nd - from : HT;
            memset(b, 0, BS);
            put32(b, 0, 16);                          // T_LIST
            put32(b, 4, blk);
            put32(b, 8, k);
            for (uint32_t j = 0; j < k; j++) put32(b, 0x18 + 4 * (HT - 1 - j), n->first + from + j);
            put32(b, 0x1F4, n->hdr);
            if (e + 1 < ne) put32(b, 0x1F8, blk + 1);
            put32(b, 0x1FC, 0xFFFFFFFD);
            sum_header(b);
            if (wblock(wr, ctx, blk, b)) return -1;
        }
    }
    return 0;
}

// ---- file data -------------------------------------------------------------------------
static int flush(ffs_t *f) {
    if (!f->buf_len) return 0;
    const uint32_t n = (f->buf_len + BS - 1) & ~(BS - 1);
    memset(f->buf + f->buf_len, 0, n - f->buf_len);
    const int r = f->wr(f->ctx, f->pos, f->buf, n);
    f->pos += n;
    f->buf_len = 0;
    return r;
}

int ffs_file_begin(ffs_t *f, int id, ffs_write_fn wr, void *ctx, uint8_t *buf, uint32_t buf_size) {
    if (id < 0 || id >= f->count || f->nodes[id].dir || buf_size < BS) return -1;
    f->wr = wr; f->ctx = ctx;
    f->buf = buf; f->buf_size = buf_size & ~(BS - 1); f->buf_len = 0;
    f->pos = f->nodes[id].first * BS;
    f->left = f->nodes[id].size;
    return 0;
}

int ffs_file_data(ffs_t *f, const void *src, uint32_t len) {
    const uint8_t *s = (const uint8_t *)src;
    if (len > f->left) return -1;
    f->left -= len;
    while (len) {
        uint32_t n = f->buf_size - f->buf_len;
        if (n > len) n = len;
        memcpy(f->buf + f->buf_len, s, n);
        f->buf_len += n; s += n; len -= n;
        if (f->buf_len == f->buf_size && flush(f)) return -1;
    }
    return 0;
}

int ffs_file_end(ffs_t *f) {
    const int r = flush(f);
    return r || f->left ? -1 : 0;
}
