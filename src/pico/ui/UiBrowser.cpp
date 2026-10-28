// pico-amiga (from pico-speccy) — fullscreen file browser of the new UI (see UiBrowser.h).
// PICO-AMIGA: pico-speccy's browser minus its emulator verbs (to slot, unzip, new
// TRD/MGT, play, the FileInfo pages) and its network flows; file types are a small
// table here (BrowseType) instead of FileUtils::fileTypes.
//
//  +-----------------------------------------------------+
//  | [rainbow] Open file                          12/345 |  header
//  |-----------------------------------------------------|
//  | SD:/amiga/games/                                     |  path bar
//  |----------------------------------+------------------|
//  | .. (up)                        » | game.tap         |  info pane: full name,
//  | demos                          » | Tape image       |  size, date, then the
//  | GAME.TAP                       * | 48,562 bytes     |  verbs that apply to
//  | readme.txt                       | 2026-07-12 14:03 |  the focused entry
//  |   ...                            | ⏎ Open  F1 Info  |
//  |----------------------------------+------------------|
//  | ^v Move  ⏎ Open  << Up  F3 Find  Esc Close          |  footer / search field
//  +-----------------------------------------------------+
//
// Directory data comes from the shared sorted_files index (SortedFiles.h): a flat
// file under /tmp, one record per name, dirs first (DIR_MARKER prefix). Nothing
// here allocates proportionally to the directory size — pages are fetched row by
// row from the index, exactly like the classic dialog, so a 5000-file directory
// costs the same RAM as an empty one.

#include "OSDNewMenu.h"


#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <string>
#include <vector>

#include "UiBrowser.h"
#include "machine.h"
#include "UiGfx.h"
#include "UiFont.h"
#include "UiDialog.h"
#include <stdlib.h>
#include "UiRender.h"        // SYM_* glyph names
#include "UiPlatform.h"
#include "SortedFiles.h"

using std::string;

namespace nm {

// ── file types (pico-speccy's FileUtils::fileTypes, cut down) ──────────────────
// The extensions a type shows bright, and its remembered position and search.
// Rows are offset by 2 as in pico-speccy (begin_row/focus are its 2-based form).
struct BrowseTypeState {
    const char* fileExts;
    int    begin_row = 2, focus = 2;
    int    fdMode = 0;
    string fileSearch;
};
static BrowseTypeState s_types[BROWSE_COUNT] = {
    { "" },                                      // BROWSE_ALL
    { ".adf,.dms,.m3u" },                        // BROWSE_ADF (an .m3u is a disk set)
    { ".rom,.bin,.a500,.a600,.a1000,.cdtv" },    // BROWSE_ROM (.A500 etc.: PUAE's names)
    { "" },                                      // BROWSE_CFG
};

static string lcExt(const string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == string::npos) return string();
    string e = name.substr(dot + 1);
    for (auto& c : e) c = (char)tolower((uint8_t)c);
    return e;
}

// Delete a folder and everything under it.
static void deleteDirRecursive(const string& path) {
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, path.c_str()) == FR_OK) {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            const string sub = path + "/" + fi.fname;
            if (fi.fattrib & AM_DIR) deleteDirRecursive(sub);
            else f_unlink(sub.c_str());
        }
        f_closedir(&d);
    }
    f_unlink(path.c_str());
}

// ── state ──────────────────────────────────────────────────────────────────────

static sorted_files s_idx;

static string  s_dir;             // current directory, always ends with '/'
// Highest directory this session may go up to, "" for the volume root. Set by
// browseFile's `root` argument (Debug > Config folders): the config tree is opened
// at a fixed folder, and walking out of it into the rest of the card would defeat
// the point of the level above, which offers exactly the two system folders.
static string  s_root;
static string  s_title;
static uint8_t s_ftype;
static std::vector<string> s_exts;   // ".tap", ".sna", ... (with the dot)

static int  s_total;              // records in the index (incl. the ".." row)
static unsigned s_ndirs, s_nfiles;
static int  s_visTotal;           // rows the filter lets through
static int  s_sel, s_top;         // selection / first visible row (vis domain)
static bool s_search;             // incremental filter active
static string s_query;

static string s_goto;             // name to land on after a rescan

// Marquee for an over-long focused name: after ~1 s idle the visible window
// slides one character every ~200 ms, holds on the tail, restarts. Any keypress
// resets it. The focused name is cached here so the idle tick never re-reads
// the index from SD.
static std::string s_mq_name;     // display form of the focused row
static int s_mq_avail;            // its pixel budget in the list column
static int s_mq_off, s_mq_idle, s_mq_tick;

static void mqReset() { s_mq_off = 0; s_mq_idle = 0; s_mq_tick = 0; }

// Advance the marquee during an idle tick; true = the focused row needs repaint.
static bool mqTick() {
    const int fits = s_mq_avail / glyphW();
    if (fits <= 0 || (int)s_mq_name.size() <= fits) return false;
    if (s_mq_idle < 200) { s_mq_idle++; return false; }   // ~1 s before it starts
    if (++s_mq_tick < 40) return false;                   // ~200 ms per step
    s_mq_tick = 0;
    if ((int)s_mq_name.size() - s_mq_off <= fits) {       // tail shown: hold, restart
        s_mq_off = 0;
        s_mq_idle = 0;
    } else {
        s_mq_off++;
    }
    return true;
}

// Draw a (possibly scrolled) focused name: hard window, no ".." while sliding.
static void mqText(int x, int y, int availw, const string& nm_, UiColor ink) {
    if (s_mq_off > 0) {
        const int fits = availw / glyphW();
        string vis = (int)nm_.size() > s_mq_off ? nm_.substr((size_t)s_mq_off) : nm_;
        if ((int)vis.size() > fits) vis.resize((size_t)fits);
        text(x, y, vis.c_str(), ink);
    } else {
        textClip(x, y, availw, nm_.c_str(), ink);
    }
}

// Directory position stack, so backing out of a folder restores the cursor.
static constexpr int POS_STACK_MAX = 16;
static struct { int top, sel; } s_stack[POS_STACK_MAX];
static int s_stack_top;

// ── layout ─────────────────────────────────────────────────────────────────────

struct BrowserLayout {
    int ix, iy, iw, ih;           // window content rect
    int margin;
    int hdr_h, path_h, foot_h;
    int body_y, body_h, row_h, rows;
    int lx, lw;                   // list pane
    int sep_x;
    int rx, rw;                   // info pane
    int pad;
};
static BrowserLayout L;

static void computeBL() {
    const int sc = Sf.glyphScale;
    L.pad    = 2 * sc;
    L.margin = 2 * sc;
    L.ix = L.margin + sc;
    L.iy = 3;
    L.iw = Sf.w - 2 * L.ix;
    L.ih = Sf.h - L.iy - 3;

    L.hdr_h  = UI_FONT_H + 6;
    L.path_h = UI_FONT_H + 4;
    L.foot_h = UI_FONT_H + 4;
    L.row_h  = UI_FONT_H + 2;

    L.body_y = L.iy + L.hdr_h + L.path_h;
    L.body_h = L.iy + L.ih - L.foot_h - L.body_y;
    L.rows   = L.body_h / L.row_h;

    // Info pane: a fixed character budget; the list takes the rest.
    const int ichars = (cols() >= 52) ? 17 : 14;
    L.rw    = ichars * glyphW() + 2 * L.pad;
    L.rx    = L.ix + L.iw - L.rw;
    L.sep_x = L.rx - sc;
    L.lx    = L.ix;
    L.lw    = L.sep_x - L.lx;
}

// ── filter (incremental search) ────────────────────────────────────────────────
// Directories always pass; files pass on a case-insensitive substring match.
// Same semantics as the classic fdMode search, over the same index.

static bool nameMatches(const string& rec) {
    if (s_query.empty() || (uint8_t)rec[0] == DIR_MARKER) return true;
    char up[FF_LFN_BUF + 1];
    size_t i = 0;
    for (; i < rec.size() && i < FF_LFN_BUF; i++) up[i] = (char)toupper((uint8_t)rec[i]);
    up[i] = 0;
    return strstr(up, s_query.c_str()) != nullptr;
}

static void recountVisible() {
    if (s_query.empty()) { s_visTotal = s_total; return; }
    s_visTotal = 0;
    for (int i = 0; i < s_total; i++)
        if (nameMatches(s_idx.get(i))) s_visTotal++;
}

// vis row -> index record. Identity without a filter; a linear scan with one.
static int visToIdx(int v) {
    if (v < 0 || v >= s_visTotal) return -1;
    if (s_query.empty()) return v;
    int seen = 0;
    for (int i = 0; i < s_total; i++)
        if (nameMatches(s_idx.get(i)))
            if (seen++ == v) return i;
    return -1;
}

static string visName(int v) {
    const int i = visToIdx(v);
    return i < 0 ? string() : s_idx.get(i);
}

// ── small helpers ──────────────────────────────────────────────────────────────

static string displayPath(const string& fdir) {
    if (fdir.find(':') != string::npos) return fdir;
    return "SD:" + fdir;
}

static bool isDirRec(const string& rec)  { return !rec.empty() && (uint8_t)rec[0] == DIR_MARKER; }
static bool isUpRec(const string& rec)   { return rec.size() > 1 && (uint8_t)rec[0] == DIR_MARKER
                                                              && (uint8_t)rec[1] == DIR_MARKER; }
static string plainName(const string& rec) {
    size_t i = 0;
    while (i < rec.size() && (uint8_t)rec[i] == DIR_MARKER) i++;
    return rec.substr(i);
}

// Two orthogonal questions the ftype answers. `manageMode` = the housekeeping verbs
// (info / rename / new dir / delete) are offered; `pickMode` = Enter on a file
// returns it to the caller. The full F5 browser is both; Debug > Config folders is
// housekeeping only; a per-type picker is neither (it just picks).
static bool manageMode() { return s_ftype == BROWSE_ALL || s_ftype == BROWSE_CFG; }
static bool pickMode()   { return s_ftype != BROWSE_CFG; }

static bool extMatches(const string& name) {
    // No extension list (BROWSE_ALL / BROWSE_CFG): nothing here is more interesting than
    // anything else, so every name draws in the normal ink instead of all-dim.
    if (s_exts.empty()) return true;
    const size_t dot = name.find_last_of('.');
    if (dot == string::npos) return false;
    string e = name.substr(dot);
    for (auto& c : e) c = (char)tolower((uint8_t)c);                // KICK.ROM, kick40063.A600
    for (const auto& x : s_exts) if (e == x) return true;
    return false;
}

static const char* typeLabel(const string& lcext) {
    if (lcext == "adf")                                     return "Amiga disk";
    if (lcext == "dms")                                     return "Amiga disk (DMS)";
    if (lcext == "m3u")                                     return "Disk set";      // PICO-AMIGA
    if (lcext == "adz" || lcext == "ipf")                   return "Disk (unsupported)";
    if (lcext == "rom" || lcext == "bin" || lcext == "a500" || lcext == "a600" ||
        lcext == "a1000" || lcext == "cdtv")                return "ROM image";
    if (lcext == "hdf")                                     return "Hard disk image";
    if (lcext == "lha" || lcext == "lzh" || lcext == "zip") return "Archive";
    if (lcext == "mod")                                     return "Module";
    if (lcext == "nvs")                                     return "Saved config";
    if (lcext == "cfg" || lcext == "ini")                   return "Config file";
    if (lcext == "log")                                     return "Log file";
    if (lcext == "txt" || lcext == "doc" || lcext == "md")  return "Text file";
    return "File";
}

// Files the built-in viewer will show. Everything pico-speccy itself writes under
// CONFIG_DIR is line-oriented text (NvsWriter's key=value, wifi.cfg, remotes.tsv,
// debug.log, cacert.pem), which is exactly what is worth reading on the device.
static bool viewableExt(const string& lcext) {
    static const char* const kExt[] = { "nvs", "cfg", "tsv", "log", "txt", "pem",
                                        "csv", "ini", "md", "diz", "nfo", "me", "1st",
                                        "asm", "a80", "inc", "json", "xml", "htm", "html",
                                        "doc", "bat", "sh", "c", "h", "cpp", "py", "lst" };
    for (const char* e : kExt) if (lcext == e) return true;
    return false;
}

// F1 in the full browser: a file shown as text when its head looks like text (no
// NUL, almost all printable / whitespace) — READ.ME, FILE_ID, a readme without an
// extension.
static bool looksLikeText(const string& path) {
    FIL* f = (FIL*)malloc(sizeof(FIL) + 512);
    if (!f) return false;
    uint8_t* b = (uint8_t*)(f + 1);
    UINT br = 0;
    bool text = false;
    if (f_open(f, path.c_str(), FA_READ) == FR_OK) {
        if (f_read(f, b, 512, &br) == FR_OK && br > 0) {
            UINT bad = 0;
            text = true;
            for (UINT i = 0; i < br; i++) {
                const uint8_t c = b[i];
                if (c == 0) { text = false; break; }
                if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != 0x1A) bad++;
            }
            if (bad * 20 > br) text = false;   // more than 5% control bytes
        }
        f_close(f);
    }
    free(f);
    return text;
}

// ── drawing ────────────────────────────────────────────────────────────────────

static void drawChrome() {
    fill(0, 0, Sf.w, Sf.h, C_BG);
    roundRect(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_PANEL);
    vline(L.sep_x, L.body_y, L.body_h, C_SEP);
}

static void drawHeader() {
    const int y = L.iy;
    fill(L.ix, y, L.iw, L.hdr_h, C_PANEL);
    rainbow(L.ix + L.pad, y + 3);
    int tx = L.ix + L.pad + rainbowW() + 2 * L.pad;
    tx += text(tx, y + 4, s_title.c_str(), C_WHITE);
    char cnt[20];
    if (s_visTotal > 0)
        snprintf(cnt, sizeof(cnt), "%d/%d", s_sel + 1, s_visTotal);
    else
        snprintf(cnt, sizeof(cnt), "empty");
    const int cx = L.ix + L.iw - textWidth(cnt) - L.pad;
    text(cx, y + 4, cnt, C_TEXT_DIM);
    uiHeaderClock(L.ix, L.iw, y + 4, tx + 2 * L.pad, cx - 2 * L.pad);
    hline(L.ix, y + L.hdr_h - 1, L.iw, C_SEP);
}

static void drawPathBar() {
    const int y = L.iy + L.hdr_h;
    fill(L.ix, y, L.iw, L.path_h, C_PANEL_ALT);
    const string p = displayPath(s_dir);
    // Long paths keep their tail (the deepest dirs are what orients the user).
    const int fits = (L.iw - 2 * L.pad) / glyphW();
    if ((int)p.size() > fits && fits > 2)
        text(L.ix + L.pad, y + 2, (".." + p.substr(p.size() - fits + 2)).c_str(), C_TEXT);
    else
        text(L.ix + L.pad, y + 2, p.c_str(), C_TEXT);
    hline(L.ix, y + L.path_h - 1, L.iw, C_SEP);
}

static void drawListRow(int visRow) {
    if (visRow < 0 || visRow >= L.rows) return;
    const int y = L.body_y + visRow * L.row_h;
    const int v = s_top + visRow;
    const bool sel = (v == s_sel) && (v < s_visTotal);

    // Room for the scrollbar hairline at the right edge of the pane.
    const int sb = (s_visTotal > L.rows) ? 2 * Sf.glyphScale : 0;
    fill(L.lx, y, L.lw - sb, L.row_h, sel ? C_SEL_BG : C_PANEL);
    if (v >= s_visTotal) return;

    const string rec = visName(v);
    if (rec.empty()) return;
    const bool dir = isDirRec(rec);
    string nm_ = isUpRec(rec) ? ".." : plainName(rec);

    UiColor ink;
    if (sel)        ink = C_WHITE;
    else if (dir)   ink = C_TEXT;
    else            ink = extMatches(nm_) ? C_TEXT : C_TEXT_DIM;

    const int markW = dir ? (chevronW() + L.pad) : 0;
    const int availw = L.lw - sb - 2 * L.pad - markW;
    if (sel) {
        s_mq_name = nm_;                  // cache for the idle marquee tick
        s_mq_avail = availw;
        mqText(L.lx + L.pad, y + 1, availw, nm_, ink);
    } else {
        textClip(L.lx + L.pad, y + 1, availw, nm_.c_str(), ink);
    }
    if (dir)
        chevron(L.lx + L.lw - sb - L.pad - chevronW(), y + L.row_h / 2 - 3,
                sel ? C_WHITE : C_DISABLED);
}

static void drawScrollbar() {
    if (s_visTotal <= L.rows) return;
    const int sc = Sf.glyphScale;
    const int x = L.sep_x - 2 * sc;
    fill(x, L.body_y, sc, L.body_h, C_PANEL_ALT);
    int th = L.body_h * L.rows / s_visTotal;
    if (th < 6) th = 6;
    const int span = L.body_h - th;
    const int maxTop = s_visTotal - L.rows;
    const int ty = L.body_y + (maxTop > 0 ? span * s_top / maxTop : 0);
    fill(x, ty, sc, th, C_DISABLED);
}

static void drawList() {
    for (int r = 0; r < L.rows; r++) drawListRow(r);
    drawScrollbar();
}

// The info pane: everything known about the focused entry, then the verbs.
static void drawInfo() {
    const int x = L.rx, w = L.rw;
    fill(x, L.body_y, w, L.body_h, C_PANEL);
    int y = L.body_y + 2;
    const int lh = UI_FONT_H + 2;
    const int tx = x + L.pad, tw = w - 2 * L.pad;
    const int fits = tw / glyphW();

    const string rec = visName(s_sel);
    if (rec.empty()) {
        text(tx, y, "No files", C_TEXT_DIM);
        return;
    }
    const bool dir = isDirRec(rec);
    const bool up  = isUpRec(rec);
    const string nm_ = up ? ".." : plainName(rec);

    // Full name, wrapped over up to 3 lines (the list may have clipped it).
    for (int ln = 0; ln < 3 && ln * fits < (int)nm_.size(); ln++) {
        text(tx, y, nm_.substr((size_t)ln * fits, fits).c_str(), C_WHITE);
        y += lh;
    }
    y += 2;

    if (up) {
        text(tx, y, "Parent folder", C_TEXT_DIM);
    } else if (dir) {
        text(tx, y, "Folder", C_TEXT_DIM);
    } else {
        const string lc = lcExt(nm_);
        text(tx, y, typeLabel(lc), extMatches(nm_) ? C_ACCENT : C_TEXT_DIM);
        y += lh;
        FILINFO fi;
        if (f_stat((s_dir + nm_).c_str(), &fi) == FR_OK) {
            char buf[24];
            if (fi.fsize < 10000)
                snprintf(buf, sizeof(buf), "%u bytes", (unsigned)fi.fsize);
            else if (fi.fsize < 10u * 1024 * 1024)
                snprintf(buf, sizeof(buf), "%u KB", (unsigned)(fi.fsize >> 10));
            else
                snprintf(buf, sizeof(buf), "%u MB", (unsigned)(fi.fsize >> 20));
            text(tx, y, buf, C_TEXT);
            y += lh;
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                     (fi.fdate >> 9) + 1980, (fi.fdate >> 5) & 15, fi.fdate & 31,
                     fi.ftime >> 11, (fi.ftime >> 5) & 63);
            text(tx, y, buf, C_TEXT_DIM);
        }
    }
    y += lh + 3;

    // Verbs from the bottom up would jump around; a fixed block reads better.
    hline(tx, y, tw, C_SEP);
    y += 3;
    struct Verb { const char* k; const char* what; bool on; };
    const bool all = (s_ftype == BROWSE_ALL);
    const bool mng = manageMode();
    const bool view = !dir && !pickMode() && viewableExt(lcExt(nm_));
    const Verb verbs[] = {
        // Nothing in the config tree is runnable: Enter there reads a text file and
        // does nothing at all for anything else, so it is advertised accordingly.
        { SYM_ENTER, dir ? "Open" : (view ? "View" : "Choose"), dir || pickMode() || view },
        // PICO-AMIGA: F1 on a disk image tells what is on it (machine_disk_info).
        { "F1", (lcExt(nm_) == "adf" || lcExt(nm_) == "dms") ? "Info" : "View", all && !dir },
        // PICO-AMIGA: Win+F5's verb (pico-speccy's "To slot"): a disk image into a drive.
        { "F5", "To drive", all && !dir && (lcExt(nm_) == "adf" || lcExt(nm_) == "dms") },
        { "F3", "Find",     true },
        { "F6", "Rename",   mng && !up },
        { "F7", "New dir",  mng },
        { "F8", "Delete",   mng && !up },
        { "F9", "New ADF",  all },   // PICO-AMIGA: pico-speccy's New TRD
    };
    for (const auto& vb : verbs) {
        if (!vb.on) continue;
        if (y + lh > L.body_y + L.body_h) break;
        const int kw = textWidth(vb.k);
        text(tx, y, vb.k, C_TEXT_DIM);
        text(tx + kw + glyphW(), y, vb.what, C_TEXT);
        y += lh;
    }
}

static void drawFooter() {
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y, L.iw, L.foot_h, C_FOOT_BG);
    hline(L.ix, y, L.iw, C_SEP);
    if (s_search) {
        const int lx = L.ix + L.pad;
        text(lx, y + 3, "Find:", C_ACCENT);
        const int qx = lx + 6 * glyphW();
        string q = s_query + "_";
        textClip(qx, y + 3, L.ix + L.iw - qx - L.pad, q.c_str(), C_WHITE);
    } else {
        text(L.ix + L.pad, y + 3,
             SYM_UP SYM_DOWN " Move  " SYM_ENTER " Open  " SYM_LEFT " Up  F3 Find  Esc Close",
             C_TEXT_DIM);
    }
}

// Bands square the window corners off — the border re-pass restores them.
static void frameFix() {
    roundRectBorder(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_BG);
}

static void drawAll() {
    drawChrome();
    drawHeader();
    drawPathBar();
    drawList();
    drawInfo();
    drawFooter();
    frameFix();
}

// A transient status line over the footer (indexing / deleting progress).
static void status(const char* msg, int percent) {
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y + 1, L.iw, L.foot_h - 1, C_FOOT_BG);
    text(L.ix + L.pad, y + 3, msg, C_TEXT);
    if (percent >= 0) {
        const int bx = L.ix + L.iw / 2, bw = L.iw / 2 - 2 * L.pad;
        frame(bx, y + 3, bw, UI_FONT_H - 2, C_SEP);
        fill(bx + 1, y + 4, (bw - 2) * percent / 100, UI_FONT_H - 4, C_ACCENT);
    }
}

// ── indexing ───────────────────────────────────────────────────────────────────
// Same cache discipline as the classic dialog: count + content-CRC first, rebuild
// the .idx only when the directory really changed. F1 aborts a long pass.

static bool abortKey() {
    uiKeyAvailable();                     // runs the input devices
    return uiKeyboard()->isVKDown(fabgl::VK_F1);
}

static bool indexCurrent() {
    Debug::log("UiBrowser: index '%s'\n", s_dir.c_str());
    s_idx.init(s_dir);
    Debug::log("UiBrowser: idx open=%d cached=%u\n", (int)s_idx.is_open(), (unsigned)s_idx.size());
    s_ndirs = 0; s_nfiles = 0;

    DIR d;
    FILINFO fi;
    if (f_opendir(&d, s_dir.c_str()) != FR_OK) {
        Debug::log("UiBrowser: f_opendir failed\n");
        return false;
    }

    status("Scanning...", -1);
    size_t crc = SORT_VERSION;
    const bool withUp = s_dir.size() > 1;
    if (withUp) { s_ndirs++; crc += sf_name_crc(string(2, DIR_MARKER) + ".."); }

    FRESULT frd;
    while ((frd = f_readdir(&d, &fi)) == FR_OK && fi.fname[0] != '\0') {
        if (abortKey()) break;
        const string fname = fi.fname;
        if (fname.compare(0, 1, ".") == 0) continue;
        if (fi.fattrib & AM_DIR) { s_ndirs++;  crc += sf_name_crc(char(DIR_MARKER) + fname); }
        else                     { s_nfiles++; crc += sf_name_crc(fname); }
    }
    f_closedir(&d);
    if (frd != FR_OK)
        Debug::log("UiBrowser: f_readdir('%s') err=%d\n", s_dir.c_str(), frd);
    Debug::log("UiBrowser: counted %u dirs %u files\n", s_ndirs, s_nfiles);

    const size_t idx_crc = s_idx.crc();
    Debug::log("UiBrowser: crc live=%u idx=%u -> %s\n", (unsigned)crc, (unsigned)idx_crc,
               idx_crc != crc ? "REBUILD" : "cached");
    if (idx_crc != crc) {                        // rebuild
        s_idx.unlink();
        if (withUp) s_idx.push(string(2, DIR_MARKER) + "..");
        if (f_opendir(&d, s_dir.c_str()) != FR_OK) return false;
        const unsigned expect = s_ndirs + s_nfiles;
        unsigned done = 0;
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0') {
            if (abortKey()) break;
            const string fname = fi.fname;
            if (fname.compare(0, 1, ".") == 0) continue;
            s_idx.push((fi.fattrib & AM_DIR) ? char(DIR_MARKER) + fname : fname);
            if ((++done & 15) == 0 && expect)
                status("Indexing...", (int)(done * 90u / expect));
        }
        f_closedir(&d);
        Debug::log("UiBrowser: pushed %u records, sorting\n", (unsigned)s_idx.size());
        status("Sorting...", 95);
        s_idx.sort();
        Debug::log("UiBrowser: sort done\n");
    }
    s_total = (int)s_idx.size();
    Debug::log("UiBrowser: total=%d\n", s_total);
    return true;
}

// Land the cursor on s_goto (or its alphabetic neighbour when it was deleted).
static void applyGoto() {
    if (s_goto.empty()) return;
    int found = -1;
    for (int i = 0; i < s_total; i++) {
        if (s_idx.get(i) == s_goto) { found = i; break; }
    }
    if (found < 0) {
        found = 0;
        for (int i = 0; i < s_total; i++) {
            if (s_idx.get(i) >= s_goto) { found = i; break; }
            found = i;
        }
    }
    s_goto.clear();
    // The goto target is an index position; with no filter vis == idx (a rescan
    // clears the filter only when the user left search mode, so map it).
    if (!s_query.empty()) {
        int v = 0;
        for (int i = 0; i < found && i < s_total; i++)
            if (nameMatches(s_idx.get(i))) v++;
        found = v;
    }
    s_sel = found < s_visTotal ? found : (s_visTotal ? s_visTotal - 1 : 0);
}

static void clampView() {
    if (s_sel >= s_visTotal) s_sel = s_visTotal ? s_visTotal - 1 : 0;
    if (s_sel < 0) s_sel = 0;
    if (s_top > s_sel) s_top = s_sel;
    if (s_sel >= s_top + L.rows) s_top = s_sel - L.rows + 1;
    const int maxTop = s_visTotal - L.rows;
    if (s_top > maxTop) s_top = maxTop > 0 ? maxTop : 0;
    if (s_top < 0) s_top = 0;
}

// ── movement ───────────────────────────────────────────────────────────────────

static void moveSel(int delta) {
    if (!s_visTotal) return;
    int ns = s_sel + delta;
    if (ns < 0) ns = 0;
    if (ns > s_visTotal - 1) ns = s_visTotal - 1;
    if (ns == s_sel) return;
    const int oldTop = s_top, oldSel = s_sel;
    s_sel = ns;
    clampView();
    if (s_top != oldTop) drawList();
    else { drawListRow(oldSel - s_top); drawListRow(s_sel - s_top); drawScrollbar(); }
    drawHeader();
    drawInfo();
}

// ── modal helpers ──────────────────────────────────────────────────────────────

// Ask for a line of text in the footer. Returns false on Esc/empty.
static bool footerAsk(const char* label, string& io) {
    const int y = L.iy + L.ih - L.foot_h;
    fill(L.ix, y + 1, L.iw, L.foot_h - 1, C_FOOT_BG);
    text(L.ix + L.pad, y + 3, label, C_ACCENT);
    const int fx = L.ix + L.pad + (int)(strlen(label) + 1) * glyphW();
    const bool ok = uiEditLine(fx, y + 3, L.ix + L.iw - fx - L.pad, io, FF_LFN_BUF - 8);
    drawFooter();
    return ok && !io.empty();
}

// ── text viewer ────────────────────────────────────────────────────────────────
// Enter on a plain-text file in the config browser shows it here. The file is
// STREAMED: the only thing held that grows with it is a 4-bytes-per-line offset
// index, so a debug.log costs a few KB however long it got. Both axes scroll
// (lines are up to VIEW_LINE_MAX display columns wide, tabs expanded so a .tsv
// lines up), Esc leaves. The FIL handle, the read window and the index are two
// heap blocks freed on every exit path — nothing here is permanently
// resident, and none of it is on the stack (a FIL alone is ~570 B of the 8 KB
// core0 stack, under an already deep menu chain).

#define VIEW_LINE_MAX  512      // display columns kept per line
#define VIEW_RAW       512      // bytes read per line (beyond this a line is cut)
#define VIEW_MAX_LINES 8192     // index ceiling, ~32 KB before the fallbacks below
#define VIEW_MAX_SCAN  (2u << 20)   // never scan more than 2 MB looking for lines
#define VIEW_HSTEP     8        // columns per Left/Right

// Text encodings the viewer knows. The UI font draws CP1251 (the Web catalog's
// Cyrillic), so CP866 — what nearly every ZX text is in — and UTF-8 are
// converted to it per character.
enum TextEnc : uint8_t { ENC_ASCII, ENC_CP866, ENC_CP1251, ENC_UTF8 };

// CP866 -> CP1251, one character. Box drawing becomes ASCII lines so tables in
// ZX docs keep their shape.
static uint8_t cp866Char(uint8_t c) {
    static const uint8_t kHi[16] = { 0xA8, 0xB8, 0xAA, 0xBA, 0xAF, 0xBF, 0xA1, 0xA2,
                                     0xB0, 0x95, 0xB7, '.', 0xB9, 0xA4, '#', ' ' };
    if (c < 0x80) return c;
    if (c <= 0xAF) return (uint8_t)(c + 0x40);                 // А-Я а-п
    if (c >= 0xE0 && c <= 0xEF) return (uint8_t)(c + 0x10);    // р-я
    if (c >= 0xF0) return kHi[c - 0xF0];
    if (c <= 0xB2 || c >= 0xDB) return '#';                    // shades, blocks
    if (c == 0xB3 || c == 0xBA) return '|';
    if (c == 0xC4 || c == 0xCD) return '-';
    return '+';                                                // corners, tees, crosses
}

// Pick the encoding from the bytes over 0x7F: UTF-8 when they pair up as
// Cyrillic lead/continuation bytes; else CP866 when а-п (0xA0-0xAF) outnumber
// р-я in CP1251 (0xF0-0xFF) — each is the other's rarest range.
struct EncCount {
    uint32_t hi = 0, utf = 0, a0 = 0, f0 = 0;
    uint8_t  prev = 0;
    void add(uint8_t c) {
        if (c >= 0x80) {
            hi++;
            if ((prev == 0xD0 || prev == 0xD1) && c >= 0x80 && c <= 0xBF) utf++;
            if (c >= 0xA0 && c <= 0xAF) a0++;
            if (c >= 0xF0) f0++;
        }
        prev = c;
    }
    TextEnc pick() const {
        if (!hi) return ENC_ASCII;
        if (utf * 4 >= hi) return ENC_UTF8;   // every Cyrillic letter is 2 bytes, half continuation
        return a0 > f0 ? ENC_CP866 : ENC_CP1251;
    }
};

static void viewTextFile(const string& path, const string& name) {
    // FIL is ~570 B (it carries a 512-byte sector window) and core0's stack is 8 KB
    // with the menu chain already on it, so the handle lives in the block too.
    const size_t scratchBytes = sizeof(FIL) + VIEW_RAW + VIEW_LINE_MAX + 1;
    uint8_t* blk = (uint8_t*)malloc(scratchBytes);
    if (!blk) { uiToast("Not enough memory", true, 1500); return; }
    FIL*  f    = (FIL*)blk;
    char* raw  = (char*)(blk + sizeof(FIL));
    char* disp = raw + VIEW_RAW;

    if (f_open(f, path.c_str(), FA_READ) != FR_OK) {
        free(blk);
        uiToast("Cannot open file", true, 1500);
        return;
    }

    // Pass 1: count lines and measure the widest one, in DISPLAY columns (tabs
    // expanded exactly as the renderer will expand them, or the pan limit lies).
    status("Reading...", -1);
    uint32_t nlines = 0, col = 0, maxw = 0, scanned = 0;
    EncCount enc;
    bool cut = false;                       // file longer than we are willing to index
    bool lineOpen = false;                  // bytes seen since the last newline
    for (;;) {
        UINT br = 0;
        if (f_read(f, raw, VIEW_RAW, &br) != FR_OK || !br) break;
        scanned += br;
        for (UINT i = 0; i < br; i++) {
            const char c = raw[i];
            enc.add((uint8_t)c);
            if (c == '\n') { if (col > maxw) maxw = col; col = 0; nlines++; lineOpen = false; continue; }
            lineOpen = true;
            if (c == '\r') continue;
            col = (c == '\t') ? ((col + 8) & ~7u) : col + 1;
            if (col > VIEW_LINE_MAX) col = VIEW_LINE_MAX;
        }
        if (nlines >= VIEW_MAX_LINES || scanned >= VIEW_MAX_SCAN) { cut = true; break; }
        if (abortKey()) { cut = true; break; }
    }
    if (lineOpen) { if (col > maxw) maxw = col; nlines++; }   // last line without \n
    const TextEnc te = enc.pick();
    if (nlines > VIEW_MAX_LINES) nlines = VIEW_MAX_LINES;

    // Pass 2: the offset index. A tight heap gets a shorter file rather than no
    // file at all — the head is what a config or a log is read for.
    uint32_t* off = nullptr;
    uint32_t   cap = nlines;
    while (cap && !(off = (uint32_t*)malloc(cap * 4)))
        cap /= 2;
    if (nlines && !off) {                 // an empty file legitimately needs none
        f_close(f);
        free(blk);
        uiToast("Not enough memory", true, 1500);
        return;
    }
    if (cap < nlines) { nlines = cap; cut = true; }

    f_lseek(f, 0);
    uint32_t n = 0, pos = 0;
    if (n < nlines) off[n++] = 0;
    while (n < nlines) {   // off is non-null whenever nlines is non-zero
        UINT br = 0;
        if (f_read(f, raw, VIEW_RAW, &br) != FR_OK || !br) break;
        for (UINT i = 0; i < br && n < nlines; i++)
            if (raw[i] == '\n') off[n++] = pos + i + 1;
        pos += br;
    }
    nlines = n;

    // One display line, read on demand from its indexed offset.
    auto readLine = [&](uint32_t li) -> int {
        f_lseek(f, off[li]);
        UINT br = 0;
        if (f_read(f, raw, VIEW_RAW, &br) != FR_OK) br = 0;
        int dl = 0;
        for (UINT i = 0; i < br && dl < VIEW_LINE_MAX; i++) {
            const char c = raw[i];
            if (c == '\n') break;
            if (c == '\r') continue;
            if (c == '\t') {            // to the next 8-column stop
                do { disp[dl++] = ' '; } while (dl < VIEW_LINE_MAX && (dl & 7));
                continue;
            }
            uint8_t u = (uint8_t)c;
            if (u >= 0x80) {
                if (te == ENC_CP866) u = cp866Char(u);
                else if (te == ENC_UTF8) {
                    // Two-byte sequences: Cyrillic to CP1251, the rest '?'; a
                    // longer one is skipped whole.
                    const uint8_t n1 = i + 1 < br ? (uint8_t)raw[i + 1] : 0;
                    if ((u & 0xE0) == 0xC0 && (n1 & 0xC0) == 0x80) {
                        const unsigned cp = ((u & 0x1F) << 6) | (n1 & 0x3F);
                        u = cp >= 0x0410 && cp <= 0x044F ? (uint8_t)(0xC0 + (cp - 0x0410))
                          : cp == 0x0401 ? 0xA8 : cp == 0x0451 ? 0xB8 : '?';
                        i++;
                    } else if ((u & 0xF0) == 0xE0) { i += 2; u = '?'; }
                    else if ((u & 0xF8) == 0xF0)   { i += 3; u = '?'; }
                    else u = '.';
                }
                else if (te != ENC_CP1251) u = '.';
                // 0x80-0x87 are the UI's own glyphs in this font.
                if (u >= 0x80 && u <= UI_FONT_LAST) u = '.';
            }
            // A file that turned out not to be text stays readable as a shape
            // instead of painting control codes through the font.
            disp[dl++] = (u >= 32 && u != 127) ? (char)u : '.';
        }
        disp[dl] = 0;
        return dl;
    };

    const int lh    = L.row_h;
    const int rows  = L.body_h / lh;
    const int tw    = L.iw - 2 * L.pad;
    const int cols_ = tw / glyphW();
    const int maxTop = (int)nlines > rows ? (int)nlines - rows : 0;
    const int maxCol = (int)maxw > cols_ ? (int)maxw - cols_ : 0;
    int top = 0, hoff = 0;

    auto drawViewChrome = [&]() {
        fill(0, 0, Sf.w, Sf.h, C_BG);
        roundRect(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_PANEL);
        // header: name + rainbow, like every other page of this UI
        fill(L.ix, L.iy, L.iw, L.hdr_h, C_PANEL);
        rainbow(L.ix + L.pad, L.iy + 3);
        textClip(L.ix + L.pad + rainbowW() + 2 * L.pad, L.iy + 4,
                 L.iw - 4 * L.pad - rainbowW(), name.c_str(), C_WHITE);
        hline(L.ix, L.iy + L.hdr_h - 1, L.iw, C_SEP);
        // path bar: the folder it came from, plus "cut" when the index is short
        const int py = L.iy + L.hdr_h;
        fill(L.ix, py, L.iw, L.path_h, C_PANEL_ALT);
        string sub = displayPath(s_dir);
        if (cut) sub += "  (first " + std::to_string((unsigned)nlines) + " lines)";
        static const char* const kEnc[] = { "", "  CP866", "  CP1251", "  UTF-8" };
        sub += kEnc[te];
        textClip(L.ix + L.pad, py + 2, L.iw - 2 * L.pad, sub.c_str(), C_TEXT);
        hline(L.ix, py + L.path_h - 1, L.iw, C_SEP);
        const int fy = L.iy + L.ih - L.foot_h;
        fill(L.ix, fy, L.iw, L.foot_h, C_FOOT_BG);
        hline(L.ix, fy, L.iw, C_SEP);
        text(L.ix + L.pad, fy + 3,
             SYM_UP SYM_DOWN " Scroll  " SYM_LEFT SYM_RIGHT " Pan  Esc Close", C_TEXT_DIM);
        roundRectBorder(L.margin, L.iy - 1, Sf.w - 2 * L.margin, L.ih + 2, 4, C_SEP, C_BG);
    };

    auto drawViewBody = [&]() {
        for (int r = 0; r < rows; r++) {
            const int y = L.body_y + r * lh;
            fill(L.ix, y, L.iw, lh, C_PANEL);
            const uint32_t li = (uint32_t)(top + r);
            if (li >= nlines) continue;
            const int dl = readLine(li);
            if (dl > hoff) textClip(L.ix + L.pad, y + 1, tw, disp + hoff, C_TEXT);
        }
        // Position, right-aligned in the footer: line span, and the pan offset
        // only while panning (a column number on an unpanned file is noise).
        const int fy = L.iy + L.ih - L.foot_h;
        char posbuf[28];
        if (hoff)
            snprintf(posbuf, sizeof(posbuf), "%u-%u/%u +%d", (unsigned)(top + 1),
                     (unsigned)((uint32_t)(top + rows) < nlines ? top + rows : (int)nlines),
                     (unsigned)nlines, hoff);
        else
            snprintf(posbuf, sizeof(posbuf), "%u-%u/%u", (unsigned)(top + 1),
                     (unsigned)((uint32_t)(top + rows) < nlines ? top + rows : (int)nlines),
                     (unsigned)nlines);
        const int pw = textWidth(posbuf);
        fill(L.ix + L.iw - pw - 2 * L.pad, fy + 1, pw + 2 * L.pad, L.foot_h - 2, C_FOOT_BG);
        text(L.ix + L.iw - pw - L.pad, fy + 3, posbuf, C_TEXT_DIM);
    };

    drawViewChrome();
    if (!nlines) {
        fill(L.ix, L.body_y, L.iw, L.body_h, C_PANEL);
        text(L.ix + L.pad, L.body_y + 2, "(empty file)", C_TEXT_DIM);
    } else {
        drawViewBody();
    }

    // The Enter that opened this must not close it.
    { fabgl::VirtualKeyItem d; while (uiKeyAvailable()) uiReadKey(&d); }
    fabgl::VirtualKeyItem k;
    while (1) {
        if (!uiKeyAvailable()) {
            uiSleepMs(5);
            continue;
        }
        if (!uiReadKey(&k) || !k.down) continue;
        int nt = top, nh = hoff;
        switch (k.vk) {
            case fabgl::VK_MENU_UP:    nt = top - 1; break;
            case fabgl::VK_MENU_DOWN:  nt = top + 1; break;
            case fabgl::VK_PAGEUP:     nt = top - rows; break;
            case fabgl::VK_PAGEDOWN:   nt = top + rows; break;
            case fabgl::VK_MENU_HOME:
            case fabgl::VK_HOME:       nt = 0; nh = 0; break;
            case fabgl::VK_END:        nt = maxTop; break;
            // Left is "back" everywhere else in this UI; here both arrows pan,
            // which is why the footer names Esc as the only way out.
            case fabgl::VK_MENU_LEFT:  nh = hoff - VIEW_HSTEP; break;
            case fabgl::VK_MENU_RIGHT: nh = hoff + VIEW_HSTEP; break;
            case fabgl::VK_ESCAPE: case fabgl::VK_F1: case fabgl::VK_MENU_ENTER:
                OSD::clickNoPause();
                f_close(f);
                free(off);
                free(blk);
                return;
            default: break;
        }
        if (nt < 0) nt = 0;
        if (nt > maxTop) nt = maxTop;
        if (nh < 0) nh = 0;
        if (nh > maxCol) nh = maxCol;
        if (nt != top || nh != hoff) {
            top = nt; hoff = nh;
            drawViewBody();
            OSD::clickNoPause();
        }
    }
}

// ── main loop ──────────────────────────────────────────────────────────────────

static string runLoop() {
    BrowseTypeState& ft = s_types[s_ftype];

    // Adopt the classic position memory (rows there are offset by 2).
    s_top    = ft.begin_row - 2; if (s_top < 0) s_top = 0;
    s_sel    = s_top + (ft.focus - 2); if (s_sel < 0) s_sel = 0;
    s_search = ft.fdMode != 0;
    s_query  = ft.fileSearch;
    for (auto& c : s_query) c = (char)toupper((uint8_t)c);
    s_stack_top = 0;
    s_goto.clear();

    // One exit point keeps the position write-back and index close together.
    auto leave = [&](const string& r) -> string {
        ft.begin_row  = s_top + 2;
        ft.focus      = s_sel - s_top + 2;
        ft.fdMode     = s_search ? 1 : 0;
        ft.fileSearch = s_query;
        s_idx.close();
        return r;
    };

    while (1) {                                       // per directory
        DIR probe;
        if (f_opendir(&probe, s_dir.c_str()) != FR_OK) {
            const string back = s_root.empty() ? string("/") : s_root;
            Debug::log("UiBrowser: cannot open '%s', falling back to %s\n",
                       s_dir.c_str(), back.c_str());
            s_dir = back;
            s_sel = s_top = 0;
            if (f_opendir(&probe, s_dir.c_str()) != FR_OK) return leave("");
        }
        f_closedir(&probe);

        drawAll();
        if (!indexCurrent()) return leave("");
        recountVisible();
        applyGoto();
        clampView();
        mqReset();
        drawAll();
        Debug::log("UiBrowser: ready vis=%d sel=%d top=%d\n", s_visTotal, s_sel, s_top);

        fabgl::VirtualKeyItem k;
        while (1) {                                   // keys
            if (!uiKeyAvailable()) {
                uiIdle();
                if (mqTick()) drawListRow(s_sel - s_top);
                if (uiClockDirty()) drawHeader();
                continue;
            }
            if (!uiReadKey(&k) || !k.down) continue;
            if (s_mq_off) { mqReset(); drawListRow(s_sel - s_top); }
            else mqReset();

            const string rec = visName(s_sel);
            const bool onDir  = isDirRec(rec);
            const bool onUp   = isUpRec(rec);
            const string name = onUp ? ".." : plainName(rec);

            // Movement keys are too chatty; everything else is worth a watermark.
            switch (k.vk) {
                case fabgl::VK_MENU_UP: case fabgl::VK_MENU_DOWN:
                case fabgl::VK_PAGEUP:  case fabgl::VK_PAGEDOWN:
                case fabgl::VK_MENU_HOME: case fabgl::VK_HOME: case fabgl::VK_END:
                    break;
                default:
                    Debug::log("UiBrowser: key vk=%d sp=%08x\n", (int)k.vk, debug_sp());
                    break;
            }

            switch (k.vk) {
                case fabgl::VK_MENU_UP:    moveSel(-1); OSD::clickNoPause(); continue;
                case fabgl::VK_MENU_DOWN:  moveSel(+1); OSD::clickNoPause(); continue;
                case fabgl::VK_PAGEUP:     moveSel(-L.rows); OSD::clickNoPause(); continue;
                case fabgl::VK_PAGEDOWN:   moveSel(+L.rows); OSD::clickNoPause(); continue;
                case fabgl::VK_MENU_HOME:
                case fabgl::VK_HOME:       moveSel(-s_visTotal); OSD::clickNoPause(); continue;
                case fabgl::VK_END:        moveSel(+s_visTotal); OSD::clickNoPause(); continue;
                default: break;
            }

            // ── going up: Left, Backspace, or Enter/Right on ".." ──────────────
            const bool wantUp =
                (k.vk == fabgl::VK_MENU_LEFT) ||
                (k.vk == fabgl::VK_MENU_BS && !s_search) ||
                ((k.vk == fabgl::VK_MENU_ENTER || k.vk == fabgl::VK_MENU_RIGHT) && onUp);
            if (wantUp) {
                Debug::log("UiBrowser: UP from '%s' (stack=%d)\n", s_dir.c_str(), s_stack_top);
                OSD::clickNoPause();
                // The ceiling is the session root when one was given, the volume
                // root otherwise.
                const bool atTop = s_root.empty() ? (s_dir == "/" || s_dir == "USB:/")
                                                  : (s_dir == s_root);
                if (atTop) {
                    if (!s_root.empty() || s_dir == "/") continue;   // nowhere higher
                    // Out of the stick to the SD root (per-type dialog case).
                    s_dir = "/";
                } else {
                    s_dir.pop_back();
                    s_dir = s_dir.substr(0, s_dir.find_last_of('/') + 1);
                }
                if (s_stack_top > 0) { s_top = s_stack[--s_stack_top].top; s_sel = s_stack[s_stack_top].sel; }
                else { s_top = s_sel = 0; }
                Debug::log("UiBrowser: UP -> '%s' top=%d sel=%d\n", s_dir.c_str(), s_top, s_sel);
                break;                                // rescan the parent
            }

            // ── enter: descend or pick ─────────────────────────────────────────
            if (k.vk == fabgl::VK_MENU_ENTER || k.vk == fabgl::VK_MENU_RIGHT) {
                if (!s_visTotal) continue;
                OSD::clickNoPause();
                if (onDir && !onUp) {
                    if (s_stack_top < POS_STACK_MAX) s_stack[s_stack_top++] = { s_top, s_sel };
                    s_dir += name + "/";
                    s_sel = s_top = 0;
                    Debug::log("UiBrowser: ENTER -> '%s'\n", s_dir.c_str());
                    break;                            // rescan the child
                }
                if (k.vk == fabgl::VK_MENU_RIGHT) continue;   // Right only navigates
                if (!pickMode()) {
                    // Nothing in the config tree is runnable, so Enter reads the
                    // file instead when it is one we can render as text.
                    if (viewableExt(lcExt(name))) {
                        viewTextFile(s_dir + name, name);
                        drawAll();
                    }
                    continue;
                }
                return leave("R" + name);
            }

            // ── search mode ────────────────────────────────────────────────────
            if (k.vk == fabgl::VK_F3) {
                s_search = !s_search;
                if (!s_search && !s_query.empty()) {
                    s_query.clear();
                    recountVisible();
                    s_sel = s_top = 0;
                    drawList(); drawInfo(); drawHeader();
                }
                drawFooter();
                OSD::clickNoPause();
                continue;
            }
            if (s_search && k.vk == fabgl::VK_MENU_BS) {
                if (!s_query.empty()) {
                    s_query.pop_back();
                    recountVisible();
                    s_sel = s_top = 0;
                    drawList(); drawInfo(); drawHeader(); drawFooter();
                    OSD::clickNoPause();
                }
                continue;
            }

            // ── per-file verbs ─────────────────────────────────────────────────
            // `all` = the emulator's own verbs (unzip / to slot / new TRD), full
            // browser only; `mng` = housekeeping, also on in Debug > Config folders.
            const bool all = (s_ftype == BROWSE_ALL);
            const bool mng = manageMode();
            // F1 shows a text file in the full browser; everywhere else F1 keeps
            // its usual "close" meaning.
            if (all && k.vk == fabgl::VK_F1 && !onDir && s_visTotal) {
                OSD::clickNoPause();
                // PICO-AMIGA: a disk image: its volume, files and startup-sequence.
                if (lcExt(name) == "adf" || lcExt(name) == "dms") {
                    if (lcExt(name) == "dms") status("Unpacking...", -1);
                    char* info = (char*)malloc(2048);
                    if (info) {
                        machine_disk_info((s_dir + name).c_str(), info, 2048);
                        uiTextPage(name.c_str(), info);
                        free(info);
                    }
                    drawAll();
                    continue;
                }
                if (viewableExt(lcExt(name)) || looksLikeText(s_dir + name)) {
                    viewTextFile(s_dir + name, name);
                    drawAll();
                } else {
                    uiToast("Not a text file", false, 1200);
                    drawAll();
                }
                continue;
            }
            if (!all && k.vk == fabgl::VK_F1) return leave("");
            // PICO-AMIGA: F5 on a disk image = the drive picker (runOpenFile).
            if (all && k.vk == fabgl::VK_F5 && !onDir && s_visTotal &&
                    (lcExt(name) == "adf" || lcExt(name) == "dms"))
                return leave("P" + name);
            if (k.vk == fabgl::VK_ESCAPE) return leave("");

            if (mng && k.vk == fabgl::VK_F6 && !onUp && s_visTotal) {   // rename
                Debug::log("UiBrowser: F6 rename '%s' sp=%08x\n", name.c_str(), debug_sp());
                OSD::clickNoPause();
                string nn = name;
                if (footerAsk("Rename:", nn) && nn != name) {
                    f_rename((s_dir + name).c_str(), (s_dir + nn).c_str());
                    s_goto = onDir ? char(DIR_MARKER) + nn : nn;
                    break;
                }
                continue;
            }
            if (mng && k.vk == fabgl::VK_F7) {                          // mkdir
                OSD::clickNoPause();
                string nn;
                if (footerAsk("New dir:", nn)) {
                    f_mkdir((s_dir + nn).c_str());
                    s_goto = char(DIR_MARKER) + nn;
                    break;
                }
                continue;
            }
            if (mng && (k.vk == fabgl::VK_F8 || k.vk == fabgl::VK_DELETE)
                    && !onUp && s_visTotal) {                           // delete
                OSD::clickNoPause();
                char q[80];
                snprintf(q, sizeof(q), "Delete %s\n%.40s ?", onDir ? "folder" : "file",
                         name.c_str());
                const bool yes = uiConfirm(q);
                drawAll();
                if (yes) {
                    if (onDir) {
                        status("Deleting...", -1);
                        deleteDirRecursive(s_dir + name);
                        s_goto = char(DIR_MARKER) + name;
                    } else {
                        f_unlink((s_dir + name).c_str());
                        s_goto = name;
                    }
                    break;
                }
                continue;
            }
                        // PICO-AMIGA: F9 = a new blank ADF here (pico-speccy's New TRD), named
            // after its volume.
            if (all && k.vk == fabgl::VK_F9) {
                OSD::clickNoPause();
                string nn;
                if (footerAsk("New ADF:", nn)) {
                    if (nn.size() > 4 && lcExt(nn) == "adf") nn = nn.substr(0, nn.size() - 4);
                    const string file = nn + ".adf";
                    status("Creating ADF...", -1);
                    if (!machine_format_adf((s_dir + file).c_str(), nn.c_str())) {
                        uiToast("The disk cannot be created (exists?)", true, 2000);
                        drawAll();
                        continue;
                    }
                    s_goto = file;
                    break;
                }
                continue;
            }

            // ── typing: search input or first-letter jump ──────────────────────
            if (k.ASCII >= 32 && k.ASCII < 127) {
                if (s_search) {
                    if (s_query.size() < 24) {
                        s_query += (char)toupper(k.ASCII);
                        recountVisible();
                        s_sel = s_top = 0;
                        drawList(); drawInfo(); drawHeader(); drawFooter();
                        OSD::clickNoPause();
                    }
                    continue;
                }
                if (isalnum((uint8_t)k.ASCII) && s_visTotal) {
                    const char want = (char)toupper(k.ASCII);
                    char curFirst = 0;
                    { const string cn = onUp ? "" : plainName(rec);
                      if (!cn.empty()) curFirst = (char)toupper((uint8_t)cn[0]); }
                    const int start = (curFirst == want) ? s_sel + 1 : 0;
                    for (int i = 0; i < s_visTotal; i++) {
                        const int v = (start + i) % s_visTotal;
                        const string s = visName(v);
                        const string pn = isUpRec(s) ? "" : plainName(s);
                        if (!pn.empty() && (char)toupper((uint8_t)pn[0]) == want) {
                            if (v != s_sel) moveSel(v - s_sel);
                            OSD::clickNoPause();
                            break;
                        }
                    }
                }
                continue;
            }
        }
    }
}

string browseFile(string& fdir, const string& title, uint8_t ftype, const char* root) {
    if (ftype >= BROWSE_COUNT) ftype = BROWSE_ALL;
    f_mkdir("/tmp");                      // SortedFiles keeps its .idx files there
    gfxBegin();
    computeBL();

    s_root  = root ? root : "";
    s_dir   = fdir;
    // Self-heal: an earlier bug could persist "/../../" chains (Right on the ".."
    // row descended into a literal ".."). FatFs resolves them to the root anyway.
    if (s_dir.find("..") != string::npos) s_dir = s_root.empty() ? string("/") : s_root;
    // A remembered path from outside the ceiling (the root moved, the card changed)
    // would open the browser somewhere it can never go up from.
    if (!s_root.empty() && s_dir.compare(0, s_root.size(), s_root) != 0) s_dir = s_root;
    s_title = title;
    s_ftype = ftype;

    // Split the type's extension list once ("(.tap,.tzx,...)" style comma list).
    s_exts.clear();
    string ss = s_types[ftype].fileExts;
    size_t pos;
    while ((pos = ss.find(',')) != string::npos) {
        if (pos) s_exts.push_back(ss.substr(0, pos));
        ss.erase(0, pos + 1);
    }
    if (!ss.empty()) s_exts.push_back(ss);

    const string ret = runLoop();
    fdir = s_dir;                         // navigation is part of the contract
    Debug::log("UiBrowser: close ret='%.1s%s' dir='%s'\n",
               ret.c_str(), ret.size() > 1 ? "..." : "", s_dir.c_str());

    s_root.clear();                       // never leaks into the next browse
    gfxEnd();
    return ret;
}

} // namespace nm

