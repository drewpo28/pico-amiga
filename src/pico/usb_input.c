// USB host input: boot-protocol keyboards and mice via TinyUSB.
//
// Keyboard reports are diffed against the previous one and turned into Amiga
// key down/up events; F12 is the emulator's reset key (Ctrl-Amiga-Amiga also
// works the Amiga way, through the keyboard handshake).  Mouse reports go to
// JOY0DAT counters and the two buttons.
#include "usb_input.h"
#include <string.h>
#include "tusb.h"
#include "amiga.h"
#include "hid_keymap.h"

static uint8_t prev_keys[6], prev_mod;
static volatile bool reset_req;

static bool in_report(const uint8_t *keys, uint8_t k) {
    for (int i = 0; i < 6; i++) if (keys[i] == k) return true;
    return false;
}

static void key_event(uint8_t usage, bool down) {
    if (usage == 0x45) { if (down) reset_req = true; return; }     // F12
    uint16_t m = hid_to_amiga[usage];
    if (!(m & 0x100) || (m & 0xFF) == AK_NONE) return;
    amiga_key((uint8_t)m, down);
}

static void keyboard_report(const hid_keyboard_report_t *r) {
    uint8_t chg = r->modifier ^ prev_mod;
    for (int b = 0; b < 8; b++)
        if (chg & (1u << b)) amiga_key(hid_mod_to_amiga[b], (r->modifier >> b) & 1);
    prev_mod = r->modifier;
    for (int i = 0; i < 6; i++) {
        uint8_t k = prev_keys[i];
        if (k > 3 && !in_report(r->keycode, k)) key_event(k, false);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t k = r->keycode[i];
        if (k > 3 && !in_report(prev_keys, k)) key_event(k, true);
    }
    memcpy(prev_keys, r->keycode, 6);
}

static void mouse_report(const hid_mouse_report_t *r) {
    amiga_mouse(r->x, r->y, r->buttons & MOUSE_BUTTON_LEFT, r->buttons & MOUSE_BUTTON_RIGHT);
}

void tuh_hid_mount_cb(uint8_t dev, uint8_t inst, const uint8_t *desc, uint16_t len) {
    (void)desc; (void)len;
    tuh_hid_receive_report(dev, inst);
}

void tuh_hid_umount_cb(uint8_t dev, uint8_t inst) { (void)dev; (void)inst; }

void tuh_hid_report_received_cb(uint8_t dev, uint8_t inst, const uint8_t *report, uint16_t len) {
    switch (tuh_hid_interface_protocol(dev, inst)) {
    case HID_ITF_PROTOCOL_KEYBOARD:
        if (len >= sizeof(hid_keyboard_report_t)) keyboard_report((const hid_keyboard_report_t *)report);
        break;
    case HID_ITF_PROTOCOL_MOUSE:
        if (len >= 3) mouse_report((const hid_mouse_report_t *)report);
        break;
    default:
        break;   // report-protocol devices (gamepads) are a TODO
    }
    tuh_hid_receive_report(dev, inst);
}

void usb_input_init(void) {
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
    tuh_init(BOARD_TUH_RHPORT);
}

void usb_input_poll(void) { tuh_task(); }

bool usb_input_reset_requested(void) {
    bool r = reset_req;
    reset_req = false;
    return r;
}
