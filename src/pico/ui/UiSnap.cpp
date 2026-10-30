// pico-amiga — Snapshots (pico-speccy's fast snapshots, UiActions.cpp there).
//
// The 40 quick slots are ONE K_PICK list in the right pane of Snapshots, the
// verbs on the function keys. Enter is whichever verb the user arrived with:
// Win+F4 from the Amiga means "save", Win+F3 and the menu row mean "load" - a
// slot load asks nothing and replaces the running machine, so "Win+F4, pick a
// slot, Enter" must not throw the session away. The footer says which.
// Alt+Win+F3 / Alt+Win+F4 load / save Config::persist_slot with no list at all.
// The files: snapshot.cpp (.pas), SNAP_DIR/persistN.pas.

#include "UiSnap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"

#include "OSDNewMenu.h"
#include "UiStage.h"
#include "UiDialog.h"
#include "UiStrings.h"
#include "UiRender.h"
#include "UiNav.h"
#include "UiBrowser.h"
#include "UiGfx.h"
#include "app/Config.h"
#include "snapshot.h"

namespace nm {

// A slot's name: "" = empty, "\x01" = a snapshot without a name.
static std::string slotName(int slot) {
    std::string n;
    if (!Snap::readName(Snap::slotPath(slot), n)) return "";
    return n.empty() ? "\x01" : n;
}

#define SNAP_LBL_LEN 24                 // the right pane shows ~20 glyphs
struct SnapRows {
    Option opts[SNAP_SLOTS];
    char   lbl[SNAP_SLOTS][SNAP_LBL_LEN];
};
static SnapRows* s_snap = nullptr;
static bool      s_snap_valid = false;
static bool      s_snap_enter_saves = false;
static int       s_vlabel_slot = -1;        // persist_vlabel's cache: the slot it shows

// The footer line, and the only place the list's verbs are written down. Mutable
// on purpose: the node points at it and the entry key decides what Enter does.
Option persist_foot[1] = {{ SYM_ENTER " Load  F4 Save  F6 Name  F8 Del", 0, nullptr }};

static void snapInvalidate() { s_snap_valid = false; s_vlabel_slot = -1; }

void persistEnterVerb(bool save) {
    s_snap_enter_saves = save;
    persist_foot[0].label = save ? SYM_ENTER " Save  F3 Load  F6 Name  F8 Del"
                                 : SYM_ENTER " Load  F4 Save  F6 Name  F8 Del";
}

void snapSessionBegin() { snapInvalidate(); }

void snapSessionEnd() {
    free(s_snap);
    s_snap = nullptr;
    s_snap_valid = false;
}

const Option* persist_rows(uint8_t& cnt) {
    if (!s_snap) {
        s_snap = (SnapRows*)malloc(sizeof(SnapRows));
        if (!s_snap) { cnt = 0; return nullptr; }
        s_snap_valid = false;
    }
    cnt = SNAP_SLOTS;
    if (s_snap_valid) return s_snap->opts;
    for (int i = 1; i <= SNAP_SLOTS; i++) {
        const std::string name = slotName(i);
        char* lbl = s_snap->lbl[i - 1];
        if (name.empty()) snprintf(lbl, SNAP_LBL_LEN, "#%02d", i);
        else snprintf(lbl, SNAP_LBL_LEN, "#%02d %s", i, name == "\x01" ? TXT_SLOT_NONAME : name.c_str());
        s_snap->opts[i - 1] = { lbl, (int32_t)i, nullptr };
    }
    s_snap_valid = true;
    return s_snap->opts;
}

// The row shows the slot the quick keys act on: Alt+Win+F3 / F4 load and save it
// with no list at all, so it is worth seeing from outside the list.
static char s_vlabel[NM_DYN_VALUE_LEN];

const char* persist_vlabel() {
    const int slot = Config::persist_slot;
    if (s_vlabel_slot != slot) {
        const std::string name = slotName(slot);
        snprintf(s_vlabel, sizeof s_vlabel, "#%02d %s", slot,
                 name.empty() ? "-" : (name == "\x01" ? TXT_SLOT_NONAME : name.c_str()));
        // The row's label is 11 glyphs against a ~24-glyph pane: the value gets 12
        // (clipped here, not by the renderer, which would clip the label instead).
        if (strlen(s_vlabel) > 12) { s_vlabel[10] = s_vlabel[11] = '.'; s_vlabel[12] = 0; }
        s_vlabel_slot = slot;
    }
    return s_vlabel;
}

bool snapLoadPath(const std::string& path) {
    uiBusy(TXT_MSG_LOADING);
    std::string err, warn;
    const bool ok = Snap::load(path, err, warn);
    // The model, the Kickstart and the drives may all have moved under the menu.
    Stage::invalidate(SET_NONE);
    if (!ok) { uiToast(err.c_str(), true, 2500); return false; }
    if (!warn.empty()) uiToast(warn.c_str(), true, 2500);
    return true;
}

static bool saveSlot(int slot, const std::string& name) {
    uiBusy(TXT_MSG_SAVING_SNAP);
    snapInvalidate();
    if (Snap::save(Snap::slotPath(slot), name)) return true;
    uiToast(TXT_MSG_SNAP_SAVE_ERR, true, 2000);
    return false;
}

void persist_key(int32_t tag, uint8_t key) {
    const int slot = (int)tag;
    if (key == 0) key = s_snap_enter_saves ? 4 : 3;  // Enter = the verb we came with
    const std::string name = slotName(slot);
    const bool empty = name.empty();

    if (key == 6) {                                  // F6 rename
        if (empty) return;
        std::string nn = name == "\x01" ? "" : name;
        if (uiPrompt(TXT_SLOT_NAME, nn, SNAP_NAME_LEN - 1, false, true)) {
            Snap::setName(Snap::slotPath(slot), nn);
            snapInvalidate();
        }
        return;
    }
    if (key == 8) {                                  // F8 remove
        if (empty) return;
        char q[48];
        snprintf(q, sizeof q, "Remove snapshot #%02d?", slot);
        if (uiConfirm(q)) { f_unlink(Snap::slotPath(slot).c_str()); snapInvalidate(); }
        return;
    }
    if (key == 4) {                                  // F4 save here
        Config::persist_slot = (uint8_t)slot;
        std::string nm_ = name;
        if (!empty) {
            // Yes by default, against uiConfirm's rule that Enter must not destroy:
            // the user came saying "save to THIS slot". F8 Remove keeps No.
            if (!uiConfirm(TXT_DLG_SLOT_OVER, nullptr, /*default_yes=*/true)) return;
            if (nm_ == "\x01") nm_.clear();
        } else {                                     // empty: offer the disk's name
            nm_ = Snap::defaultName();
            if (!uiPrompt(TXT_SLOT_NAME, nm_, SNAP_NAME_LEN - 1, false, true)) return;
        }
        if (saveSlot(slot, nm_)) { Config::save(); requestClose(); }
        return;
    }
    if (key != 3) return;                            // F3 load
    if (empty) { uiToast(TXT_MSG_SLOT_EMPTY, true, 1200); return; }
    Config::persist_slot = (uint8_t)slot;
    if (snapLoadPath(Snap::slotPath(slot))) requestClose();
}

// Snapshots > Load from file: the browser on the snapshot folder.
void loadSnapshotFile() {
    static std::string dir;
    if (dir.empty()) {
        FILINFO fi;
        dir = f_stat(SNAP_DIR, &fi) == FR_OK ? std::string(SNAP_DIR "/") : std::string("/");
    }
    const std::string r = browseFile(dir, TXT_SNAP_PICK, BROWSE_SNAP);
    if (r.size() < 2 || r[0] != 'R') return;
    if (snapLoadPath(dir + r.substr(1))) requestClose();
}

// Alt+Win+F3 / Alt+Win+F4: Config::persist_slot, no list. A save keeps the
// slot's name (an empty slot gets the disk's); a load asks nothing.
void runQuickSnap(bool save) {
    gfxBegin();
    const int slot = Config::persist_slot;
    const std::string name = slotName(slot);
    char msg[48];
    if (save) {
        if (saveSlot(slot, name.empty() ? Snap::defaultName() : name == "\x01" ? "" : name)) {
            snprintf(msg, sizeof msg, "Saved to slot #%02d", slot);
            uiToast(msg, false, 1000);
        }
    } else if (name.empty()) {
        snprintf(msg, sizeof msg, "Slot #%02d is empty", slot);
        uiToast(msg, true, 1200);
    } else {
        snapLoadPath(Snap::slotPath(slot));
    }
    gfxEnd();
}

} // namespace nm
