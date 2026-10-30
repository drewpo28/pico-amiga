// pico-amiga snapshot (.pas) - pico-speccy's .pss, for the Amiga.
//
//   "PAS1" ver_major ver_minor model 0       8-byte header (model: Config::machine)
//   blocks: id[4] size(u32 LE) data[size]    unknown ids are skipped
//
//   NAME  64 bytes, zero padded, first: renamed in place (setName)
//   CFG   key=value lines: the model, the Kickstart (Config::kick / kick_file and
//         the CRC-32s of the ROM and extended ROM), Floppy drives, and per drive
//         its disk, write protection, whether it was put there by hand, and the
//         file's size and date (a disk changed since is reported on load); the
//         .m3u set and its disk in DF0
//   then the core's blocks (amiga_state_save, src/amiga/state.c), MACH first.
//
// The disks' contents are not in the file: an ADF is the card's, and a snapshot
// of a game that wrote its disk afterwards finds the disk as it is now.
//
// Cold code (flash): the menu and the hot keys run it, between frames.
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "amiga.h"
#include "machine.h"
#include "app/Config.h"

namespace Snap {

static const uint8_t VER_MAJOR = 1, VER_MINOR = 0;

// ---- the file, through a 512-byte buffer -----------------------------------------
// The core's blocks reach chip and slow RAM, which are PSRAM on the board: they
// go through this SRAM buffer, never straight between FatFs and PSRAM (the SD
// driver's DMA reading PSRAM stalls the scanout's).
struct Io {
    FIL f;
    uint8_t buf[512];
    UINT len, pos;          // read: bytes in buf and the next one; write: bytes in buf
    bool ok;
};

static void io_flush(Io* io) {
    UINT bw = 0;
    if (io->len && (f_write(&io->f, io->buf, io->len, &bw) != FR_OK || bw != io->len)) io->ok = false;
    io->len = 0;
}

static bool io_write(void* ctx, const void* p, uint32_t n) {
    Io* io = (Io*)ctx;
    const uint8_t* s = (const uint8_t*)p;
    while (n && io->ok) {
        const UINT k = n < sizeof io->buf - io->len ? (UINT)n : (UINT)(sizeof io->buf - io->len);
        memcpy(io->buf + io->len, s, k);
        io->len += k; s += k; n -= k;
        if (io->len == sizeof io->buf) io_flush(io);
    }
    return io->ok;
}

static uint32_t io_read(void* ctx, void* p, uint32_t n) {
    Io* io = (Io*)ctx;
    uint8_t* d = (uint8_t*)p;
    uint32_t got = 0;
    while (n) {
        if (io->pos == io->len) {
            io->pos = 0;
            if (f_read(&io->f, io->buf, sizeof io->buf, &io->len) != FR_OK) io->len = 0;
            if (!io->len) break;
        }
        const UINT k = n < io->len - io->pos ? (UINT)n : io->len - io->pos;
        memcpy(d, io->buf + io->pos, k);
        io->pos += k; d += k; n -= k; got += k;
    }
    return got;
}

// Where the next io_read byte is in the file.
static FSIZE_t io_tell(Io* io) { return f_tell(&io->f) - (io->len - io->pos); }

static bool io_seek(Io* io, FSIZE_t at) {
    io->len = io->pos = 0;
    return f_lseek(&io->f, at) == FR_OK;
}

static Io* io_open(const char* path, BYTE mode) {
    Io* io = (Io*)calloc(1, sizeof(Io));
    if (!io) return nullptr;
    if (f_open(&io->f, path, mode) != FR_OK) { free(io); return nullptr; }
    io->ok = true;
    return io;
}

static void io_close(Io* io) {
    f_close(&io->f);
    free(io);
}

static void put_u32(uint8_t* b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t* b) {
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

static bool put_block(Io* io, const char* id, const void* data, uint32_t n) {
    uint8_t h[8];
    memcpy(h, id, 4);
    put_u32(h + 4, n);
    return io_write(io, h, 8) && io_write(io, data, n);
}

// ---- CFG ------------------------------------------------------------------------------
struct Cfg {
    int machine = 0;
    std::string kick, kick_file;
    uint32_t rom_crc = 0, ext_crc = 0;
    int drives = 0;
    bool df0_wp = false;
    MachineDisks disks;
    uint32_t size[4] = {}, stamp[4] = {};   // the disks' files as they were (stamp: fdate << 16 | ftime)
};

static std::string cfgText() {
    MachineDisks d;
    machine_get_disks(d);
    uint32_t rom = 0, ext = 0;
    machine_rom_crcs(Config::kick, Config::kick_file, &rom, &ext);
    char l[64];
    std::string t;
    const auto kv = [&](const char* k, const std::string& v) { t += k; t += '='; t += v; t += '\n'; };
    const auto kn = [&](const char* k, unsigned v) { snprintf(l, sizeof l, "%u", v); kv(k, l); };
    kn("machine", Config::machine);
    kv("kick", Config::kick);
    kv("kick_file", Config::kick_file);
    snprintf(l, sizeof l, "%08X", (unsigned)rom); kv("rom_crc", l);
    snprintf(l, sizeof l, "%08X", (unsigned)ext); kv("ext_crc", l);
    kn("drives", Config::drives);
    kn("df0_wp", Config::df0_wp);
    kv("set", d.set);
    kn("set_index", (unsigned)d.set_index);
    for (int i = 0; i < 4; i++) {
        if (d.path[i].empty()) continue;
        char k[16];
        snprintf(k, sizeof k, "df%d", i);         kv(k, d.path[i]);
        snprintf(k, sizeof k, "df%d_wp", i);      kn(k, d.wp[i]);
        snprintf(k, sizeof k, "df%d_manual", i);  kn(k, d.manual[i]);
        FILINFO fi;
        if (f_stat(d.path[i].c_str(), &fi) == FR_OK) {
            snprintf(k, sizeof k, "df%d_file", i);
            snprintf(l, sizeof l, "%u:%u", (unsigned)fi.fsize, (unsigned)((fi.fdate << 16) | fi.ftime));
            kv(k, l);
        }
    }
    return t;
}

static void cfgParse(const std::string& t, Cfg& c) {
    c.disks.set.clear();
    c.disks.set_index = 0;
    for (int i = 0; i < 4; i++) { c.disks.path[i].clear(); c.disks.wp[i] = c.disks.manual[i] = false; }
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        const std::string line = t.substr(p, e - p);
        p = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const unsigned long n = strtoul(v.c_str(), nullptr, 10);
        if (k == "machine") c.machine = (int)n;
        else if (k == "kick") c.kick = v;
        else if (k == "kick_file") c.kick_file = v;
        else if (k == "rom_crc") c.rom_crc = (uint32_t)strtoul(v.c_str(), nullptr, 16);
        else if (k == "ext_crc") c.ext_crc = (uint32_t)strtoul(v.c_str(), nullptr, 16);
        else if (k == "drives") c.drives = (int)n;
        else if (k == "df0_wp") c.df0_wp = n != 0;
        else if (k == "set") c.disks.set = v;
        else if (k == "set_index") c.disks.set_index = (int)n;
        else if (k.size() >= 3 && k.compare(0, 2, "df") == 0 && k[2] >= '0' && k[2] <= '3') {
            const int d = k[2] - '0';
            const std::string f = k.substr(3);
            if (f.empty()) c.disks.path[d] = v;
            else if (f == "_wp") c.disks.wp[d] = n != 0;
            else if (f == "_manual") c.disks.manual[d] = n != 0;
            else if (f == "_file") {
                c.size[d] = (uint32_t)n;
                const size_t colon = v.find(':');
                if (colon != std::string::npos) c.stamp[d] = (uint32_t)strtoul(v.c_str() + colon + 1, nullptr, 10);
            }
        }
    }
}

// ---- reading the header and our blocks ------------------------------------------------
// The header, NAME and CFG; `core` = where the core's blocks start.
static bool readHead(Io* io, std::string* name, Cfg* cfg, FSIZE_t* core) {
    uint8_t h[8];
    if (io_read(io, h, 8) != 8 || memcmp(h, "PAS1", 4) || h[4] != VER_MAJOR) return false;
    bool named = false, configured = false;
    for (;;) {
        const FSIZE_t at = io_tell(io);
        if (io_read(io, h, 8) != 8) return false;
        const uint32_t n = get_u32(h + 4);
        if (!memcmp(h, "MACH", 4)) { if (core) *core = at; break; }
        if (!memcmp(h, "NAME", 4) && n == SNAP_NAME_LEN) {
            char b[SNAP_NAME_LEN + 1] = {};
            if (io_read(io, b, SNAP_NAME_LEN) != SNAP_NAME_LEN) return false;
            if (name) *name = b;
            named = true;
            if (!cfg && !core) return true;
        } else if (!memcmp(h, "CFG ", 4) && n < 4096) {
            std::string t(n, '\0');
            if (io_read(io, &t[0], n) != n) return false;
            if (cfg) cfgParse(t, *cfg);
            configured = true;
            if (!core) break;
        } else if (!io_seek(io, at + 8 + n)) {
            return false;
        }
    }
    return named && configured;
}

// ---- API --------------------------------------------------------------------------------
std::string slotPath(int slot) {
    char p[48];
    snprintf(p, sizeof p, SNAP_DIR "/persist%d.pas", slot);
    return p;
}

std::string defaultName() {
    MachineDisks d;
    machine_get_disks(d);
    std::string n = !d.set.empty() ? d.set : d.path[0];
    n = n.substr(n.find_last_of('/') + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);
    return n.substr(0, SNAP_NAME_LEN - 1);
}

bool save(const std::string& path, const std::string& name) {
    f_mkdir("/amiga");
    f_mkdir(SNAP_DIR);
    const std::string tmp = path + ".tmp";
    Io* io = io_open(tmp.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    if (!io) return false;
    const uint8_t h[8] = { 'P', 'A', 'S', '1', VER_MAJOR, VER_MINOR, (uint8_t)Config::machine, 0 };
    char nm[SNAP_NAME_LEN] = {};
    strncpy(nm, name.c_str(), SNAP_NAME_LEN - 1);
    const std::string cfg = cfgText();
    const amiga_stream_t st = { io_write, nullptr, io };
    bool ok = io_write(io, h, 8) && put_block(io, "NAME", nm, SNAP_NAME_LEN) &&
              put_block(io, "CFG ", cfg.data(), (uint32_t)cfg.size()) && amiga_state_save(&st);
    if (ok) { io_flush(io); ok = io->ok; }
    io_close(io);
    if (ok) {
        f_unlink(path.c_str());
        ok = f_rename(tmp.c_str(), path.c_str()) == FR_OK;
    }
    if (!ok) f_unlink(tmp.c_str());
    return ok;
}

bool readName(const std::string& path, std::string& name) {
    Io* io = io_open(path.c_str(), FA_READ);
    if (!io) return false;
    const bool ok = readHead(io, &name, nullptr, nullptr);
    io_close(io);
    return ok;
}

bool setName(const std::string& path, const std::string& name) {
    std::string old;
    if (!readName(path, old)) return false;
    FIL f;
    if (f_open(&f, path.c_str(), FA_READ | FA_WRITE) != FR_OK) return false;
    char nm[SNAP_NAME_LEN] = {};
    strncpy(nm, name.c_str(), SNAP_NAME_LEN - 1);
    UINT bw = 0;
    const bool ok = f_lseek(&f, 16) == FR_OK && f_write(&f, nm, SNAP_NAME_LEN, &bw) == FR_OK && bw == SNAP_NAME_LEN;
    f_close(&f);
    return ok;
}

static const char* baseName(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return p.c_str() + (s == std::string::npos ? 0 : s + 1);
}

void describe(const std::string& path, char* out, unsigned size) {
    if (!size) return;
    out[0] = 0;
    Io* io = io_open(path.c_str(), FA_READ);
    std::string name;
    Cfg* c = new Cfg;
    const bool ok = io && readHead(io, &name, c, nullptr);
    if (io) io_close(io);
    unsigned pos = 0;
    const auto add = [&](const char* fmt, auto... a) {
        if (pos < size) pos += snprintf(out + pos, size - pos, fmt, a...);
    };
    if (!ok) {
        add("Not a pico-amiga snapshot, or a damaged one.\n");
    } else {
        add(" Name      : %s\n", name.empty() ? "(none)" : name.c_str());
        add(" Machine   : %s\n", c->machine == 1 ? "A500+ (ECS, 1 MB chip RAM)" : "A500 (OCS, 512 KB chip + 512 KB slow)");
        add(" Kickstart : %s\n", machine_rom_label(c->kick, c->kick_file).c_str());
        if (!c->disks.set.empty())
            add(" Disk set  : %s, disk %d\n", baseName(c->disks.set), c->disks.set_index + 1);
        for (int i = 0; i < 4; i++)
            if (!c->disks.path[i].empty())
                add(" DF%d       : %s%s\n", i, baseName(c->disks.path[i]), c->disks.wp[i] ? " (protected)" : "");
        FILINFO fi;
        if (f_stat(path.c_str(), &fi) == FR_OK)
            add(" Saved     : %04u-%02u-%02u %02u:%02u\n", (fi.fdate >> 9) + 1980u, (fi.fdate >> 5) & 15u,
                fi.fdate & 31u, fi.ftime >> 11, (fi.ftime >> 5) & 63u);
    }
    delete c;
}

bool load(const std::string& path, std::string& err, std::string& warn) {
    err.clear();
    warn.clear();
    Io* io = io_open(path.c_str(), FA_READ);
    if (!io) { err = "The snapshot cannot be opened"; return false; }
    Cfg* c = new Cfg;
    FSIZE_t core = 0;
    if (!readHead(io, nullptr, c, &core) || c->machine < 0 || c->machine > 1) {
        err = "Not a pico-amiga snapshot, or a damaged one";
        delete c; io_close(io);
        return false;
    }
    // The Kickstart first: a snapshot without its own ROM changes nothing.
    uint32_t rom = 0, ext = 0;
    if (!machine_rom_crcs(c->kick, c->kick_file, &rom, &ext) || rom != c->rom_crc || ext != c->ext_crc) {
        err = "It needs " + machine_rom_label(c->kick, c->kick_file) + (c->kick == "sd" ? " (a Custom ROM)" : "") +
              ", not found";
        delete c; io_close(io);
        return false;
    }

    Config::machine = (uint8_t)c->machine;
    Config::kick = c->kick;
    Config::kick_file = c->kick_file;
    Config::drives = (uint8_t)c->drives;
    Config::df0_wp = c->df0_wp;
    Config::df0 = !c->disks.set.empty() ? c->disks.set : c->disks.path[0];
    Config::df0_disk = (uint8_t)c->disks.set_index;
    amiga_config_t ac;
    memset(&ac, 0, sizeof ac);
    machine_select_rom(&ac);
    amiga_set_roms(ac.rom, ac.rom_size, ac.ext_rom, ac.ext_size);
    amiga_set_machine(ac.chipset, ac.chip_size, ac.slow_size);

    std::string missing, changed;
    machine_put_disks(c->disks, missing);
    for (int i = 0; i < 4; i++) {
        const std::string& p = c->disks.path[i];
        FILINFO fi;
        if (p.empty() || !c->size[i] || p != machine_disk(i) || f_stat(p.c_str(), &fi) != FR_OK) continue;
        if (fi.fsize != c->size[i] || (uint32_t)((fi.fdate << 16) | fi.ftime) != c->stamp[i]) {
            changed += changed.empty() ? "" : ", ";
            changed += baseName(p);
        }
    }
    if (machine_disk(0)[0] == 0 && c->disks.set.empty()) Config::df0.clear();

    const amiga_stream_t st = { nullptr, io_read, io };
    const bool ok = io_seek(io, core) && amiga_state_load(&st);
    io_close(io);
    delete c;
    if (!ok) {
        amiga_reset(true);
        err = "The snapshot is damaged: the Amiga was reset";
    }
    if (!missing.empty()) warn = "Missing: " + missing;
    if (!changed.empty()) warn += (warn.empty() ? "" : "; ") + std::string("Changed since: ") + changed;
    Config::save();
    return ok;
}

} // namespace Snap
