#include "virtual_remote.h"

#include "display_settings.h"
#include "input_bindings.h"
#include "keybinds.h"
#include "runtime_config.h"
#include "memory.h"
#include "runtime_log.h"
#include "wii_remote_input.h"

#include <aurora/aurora.h>

#include <dolphin/pad.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace VirtualRemote {
namespace {

// WPAD_BUTTON_* bits as the game reads them from KPADStatus.hold.
constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                   kWpadPlus = 0x0010, kWpadTwo = 0x0100, kWpadOne = 0x0200, kWpadB = 0x0400, kWpadA = 0x0800,
                   kWpadMinus = 0x1000, kWpadHome = 0x8000;

// The remote's D-pad by screen direction. Held sideways with the D-pad under the left thumb, the
// pad's "up" arrow points left, "right" points up, "down" points right and "left" points down.
constexpr uint32_t kScreenLeft = kWpadUp, kScreenRight = kWpadDown, kScreenUp = kWpadRight,
                   kScreenDown = kWpadLeft;

// What the game's motion code looks for (dGameKeyCore_c::setShakeY and the tilt code after
// setConfigKey): a shake is acc.y changing by at least 0.28 g between consecutive frames on four
// frames (gaps of up to two allowed), then a five-frame cool-down; the tilt angle is -acc.z, through
// KPAD's acc_vertical, at 16384 angle units per g. Super Smash Bros. Brawl (wpadMain 0x80234528, check
// 0x80235180) instead averages acc.y over four frames at a time and wants a dip then a steep rise
// (at least 1 g, 0.75 g per four frames); a swing flipping every frame averages to nothing there. So a
// shake is one flick, added to rest acc.y: a quick dip, a steep rise, back to rest. All but one frame
// change by at least 0.6 g, for the frame-to-frame check. Holding the button keeps flicking.
constexpr float kShakeFlickG[] = {-0.8f, -1.6f, -0.6f, 0.4f, 1.4f, 2.4f, 2.4f, 1.2f, 0.0f};
constexpr int kShakeFrames = static_cast<int>(std::size(kShakeFlickG));
constexpr float kMaxTiltRadians = 0.7853982f;  // 45 degrees
constexpr float kTiltFollow = 0.3f;            // share of the way to the requested tilt per frame
constexpr float kTiltDeadZone = 0.25f;         // of the right stick's travel
constexpr int kStickThreshold = 48;            // of aurora's +-127 stick range
constexpr float kDiagonalSlope = 0.4142136f;   // tan(22.5 degrees): eight-way stick sectors
constexpr int kTriggerThreshold = 128;         // of 255
// aurora's PADRead polls SDL; the game probes and reads all four channels each frame.
constexpr uint64_t kPadRefreshNs = 4'000'000;

struct Intent {
    uint32_t buttons = 0;  // WPAD bits, already in the sideways layout
    bool shake = false;
    float tilt = 0.0f;     // -1 counter-clockwise (left) .. 1 clockwise (right)
};

struct Motion {
    int shakeFrame = kShakeFrames;  // the flick's next frame; kShakeFrames = at rest
    float tilt = 0.0f;
};

// Written by the Android UI thread (SetTouchState), read on the guest thread. The *SinceRead
// latches keep a press shorter than one 60 Hz input read (a quick tap) visible for one read.
std::atomic<uint32_t> g_touchButtons{0};
std::atomic<uint32_t> g_touchButtonsSinceRead{0};
std::atomic<bool> g_touchShake{false};
std::atomic<bool> g_touchShakeSinceRead{false};
std::atomic<float> g_touchTilt{0.0f};

// In one of SPM's arcade minigames (maps mg*: Forget Me Not, Mansion Patrol, Tilt Island ...)? The map name sits at
// +0x44 of the spmario globals (gp).
bool InArcade() {
    uint32_t gp = 0;
    return Memory::TryRead32(0x805A1608u, gp) && Memory::Contains(gp, 0x48) && Memory::Read8(gp + 0x44) == 'm' &&
           Memory::Read8(gp + 0x45) == 'g';
}

std::mutex g_mutex;
std::array<PADStatus, PAD_MAX_CONTROLLERS> g_pads{};
uint64_t g_padsReadNs = 0;
bool g_keyboardOnAPort = false;
std::array<Motion, PAD_MAX_CONTROLLERS> g_motion{};
std::array<bool, PAD_MAX_CONTROLLERS> g_announced{};

void RefreshPadsLocked() {
    const uint64_t now = SDL_GetTicksNS();
    if (g_padsReadNs != 0 && now - g_padsReadNs < kPadRefreshNs) {
        return;
    }
    g_padsReadNs = now;
    PADRead(g_pads.data());
    InputBindings::Apply(g_pads.data());
    g_keyboardOnAPort = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        uint32_t count = 0;
        if (PADGetKeyButtonBindings(port, &count) != nullptr) {
            g_keyboardOnAPort = true;
        }
    }
}

// The built-in layout for port 1, used while no port has aurora's keyboard switched on.
Intent KeyboardIntent() {
    Intent intent;
    if (SDL_GetKeyboardFocus() == nullptr) {
        return intent;
    }
    // The keys come from the game's Keybinds tab (Config.toml [keyboard]); none count while one
    // is being picked there, or until the key picked is let go.
    if (Keybinds::Suppressed()) {
        return intent;
    }
    using namespace Keybinds;
    if (Held(kLeft)) intent.buttons |= kScreenLeft;
    if (Held(kRight)) intent.buttons |= kScreenRight;
    if (Held(kUp)) intent.buttons |= kScreenUp;
    if (Held(kDown)) intent.buttons |= kScreenDown;
    if (Held(kJump)) intent.buttons |= kWpadTwo;
    if (Held(kRun)) intent.buttons |= kWpadOne;
    if (Held(Keybinds::kA)) intent.buttons |= kWpadA;
    if (Held(kPlus)) intent.buttons |= kWpadPlus;
    if (Held(kMinus)) intent.buttons |= kWpadMinus;
    if (Held(Keybinds::kB)) intent.buttons |= kWpadB;
    if (Held(kHome)) intent.buttons |= kWpadHome;
    intent.shake = Held(kShake);
    intent.tilt = (Held(kTiltRight) ? 1.0f : 0.0f) - (Held(kTiltLeft) ? 1.0f : 0.0f);
    return intent;
}

// Gamepads. Super Smash Bros. Brawl is single-player, so every connected gamepad (not a real Wii Remote, which
// reaches the game through KPAD) drives player 1 alongside the keyboard; there is no port to pick.
// The pad's type (SDL_GetGamepadType) picks the default layout; Config.toml [gamepad] <type>_<action>
// (xbox_jump = "a,b", types xbox / playstation / nintendo, actions in Keybinds order) replaces one
// action's default with SDL gamepad names: buttons by position (a = bottom, b = right, x = left, y = top,
// back, start, leftshoulder, dpup, touchpad ...) and axes with a direction (rightx-) or none for a
// trigger (lefttrigger). The left stick always moves the remote's D-pad on top of that. The launcher
// writes these; parsed once at the first read.
struct PadInput {
    bool axis = false;
    int id = 0;
    int sign = 0;  // stick direction for an axis, 0 for a trigger
};
using PadLayout = std::array<std::vector<PadInput>, Keybinds::kActionCount>;
enum PadStyle { kXbox, kPlayStation, kNintendo, kPadStyleCount };

// Defaults per style, in Keybinds order: the same jobs the GameCube-pad route gave each position
// (bottom = 2/jump, left and right = 1, top = A, start = +, back/LB/LT = -, RB/RT = shake, right
// stick = tilt). Nintendo pads jump with both B and A (bottom and right), like Mario games there.
constexpr const char* kPadStyleNames[kPadStyleCount] = {"xbox", "playstation", "nintendo"};
constexpr const char* kPadDefaults[Keybinds::kActionCount][kPadStyleCount] = {
    {"lefty-,dpup", "lefty-,dpup", "lefty-,dpup"},
    {"lefty+,dpdown", "lefty+,dpdown", "lefty+,dpdown"},
    {"leftx-,dpleft", "leftx-,dpleft", "leftx-,dpleft"},
    {"leftx+,dpright", "leftx+,dpright", "leftx+,dpright"},
    {"a", "a", "a,b"},
    {"b,x", "b,x", "x"},
    {"y", "y", "y"},
    {"rightshoulder,righttrigger", "rightshoulder,righttrigger", "rightshoulder,righttrigger"},
    {"", "", ""},  // tilt: SPM gameplay never reads it; the right stick aims the pointer instead
    {"", "", ""},
    {"start", "start", "start"},
    {"back,leftshoulder,lefttrigger", "back,touchpad,leftshoulder,lefttrigger", "back,leftshoulder,lefttrigger"},
    {"", "", ""},  // B and Home: the keyboard's only
    {"", "", ""},
};
constexpr const char* kPadActionKeys[Keybinds::kActionCount] = {
    "up", "down", "left", "right", "jump", "run", "a", "shake", "tilt_left", "tilt_right", "plus", "minus", "b", "home",
};

std::vector<PadInput> ParsePadInputs(const std::string& text) {
    std::vector<PadInput> inputs;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        std::string name = RuntimeConfigFile::Trim(
            std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        PadInput input;
        if (!name.empty() && (name.back() == '+' || name.back() == '-')) {
            input.sign = name.back() == '+' ? 1 : -1;
            name.pop_back();
        }
        if (const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str()); axis != SDL_GAMEPAD_AXIS_INVALID) {
            input.axis = true;
            input.id = axis;
            inputs.push_back(input);
        } else if (const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
                   button != SDL_GAMEPAD_BUTTON_INVALID) {
            input.id = button;
            inputs.push_back(input);
        } else if (!name.empty()) {
            RT_LOGF(RT_TAG_CONFIG, "[gamepad] unknown input '%s'\n", name.c_str());
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return inputs;
}

const std::array<PadLayout, kPadStyleCount>& PadLayouts() {
    static const std::array<PadLayout, kPadStyleCount> layouts = [] {
        std::array<PadLayout, kPadStyleCount> result;
        for (int style = 0; style < kPadStyleCount; ++style) {
            for (int action = 0; action < Keybinds::kActionCount; ++action) {
                const std::string custom =
                    RuntimeConfigFile::GamepadBinding(std::string(kPadStyleNames[style]) + "_" + kPadActionKeys[action]);
                result[style][action] = ParsePadInputs(custom.empty() ? kPadDefaults[action][style] : custom);
            }
        }
        return result;
    }();
    return layouts;
}

PadStyle StyleFor(SDL_Gamepad* gamepad) {
    switch (SDL_GetGamepadType(gamepad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return kNintendo;
    default:
        return kXbox;
    }
}

bool PadInputHeld(SDL_Gamepad* gamepad, const PadInput& input) {
    if (!input.axis) {
        return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(input.id));
    }
    const float value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(input.id)) / 32767.0f;
    return (input.sign == 0 ? value : value * static_cast<float>(input.sign)) > 0.5f;
}

Intent GamepadIntent() {
    Intent intent;
    const auto& layouts = PadLayouts();  // parsed (and bad [gamepad] entries logged) on the first read
    // On-screen button prompts follow the pad last pressed (else the first one); none = the game's Wii Remote icons.
    static int s_lastPressedStyle = -1;
    int firstStyle = -1;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(ids[i]);
        if (gamepad == nullptr) gamepad = SDL_OpenGamepad(ids[i]);  // not opened by Aurora's GAMEPAD_ADDED handler (yet)
        if (gamepad == nullptr) {
            static SDL_JoystickID s_failed = 0;
            if (s_failed != ids[i]) { s_failed = ids[i]; RT_LOGF(RT_TAG_CONFIG, "[gamepad] cannot open pad %u: %s\n", unsigned(ids[i]), SDL_GetError()); }
            continue;
        }
        {  // once per pad: what was found, and its first input (diagnoses "controller does nothing")
            static std::vector<std::pair<SDL_JoystickID, bool>> s_seen;
            auto it = std::find_if(s_seen.begin(), s_seen.end(), [&](const auto& e) { return e.first == ids[i]; });
            if (it == s_seen.end()) {
                RT_LOGF(RT_TAG_CONFIG, "[gamepad] using '%s' (type %d) for player 1\n", SDL_GetGamepadName(gamepad), int(SDL_GetGamepadType(gamepad)));
                s_seen.emplace_back(ids[i], false);
                it = s_seen.end() - 1;
            }
            if (!it->second && (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST) ||
                                std::fabs(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f) > 0.5f)) {
                it->second = true;
                RT_LOGF(RT_TAG_CONFIG, "[gamepad] first input from '%s'\n", SDL_GetGamepadName(gamepad));
            }
        }
        const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForName(SDL_GetGamepadName(gamepad));
        if (kind != WiiRemoteInput::Kind::NotWii && kind != WiiRemoteInput::Kind::WiiUPro) {
            continue;  // a real Wii Remote: the game reads it through KPAD
        }
        const PadStyle style = StyleFor(gamepad);
        const PadLayout& layout = layouts[style];
        if (firstStyle < 0) firstStyle = style;
        const Intent before = intent;
        const auto held = [&](int action) {
            for (const PadInput& input : layout[action]) {
                if (PadInputHeld(gamepad, input)) return true;
            }
            return false;
        };
        using namespace Keybinds;
        if (held(kLeft)) intent.buttons |= kScreenLeft;
        if (held(kRight)) intent.buttons |= kScreenRight;
        if (held(kUp)) intent.buttons |= kScreenUp;
        if (held(kDown)) intent.buttons |= kScreenDown;
        // The left stick is the remote's D-pad, in eight directions (same sectors as the pad route).
        const float sx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        const float sy = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;  // +y up
        constexpr float kStickMin = kStickThreshold / 127.0f;
        if (sx * sx + sy * sy >= kStickMin * kStickMin) {
            if (std::fabs(sx) >= kDiagonalSlope * std::fabs(sy)) intent.buttons |= sx < 0 ? kScreenLeft : kScreenRight;
            if (std::fabs(sy) >= kDiagonalSlope * std::fabs(sx)) intent.buttons |= sy < 0 ? kScreenDown : kScreenUp;
        }
        if (held(kJump)) intent.buttons |= kWpadTwo;
        if (held(kRun)) intent.buttons |= kWpadOne;
        if (held(Keybinds::kA)) intent.buttons |= kWpadA;
        if (held(kPlus)) intent.buttons |= kWpadPlus;
        if (held(kMinus)) intent.buttons |= kWpadMinus;
        intent.shake = intent.shake || held(kShake);
        const float tilt = (held(kTiltRight) ? 1.0f : 0.0f) - (held(kTiltLeft) ? 1.0f : 0.0f);
        if (tilt != 0.0f) intent.tilt = tilt;
        // Tilt Island and the other arcade games steer by tilting the remote: there the left stick tilts it too.
        else if (std::fabs(sx) >= kTiltDeadZone && InArcade()) intent.tilt = std::clamp(sx, -1.0f, 1.0f);
        if (intent.buttons != before.buttons || intent.shake != before.shake) s_lastPressedStyle = style;
    }
    SDL_free(ids);
    if (firstStyle < 0) s_lastPressedStyle = -1;
    aurora_set_button_icon_style(s_lastPressedStyle >= 0 ? s_lastPressedStyle : firstStyle);
    return intent;
}

// The on-screen remote and phone motion set through SetTouchState.
Intent TouchIntent() {
    Intent intent;
    const uint32_t touch = g_touchButtons.load(std::memory_order_relaxed) |
                           g_touchButtonsSinceRead.exchange(0, std::memory_order_relaxed);
    if (touch & kTouchLeft) intent.buttons |= kScreenLeft;
    if (touch & kTouchRight) intent.buttons |= kScreenRight;
    if (touch & kTouchUp) intent.buttons |= kScreenUp;
    if (touch & kTouchDown) intent.buttons |= kScreenDown;
    if (touch & kTouchOne) intent.buttons |= kWpadOne;
    if (touch & kTouchTwo) intent.buttons |= kWpadTwo;
    if (touch & kTouchA) intent.buttons |= kWpadA;
    if (touch & kTouchPlus) intent.buttons |= kWpadPlus;
    if (touch & kTouchMinus) intent.buttons |= kWpadMinus;
    intent.shake = g_touchShake.load(std::memory_order_relaxed) ||
                   g_touchShakeSinceRead.exchange(false, std::memory_order_relaxed);
    intent.tilt = std::clamp(g_touchTilt.load(std::memory_order_relaxed), -1.0f, 1.0f);
    return intent;
}

// A GameCube-style pad state (after the user's remapping) as the sideways remote.
Intent PadIntent(const PADStatus& pad) {
    Intent intent;
    const uint32_t b = pad.button;
    if (b & PAD_BUTTON_LEFT) intent.buttons |= kScreenLeft;
    if (b & PAD_BUTTON_RIGHT) intent.buttons |= kScreenRight;
    if (b & PAD_BUTTON_UP) intent.buttons |= kScreenUp;
    if (b & PAD_BUTTON_DOWN) intent.buttons |= kScreenDown;
    const int sx = pad.stickX;
    const int sy = pad.stickY;  // +y up
    if (sx * sx + sy * sy >= kStickThreshold * kStickThreshold) {
        if (std::abs(sx) >= kDiagonalSlope * std::abs(sy)) intent.buttons |= sx < 0 ? kScreenLeft : kScreenRight;
        if (std::abs(sy) >= kDiagonalSlope * std::abs(sx)) intent.buttons |= sy < 0 ? kScreenDown : kScreenUp;
    }
    if (b & PAD_BUTTON_A) intent.buttons |= kWpadTwo;
    if (b & (PAD_BUTTON_B | PAD_BUTTON_X)) intent.buttons |= kWpadOne;
    if (b & PAD_BUTTON_Y) intent.buttons |= kWpadA;
    if (b & PAD_BUTTON_START) intent.buttons |= kWpadPlus;
    if ((pad.extButton & PAD_BUTTON_BACK) != 0 || (b & PAD_TRIGGER_L) != 0 || pad.triggerLeft >= kTriggerThreshold) {
        intent.buttons |= kWpadMinus;
    }
    intent.shake = (b & (PAD_TRIGGER_Z | PAD_TRIGGER_R)) != 0 || pad.triggerRight >= kTriggerThreshold;
    const float x = static_cast<float>(pad.substickX) / 127.0f;
    intent.tilt = std::fabs(x) < kTiltDeadZone ? 0.0f : std::clamp(x, -1.0f, 1.0f);
    return intent;
}

// Accelerometer for the requested motion, in KPAD's frame (rest: y = -1). Tilting clockwise lowers
// the right end and raises the left one, where the remote's pointer end is, so gravity shows up on
// the remote's long axis (KPAD z) as +sin(angle).
void ApplyMotion(Motion& motion, const Intent& intent, WiiRemoteInput::KpadSample& sample) {
    if (intent.shake && motion.shakeFrame >= kShakeFrames) {
        motion.shakeFrame = 0;
    }
    motion.tilt += (intent.tilt - motion.tilt) * kTiltFollow;
    if (std::fabs(motion.tilt) < 0.001f) {
        motion.tilt = 0.0f;
    }
    const float angle = motion.tilt * kMaxTiltRadians;
    sample.acc[0] = 0.0f;
    sample.acc[1] = -std::cos(angle);
    sample.acc[2] = std::sin(angle);
    if (motion.shakeFrame < kShakeFrames) {
        sample.acc[1] += kShakeFlickG[motion.shakeFrame++];
    }
}

void AnnounceLocked(uint32_t chan, bool pad) {
    if (g_announced[chan]) {
        return;
    }
    g_announced[chan] = true;
    const char* source = "keyboard and every connected gamepad";
    if (pad) {
        const s32 index = PADGetIndexForPort(chan);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        const char* name = gamepad != nullptr ? SDL_GetGamepadName(gamepad) : nullptr;
        source = name != nullptr ? name : g_keyboardOnAPort ? "keyboard (settings bindings)" : "controller";
    }
    RT_LOGF(RT_TAG_RUNTIME, "player %u: %s as a sideways Wii Remote\n", chan + 1, source);
}

} // namespace

void SetTouchState(uint32_t touchButtons, bool shake, float tilt) {
    g_touchButtons.store(touchButtons, std::memory_order_relaxed);
    g_touchButtonsSinceRead.fetch_or(touchButtons, std::memory_order_relaxed);
    g_touchShake.store(shake, std::memory_order_relaxed);
    if (shake) {
        g_touchShakeSinceRead.store(true, std::memory_order_relaxed);
    }
    g_touchTilt.store(std::isfinite(tilt) ? tilt : 0.0f, std::memory_order_relaxed);
}

// -1 until read from Config.toml.
std::atomic<int> g_keyboardRemote{-1};

bool KeyboardIsRemote() {
    int value = g_keyboardRemote.load(std::memory_order_relaxed);
    if (value < 0) {
        value = RuntimeConfigFile::KeyboardBinding("controller") == "wii_remote" ? 1 : 0;
        g_keyboardRemote.store(value, std::memory_order_relaxed);
    }
    return value == 1;
}

void SetKeyboardIsRemote(bool remote) {
    g_keyboardRemote.store(remote ? 1 : 0, std::memory_order_relaxed);
    RuntimeConfigFile::SetKeyboardBinding("controller", remote ? "wii_remote" : "gamecube");
}

bool Present(uint32_t chan) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    // Brawl: gamepads are GameCube controllers (PADRead), the game's main input, and so is the keyboard unless
    // the player picked the Wii Remote for it. Then the port the keyboard plays is a sideways Wii Remote instead
    // (WiiRemoteInput::HideRemotesFromPad takes its GameCube controller away), never both: that would give every
    // press twice. Real Wii Remotes still connect over Bluetooth.
    uint32_t count = 0;
    return KeyboardIsRemote() && PADGetKeyButtonBindings(chan, &count) != nullptr;
}

// The Wii Remote's IR pointer. SPM reads KPAD pos / dpd_valid_fg straight from wpadmgr for Tippi's pointer, item
// aiming and the HOME menu; a sideways remote is "pointing at the screen" only while the player aims it. Here the
// right stick (any gamepad) or mouse movement over the window aims it, and it stays on screen for a moment after.
// Never keep it up from the game's wpadmgr cursor flag (flags bit 2): seq_gameInit sets that for all gameplay, and a
// pointer that is always on screen freezes Mario (he runs in place).
// Which device player 1 last pressed something on. While it's a gamepad the mouse never aims the pointer: a nudged
// mouse (or one moved to start a screen recording) would bring Tippi's pointer up and freeze Mario mid-run.
bool g_lastInputWasPad = false;

void UpdatePointer(WiiRemoteInput::KpadSample& sample) {
    static float pos[2] = {0.0f, 0.0f};
    static uint64_t lastNs = 0, visibleUntilNs = 0;
    static float lastMouse[2] = {-1.0f, -1.0f};
    static bool mouseAiming = false;
    constexpr uint64_t kLingerNs = 2'000'000'000ull;  // pointer stays up this long after the last aim input
    constexpr float kStickSpeed = 1.6f;                // half-screens per second at full right-stick deflection
    constexpr float kStickDeadZone = 0.25f;

    const uint64_t now = SDL_GetTicksNS();
    const float dt = lastNs ? std::min(0.1f, static_cast<float>(now - lastNs) / 1e9f) : 0.0f;
    lastNs = now;
    // Arcade minigames aim with the pointer the whole time: once the stick has brought it up it stays, where it was,
    // instead of hiding after kLingerNs (issue #35).
    if (InArcade() && visibleUntilNs != 0) visibleUntilNs = now + kLingerNs;
    const bool wasVisible = now < visibleUntilNs;
    const auto aim = [&](bool fromMouse) {
        if (!wasVisible && !fromMouse) pos[0] = pos[1] = 0.0f;  // the stick brings it up in the middle
        visibleUntilNs = now + kLingerNs;
        mouseAiming = fromMouse;
    };

    // Right stick of every gamepad (velocity).
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) continue;
        const float rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        const float ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;  // +y down, like KPAD pos
        if (rx * rx + ry * ry < kStickDeadZone * kStickDeadZone) continue;
        aim(false);
        pos[0] += rx * kStickSpeed * dt;
        pos[1] += ry * kStickSpeed * dt;
    }
    SDL_free(ids);

    // Mouse over the game window (absolute): the game's own 16:9 (or 4:3) view inside the presented picture.
    if (SDL_Window* window = SDL_GetMouseFocus()) {
        float mx = 0.0f, my = 0.0f;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        int w = 0, h = 0;
        if (SDL_GetWindowSize(window, &w, &h) && w > 0 && h > 0) {
            // Aim only on a real movement (several pixels), and only when keyboard/mouse is the device in use.
            const float moved = std::fabs(mx - lastMouse[0]) + std::fabs(my - lastMouse[1]);
            if (lastMouse[0] < 0.0f || g_lastInputWasPad) {
                lastMouse[0] = mx;
                lastMouse[1] = my;
                if (g_lastInputWasPad && mouseAiming) {  // gamepad took over: drop the mouse's pointer now
                    mouseAiming = false;
                    visibleUntilNs = 0;
                }
            } else if (moved >= 6.0f) {
                aim(true);
                lastMouse[0] = mx;
                lastMouse[1] = my;
            }
            if (mouseAiming && now < visibleUntilNs) {
                const auto settings = DisplaySettings::Current();
                const float windowAspect = static_cast<float>(w) / static_cast<float>(h);
                const float pictureAspect = settings.aspect == DisplaySettings::kAspect4x3    ? 4.0f / 3.0f
                                            : settings.aspect == DisplaySettings::kAspect16x9  ? 16.0f / 9.0f
                                            : settings.aspect == DisplaySettings::kAspectCinematic
                                                ? DisplaySettings::kRatioCinematic
                                                : windowAspect;
                const float pictureH = pictureAspect >= windowAspect ? static_cast<float>(w) / pictureAspect
                                                                     : static_cast<float>(h);
                const float pictureW = pictureH * pictureAspect;
                const float viewW = std::min(pictureW, pictureH * (DisplaySettings::GameWidescreen() ? 16.0f / 9.0f : 4.0f / 3.0f));
                pos[0] = (mx - w * 0.5f) / (viewW * 0.5f);
                pos[1] = (my - h * 0.5f) / (pictureH * 0.5f);
                if (buttons & SDL_BUTTON_LMASK) sample.hold |= kWpadA;  // click = A where you point
            }
        }
    }

    if (now >= visibleUntilNs) return;
    pos[0] = std::clamp(pos[0], -1.0f, 1.0f);
    pos[1] = std::clamp(pos[1], -1.0f, 1.0f);
    sample.pointerValid = true;
    sample.pointer[0] = pos[0];
    sample.pointer[1] = pos[1];
}

bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    // No stand-in remote on this port (Brawl: gamepads, and the keyboard unless it plays as a Wii Remote, are
    // GameCube controllers): reading one here would hand the game the same gamepad again as Wii Remote 1, so one
    // pad drove both P1 and P2.
    if (chan >= PAD_MAX_CONTROLLERS || !Present(chan)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    // Brawl: the keyboard alone, on its own layout (Keybinds). The pad, gamepad, touch and pointer sources below
    // are NSMBW's and SPM's; Present() is true here only for the keyboard's port.
    if (KeyboardIsRemote()) {
        if (!g_announced[chan]) {
            g_announced[chan] = true;
            RT_LOGF(RT_TAG_RUNTIME, "player %u: keyboard as a sideways Wii Remote\n", chan + 1);
        }
        const Intent keys = KeyboardIntent();
        sample = {};
        sample.hold = keys.buttons;
        ApplyMotion(g_motion[chan], keys, sample);
        return true;
    }
    RefreshPadsLocked();
    // Player 1 reads every gamepad straight from SDL (GamepadIntent); the pad route serves players 2-4.
    const bool pad = chan != 0 && g_pads[chan].err == PAD_ERR_NONE;
    if (chan != 0 && !pad) {
        return false;
    }
    AnnounceLocked(chan, pad);
    Intent intent;
    if (pad) {
        intent = PadIntent(g_pads[chan]);
    }
    const auto merge = [&intent](const Intent& other) {
        intent.buttons |= other.buttons;
        intent.shake = intent.shake || other.shake;
        if (other.tilt != 0.0f) {
            intent.tilt = other.tilt;
        }
    };
    if (chan == 0 && !g_keyboardOnAPort) {
        const Intent keys = KeyboardIntent();
        if (keys.buttons != 0 || keys.shake || keys.tilt != 0.0f) g_lastInputWasPad = false;
        merge(keys);
    }
    if (chan == 0) {
        const Intent pads = GamepadIntent();
        if (pads.buttons != 0 || pads.shake || pads.tilt != 0.0f) g_lastInputWasPad = true;
        merge(pads);
        merge(TouchIntent());
    }
    // No InputBlocked() check here: the startup cover must still see A/2, and frame_input.cpp / kpad.cpp blank the
    // game's copy after that check.
    sample = {};
    sample.hold = intent.buttons;
    ApplyMotion(g_motion[chan], intent, sample);
    if (chan == 0) UpdatePointer(sample);
    return true;
}

} // namespace VirtualRemote
