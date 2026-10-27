// The machine around the core (machine.h): Kickstart selection and the drives.
#include "machine.h"
#include <string.h>
#include <string>
#include "ff.h"
#include "roms.h"
#include "app/Config.h"

#define KB 1024u

static uint8_t *s_rom_buf, *s_ext_buf;
static const char *s_rom_name = "";

void machine_init(uint8_t *rom_buf, uint8_t *ext_buf) {
    s_rom_buf = rom_buf;
    s_ext_buf = ext_buf;
}

static uint32_t load_file(const char *path, uint8_t *dst, uint32_t max) {
    FIL f;
    if (!dst || f_open(&f, path, FA_READ) != FR_OK) return 0;
    UINT n = 0;
    FSIZE_t size = f_size(&f);
    if (size > max) size = max;
    if (f_read(&f, dst, (UINT)size, &n) != FR_OK) n = 0;
    f_close(&f);
    return n;
}

bool machine_select_rom(amiga_config_t *cfg) {
    cfg->ext_rom = NULL;
    cfg->ext_size = 0;
    if (Config::kick == "sd") {
        const uint32_t rs = load_file("/amiga/kick.rom", s_rom_buf, 512 * KB);
        if (rs == 256 * KB || rs == 512 * KB) {
            cfg->rom = s_rom_buf;
            cfg->rom_size = rs;
            const uint32_t es = load_file("/amiga/ext.rom", s_ext_buf, 512 * KB);
            if (es == 512 * KB) { cfg->ext_rom = s_ext_buf; cfg->ext_size = es; }
            s_rom_name = "kick.rom (SD)";
            return true;
        }
    }
    const builtin_rom_t *r = builtin_rom_find(Config::kick.c_str());
    if (!r && builtin_rom_count) r = &builtin_roms[0];
    if (!r) return false;
    cfg->rom = r->data;
    cfg->rom_size = r->size;
    s_rom_name = r->label;
    return true;
}

bool machine_apply_kickstart(void) {
    amiga_config_t c;
    memset(&c, 0, sizeof c);
    if (!machine_select_rom(&c)) return false;
    amiga_set_roms(c.rom, c.rom_size, c.ext_rom, c.ext_size);
    amiga_reset(true);
    return true;
}

const char *machine_rom_name(void) { return s_rom_name; }

// ---- drives ------------------------------------------------------------------------
// An ADF is streamed from the card: the core reads and writes whole tracks through
// these callbacks, the FIL stays open while the disk is in.
struct Drive {
    FIL f;
    bool open;
    std::string path;
};
static Drive s_drv[4];

static int adf_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_read(f, dst, len, &n) != FR_OK || n != len) return -1;
    return 0;
}
static int adf_write(void *ctx, uint32_t off, const void *src, uint32_t len) {
    FIL *f = (FIL *)ctx; UINT n;
    if (f_lseek(f, off) != FR_OK || f_write(f, src, len, &n) != FR_OK || n != len) return -1;
    f_sync(f);
    return 0;
}

void machine_eject(int drive) {
    if (drive < 0 || drive > 3) return;
    Drive &d = s_drv[drive];
    amiga_eject_disk(drive);
    if (d.open) f_close(&d.f);
    d.open = false;
    d.path.clear();
}

bool machine_insert(int drive, const char *path, bool write_protect) {
    if (drive != 0 || !path || !*path) return false;      // the core wires DF0 only
    machine_eject(drive);
    Drive &d = s_drv[drive];
    bool rw = !write_protect && f_open(&d.f, path, FA_READ | FA_WRITE) == FR_OK;
    if (!rw && f_open(&d.f, path, FA_READ) != FR_OK) return false;
    if (f_size(&d.f) != 901120) {                          // 880 KB DD ADF only
        f_close(&d.f);
        return false;
    }
    d.open = true;
    d.path = path;
    amiga_disk_t disk = { adf_read, rw ? adf_write : NULL, &d.f, (uint32_t)f_size(&d.f) };
    amiga_insert_disk(drive, &disk);
    return true;
}

const char *machine_disk(int drive) {
    return (drive >= 0 && drive <= 3) ? s_drv[drive].path.c_str() : "";
}

void machine_restore_disks(void) {
    if (!Config::df0.empty()) {
        if (!machine_insert(0, Config::df0.c_str(), Config::df0_wp)) Config::df0.clear();
        return;
    }
    // The card layout before the menu existed: /amiga/df0.adf goes in by itself.
    machine_insert(0, "/amiga/df0.adf", false);
}
