#pragma once

#include <cstdint>

struct PADStatus;

// The Android app's on-screen GameCube controller (its TouchControlsView, through JNI), merged into port 1 the way
// a real pad there would be read. Brawl's main input is the GameCube pad, so the phone's touch screen becomes one:
// an analog stick, A B X Y, Z, L, R and Start. A controller plugged into port 1 still works alongside it.
namespace TouchPad {

// Whether the on-screen controller is shown. While it is, port 1 always reports a connected controller.
void SetEnabled(bool enabled);
// PAD_BUTTON_* / PAD_TRIGGER_* bits held, and the stick as -1..1 (y up). From the UI thread.
void Set(uint32_t buttons, float stickX, float stickY);
// Whether the on-screen controller is shown.
bool Enabled();
// Whether any button in `mask` was pressed since the last call (the title card's start press).
bool TakePress(uint32_t mask);
// Adds the touch state to port 1's status for this read (the guest thread, in PADRead).
void Merge(PADStatus* port1);

} // namespace TouchPad
