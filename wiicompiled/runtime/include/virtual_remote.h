#pragma once

#include "wii_remote_input.h"

#include <cstdint>

// New Super Mario Bros. Wii reads nothing but Wii Remotes (held sideways, or with a Nunchuk), so a
// keyboard or an ordinary gamepad has to become one. Every port with no real Wii Remote but with an
// aurora PAD source (an SDL gamepad, or the keyboard when it is switched on for that port in the
// settings overlay) is presented to the game as a bare Wii Remote held sideways: aurora's PADRead
// supplies the GameCube-style state, which picks up the user's button remapping, and Translate()
// turns that into WPAD buttons, a shake and a tilt. Port 1 also always has a built-in keyboard
// layout, so the game is playable without any setup:
//
//   arrows        D-pad (screen directions; rotated for the sideways remote)
//   X / Space     2  (jump, confirm)          Z / Left Shift   1 (run, fireball, back)
//   C / Ctrl      shake (spin jump)           A                A
//   Enter         +  (pause)                  - / Tab          -
//   Q / E         tilt left / right
//
// Gamepads (aurora's default GameCube mapping): left stick or D-pad moves, south button jumps,
// east and west run, north is A, Start is +, Back or left trigger is -, right shoulder or right
// trigger shakes, right stick tilts.
namespace VirtualRemote {

// Brawl: whether the keyboard plays as a sideways Wii Remote (keys from Keybinds) instead of a GameCube
// controller. Config.toml [keyboard] controller = "wii_remote" / "gamecube"; F10 > Controls changes it.
bool KeyboardIsRemote();
void SetKeyboardIsRemote(bool remote);

// True when `chan` should report a connected Wii Remote driven by a keyboard or gamepad.
bool Present(uint32_t chan);
// Fills one frame of the virtual remote on `chan`; false when Present(chan) is false.
bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample);

// On-screen remote and phone motion for player 1 (the Android app's overlay, through JNI). Buttons
// use screen directions, like the keyboard; tilt is -1 (counter-clockwise) .. 1 (clockwise).
enum TouchButton : uint32_t {
    kTouchLeft = 0x001, kTouchRight = 0x002, kTouchUp = 0x004, kTouchDown = 0x008,
    kTouchOne = 0x010, kTouchTwo = 0x020, kTouchA = 0x040, kTouchPlus = 0x080, kTouchMinus = 0x100,
};
void SetTouchState(uint32_t touchButtons, bool shake, float tilt);

} // namespace VirtualRemote
