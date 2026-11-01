#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_bindings.h"
#include "touch_pad.h"
#include "wii_remote_input.h"
#include "frame_input.h"
#include "runtime_config.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <dolphin/pad.h>

namespace {

std::atomic<bool> g_rumbleEnabled{true};

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

namespace {

// The launcher's keyboard mapping (Config.toml [gc_keyboard]): enabled, then one key per GameCube button and stick
// direction, by SDL scancode name ("Space", "Left Shift") or "Mouse Left" / "Mouse Middle" / "Mouse Right". The
// keyboard then plays port 1, as the in-game menu's "Keyboard and mouse" switch does.
int32_t KeyFromName(const std::string& name) {
    if (name == "Mouse Left") return PAD_KEY_MOUSE_LEFT;
    if (name == "Mouse Middle") return PAD_KEY_MOUSE_MIDDLE;
    if (name == "Mouse Right") return PAD_KEY_MOUSE_RIGHT;
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? PAD_KEY_INVALID : static_cast<int32_t>(code);
}

void ApplyKeyboardConfig() {
    toml::value table;
    try {
        std::ifstream in(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
        if (!in) return;
        table = toml::parse(in, "Config.toml");
    } catch (const std::exception&) {
        return;  // the game's own config reader reports a broken Config.toml
    }
    const auto enabled = RuntimeConfigFile::FindConfigValue<bool>(table, "gc_keyboard", "enabled");
    if (!enabled) return;  // no launcher setting: the in-game menu's own choice stands
    u32 loaded = 0;
    PADGetKeyButtonBindings(0, &loaded);  // aurora reads its saved bindings on first use: before ours, not over them
    PADSetKeyboardActive(0, *enabled ? TRUE : FALSE);
    if (*enabled) {
        static constexpr std::pair<const char*, PADButton> kButtons[] = {
            {"a", PAD_BUTTON_A}, {"b", PAD_BUTTON_B}, {"x", PAD_BUTTON_X}, {"y", PAD_BUTTON_Y},
            {"start", PAD_BUTTON_START}, {"z", PAD_TRIGGER_Z}, {"l", PAD_TRIGGER_L}, {"r", PAD_TRIGGER_R},
            {"dpad_up", PAD_BUTTON_UP}, {"dpad_down", PAD_BUTTON_DOWN}, {"dpad_left", PAD_BUTTON_LEFT},
            {"dpad_right", PAD_BUTTON_RIGHT}};
        static constexpr std::pair<const char*, PADAxis> kAxes[] = {
            {"stick_right", PAD_AXIS_LEFT_X_POS}, {"stick_left", PAD_AXIS_LEFT_X_NEG},
            {"stick_up", PAD_AXIS_LEFT_Y_POS}, {"stick_down", PAD_AXIS_LEFT_Y_NEG},
            {"cstick_right", PAD_AXIS_RIGHT_X_POS}, {"cstick_left", PAD_AXIS_RIGHT_X_NEG},
            {"cstick_up", PAD_AXIS_RIGHT_Y_POS}, {"cstick_down", PAD_AXIS_RIGHT_Y_NEG}};
        for (const auto& [key, button] : kButtons) {
            if (const auto name = RuntimeConfigFile::FindConfigValue<std::string>(table, "gc_keyboard", key))
                PADSetKeyButtonBinding(0, {KeyFromName(*name), button});
        }
        for (const auto& [key, axis] : kAxes) {
            if (const auto name = RuntimeConfigFile::FindConfigValue<std::string>(table, "gc_keyboard", key))
                PADSetKeyAxisBinding(0, {KeyFromName(*name), axis, 1});
        }
    }
    PADSerializeMappings();
}

} // namespace

extern "C" uint32_t PAD__Init_HLE()
{
    const bool ok = PADInit();
    ApplyKeyboardConfig();
    return ok ? 1u : 0u;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215E68, PAD__Init_HLE, uint32_t, (), ());

// Every GameCube pad source for this poll, as the game would read them: SDL pads, the keyboard, bindings, and a
// test script last.
static uint32_t ReadLocalPads(PADStatus* statuses)
{
    // Keep looking for a Bluetooth Wii Remote that dropped out (or was turned on late).
    WiiRemoteInput::Poll();
    uint32_t rumbleMask = PADRead(statuses);
    // Wii Remotes reach the game through KPAD, not as GameCube pads. This also
    // applies while input is blocked (overlay open) so the port does not flip
    // between "connected" and "no controller" every time the overlay toggles.
    WiiRemoteInput::HideRemotesFromPad(statuses, PAD_CHANMAX);

    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);
    TouchPad::Merge(&statuses[0]);  // the Android app's on-screen controller
    // Test scripts last, and alone: a key or mouse button pressed at the PC during a scripted run must not reach
    // the game (the keyboard pad maps the mouse buttons to A and B).
    // A script's channel field drives that port (a second player's picks in a scripted match).
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        uint16_t scripted = 0;
        int8_t stickX = 0, stickY = 0;
        if (!WiiRemoteInput::ScriptedPadButtons(port, scripted, stickX, stickY)) continue;
        statuses[port] = {};
        statuses[port].err = PAD_ERR_NONE;
        statuses[port].button = scripted;
        statuses[port].stickX = stickX;
        statuses[port].stickY = stickY;
    }
    return rumbleMask;
}

// Deterministic mode (network play): the local ports for frame_input.cpp to latch at the retrace.
void PAD_HLE_SampleLocalPads(FrameInput::GcPad* pads)
{
    PADStatus statuses[PAD_CHANMAX]{};
    ReadLocalPads(statuses);
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        const PADStatus& s = statuses[port];
        pads[port] = {s.err == PAD_ERR_NONE, s.button, s.stickX, s.stickY, s.substickX, s.substickY,
                      s.triggerLeft, s.triggerRight};
    }
}

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory. In network
// play every port is the frame's latched input from all players instead (frame_input.h).
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    uint32_t rumbleMask = 0;
    if (FrameInput::Active()) {
        for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
            const FrameInput::GcPad& pad = FrameInput::Get(port).pad;
            PADStatus& s = statuses[port];
            s.err = pad.connected ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
            if (!pad.connected) continue;
            s.button = pad.button;
            s.stickX = pad.stickX;
            s.stickY = pad.stickY;
            s.substickX = pad.substickX;
            s.substickY = pad.substickY;
            s.triggerLeft = pad.triggerL;
            s.triggerRight = pad.triggerR;
            rumbleMask |= 0x80000000u >> port;
        }
    } else {
        rumbleMask = ReadLocalPads(statuses);
    }

    // Which ports report a controller, and the device behind each: logged whenever that changes.
    static std::string lastPorts;
    std::string ports;
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) continue;
        const s32 index = PADGetIndexForPort(port);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        const char* name = gamepad != nullptr ? SDL_GetGamepadName(gamepad) : nullptr;
        ports += " P" + std::to_string(port + 1) + "=" + (name != nullptr ? name : index >= 0 ? "pad" : "keyboard/script");
    }
    if (ports != lastPorts) {
        lastPorts = ports;
        std::printf("[pad] controllers:%s\n", ports.empty() ? " none" : ports.c_str());
        std::fflush(stdout);
    }

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215FC4, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215C54, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE(80215D5C, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    PADControlMotor(chan, command);
}
// Brawl (RSBE01): GameCube controllers are its main input.
PPC_NATIVE_OVERRIDE_VOID(802162A4, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
