// pico-amiga — the right pane's "Description" block (UiDesc.h): Video > Output > Mode
// tells the refresh and how much of the picture a mode shows; every other
// option draws no block.
#include "UiDesc.h"

#include <stdio.h>
#include <string.h>

#include "UiStage.h"
#include "amiga.h"
#include "machine.h"
#include "roms.h"
extern "C" {
#include "video.h"
}

namespace nm {

static int addLine(DescLine* out, int n, int max, uint8_t kind, const char* label,
                   const char* value, uint8_t state) {
    if (n >= max) return n;
    out[n].kind  = kind;
    out[n].state = state;
    snprintf(out[n].label, sizeof(out[n].label), "%s", label);
    snprintf(out[n].value, sizeof(out[n].value), "%s", value ? value : "");
    return n + 1;
}

static int sub(DescLine* out, int n, int max, const char* name, const char* now, uint8_t st) {
    return addLine(out, n, max, DESC_SUB, name, now, st);
}
static int rule(DescLine* out, int n, int max, const char* text) {
    return addLine(out, n, max, DESC_RULE, text, "", DESC_PLAIN);
}

// 50 Hz is the Amiga's own rate, and not every display takes it; 60 Hz every
// display does, but shows the emulator's 50 frames a second with judder and a
// cut that moves down the screen (the pacing of video_*.c). The picture: the
// framebuffer is 360 x 288 (the 320 x 256 standard screen and its overscan); a
// mode under 576 lines shows 240 of them, one under 720 pixels 320 columns.
static int describeVideoMode(int32_t vm, DescLine* out, int max) {
    video_timing_t t;
    if (!video_mode_timing((int)vm, video_out(), (int)Stage::get(SET_CPU_MHZ), &t)) return 0;
    video_window_t w;
    video_mode_window(&t, 0, &w);
    char buf[20];
    int n = 0;
    snprintf(buf, sizeof buf, "%u Hz", t.hz);
    n = sub(out, n, max, "Refresh", buf, t.hz == 50 ? DESC_PLAIN : DESC_OK);
    if (t.hz == 50) {
        n = rule(out, n, max, "The Amiga's own: smooth");
        n = rule(out, n, max, "Some displays refuse it");
    } else {
        n = rule(out, n, max, vm == VM_640x480_60 ? "Every display takes it" : "Most displays take it");
        n = rule(out, n, max, "50 Amiga frames judder");
        n = rule(out, n, max, "and a cut moves down");
    }
    const bool whole = w.cols == AMIGA_FB_W && w.rows == AMIGA_FB_H;
    snprintf(buf, sizeof buf, "%dx%d", w.cols, w.rows);
    n = sub(out, n, max, "Picture", buf, whole ? DESC_OK : DESC_BAD);
    if (whole) {
        n = rule(out, n, max, w.x || w.y ? "All of it, framed" : "All of it");
    } else {
        char cut[24];
        snprintf(cut, sizeof cut, "%d of 256 lines cut", 256 - w.rows);
        n = rule(out, n, max, cut);
        n = rule(out, n, max, w.cols == AMIGA_FB_W ? "Side overscan kept" : "No side overscan");
    }
    return n;
}

// Devices > Disks > DFn: what the row under the cursor does, and what to know
// about the disk (nothing is read from it here: that is "Enter: what is on it").
static int describeDrive(int d, int32_t tag, DescLine* out, int max) {
    const bool dms = machine_disk_dms(d), ro = machine_disk_ro(d);
    int n = 0;
    char buf[24];
    switch (tag) {
    case DRV_T_INFO:
        n = sub(out, n, max, "Image", dms ? "DMS" : "ADF", DESC_PLAIN);
        if (dms) { n = rule(out, n, max, "Unpacked into PSRAM,"); n = rule(out, n, max, "read-only"); }
        else     n = rule(out, n, max, ro ? "Read-only" : "Saves go to the file");
        n = rule(out, n, max, "Enter: what is on it");
        break;
    case DRV_T_INSERT:
        n = sub(out, n, max, "Takes", d == 0 ? "ADF, DMS, .m3u" : "ADF, DMS", DESC_PLAIN);
        if (d == 0) {
            n = rule(out, n, max, ".m3u: a disk set,");
            n = rule(out, n, max, "Ctrl+F12 swaps them");
        } else {
            n = rule(out, n, max, "A disk set goes to DF0");
        }
        break;
    case DRV_T_EJECT:
        if (d == 0 && machine_set_count() > 1) n = rule(out, n, max, "Ends the disk set");
        n = rule(out, n, max, dms || ro ? "Nothing was written" : "Its writes are saved");
        break;
    case DRV_T_WP:
        n = sub(out, n, max, "Write protect", ro ? "On" : "Off", ro ? DESC_BAD : DESC_OK);
        n = rule(out, n, max, "On: the Amiga cannot");
        n = rule(out, n, max, "write to the disk");
        n = rule(out, n, max, dms ? "A DMS is always On" : "Game saves need Off");
        break;
    default:                                          // a disk of DF0's set
        if (tag < 0 || tag >= machine_set_count()) break;
        snprintf(buf, sizeof buf, "%d of %d", (int)tag + 1, machine_set_count());
        n = sub(out, n, max, "Disk set", buf, DESC_PLAIN);
        for (int k = 1; k < machine_drives(); k++)
            if (!strcmp(machine_disk(k), machine_set_disk((int)tag))) {
                snprintf(buf, sizeof buf, "Now in DF%d", k);
                n = rule(out, n, max, buf);
            }
        n = rule(out, n, max, tag == machine_set_index() ? "In DF0 now" : "Enter puts it in DF0");
        break;
    }
    return n;
}

// Devices > Disks > Floppy drives.
static int describeDrives(int32_t v, DescLine* out, int max) {
    int n = 0;
    if (v == 0) {
        n = rule(out, n, max, "One per disk of an .m3u");
        n = rule(out, n, max, "set (up to 4), else one");
    }
    n = sub(out, n, max, "Kickstart", "", DESC_PLAIN);
    n = rule(out, n, max, "Finds drives at reset:");
    n = rule(out, n, max, "a change resets it");
    n = rule(out, n, max, "Each drive takes memory");
    n = rule(out, n, max, "(Dune II: 1 or 2)");
    return n;
}

// Video > Picture > Position: it moves a window narrower than the picture
// (640x480's 320 columns); wider modes show all 360.
static int describePosition(int32_t v, DescLine* out, int max) {
    int n = 0;
    if (v == 0) {
        n = rule(out, n, max, "The standard 320 px");
        n = rule(out, n, max, "screen in the middle");
    } else {
        n = rule(out, n, max, "12 more columns on the");
        n = rule(out, n, max, "left: for games that");
        n = rule(out, n, max, "draw into the overscan");
    }
    video_timing_t t;
    if (video_mode_timing((int)Stage::get(SET_VIDEO_MODE), video_out(), (int)Stage::get(SET_CPU_MHZ), &t)) {
        video_window_t w;
        video_mode_window(&t, 0, &w);
        if (w.cols == AMIGA_FB_W) {
            n = sub(out, n, max, "This mode", "no effect", DESC_DIM);
            n = rule(out, n, max, "it shows the full width");
        }
    }
    return n;
}

static int describeOnOff(uint16_t setting, int32_t v, DescLine* out, int max) {
    int n = 0;
    switch (setting) {
    case SET_VCENTRE:
        if (v) {
            n = rule(out, n, max, "A picture shorter than");
            n = rule(out, n, max, "the screen (200 lines,");
            n = rule(out, n, max, "NTSC-style) centred");
        } else {
            n = rule(out, n, max, "Short pictures at the");
            n = rule(out, n, max, "top, as on a PAL TV");
        }
        break;
    case SET_DISK_LED:
        n = rule(out, n, max, v ? "A floppy icon top right" : "No icon");
        if (v) n = rule(out, n, max, "while a drive works");
        break;
    case SET_HDMI_CAPTURE:
        if (v) {
            n = rule(out, n, max, "Simpler HDMI signal for");
            n = rule(out, n, max, "USB capture cards that");
            n = rule(out, n, max, "show noise (MS2130)");
        } else {
            n = rule(out, n, max, "Best for TVs and");
            n = rule(out, n, max, "monitors");
        }
        n = sub(out, n, max, "Applies", "after a reboot", DESC_PLAIN);
        break;
    }
    return n;
}

// Machine: what a built-in Kickstart is good for (value: the ROM's index, +200
// on the A500+ row; 100/101 = a file from the card).
static int describeKick(int32_t v, DescLine* out, int max) {
    if (v >= 100 && v < 200) return 0;
    const bool plus = v >= 200;
    const unsigned i = (unsigned)(plus ? v - 200 : v);
    if (i >= builtin_rom_count) return 0;
    const char* id = builtin_roms[i].id;
    int n = 0;
    if (!strcmp(id, "diag")) {
        n = rule(out, n, max, "Hardware test: memory,");
        n = rule(out, n, max, "chips, keyboard, audio");
        n = rule(out, n, max, "(not a Kickstart)");
        return n;
    }
    n = sub(out, n, max, plus ? "A500+" : "A500", plus ? "ECS, 1 MB chip" : "OCS, 512K+512K", DESC_PLAIN);
    if (!strcmp(id, "ks12"))       { n = rule(out, n, max, "For old games that"); n = rule(out, n, max, "do not run on 1.3"); }
    else if (!strcmp(id, "ks13"))  { n = rule(out, n, max, "The A500's own: most"); n = rule(out, n, max, "games expect it"); }
    else if (!strcmp(id, "ks204")) { n = rule(out, n, max, "The A500+'s own;"); n = rule(out, n, max, "Workbench 2.0"); }
    else if (!strcmp(id, "ks205")) { n = rule(out, n, max, "The A600's; Workbench"); n = rule(out, n, max, "2.05"); }
    else if (!strcmp(id, "ks31"))  { n = rule(out, n, max, "Workbench 3.1; some"); n = rule(out, n, max, "old games fail on it"); }
    n = rule(out, n, max, "A change restarts it");
    return n;
}

static int describeOther(uint16_t setting, int32_t v, DescLine* out, int max) {
    int n = 0;
    switch (setting) {
    case SET_AUDIO_OUT:
        switch (v) {
        case 0: n = rule(out, n, max, "An I2S DAC if one is");
                n = rule(out, n, max, "found, else PWM; and");
                n = rule(out, n, max, "HDMI on an HDMI display"); break;
        case 1: n = rule(out, n, max, "The board's audio pins");
                n = rule(out, n, max, "(an RC filter on them)"); break;
        case 2: n = rule(out, n, max, "An I2S DAC (PCM5102)"); break;
        case 3: n = rule(out, n, max, "Sound in the HDMI"); n = rule(out, n, max, "signal only"); break;
        }
        n = sub(out, n, max, "Applies", "after a reboot", DESC_PLAIN);
        break;
    case SET_CPU_MHZ:
        n = sub(out, n, max, "Speed", v >= 504 ? "full" : "not guaranteed", v >= 504 ? DESC_OK : DESC_BAD);
        if (v < 504) { n = rule(out, n, max, "Most likely too slow"); n = rule(out, n, max, "for the emulation"); }
        else           n = rule(out, n, max, "What the Amiga needs");
        n = rule(out, n, max, "Some video modes run");
        n = rule(out, n, max, "at one clock only");
        n = sub(out, n, max, "Applies", "after a reboot", DESC_PLAIN);
        break;
    case SET_VREG:
        if (v < 1600)       { n = rule(out, n, max, "Cooler, but 504 MHz"); n = rule(out, n, max, "may not hold"); }
        else if (v == 1600)   n = rule(out, n, max, "The tested one");
        else                { n = rule(out, n, max, "For a board that hangs"); n = rule(out, n, max, "at 504 MHz; hotter"); }
        n = sub(out, n, max, "Applies", "after a reboot", DESC_PLAIN);
        break;
    case SET_FLASH_FREQ:
    case SET_PSRAM_FREQ:
        n = rule(out, n, max, "Lower it if the board");
        n = rule(out, n, max, "hangs or games crash");
        if (setting == SET_PSRAM_FREQ) n = rule(out, n, max, "(PSRAM: the Amiga RAM)");
        n = sub(out, n, max, "Applies", "after a reboot", DESC_PLAIN);
        break;
    case SET_CURSOR_JOY:
        if (v) {
            n = rule(out, n, max, "Arrows move joystick 2,");
            n = rule(out, n, max, "Left Ctrl is fire; the");
            n = rule(out, n, max, "Amiga no longer gets");
            n = rule(out, n, max, "the arrow keys");
        } else {
            n = rule(out, n, max, "Arrows are keys; use a");
            n = rule(out, n, max, "pad for joystick 2");
        }
        n = rule(out, n, max, "Ctrl+J switches it");
        break;
    case SET_MOUSE_SENS:
        n = rule(out, n, max, "1:1 as the USB mouse");
        n = rule(out, n, max, "reports it");
        break;
    case SET_MOUSE_SWAP:
        if (v == 2) { n = rule(out, n, max, "The first button pushed"); n = rule(out, n, max, "alone becomes the left"); }
        else n = rule(out, n, max, v ? "Left and right swapped" : "As the mouse has them");
        break;
    case SET_UI_VGA_SOLID:
        n = rule(out, n, max, v ? "Flat colours, crisp" : "Mixed dots: all colours");
        n = rule(out, n, max, "(the menu on VGA only)");
        break;
    case SET_DBG_UART:
        n = rule(out, n, max, "115200 baud on the");
        n = rule(out, n, max, "board's log pin: boot");
        n = rule(out, n, max, "log, PERF each second");
        n = sub(out, n, max, "Applies", "after a reboot", DESC_PLAIN);
        break;
    case SET_RENDER_C1:
        n = rule(out, n, max, v ? "Denise on both cores" : "All on core 0: slower,");
        if (!v) n = rule(out, n, max, "for debugging only");
        break;
    }
    return n;
}

int describe(const Node& n, int32_t value, DescLine* out, int max) {
    if (n.setting == SET_KICK) return describeKick(value, out, max);
    if (int c = describeOther(n.setting, value, out, max)) return c;
    if (n.setting == SET_VIDEO_MODE) return describeVideoMode(value, out, max);
    if (n.setting == SET_HPOS) return describePosition(value, out, max);
    if (n.setting == SET_VCENTRE || n.setting == SET_DISK_LED || n.setting == SET_HDMI_CAPTURE)
        return describeOnOff(n.setting, value, out, max);
    if (n.setting == SET_DRIVES) return describeDrives(value, out, max);
    if (n.kind == K_PICK && n.label[0] == 'D' && n.label[1] == 'F')
        return describeDrive(n.label[2] - '0', value, out, max);
    return 0;
}

}  // namespace nm
