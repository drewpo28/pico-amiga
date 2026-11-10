// The game pad's map (Devices > Input): what each control of the pad in port 2
// does, and which keyboard keys stand in for the pad.  pico-speccy's joydef, for
// the Amiga: one pad type, its targets the joystick's directions and the CD32
// pad's seven buttons (red is fire on every machine, blue the second button).
#pragma once
#include <stdint.h>

// The pad's controls, pico-speccy's 14 slots in its order.  A NES pad has the
// first eight less C; a SNES pad adds X, Y and the shoulders (Z, C); a USB /
// XInput pad has them all (L2 / R2: the triggers).
enum { JOY_S_LEFT, JOY_S_RIGHT, JOY_S_UP, JOY_S_DOWN, JOY_S_START, JOY_S_MODE, JOY_S_A, JOY_S_B,
       JOY_S_C, JOY_S_X, JOY_S_Y, JOY_S_Z, JOY_S_L2, JOY_S_R2, JOY_SLOTS };
// What a control (or a key) does.
enum { JOY_A_NONE, JOY_A_UP, JOY_A_DOWN, JOY_A_LEFT, JOY_A_RIGHT, JOY_A_RED, JOY_A_BLUE, JOY_A_GREEN,
       JOY_A_YELLOW, JOY_A_PLAY, JOY_A_RWD, JOY_A_FFW, JOY_ACTIONS };

#ifdef __cplusplus
extern "C" {
#endif
extern const char *const joymap_action_name[JOY_ACTIONS];   // "None", "Up", ... "Forward"
extern const char *const joymap_slot_name[JOY_SLOTS];       // "Left", ... "R2"
void joymap_pad_defaults(uint8_t out[JOY_SLOTS]);
// The keys (fabgl virtual keys; 0 = none) for actions 1..JOY_ACTIONS-1, out[0] unused:
// the cursor keys, Left Ctrl red, Right Ctrl blue, the rest unassigned.
void joymap_key_defaults(uint16_t out[JOY_ACTIONS]);
// A control of the pad is held now / a key is (a letter in either case).
bool joymap_slot_down(int slot);
bool joymap_key_down(uint16_t vk);
// A key's name for the map ("Left Ctrl", "A", "F5"), into out.
const char *joymap_key_name(uint16_t vk, char out[16]);
// A key the map can take (not one of the menu's or the pad's synthetic ones), a
// letter as its lower-case key; 0 = no.
uint16_t joymap_key_norm(uint16_t vk);
#ifdef __cplusplus
}
#endif
