// An LHA archive as a bootable FFS volume (whdboot.h).
//
// whd_plan reads the archive's headers, finds what boots (a WHDLoad slave, by
// the icon whose SLAVE= tooltype names it; else a program: a hunk file near the
// top), reads the slave for the Kickstart images it names and its memory needs,
// writes the startup-sequence and lays the volume out (ffs.c).  whd_write then
// writes the metadata and every file's data, the archive's unpacked straight
// into its blocks.
//
// The arena: the LHA decoder's work area, a 64 KB buffer for the small files read
// while planning (icons, the slave) and for the extra files' chunks, the 16 KB
// staging buffer of the writes, the table of where each file's bytes come from,
// and the volume's tree in the rest.
#include <stdio.h>
#include <string.h>
#include "whdboot.h"

#define SMALL_MAX  (64u * 1024u)
#define STAGE      (16u * 1024u)
#define MAX_FILES  4096
#define SRC_EXTRA  0x80000000u      // src: an extra file's index
#define SRC_START  0xFFFFFFFFu      // src: the generated startup-sequence

typedef struct { int32_t node; uint32_t src; } file_t;

typedef struct {
    char slave[128];       // SLAVE= as written
    char opts[200];        // the options, CLI-ready
    bool preload;
} icon_t;

// In the arena (PSRAM on the board), with whd_plan's larger working variables:
// the build runs on a small stack.
typedef struct {
    uint8_t *lha_work, *small, *stage;
    file_t  *files; int nfiles;
    ffs_t   *fs;
    char     startup[512]; uint32_t startup_len;
    lha_entry_t e;
    icon_t   ic, best, opts;
    char     best_dir[128], dir[256], name[256], path[260];
} plan_t;

static uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }

static plan_t *arena_plan(void *arena) { return (plan_t *)arena; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool ends_with(const char *s, const char *tail) {
    const size_t a = strlen(s), b = strlen(tail);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) if (lower((uint8_t)s[a - b + i]) != lower((uint8_t)tail[i])) return false;
    return true;
}
static bool same(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (lower((uint8_t)*a) != lower((uint8_t)*b)) return false;
    return *a == *b;
}
static int depth(const char *p) { int d = 0; for (; *p; p++) d += *p == '/'; return d; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// Case-insensitive search of `needle` in a buffer.
static bool contains(const uint8_t *buf, uint32_t n, const char *needle) {
    const uint32_t k = (uint32_t)strlen(needle);
    if (!k || k > n) return false;
    for (uint32_t i = 0; i + k <= n; i++) {
        uint32_t j = 0;
        while (j < k && lower(buf[i + j]) == lower((uint8_t)needle[j])) j++;
        if (j == k) return true;
    }
    return false;
}

// ---- reading a member into memory -----------------------------------------------------
typedef struct { uint8_t *buf; uint32_t len, cap; } mem_t;
static int mem_put(void *ctx, const uint8_t *src, uint32_t len) {
    mem_t *m = (mem_t *)ctx;
    uint32_t n = m->cap - m->len;
    if (n > len) n = len;
    memcpy(m->buf + m->len, src, n);
    m->len += n;
    return m->len == m->cap ? LHA_STOP : 0;
}
// The member's first `cap` bytes (all of a small one) into p->small.
static uint32_t read_small(whd_job_t *job, plan_t *p, const lha_entry_t *e, uint32_t cap) {
    mem_t m = { p->small, 0, cap < SMALL_MAX ? cap : SMALL_MAX };
    if (lha_unpack(job->rd, job->rctx, e, mem_put, &m, p->lha_work) != LHA_OK) return 0;
    return m.len;
}

// ---- icons: a DiskObject's tooltypes ----------------------------------------------------
// Calls fn for each tooltype string of the icon in buf; false: not an icon.
static bool icon_tooltypes(const uint8_t *b, uint32_t n, void (*fn)(void *, const char *), void *ctx) {
    if (n < 78 || be16(b) != 0xE310) return false;
    const uint32_t render = be32(b + 22), select = be32(b + 26);
    const uint32_t tool = be32(b + 50), types = be32(b + 54), drawer = be32(b + 66);
    uint32_t p = 78;
    if (drawer) p += 56;
    for (int img = 0; img < 2; img++) {
        if (!(img ? select : render)) continue;
        if (p + 20 > n) return false;
        const uint32_t w = be16(b + p + 4), h = be16(b + p + 6), d = be16(b + p + 8), data = be32(b + p + 10);
        p += 20;
        if (data) p += ((w + 15) / 16) * 2 * h * d;
    }
    if (tool) { if (p + 4 > n) return false; p += 4 + be32(b + p); }
    if (!types) return true;
    if (p + 4 > n) return false;
    uint32_t count = be32(b + p) / 4;
    p += 4;
    for (uint32_t i = 0; i + 1 < count; i++) {
        if (p + 4 > n) return false;
        const uint32_t len = be32(b + p);
        p += 4;
        if (!len || p + len > n || len > 256) return false;
        char s[257];
        memcpy(s, b + p, len);
        s[len] = 0;
        fn(ctx, s);
        p += len;
    }
    return true;
}

// WHDLoad's local options (its docs' table, "Local" column): what a tooltype may
// pass on the command line.  MMU and the Snoop / Chk family are left out (the
// 68000 / 68EC020 have no MMU: WHDLoad would refuse to start).
static const char *const kSwitches[] = {
    "BranchCache", "ButtonWait", "Cache", "ChipNoCache", "CoreDump", "D", "DblNTSC", "DblPAL",
    "DCache", "ExpChip", "ExpLocal", "Exp24Bit", "FileLog", "FullChip", "NoAutoVec", "NoCache",
    "NoFileCache", "NoFilter", "NoFlushMem", "NoMMU", "NoNetwork", "NoResInt", "NoReq", "NoVBRMove",
    "NoWriteCache", "NoWriteInfo", "NTSC", "PAL", "Preload", "PreloadSmart", "StoreBuffer",
    "SuperScalar", "WriteCache", 0 };
static const char *const kNumbers[] = {
    "AddChipMem", "AddExpMem", "ConfigDelay", "Custom1", "Custom2", "Custom3", "Custom4", "Custom5",
    "DebugKey", "FreezeKey", "LogBuffer", "MemConfig", "PreloadSize", "QuitKey", "RestartKey",
    "SplashDelay", "TimeOut", "WriteDelay", 0 };
static const char *const kStrings[] = {
    "Config", "Custom", "Data", "ExecuteArgs", "ExecuteCleanup", "ExecutePreDisk", "ExecutePostDisk",
    "ExecuteStartup", "SaveDir", "SavePath", 0 };

static int find_opt(const char *const *list, const char *name) {
    for (int i = 0; list[i]; i++) if (same(list[i], name)) return i;
    return -1;
}

static void opt_append(icon_t *ic, const char *s) {
    const size_t a = strlen(ic->opts), b = strlen(s);
    if (a + b + 2 >= sizeof ic->opts) return;
    ic->opts[a] = ' ';
    memcpy(ic->opts + a + 1, s, b + 1);
}

static void tooltype(void *ctx, const char *tt) {
    icon_t *ic = (icon_t *)ctx;
    while (*tt == ' ') tt++;
    if (!*tt || *tt == '(' || *tt == ';' || *tt == '*' || *tt == '#') return;   // commented out
    char name[40], val[160] = "";
    const char *eq = strchr(tt, '=');
    size_t nl = eq ? (size_t)(eq - tt) : strlen(tt);
    while (nl && tt[nl - 1] == ' ') nl--;
    if (nl >= sizeof name) return;
    memcpy(name, tt, nl);
    name[nl] = 0;
    if (eq) {
        const char *v = eq + 1;
        while (*v == ' ') v++;
        snprintf(val, sizeof val, "%s", v);
        size_t vl = strlen(val);
        while (vl && val[vl - 1] == ' ') val[--vl] = 0;
    }
    char out[200];
    int i;
    if (same(name, "Slave")) {
        snprintf(ic->slave, sizeof ic->slave, "%s", val);
    } else if ((i = find_opt(kSwitches, name)) >= 0) {
        if (same(name, "Preload")) ic->preload = true;
        else opt_append(ic, kSwitches[i]);
    } else if ((i = find_opt(kNumbers, name)) >= 0 && val[0]) {
        long v = 0;                                  // the CLI takes decimal only
        if (val[0] == '$') sscanf(val + 1, "%lx", (unsigned long *)&v);
        else if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) sscanf(val + 2, "%lx", (unsigned long *)&v);
        else sscanf(val, "%ld", &v);
        snprintf(out, sizeof out, "%s=%ld", kNumbers[i], v);
        opt_append(ic, out);
    } else if ((i = find_opt(kStrings, name)) >= 0 && val[0]) {
        if (val[0] == '"') snprintf(out, sizeof out, "%s=%s", kStrings[i], val);
        else snprintf(out, sizeof out, "%s=\"%s\"", kStrings[i], val);
        opt_append(ic, out);
    }
}

// ---- the slave's header ----------------------------------------------------------------
// ws_BaseMemSize and ws_ExpMem from the first hunk of a slave file.
static void slave_mem(const uint8_t *b, uint32_t n, uint32_t *base, uint32_t *exp) {
    *base = *exp = 0;
    if (n < 32 || be32(b) != 0x3F3) return;
    uint32_t p = 4;
    while (p + 4 <= n && be32(b + p)) p += 4 + 4 * be32(b + p);   // resident library names
    p += 4;
    if (p + 12 > n) return;
    const uint32_t first = be32(b + p + 4), last = be32(b + p + 8);
    p += 12 + 4 * (last - first + 1);
    if (p + 8 > n || (be32(b + p) & 0x3FFFFFFF) != 0x3E9) return;  // HUNK_CODE
    p += 8;
    const uint8_t *s = b + p;
    if (p + 36 > n || memcmp(s + 4, "WHDLOADS", 8)) return;
    *base = be32(s + 16);
    if (be16(s + 12) >= 8) *exp = be32(s + 32);
}

// Kickstart versions a slave may name (for "missing").
static const char *const kKicks[] = {
    "33180.A500", "34005.A500", "37175.A500", "37350.A600", "39106.A1200", "40063.A600",
    "40068.A1200", "40068.A4000", "40060.CD32", 0 };

// ---- planning ------------------------------------------------------------------------------
static bool add_file(plan_t *p, int node, uint32_t src) {
    if (p->nfiles >= MAX_FILES) return false;
    p->files[p->nfiles].node = node;
    p->files[p->nfiles].src = src;
    p->nfiles++;
    return true;
}

static void strlcat_(char *d, const char *s, size_t n) {
    const size_t a = strlen(d);
    if (a + 1 < n) { strncpy(d + a, s, n - a - 1); d[n - 1] = 0; }
}

int whd_plan(whd_job_t *job, void *arena, uint32_t arena_size) {
    uint8_t *a = (uint8_t *)arena;
    uint32_t off = align8(sizeof(plan_t));
    plan_t *p = arena_plan(arena);
    memset(p, 0, sizeof *p);
    p->lha_work = a + off; off = align8(off + lha_work_size());
    p->small = a + off;    off += SMALL_MAX;
    p->stage = a + off;    off += STAGE;
    p->files = (file_t *)(a + off); off = align8(off + MAX_FILES * sizeof(file_t));
    if (off + 64 * 1024 > arena_size) return WHD_ERR_ARENA;
    p->fs = ffs_begin(a + off, arena_size - off, job->volume, job->unix_time);
    if (!p->fs) return WHD_ERR_ARENA;
    job->slave[0] = job->program[0] = job->command[0] = job->kicks[0] = job->missing[0] = 0;
    job->base_mem = job->exp_mem = 0;
    job->own_startup = false;

    // 1. the archive's members into the tree; note the slaves, icons and programs
    lha_entry_t *e = &p->e;
    uint32_t hoff = 0;
    int r, members = 0;
    uint32_t slave_off = 0, prog_off = 0;
    int slave_depth = 99, prog_depth = 99;
    icon_t *best = &p->best;                      // the icon whose SLAVE= names a slave
    char *best_dir = p->best_dir;
    bool have_icon = false;
    // (no archive: a lone program among the extra files, job->run)
    r = LHA_END;
    while (job->rd && (r = lha_header(job->rd, job->rctx, hoff, e)) == LHA_OK) {
        const uint8_t prot = e->os == 'A' ? e->attr : 0;
        const int id = ffs_add(p->fs, e->path, e->dir, e->size, prot, e->comment, e->unix_time);
        if (id == -1) return WHD_ERR_ARENA;
        if (id >= 0 && !e->dir) {
            if (!add_file(p, id, hoff)) return WHD_ERR_ARENA;
            members++;
            const int d = depth(e->path);
            if (same(e->path, "S/Startup-Sequence")) job->own_startup = true;
            if (ends_with(e->path, ".slave") && d < slave_depth) {
                slave_depth = d; slave_off = hoff;
            } else if (ends_with(e->path, ".info") && e->size <= SMALL_MAX && !have_icon) {
                icon_t *ic = &p->ic;
                memset(ic, 0, sizeof *ic);
                const uint32_t n = read_small(job, p, e, e->size);
                if (icon_tooltypes(p->small, n, tooltype, ic) && ic->slave[0]) {
                    *best = *ic; have_icon = true;
                    const char *sl = strrchr(e->path, '/');
                    const size_t dl = sl ? (size_t)(sl - e->path) : 0;
                    const size_t k = dl < sizeof p->best_dir ? dl : sizeof p->best_dir - 1;
                    memcpy(best_dir, e->path, k);
                    best_dir[k] = 0;
                }
            } else if (!ends_with(e->path, ".info") && e->size >= 32 && d <= 1 && d < prog_depth) {
                if (read_small(job, p, e, 4) == 4 && be32(p->small) == 0x3F3) { prog_depth = d; prog_off = hoff; }
            }
        }
        hoff = e->next_off;
    }
    if (job->rd && (r != LHA_END || !members)) return WHD_ERR_ARCHIVE;

    // 2. what boots
    char *dir = p->dir, *name = p->name;
    dir[0] = name[0] = 0;
    icon_t *opts = &p->opts;
    memset(opts, 0, sizeof *opts);
    if (have_icon) {                             // the icon's slave, if it is there
        char *path = p->path;
        snprintf(path, sizeof p->path, "%s%s%s", best_dir, best_dir[0] ? "/" : "", best->slave);
        const int id = ffs_find(p->fs, path);
        if (id >= 0) {
            for (int i = 0; i < p->nfiles; i++)
                if (p->files[i].node == id) { slave_off = p->files[i].src; break; }
            *opts = *best;
        }
    }
    if (slave_depth < 99 || opts->slave[0]) {
        if (lha_header(job->rd, job->rctx, slave_off, e) != LHA_OK) return WHD_ERR_ARCHIVE;
        snprintf(job->slave, sizeof job->slave, "%s", e->path);
        const uint32_t n = read_small(job, p, e, e->size);
        slave_mem(p->small, n, &job->base_mem, &job->exp_mem);
        // the Kickstarts it names that the caller has
        for (int i = 0; i < job->extra_count; i++) {
            const whd_extra_t *x = &job->extra[i];
            if (x->kick && x->kick[0] && contains(p->small, n, x->kick) && !strstr(job->kicks, x->kick)) {
                if (job->kicks[0]) strlcat_(job->kicks, " ", sizeof job->kicks);
                strlcat_(job->kicks, x->kick, sizeof job->kicks);
            }
        }
        if (!job->kicks[0])
            for (int i = 0; kKicks[i]; i++)
                if (contains(p->small, n, kKicks[i])) {
                    if (job->missing[0]) strlcat_(job->missing, " ", sizeof job->missing);
                    strlcat_(job->missing, kKicks[i], sizeof job->missing);
                }
        const char *sl = strrchr(job->slave, '/');
        if (sl) { memcpy(dir, job->slave, (size_t)(sl - job->slave)); dir[sl - job->slave] = 0; }
        snprintf(name, sizeof p->name, "%s", sl ? sl + 1 : job->slave);
        snprintf(job->command, sizeof job->command, "WHDLoad SLAVE=\"%.120s\" PRELOAD%s", name, opts->opts);
    } else if (prog_depth < 99) {
        if (lha_header(job->rd, job->rctx, prog_off, e) != LHA_OK) return WHD_ERR_ARCHIVE;
        snprintf(job->program, sizeof job->program, "%s", e->path);
        const char *sl = strrchr(job->program, '/');
        if (sl) { memcpy(dir, job->program, (size_t)(sl - job->program)); dir[sl - job->program] = 0; }
        snprintf(name, sizeof p->name, "%s", sl ? sl + 1 : job->program);
        snprintf(job->command, sizeof job->command, "\"%.200s\"", name);
    } else if (job->run && job->run[0]) {                 // the lone program
        snprintf(job->program, sizeof job->program, "%s", job->run);
        snprintf(name, sizeof p->name, "%s", job->run);
        snprintf(job->command, sizeof job->command, "\"%.200s\"", name);
    } else if (!job->own_startup) {
        return WHD_ERR_NOTHING;
    }

    // 3. the startup-sequence
    if (!job->own_startup) {
        int n = snprintf(p->startup, sizeof p->startup,
                         "; pico-amiga: the boot of %s\n"
                         "Stack 16384\n"
                         "CD \"SYS:%s\"\n"
                         "%s%s\n",
                         job->volume, dir, job->slave[0] ? "C:" : "", job->command);
        if (n < 0 || n >= (int)sizeof p->startup) return WHD_ERR_ARENA;
        p->startup_len = (uint32_t)n;
        const int id = ffs_add(p->fs, "S/Startup-Sequence", false, p->startup_len, 0, NULL, job->unix_time);
        if (id < 0 || !add_file(p, id, SRC_START)) return WHD_ERR_ARENA;
    }

    // The system's folders, empty unless something went there: the boot assigns
    // C: S: L: LIBS: DEVS: FONTS: only to folders that exist, and a missing one
    // asks for a volume ("Please insert volume LIBS:") instead of failing.
    static const char *const kDirs[] = { "C", "S", "L", "Libs", "Devs", "Fonts" };
    for (unsigned i = 0; i < sizeof kDirs / sizeof kDirs[0]; i++)
        if (ffs_add(p->fs, kDirs[i], true, 0, 0, NULL, job->unix_time) == -1) return WHD_ERR_ARENA;

    // 4. the extra files (Kickstarts only those the slave named)
    for (int i = 0; i < job->extra_count; i++) {
        const whd_extra_t *x = &job->extra[i];
        if (x->kick && x->kick[0] && !strstr(job->kicks, x->kick)) continue;
        if (!x->data && !x->rd) return WHD_ERR_EXTRA;
        const int id = ffs_add(p->fs, x->path, false, x->size, 0, NULL, job->unix_time);
        if (id == -1) return WHD_ERR_ARENA;
        if (id >= 0 && !add_file(p, id, SRC_EXTRA | (uint32_t)i)) return WHD_ERR_ARENA;
    }

    job->blocks = ffs_layout(p->fs, job->free_blocks ? job->free_blocks : 4096);
    return job->blocks ? WHD_OK : WHD_ERR_TOO_BIG;
}

// ---- writing --------------------------------------------------------------------------------
static int to_ffs(void *ctx, const uint8_t *src, uint32_t len) {
    return ffs_file_data((ffs_t *)ctx, src, len) ? 1 : 0;
}

int whd_write(whd_job_t *job, void *arena, ffs_write_fn wr, void *wctx) {
    plan_t *p = arena_plan(arena);
    if (ffs_write_meta(p->fs, wr, wctx)) return WHD_ERR_WRITE;
    for (int i = 0; i < p->nfiles; i++) {
        const file_t *f = &p->files[i];
        if (ffs_file_begin(p->fs, f->node, wr, wctx, p->stage, STAGE)) return WHD_ERR_WRITE;
        if (f->src == SRC_START) {
            if (ffs_file_data(p->fs, p->startup, p->startup_len)) return WHD_ERR_WRITE;
        } else if (f->src & SRC_EXTRA) {
            const whd_extra_t *x = &job->extra[f->src & ~SRC_EXTRA];
            if (x->data) {
                if (ffs_file_data(p->fs, x->data, x->size)) return WHD_ERR_WRITE;
            } else {
                for (uint32_t o = 0; o < x->size; ) {
                    uint32_t n = x->size - o < SMALL_MAX ? x->size - o : SMALL_MAX;
                    if (x->rd(x->ctx, o, p->small, n) != n) return WHD_ERR_EXTRA;
                    if (ffs_file_data(p->fs, p->small, n)) return WHD_ERR_WRITE;
                    o += n;
                }
            }
        } else {
            lha_entry_t *e = &p->e;
            if (lha_header(job->rd, job->rctx, f->src, e) != LHA_OK) return WHD_ERR_ARCHIVE;
            const int r = lha_unpack(job->rd, job->rctx, e, to_ffs, p->fs, p->lha_work);
            if (r == LHA_ERR_WRITE) return WHD_ERR_WRITE;
            if (r != LHA_OK) return WHD_ERR_ARCHIVE;
        }
        if (ffs_file_end(p->fs)) return WHD_ERR_WRITE;
    }
    return WHD_OK;
}

const char *whd_kick_tag(const char *path, char *out) {
    const char *n = strrchr(path, '/');
    n = n ? n + 1 : path;
    out[0] = 0;
    if (lower((uint8_t)n[0]) != 'k' || lower((uint8_t)n[1]) != 'i' || lower((uint8_t)n[2]) != 'c' || lower((uint8_t)n[3]) != 'k')
        return out;
    n += 4;
    size_t len = strlen(n);
    if (ends_with(n, ".rtb")) len -= 4;
    const char *dot = memchr(n, '.', len);
    if (!dot || dot == n || len >= 24) return out;
    for (const char *c = n; c < dot; c++) if (*c < '0' || *c > '9') return out;
    memcpy(out, n, len);
    out[len] = 0;
    return out;
}

const char *whd_error(int err) {
    switch (err) {
    case WHD_OK: return "ok";
    case WHD_ERR_ARCHIVE: return "a bad or unsupported LHA archive";
    case WHD_ERR_ARENA: return "too many files";
    case WHD_ERR_TOO_BIG: return "too large (over 49 MB)";
    case WHD_ERR_WRITE: return "the image could not be written";
    case WHD_ERR_EXTRA: return "an extra file could not be read";
    case WHD_ERR_NOTHING: return "nothing to boot: no slave, no program";
    default: return "?";
    }
}
