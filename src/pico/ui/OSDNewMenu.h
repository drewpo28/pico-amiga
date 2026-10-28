// pico-amiga (from pico-speccy) — the fullscreen two-pane menu (F12).
//
// Everything about this UI lives under src/pico/ui/. It owns its own geometry,
// colours and key loop, and draws straight into the Amiga framebuffer, which the
// scanout keeps showing; the emulator does not run while it is open, and repaints
// the whole frame on the next amiga_run_frame() after it closes.

#pragma once

#include <string>

namespace nm {

// True when the current video mode can host the fullscreen layout.
bool available();

// Open the menu. Blocks until the user closes it (input.cpp calls it on F12, from
// the emulator loop), owning the screen and the keyboard while it runs.
void run();

// The Ctrl+F12 hotkey: the file browser straight onto "insert into DF<drive>",
// outside the menu; the disk goes in when one is picked.
void runDiskInsert(int drive);

// The Win+F1 hotkey (pico-speccy's Alt+F1; Alt is the Amiga's): the System info
// page, live at 1 Hz, outside the menu. Also Help > System info.
void runSysInfo();

// The Win+F5 hotkey (pico-speccy's F5 browser): every file on the card; Enter
// opens a disk / disk set / Kickstart, F5 on a disk image picks the drive.
void runOpenFile();

// A yes/no question in THIS UI, asked from outside the menu while the machine
// runs (the SD automount's "settings found on the card — reboot?"). Owns its
// own gfx session like the game page above. `body` may carry '\n'. True = the
// first button; Esc answers the second.
bool uiConfirmStandalone(const char* body, const char* yes_btn, const char* no_btn);
// A Yes/No question in the fullscreen UI's look, from anywhere (inside a menu
// session or not); restores what it covered. 1 = Yes, 0 = No, -1 = Esc.
int uiAskAnywhere(const char* title, const char* body, bool default_yes);

} // namespace nm

