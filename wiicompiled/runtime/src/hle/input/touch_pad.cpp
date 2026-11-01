// touch_pad.cpp - the Android app's on-screen GameCube controller (touch_pad.h).
#include "touch_pad.h"

#include "input_bindings.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include <dolphin/pad.h>

namespace TouchPad {
namespace {

// Written by the UI thread, read by the guest thread. Buttons pressed since the last read are kept as well, so a
// tap shorter than one game frame still reaches the game.
std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_buttons{0};
std::atomic<uint32_t> g_buttonsSinceRead{0};
std::atomic<uint32_t> g_pressLatch{0};
std::atomic<int32_t> g_stickX{0};
std::atomic<int32_t> g_stickY{0};

// A real GameCube stick reaches about 100 at the gate; the game's own dead zone handles the middle.
constexpr float kStickRange = 100.0f;

int32_t ToStick(float value) {
    if (!std::isfinite(value)) return 0;
    return static_cast<int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * kStickRange));
}

} // namespace

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) Set(0, 0.0f, 0.0f);
}

void Set(uint32_t buttons, float stickX, float stickY) {
    g_buttons.store(buttons, std::memory_order_relaxed);
    g_buttonsSinceRead.fetch_or(buttons, std::memory_order_relaxed);
    g_pressLatch.fetch_or(buttons, std::memory_order_relaxed);
    g_stickX.store(ToStick(stickX), std::memory_order_relaxed);
    g_stickY.store(ToStick(stickY), std::memory_order_relaxed);
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

bool TakePress(uint32_t mask) {
    return (g_pressLatch.exchange(0, std::memory_order_relaxed) & mask) != 0;
}

void Merge(PADStatus* port1) {
    if (port1 == nullptr || !g_enabled.load(std::memory_order_relaxed)) return;
    // A press stays down for at least kMinHeldReads polls: the game tells a new press from the previous poll's
    // state, so a tap shorter than that would come and go unseen.
    constexpr int kMinHeldReads = 3;
    static int heldReads[16] = {};  // only the guest thread merges
    uint32_t buttons = g_buttons.load(std::memory_order_relaxed);
    const uint32_t pressed = g_buttonsSinceRead.exchange(0, std::memory_order_relaxed);
    for (int bit = 0; bit < 16; ++bit) {
        if (pressed & (1u << bit)) heldReads[bit] = kMinHeldReads;
        if (heldReads[bit] > 0) {
            --heldReads[bit];
            buttons |= 1u << bit;
        }
    }
    if (port1->err != PAD_ERR_NONE) {
        *port1 = {};
        port1->err = PAD_ERR_NONE;
    }
    if (InputBindings::InputBlocked()) return;  // the options menu is open and takes the input itself
    port1->button |= static_cast<uint16_t>(buttons & 0xFFFF);
    // The stick wins over a controller's only while it is held off centre.
    const int32_t x = g_stickX.load(std::memory_order_relaxed);
    const int32_t y = g_stickY.load(std::memory_order_relaxed);
    if (x != 0 || y != 0) {
        port1->stickX = static_cast<s8>(x);
        port1->stickY = static_cast<s8>(y);
    }
    // L and R pressed on screen are full pulls, like a digital click at the bottom of the trigger.
    if (buttons & PAD_TRIGGER_L) port1->triggerLeft = 0xFF;
    if (buttons & PAD_TRIGGER_R) port1->triggerRight = 0xFF;
}

} // namespace TouchPad
