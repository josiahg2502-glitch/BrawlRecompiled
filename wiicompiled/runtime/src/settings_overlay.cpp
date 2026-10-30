#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <unordered_map>
#include <string>
#include <cstdlib>
#include <filesystem>
extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);
#include "netplay/game_layout.h"
#include "netplay/menu_kit.h"
#include "netplay_start.h"
#include "save_states.h"
#include "updates.h"
#include "brawl_version.h"
#include "netplay_lobby.h"
#include "netplay_session.h"
#include "netplay_ui.h"
#include "settings_overlay.h"
#include "online_menu.h"
#include "graphics_info.h"
#include "hle/gecko.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "input_bindings.h"
#include "keybinds.h"
#include "game_graphics_options.h"
#include "music_attenuation.h"
#include "runtime_config.h"
#include "display_settings.h"
#include <dolphin/gx/GXAurora.h>
#include "runtime_log.h"
#include "memory.h"
#include "generated/RuntimeConfig.h"
#include "wii_remote_input.h"

#include <imgui.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled);
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.
namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
bool g_exitPromptOpen = false;
std::atomic<bool> g_fastForward{std::getenv("BRAWL_FAST_FORWARD") != nullptr};  // the env var: tests start with it on
bool g_rumbleEnabled = RuntimeConfigFile::RumbleEnabled(true);
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
int g_musicVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::MusicVolume(1.0f) * 100.0f));
int g_soundEffectsVolumePercent =
    static_cast<int>(std::lround(RuntimeConfigFile::SoundEffectsVolume(1.0f) * 100.0f));
int g_uiVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::UiVolume(1.0f) * 100.0f));
int g_voicesVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::VoicesVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
bool g_attenuateMusicWhenMediaPlays = RuntimeConfigFile::AttenuateMusicWhenMediaPlays(false);
int g_frameInterpolationMode = [] {
    switch (RuntimeConfigFile::FrameInterpolationFps(0)) {
    case 120:
        return 1;
    case 180:
        return 2;
    case 240:
        return 3;
    default:
        return 0;
    }
}();
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("borderless");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_showFps = RuntimeConfigFile::ShowFps(false);  // off unless turned on
bool g_frameGraph = RuntimeConfigFile::FrameGraph(false);
std::atomic<bool> g_vsync{RuntimeConfigFile::VSync(true)};  // default on: no tearing
#if defined(__APPLE__)
bool g_metalFxSpatialUpscaling = RuntimeConfigFile::MetalFxSpatialUpscaling(false);
#endif
uint32_t g_disabledPostProcessingPaths = RuntimeConfigFile::DisabledPostProcessingPaths(0);
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

// Render heights: 1x is 480 lines in every aspect (DisplaySettings), the width follows the aspect.
constexpr std::array<ResolutionItem, 12> kResolutions = {{
    {"Auto (screen size)", 0.0f}, {"0.5x (240p)", 0.5f}, {"0.75x (360p)", 0.75f}, {"1x (480p)", 1.0f},
    {"1.5x (720p)", 1.5f}, {"2x (960p)", 2.0f}, {"2.25x (1080p)", 2.25f}, {"3x (1440p)", 3.0f},
    {"4x (1920p)", 4.0f}, {"4.5x (4K)", 4.5f}, {"6x (2880p)", 6.0f}, {"8x (3840p)", 8.0f},
}};

struct AspectItem {
    const char* label;
    int32_t mode;
};
constexpr std::array<AspectItem, 4> kAspects = {{
    {"Original 4:3", DisplaySettings::kAspect4x3},
    {"16:9", DisplaySettings::kAspect16x9},
    {"Fill Window", DisplaySettings::kAspectFill},
    {"Cinematic 2.39:1", DisplaySettings::kAspectCinematic},
}};

constexpr std::array<uint32_t, 4> kFrameInterpolationTargetFps{0, 120, 180, 240};

bool IsHighResolutionScale(float scale) {
    return std::fabs(scale - 6.0f) < 0.001f || std::fabs(scale - 8.0f) < 0.001f;
}

bool IsHighFrameRateMode() {
    return kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)] > 60;
}

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.renderScale = scale;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

void SetAspectMode(int32_t mode) {
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.aspect = mode;
    DisplaySettings::Request(settings);
    RuntimeConfigFile::SetAspectMode(mode);
}

void LimitResolutionForFrameRate() {
    if (IsHighFrameRateMode() && IsHighResolutionScale(g_resolutionScale)) {
        SetResolutionScale(4.0f);
    }
}

// A short message along the bottom of the screen for a few seconds (after the title card).
std::string g_notice;
int g_noticeFrames = 0;

// An integrated GPU (a mini PC's Radeon 780M in a player's video) drops well below 60 fps above 1080p: start at
// 1080p for this session and say so. The saved choice stays, and F10 can raise it again.
void LimitResolutionForIntegratedGpu() {
    if (!GraphicsInfo::Integrated() || DisplaySettings::LinesForScale(g_resolutionScale) <= 1080.5f) {
        return;
    }
    g_resolutionScale = 2.25f;
    DisplaySettings::Settings settings = DisplaySettings::Latest();
    settings.renderScale = g_resolutionScale;
    DisplaySettings::Request(settings);
    g_notice = "Integrated graphics: rendering at 1080p to keep the game at full speed. Options (F10) > Graphics > Resolution raises it.";
    g_noticeFrames = 60 * 8;
    RT_LOGF(RT_TAG_CONFIG, "integrated GPU: rendering at 1080p this session (the saved resolution is kept)\n");
}

void DrawNotice() {
    static const bool modProblemShown = [] {
        if (Gecko::SetupProblem().empty()) return false;
        g_notice = Gecko::SetupProblem();
        g_noticeFrames = 60 * 15;
        return true;
    }();
    (void)modProblemShown;
    if (g_noticeFrames <= 0 || StartupScreenVisible()) {
        return;
    }
    --g_noticeFrames;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y - 24.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    if (ImGui::Begin("Notice", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::SetWindowFontScale(std::max(1.0f, viewport->Size.y / 720.0f));
        ImGui::TextUnformatted(g_notice.c_str());
    }
    ImGui::End();
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. (Wii Remotes with any
        // extension never reach the PAD layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0). Sideways as a wheel: (1, 0, 0); z follows the turn.");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(220.0f, 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the steering offset of a remote held sideways.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImGui::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawWiiRemoteSettings(uint32_t selectedGamePort) {
    if (!ImGui::BeginMenu("Wii Remotes (Bluetooth)")) {
        return;
    }
    if (ImGui::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. Turn this off if you use a Mayflash DolphinBar.");
    }
    ImGui::TextDisabled("Pairing: Windows Settings > Bluetooth > Add device, then press 1+2");
    ImGui::TextDisabled("(or the red SYNC button) on the remote. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
    if (ImGui::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii controller is connected, re-check Bluetooth every 2 seconds so a\n"
                          "remote that dropped out (\"Communications with the controller have been\n"
                          "interrupted\") or was turned on after launch comes back by itself.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (ImGui::Button("Rescan now")) {
        WiiRemoteInput::RescanNow();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (WiiRemoteInput::IsScanning()) {
        ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
    } else {
        ImGui::TextDisabled("Not scanning");
    }
    ImGui::Separator();

    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
        kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }

    ImGui::EndMenu();
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

// Waits for the input a rebind asked for (any keyboard key, mouse button, or controller input), or its timeout.
void PollRebind(bool overControl, bool clear = false) {
    if (!g_rebind.active) return;
    const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
    if (g_rebind.active && (clear || remaining <= 0.0f)) {
        CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                              : static_cast<uint32_t>(PAD_KEY_INVALID));
    } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
            if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
            g_rebind.keys[i] = keys[i];
        }
        const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
        for (int i = 1; i <= 5 && g_rebind.active; ++i)
            if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
        g_rebind.mouse = mouse;
    } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
        auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
        if (pad != nullptr) {
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                g_rebind.buttons[i] = pressed;
            }
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                    CompleteRebind(PADEncodeAxisButton(i, value < 0));
            }
        }
    }
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        PollRebind(overControl, clear);
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

bool DrawKeyboardSettings(uint32_t port) {
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (ImGui::Checkbox("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    if (!enabled) return false;
    ImGui::TextDisabled("Replaces the gamepad on this port. F10 opens settings.");
    if (ImGui::Button("Use WASD + mouse preset") || usePreset) {
        const std::array<int, PAD_BUTTON_COUNT> keys = {
            PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        };
        for (size_t i = 0; i < keys.size(); ++i)
            PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
        const std::array<int, PAD_AXIS_COUNT> axes = {
            SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
            SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        };
        uint32_t axisCount = 0;
        auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i)
            PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
        PADSerializeMappings();
    }
    ImGui::SeparatorText("Button mapping");
    for (uint32_t i = 0; i < count; ++i) {
        int key = buttons[i].scancode;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), key, RebindKind::KeyboardButton, buttons[i].padButton);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Stick and trigger mapping");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        int key = axes[i].scancode;
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(label.c_str(), key, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    return true;
}

// Controller settings menu: port selection, controller assignment and button mapping.
int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImGui::SeparatorText("Expressions (Dolphin syntax)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

void DrawRumbleSettings() {
    ImGui::SeparatorText("Vibration");
    if (ImGui::Checkbox("Controller vibration", &g_rumbleEnabled)) {
        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
        if (!g_rumbleEnabled) {
            // Stop whatever is already running: the game will not send another
            // motor command until its own state machine decides to.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies to every port.");
    }
}

void DrawControllerSettings() {
    ImGui::TextUnformatted("Every connected gamepad controls Mario (player 1), mapped for its type.");
    ImGui::TextDisabled("Pick the controller type or remap buttons in the launcher's Controls page.");
    ImGui::Separator();
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Port " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    if (DrawKeyboardSettings(selectedGamePort)) {
        return;
    }
    ImGui::Separator();
    const char* currentName = PADGetName(selectedGamePort);
    ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
    if (ImGui::MenuItem("Unassign controller")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    DrawWiiRemoteSettings(selectedGamePort);
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        return;
    }

    if (ImGui::BeginMenu("Assign connected controller")) {
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::MenuItem(name != nullptr ? name : "Unknown controller")) {
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };

    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::Button("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImGui::SeparatorText("Button mapping");
    ImGui::TextDisabled("LT / L2 = left trigger. RT / R2 = right trigger.");
    ImGui::TextDisabled("LB / L1 = left shoulder. RB / R1 = right shoulder.");
    ImGui::TextDisabled("Click a binding, then press an input. No input for 10 seconds clears it.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(bindingWidth);
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(bindingWidth);
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawExpressionSettings();
    DrawRumbleSettings();
}

void DrawAudioSettings() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    if (ImGui::SliderInt("Music", &g_musicVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_musicVolumePercent) / 100.0f;
        MusicAttenuation::SetMusicVolume(volume);
        RuntimeConfigFile::SetMusicVolume(volume);
    }
    if (ImGui::SliderInt("Sound Effects", &g_soundEffectsVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_soundEffectsVolumePercent) / 100.0f;
        MusicAttenuation::SetSoundEffectsVolume(volume);
        RuntimeConfigFile::SetSoundEffectsVolume(volume);
    }
    if (ImGui::SliderInt("UI", &g_uiVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_uiVolumePercent) / 100.0f;
        MusicAttenuation::SetUiVolume(volume);
        RuntimeConfigFile::SetUiVolume(volume);
    }
    if (ImGui::SliderInt("Voices", &g_voicesVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_voicesVolumePercent) / 100.0f;
        MusicAttenuation::SetVoicesVolume(volume);
        RuntimeConfigFile::SetVoicesVolume(volume);
    }
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Mute game music while external media is playing",
                        &g_attenuateMusicWhenMediaPlays)) {
        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
    }
#if defined(__APPLE__)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Detects other apps with active audio output on macOS 14.2 or later. "
            "Apps that keep an output stream running silently may keep game music muted.");
    }
#endif
    if (g_attenuateMusicWhenMediaPlays) {
        if (MusicAttenuation::IsExternalMediaPlaying()) {
            ImGui::TextDisabled("External media is playing; game music is muted.");
        } else if (!MusicAttenuation::IsMediaControlInitializationComplete()) {
            ImGui::TextDisabled("Checking external audio...");
        } else if (!MusicAttenuation::IsMediaControlAvailable()) {
#if defined(__APPLE__)
            ImGui::TextDisabled("External audio detection unavailable (requires macOS 14.2 or later).");
#else
            ImGui::TextDisabled("Media controls are unavailable.");
#endif
        } else {
            ImGui::TextDisabled("No external media is currently playing.");
        }
    }
}

// LAN Play: this PC's addresses for other players, and the devices asked for rooms directly
// (Tailscale doesn't carry broadcasts; netplay_lobby.h). Peers from `tailscale status` are asked
// anyway; this is for typing in a device's Tailscale name or address by hand.
void DrawLanPlaySettings() {
    static std::vector<std::string> ownAddresses;
    static uint64_t ownAddressesAt = 0;
    if (ownAddressesAt == 0 || SDL_GetTicks() - ownAddressesAt > 2000) {
        ownAddresses = NetplayLobby::OwnAddresses();
        ownAddressesAt = SDL_GetTicks();
    }
    ImGui::TextUnformatted("Rooms are found on your Wi-Fi by themselves, and over Tailscale through");
    ImGui::TextUnformatted("the devices in `tailscale status` and the ones below.");
    ImGui::Separator();
    ImGui::TextUnformatted("This PC (tell other players one of these):");
    for (const auto& address : ownAddresses) {
        ImGui::BulletText("%s%s", address.c_str(), address.rfind("100.", 0) == 0 ? "  (Tailscale)" : "");
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Devices asked for rooms directly:");
    const auto known = NetplayLobby::KnownDevices();
    if (known.empty()) {
        ImGui::TextDisabled("none yet: playing together adds them");
    }
    for (const auto& device : known) {
        ImGui::PushID(device.c_str());
        if (ImGui::SmallButton("Remove")) {
            NetplayLobby::RemoveKnownDevice(device);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(device.c_str());
        ImGui::PopID();
    }
    static char entry[128] = {};
    static std::string result;
    ImGui::SetNextItemWidth(260.0f);
    const bool submitted = ImGui::InputTextWithHint("##add-device", "Tailscale name or address", entry, sizeof(entry),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || submitted) && entry[0] != '\0') {
        result = NetplayLobby::AddKnownDevice(entry) ? std::string("Added ") + entry : "Already in the list, or not a name";
        entry[0] = '\0';
    }
    if (!result.empty()) {
        ImGui::TextDisabled("%s", result.c_str());
    }
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    // NSMBW: the aspect (original 4:3, 16:9, fill) lives in the Aspect menu (DisplaySettings).
    struct EffectFlag {
        const char* label;
        uint32_t flag;
    };
    static constexpr std::array<EffectFlag, 1> kEffectFlags = {{
        {"Disable bloom", 0x10u},
    }};

    for (const auto& effect : kEffectFlags) {
        bool disabled = (g_disabledPostProcessingPaths & effect.flag) != 0;
        if (ImGui::Checkbox(effect.label, &disabled)) {
            if (disabled) {
                g_disabledPostProcessingPaths |= effect.flag;
            } else {
                g_disabledPostProcessingPaths &= ~effect.flag;
            }
            RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
            RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
        }
    }
    ImGui::TextDisabled("Applied when the next scene renderer is created.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImGui::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImGui::TextDisabled(
            "Requests the closest native-resolution display mode to the output frame "
            "rate (60 Hz, or the frame interpolation target).");
    }
    constexpr std::array<const char*, 4> kFrameInterpolationModes{
        "Off", "120 FPS (120 Hz+ screen)", "180 FPS (180 Hz+ screen)", "240 FPS (240 Hz+ screen)",
    };
    const char* currentFrameInterpolationMode =
        kFrameInterpolationModes[static_cast<size_t>(g_frameInterpolationMode)];
    bool frameInterpolationModeChanged = false;
    if (ImGui::BeginCombo("Frame interpolation (experimental)", currentFrameInterpolationMode)) {
        for (int mode = 0; mode < static_cast<int>(kFrameInterpolationModes.size()); ++mode) {
            const bool selected = g_frameInterpolationMode == mode;
            if (ImGui::Selectable(kFrameInterpolationModes[static_cast<size_t>(mode)], selected)) {
                g_frameInterpolationMode = mode;
                frameInterpolationModeChanged = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (frameInterpolationModeChanged) {
        const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
        aurora_set_frame_interpolation_fps(targetFps);
        RuntimeConfigFile::SetFrameInterpolationFps(targetFps);
        LimitResolutionForFrameRate();
        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) {
            // Re-apply exclusive mode so the display refresh tracks the new target.
            aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
        }
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Frame interpolation is experimental, you might find visual artifacts");
    ImGui::PopTextWrapPos();
    if (bool vsync = g_vsync.load(); ImGui::Checkbox("Vsync (no screen tearing)", &vsync)) {
        SetVSync(vsync);
        RuntimeConfigFile::SetVSync(vsync);
    }
#if defined(__APPLE__)
    const bool metalFxSupported = aurora_is_metalfx_spatial_supported();
    ImGui::BeginDisabled(!metalFxSupported);
    if (ImGui::Checkbox("MetalFX spatial upscaling", &g_metalFxSpatialUpscaling)) {
        aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
        RuntimeConfigFile::SetMetalFxSpatialUpscaling(g_metalFxSpatialUpscaling);
    }
    ImGui::EndDisabled();
    switch (aurora_get_metalfx_status()) {
    case AURORA_METALFX_ACTIVE:
        ImGui::TextDisabled("Active: upscaling the game image before the overlay.");
        break;
    case AURORA_METALFX_NOT_UPSCALING:
        ImGui::TextDisabled("Choose a lower internal resolution to use MetalFX.");
        break;
    case AURORA_METALFX_UNSUPPORTED:
        ImGui::TextDisabled("Requires macOS 13+, Metal, and a MetalFX-capable GPU.");
        break;
    case AURORA_METALFX_ERROR:
        ImGui::TextDisabled("Unavailable after a renderer error; toggle off and on to retry.");
        break;
    case AURORA_METALFX_DISABLED:
        if (metalFxSupported) {
            ImGui::TextDisabled("Render below output resolution for sharper lower-cost output.");
        } else {
            ImGui::TextDisabled("MetalFX spatial upscaling is unavailable on this device.");
        }
        break;
    }
#endif
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
}

// The last frames' times (the UI's frame is the presented one), for the graph.
constexpr int kFrameTimeCount = 180;
float g_frameTimes[kFrameTimeCount] = {};
int g_frameTimeNext = 0;

void DrawFrameGraph() {
    float last = g_frameTimes[(g_frameTimeNext + kFrameTimeCount - 1) % kFrameTimeCount];
    float peak = 0.0f, sum = 0.0f;
    int samples = 0;
    for (float t : g_frameTimes) {
        if (t <= 0.0f) continue;
        peak = std::max(peak, t);
        sum += t;
        ++samples;
    }
    ImGui::Text("Frame: %.1f ms", last);
    ImGui::TextDisabled("avg %.1f  max %.1f", samples > 0 ? sum / samples : 0.0f, peak);
    const float scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 size(kFrameTimeCount * scale, 50.0f * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* list = ImGui::GetWindowDrawList();
    // 0 to 33.3 ms (two 60 Hz frames), more when a frame took longer.
    const float top = std::max(100.0f / 3.0f, peak * 1.1f);
    const auto yOf = [&](float ms) { return origin.y + size.y - std::min(ms / top, 1.0f) * size.y; };
    list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 90));
    for (const float line : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
        if (line < top) {
            list->AddLine(ImVec2(origin.x, yOf(line)), ImVec2(origin.x + size.x, yOf(line)), IM_COL32(255, 255, 255, 60));
        }
    }
    const float w = size.x / kFrameTimeCount;
    for (int i = 0; i < kFrameTimeCount; ++i) {
        const float t = g_frameTimes[(g_frameTimeNext + i) % kFrameTimeCount];
        if (t <= 0.0f) continue;
        const ImU32 color = t < 1000.0f / 55.0f ? IM_COL32(90, 220, 90, 230)
                            : t < 1000.0f / 28.0f ? IM_COL32(240, 200, 60, 230)
                                                  : IM_COL32(240, 80, 70, 240);
        const float x = origin.x + i * w;
        list->AddRectFilled(ImVec2(x, yOf(t)), ImVec2(x + std::max(w, 1.0f), origin.y + size.y), color);
    }
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    {
        const float dt = ImGui::GetIO().DeltaTime;
        g_frameTimes[g_frameTimeNext] = dt > 0.0f && dt < 1.0f ? dt * 1000.0f : 0.0f;
        g_frameTimeNext = (g_frameTimeNext + 1) % kFrameTimeCount;
    }
    // Tests: BRAWL_LOG_PRESENT=1 logs the displayed and motion frame rates and the worst frame every 2 s.
    static const bool logPresent = std::getenv("BRAWL_LOG_PRESENT") != nullptr;
    if (logPresent) {
        static double windowStart = ImGui::GetTime(), worst = 0;
        static int frames = 0;
        worst = std::max(worst, static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0);
        ++frames;
        if (ImGui::GetTime() - windowStart >= 2.0) {
            RT_LOGF(RT_TAG_VI, "[present] %.1f fps (motion %.1f), %d frames, worst %.1f ms, scene %s\n", presentTiming.framesPerSecond,
                    presentTiming.effectiveFramesPerSecond, frames, worst, SaveStates::SceneName().c_str());
            windowStart = ImGui::GetTime();
            worst = 0;
            frames = 0;
        }
    }
    if (!g_showFps && !g_frameGraph) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (!g_showFps) {
        } else if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            // Present timing includes the additional frames produced by
            // interpolation, so this remains the actual displayed FPS.
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
            // Replay-unsafe frames hold the presented cadence with duplicated
            // slots, so the counter alone reads 180 while the motion on screen
            // is 60 Hz. Surface the divergence instead of hiding it.
            if (presentTiming.effectiveFramesPerSecond <
                presentTiming.framesPerSecond * 0.95) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Motion: %.1f",
                                   presentTiming.effectiveFramesPerSecond);
            }
        }
        if (g_frameGraph) {
            DrawFrameGraph();
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

std::string StartButtonName();
std::string g_busyLabel;  // SetBusyProgress
float g_busyFraction = 0.0f;

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        // The credits, in the game's own fonts when the game folder is readable (it always is once
        // the game can start), else in the overlay's font.
        constexpr const char* kTitle = "Made by JosiahG";
        constexpr const char* kCredit =
            "A Super Smash Bros. Brawl PC recomp. Built on WiiCompiled's code.";
        // At the bottom of the screen.
        constexpr const char* kReport = "";
        // Super Smash Bros. Brawl's title font and its menu font, read from the disc.
        static const std::shared_ptr<GameLayout::Font> pmFont = GameLayout::LoadFont("/system/font/font_hira.brfnt");
        static const std::shared_ptr<GameLayout::Font> menuFont = GameLayout::LoadFont("/system/font/font_melee.brfnt");
        float startY = 0.0f;
        float titleHeight = ImGui::GetTextLineHeight();
        const bool gameFonts = GameLayout::FontReady(pmFont.get());
        char shaderLine[96] = {};
        if (!g_busyLabel.empty()) {
            std::snprintf(shaderLine, sizeof(shaderLine), "%s", g_busyLabel.c_str());
        } else if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
            std::snprintf(shaderLine, sizeof(shaderLine), "Compiling shaders... %u to go",
                          aurora_get_queued_pipeline_count());
        }
        if (gameFonts) {
            // A Brawl title card: a night sky with rising embers and light rays, a dark card with the credits, and the
            // Smash Ball turning in the top-left corner (<game folder>/startup/smash_ball.png, from tools/make_icon.py).
            const GameLayout::Font& small = GameLayout::FontReady(menuFont.get()) ? *menuFont : *pmFont;
            struct Sprite { ImTextureID id{}; float w = 0, h = 0; };
            static std::unordered_map<std::string, Sprite> sprites;
            auto Spr = [&](const char* name) -> const Sprite& {
                auto it = sprites.find(name);
                if (it != sprites.end()) return it->second;
                Sprite sp;
                uint32_t w = 0, h = 0;
                const std::string file = name[0] == '@' ? std::string(name + 1) : std::string("startup/") + name + ".png";  // '@' = a path
                if (uint8_t* rgba = aurora_load_png_rgba(file.c_str(), &w, &h)) {
                    sp.id = aurora_imgui_add_texture(w, h, rgba);
                    sp.w = float(w);
                    sp.h = float(h);
                    std::free(rgba);
                }
                return sprites.emplace(name, sp).first->second;
            };
            ImDrawList* list = ImGui::GetWindowDrawList();
            const GameLayout::View view = GameLayout::GameView();
            const float t = static_cast<float>(ImGui::GetTime());
            const ImVec2 o = viewport->Pos, sz = viewport->Size;
            const float k = view.scale;
            auto S = [&](float x, float y) { return ImVec2(view.centerX + x * k, view.centerY - y * k); };
            // Sprite of height `hgt` layout units centred at (x, y), tilted by `ang`, optionally mirrored.
            auto Draw = [&](const char* name, float x, float y, float hgt, float ang, bool flip, uint32_t col = IM_COL32_WHITE) {
                const Sprite& sp = Spr(name);
                if (!sp.id || sp.h <= 0) return;
                const float hw = hgt * sp.w / sp.h * 0.5f, hh = hgt * 0.5f;
                const float c = std::cos(ang), sn = std::sin(ang);
                auto P = [&](float dx, float dy) { return S(x + dx * c - dy * sn, y + dx * sn + dy * c); };
                const float u0 = flip ? 1.f : 0.f, u1 = flip ? 0.f : 1.f;
                list->AddImageQuad(sp.id, P(-hw, hh), P(hw, hh), P(hw, -hh), P(-hw, -hh), ImVec2(u0, 0), ImVec2(u1, 0),
                                   ImVec2(u1, 1), ImVec2(u0, 1), col);
            };
            // A four-point sparkle of radius r at (x, y).
            auto Sparkle = [&](float x, float y, float r, float spin, uint32_t col) {
                ImVec2 pts[8];
                for (int j = 0; j < 8; ++j) {
                    const float a = spin + j * 0.7854f, rr = (j % 2 == 0) ? r : r * 0.3f;
                    pts[j] = S(x + std::cos(a) * rr, y + std::sin(a) * rr);
                }
                list->AddConvexPolyFilled(pts, 8, col);
            };
            // Night sky, and light rays turning slowly from behind the card.
            list->AddRectFilledMultiColor(o, ImVec2(o.x + sz.x, o.y + sz.y), IM_COL32(12, 14, 30, 255),
                                          IM_COL32(12, 14, 30, 255), IM_COL32(52, 26, 40, 255), IM_COL32(52, 26, 40, 255));
            for (int j = 0; j < 12; ++j) {
                const float a = t * 0.08f + j * 0.5236f, w = 0.11f;
                const ImVec2 c = S(0.0f, 30.0f);
                const float r = std::max(sz.x, sz.y);
                list->AddTriangleFilled(c, ImVec2(c.x + std::cos(a - w) * r, c.y + std::sin(a - w) * r),
                                        ImVec2(c.x + std::cos(a + w) * r, c.y + std::sin(a + w) * r), IM_COL32(255, 196, 80, 14));
            }
            // Embers rise from the bottom and fade, each on its own lane.
            for (int i = 0; i < 48; ++i) {
                const float life = std::fmod(t * (0.10f + 0.035f * (i % 5)) + i * 0.37f, 1.0f);
                const float x = o.x + std::fmod(i * 97.0f, 100.0f) / 100.0f * sz.x + std::sin(t * 1.3f + i) * 10.0f * k;
                const float y = o.y + sz.y - life * sz.y;
                list->AddCircleFilled(ImVec2(x, y), (2.0f + i % 3) * k, (i % 2) ? IM_COL32(255, 140, 40, int(180 * (1 - life)))
                                                                               : IM_COL32(255, 196, 58, int(180 * (1 - life))), 8);
            }
            // The card.
            const float bob = std::sin(t * 1.6f) * 3.0f;
            const float ang = -0.02f + std::sin(t * 0.9f) * 0.005f;
            auto R = [&](float x, float y, float dx, float dy) {
                const float c = std::cos(ang), sn = std::sin(ang);
                return S(dx + x * c - y * sn, dy + bob + x * sn + y * c);
            };
            const float hw = 280.0f, hh = 140.0f, cardY = 20.0f;
            auto Q = [&](float dx, float dy, float grow, uint32_t col) {
                list->AddQuadFilled(R(-hw - grow, cardY + hh + grow, dx, dy), R(hw + grow, cardY + hh + grow, dx, dy),
                                    R(hw + grow, cardY - hh - grow, dx, dy), R(-hw - grow, cardY - hh - grow, dx, dy), col);
            };
            Q(12, -12, 0, IM_COL32(0, 0, 0, 120));
            Q(0, 0, 6, IM_COL32(255, 140, 40, 255));   // orange frame
            Q(0, 0, 0, IM_COL32(26, 30, 48, 245));
            list->AddLine(R(-hw + 30, cardY + 36, 0, 0), R(hw - 30, cardY + 36, 0, 0), IM_COL32(255, 196, 58, 120), 2.0f * k);
            list->AddLine(R(-hw + 30, cardY - 2, 0, 0), R(hw - 30, cardY - 2, 0, 0), IM_COL32(255, 196, 58, 120), 2.0f * k);
            // Text, shrunk to fit inside the card.
            const float maxW = 2 * hw - 70.0f;
            auto Fit = [&](const GameLayout::Font& font, const std::u16string& u, float size) {
                const float w = GameLayout::MeasureText(font, u, size);
                return w > maxW ? size * maxW / w : size;
            };
            auto Outlined = [&](const char* text, float y, float size, uint32_t top, uint32_t bottom, float ink, uint32_t inkCol = 0x0B0D16FFu) {
                const auto u = GameLayout::Utf16(text);
                const float fs = Fit(*pmFont, u, size);
                for (int j = 0; j < 12; ++j) {
                    const float a = j * 0.5236f;
                    GameLayout::DrawText(list, view, *pmFont, u, std::cos(a) * ink, y + bob + std::sin(a) * ink, fs,
                                         inkCol, inkCol, 1.0f, 1, 1);
                }
                GameLayout::DrawText(list, view, *pmFont, u, 0.0f, y + bob, fs, top, bottom, 1.0f, 1, 1);
            };
            Outlined("SUPER SMASH BROS. BRAWL", cardY + 108.0f, 22.0f, 0xF2F2F6FFu, 0xB8BCCBFFu, 2.0f);
            Outlined("RECOMPILED", cardY + 70.0f, 46.0f, 0xFFF3B0FFu, 0xFF8C28FFu, 3.5f);
            Outlined("Made by JosiahG", cardY + 16.0f, 44.0f, 0xFFFFFFFFu, 0xFFC86AFFu, 3.5f, 0x7A1A10FFu);
            {
                const auto u = GameLayout::Utf16(kCredit);
                GameLayout::DrawText(list, view, small, u, 0.0f, cardY - 30.0f + bob, Fit(small, u, 17.0f), 0xD6D9E4FFu,
                                     0xB8BCCBFFu, 1.0f, 1, 1);
                // How to reach the settings, along the card's bottom edge.
                const auto hint = GameLayout::Utf16("F10 or Home = options   F5 / F7 = save / load state   F8 = fast-forward   F9 = online");
                GameLayout::DrawText(list, view, small, hint, 0.0f, cardY - hh + 16.0f + bob, Fit(small, hint, 12.0f),
                                     0x8E93A6FFu, 0x8E93A6FFu, 1.0f, 1, 1);
            }
            if (shaderLine[0] != '\0') {
                const float bw = 180.0f, by = cardY - 70.0f;
                list->AddRectFilled(R(-bw, by + 10, 0, 0), R(bw, by - 10, 0, 0), IM_COL32(8, 10, 18, 255), 5.0f * k);
                list->AddRectFilled(R(-bw + 4, by + 6, 0, 0), R(bw - 4, by - 6, 0, 0), IM_COL32(52, 58, 80, 255), 3.0f * k);
                if (!g_busyLabel.empty()) {
                    const float fill = -bw + 4 + std::clamp(g_busyFraction, 0.0f, 1.0f) * (2 * bw - 8);
                    list->AddRectFilled(R(-bw + 4, by + 6, 0, 0), R(fill, by - 6, 0, 0), IM_COL32(255, 140, 40, 255),
                                        3.0f * k);
                } else {
                    const float p = std::fmod(t * 0.6f, 1.0f);
                    const float sx = -bw + 4 + p * (2 * bw - 58);
                    list->AddRectFilled(R(sx, by + 6, 0, 0), R(sx + 50, by - 6, 0, 0), IM_COL32(255, 140, 40, 255),
                                        3.0f * k);
                }
                const auto u = GameLayout::Utf16(shaderLine);
                GameLayout::DrawText(list, view, small, u, 0.0f, by - 26.0f + bob, Fit(small, u, 15.0f), 0xD6D9E4FFu,
                                     0xD6D9E4FFu, 1.0f, 1, 1);
            } else {
                // Press start: A on a GameCube controller (any gamepad's bottom face button), Enter on the keyboard
                // pad, A or 2 on a Wii Remote (frame_input.cpp lifts the cover).
                const float pop = 1.0f + 0.06f * std::sin(t * 5.0f), py = cardY - 82.0f + bob;
                const auto press = GameLayout::Utf16("Press " + StartButtonName() + " to start!");
                const float fs = Fit(*pmFont, press, 26.0f) * pop;
                for (int j = 0; j < 12; ++j) {
                    const float a = j * 0.5236f;
                    GameLayout::DrawText(list, view, *pmFont, press, std::cos(a) * 2.5f, py + std::sin(a) * 2.5f, fs,
                                         0x0B0D16FFu, 0x0B0D16FFu, 1.0f, 1, 1);
                }
                GameLayout::DrawText(list, view, *pmFont, press, 0.0f, py, fs, 0xFFFFFFFFu, 0xFF8C28FFu, 1.0f, 1, 1);
            }
            // The Smash Ball in the top-left corner, turning slowly.
            {
                const float size = std::min(sz.x, sz.y) * 0.16f;
                const ImVec2 at(o.x + size * 0.75f, o.y + size * 0.75f);
                Draw("smash_ball", (at.x - view.centerX) / k, (view.centerY - at.y) / k, size / k, t * 0.35f, false);
            }
            // Twinkles scattered over the sky.
            const float tw[6][2] = {{-420, 120}, {-230, 245}, {250, 240}, {430, 120}, {-60, 255}, {120, 260}};
            for (int j = 0; j < 6; ++j) {
                const float s2 = 0.5f + 0.5f * std::sin(t * 2.4f + j * 1.7f);
                Sparkle(tw[j][0], tw[j][1], 12.0f * s2 + 3.0f, t + j, IM_COL32(255, 255, 255, int(120 + 135 * s2)));
            }
            startY = view.centerY + 200.0f * k;
            titleHeight = 0.0f;
        } else {
            ImGui::SetWindowFontScale(1.25f);
            const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
            startY = std::max(0.0f, viewport->Size.y * 0.3f);
            ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f), startY));
            ImGui::TextUnformatted(kTitle);
            ImGui::SetWindowFontScale(1.0f);
            const float wrapWidth = std::min(viewport->Size.x - 48.0f, 640.0f);
            ImGui::SetCursorPos(ImVec2((viewport->Size.x - wrapWidth) * 0.5f, startY + titleSize.y * 2.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
            ImGui::TextUnformatted(kCredit);
            ImGui::PopTextWrapPos();
            startY = ImGui::GetCursorPosY();
            const ImVec2 reportSize = ImGui::CalcTextSize(kReport);
            ImGui::SetCursorPos(ImVec2(std::max(0.0f, (viewport->Size.x - reportSize.x) * 0.5f),
                                       viewport->Size.y - reportSize.y - 24.0f));
            ImGui::TextUnformatted(kReport);
            titleHeight = 0.0f;
        }
        const ImVec2 titleSize{0.0f, titleHeight};
        if (!gameFonts && shaderLine[0] != '\0') {
            ImGui::SetWindowFontScale(0.9f);
            const float lineX = std::max(0.0f, (viewport->Size.x - ImGui::CalcTextSize(shaderLine).x) * 0.5f);
            ImGui::SetCursorPos(ImVec2(lineX, startY + titleSize.y * 1.8f));
            ImGui::TextUnformatted(shaderLine);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    if (queued == 0 || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
        // The title card then waits for a press (UpdateStartupPress); scripted test runs go straight on.
        if (std::getenv("NSMBW_INPUT_SCRIPT") != nullptr || std::filesystem::exists("input_script.txt")) {
            NotifyStrapInputAccepted();
        }
    }
}

// The button that starts, as printed on the first connected gamepad (its bottom face button: A on Xbox and GameCube
// style pads, B on Nintendo's, Cross on PlayStation's), or Enter without one.
std::string StartButtonName() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::string name = "Enter";
    if (ids != nullptr && count > 0) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[0]);
        switch (pad != nullptr ? SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH) : SDL_GAMEPAD_BUTTON_LABEL_A) {
        case SDL_GAMEPAD_BUTTON_LABEL_B: name = "B"; break;
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: name = "Cross"; break;
        default: name = "A"; break;
        }
    }
    SDL_free(ids);
    return name;
}

// The title card lifts on the start button (any gamepad's bottom face button or Start), Enter or Space, or a click,
// once it has been up for a moment so the credits are seen. Wii Remotes lift it from frame_input.cpp.
void UpdateStartupPress() {
    static int shownFrames = 0;
    if (!g_bootShadersReady.load(std::memory_order_acquire) || g_strapInputAccepted.load(std::memory_order_acquire)) return;
    if (++shownFrames < 90) return;
    bool pressed = false;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i])) {
            pressed = pressed || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        }
    }
    SDL_free(ids);
    // Keys and clicks count only while the game window has them (typing or clicking in another program does not).
    const bool* keys = SDL_GetKeyboardState(nullptr);
    pressed = pressed || (SDL_GetKeyboardFocus() != nullptr && keys != nullptr &&
                          (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_KP_ENTER]));
    pressed = pressed || (SDL_GetMouseFocus() != nullptr && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0);
    if (pressed) NotifyStrapInputAccepted();
}

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Quit the game?");
    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

#include "settings_menu.inc"

void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 70));
    constexpr float kHintMargin = 10.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + ImGui::GetFrameHeight() + kHintMargin),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Settings input hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        for (const char* line : {"Settings open - game controls disabled.",
                                 "Press F10 to return to the game."}) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::End();
    if (!ImGui::BeginMainMenuBar()) return;

    ImGui::TextUnformatted("Brawl Recompiled");
    ImGui::Separator();
    const auto resolutionIt = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    const char* resolutionLabel = resolutionIt != kResolutions.end() ? resolutionIt->label : "Custom";
    const std::string resolutionMenuLabel = std::string("Resolution: ") + resolutionLabel;
    if (ImGui::BeginMenu(resolutionMenuLabel.c_str())) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            const bool disabled = IsHighFrameRateMode() && IsHighResolutionScale(resolution.scale);
            ImGui::BeginDisabled(disabled);
            const bool clicked = ImGui::MenuItem(resolution.label, nullptr, selected);
            ImGui::EndDisabled();
            if (clicked) {
                SetResolutionScale(resolution.scale);
            }
        }
        uint32_t pictureWidth = 0;
        uint32_t pictureHeight = 0;
        AuroraGetPictureSize(&pictureWidth, &pictureHeight);
        ImGui::Separator();
        ImGui::TextDisabled("Rendering %ux%u", pictureWidth, pictureHeight);
        ImGui::EndMenu();
    }

    const DisplaySettings::Settings display = DisplaySettings::Current();
    const auto aspectIt = std::find_if(kAspects.begin(), kAspects.end(), [&](const AspectItem& item) {
        return item.mode == display.aspect;
    });
    const std::string aspectMenuLabel =
        std::string("Aspect: ") + (aspectIt != kAspects.end() ? aspectIt->label : "Custom");
    if (ImGui::BeginMenu(aspectMenuLabel.c_str())) {
        for (const auto& aspect : kAspects) {
            if (ImGui::MenuItem(aspect.label, nullptr, aspect.mode == display.aspect)) {
                SetAspectMode(aspect.mode);
            }
        }
        // The game reads 4:3 versus widescreen once, at boot.
        if ((display.aspect != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen()) {
            ImGui::Separator();
            ImGui::TextDisabled("Switching between 4:3 and widescreen takes effect after a restart.");
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Graphics")) {
        DrawGraphicsSettings();
        ImGui::EndMenu();
    }

    if (bool fast = g_fastForward.load(); ImGui::MenuItem("Fast-forward (F8)", nullptr, &fast)) g_fastForward.store(fast);

    if (ImGui::MenuItem("Online (F9)")) {
        OnlineMenu::SetOpen(true);
        SetTopBarVisible(false);
    }

    if (ImGui::BeginMenu("Controller settings")) {
        DrawControllerSettings();
        // Nest capture under this menu so opening/closing the modal preserves
        // the settings popup and its current port and scroll position.
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("LAN Play")) {
        DrawLanPlaySettings();
        ImGui::EndMenu();
    }

    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImGui::BeginMenu(audioMenuLabel.c_str())) {
        DrawAudioSettings();
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + style.FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetWindowWidth() - hideWidth - exitWidth - style.ItemSpacing.x - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    if (ImGui::MenuItem("X")) {
        g_exitPromptOpen = true;
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active ||
                         g_exitPromptOpen || g_topBarVisible || StartupScreenVisible() || OnlineMenu::IsOpen();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    MusicAttenuation::SetMusicVolume(static_cast<float>(g_musicVolumePercent) / 100.0f);
    MusicAttenuation::SetSoundEffectsVolume(static_cast<float>(g_soundEffectsVolumePercent) / 100.0f);
    MusicAttenuation::SetUiVolume(static_cast<float>(g_uiVolumePercent) / 100.0f);
    MusicAttenuation::SetVoicesVolume(static_cast<float>(g_voicesVolumePercent) / 100.0f);
    MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
    RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
#if defined(__APPLE__)
    aurora_set_metalfx_spatial(g_metalFxSpatialUpscaling);
#endif
    const uint32_t targetFps = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
    LimitResolutionForFrameRate();
    LimitResolutionForIntegratedGpu();
    aurora_set_frame_interpolation_fps(targetFps);
    AuroraSetTexturePatches(RuntimeConfigFile::SpinningCoins(true));
    aurora_set_vsync(g_vsync.load());
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        // NSMBW: F11 toggles between a window and borderless fullscreen (saved like the F10 bar's
        // Display mode).
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F11)) {
            const AuroraDisplayMode mode = aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED
                                               ? AURORA_DISPLAY_MODE_BORDERLESS
                                               : AURORA_DISPLAY_MODE_WINDOWED;
            aurora_set_display_mode(mode);
            g_displayMode = static_cast<int>(aurora_get_display_mode());
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F8)) g_fastForward.store(!g_fastForward.load());
        // F5 / F7: save / load state in the chosen slot, F6: the next slot.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F5)) SaveStates::RequestSave();
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F7)) SaveStates::RequestLoad();
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F6)) {
            SaveStates::SetSlot((SaveStates::Slot() + 1) % SaveStates::kSlots);
            g_notice = "Save state slot " + std::to_string(SaveStates::Slot() + 1) + ": " + SaveStates::Describe(SaveStates::Slot());
            g_noticeFrames = 60 * 3;
        }
        // F9: the online play menu.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F9)) {
            OnlineMenu::SetOpen(!OnlineMenu::IsOpen());
            if (OnlineMenu::IsOpen()) SetTopBarVisible(false);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        // NSMBW: Escape is the game's + (its pause menus, Keybinds); here it only closes the F10 bar
        // or the quit prompt.
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            }
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

bool g_spinningCoins = RuntimeConfigFile::SpinningCoins(true);

void SetSpinningCoins(bool enabled) noexcept {
    g_spinningCoins = enabled;
    AuroraSetTexturePatches(enabled);
    RuntimeConfigFile::SetSpinningCoins(enabled);
}

bool SpinningCoins() noexcept {
    return g_spinningCoins;
}

void SetShowFps(bool show) noexcept {
    g_showFps = show;
    RuntimeConfigFile::SetShowFps(show);
}

bool ShowFps() noexcept {
    return g_showFps;
}

int GameSpeed() noexcept { return g_gameSpeed.load(std::memory_order_relaxed); }

bool FastForward() noexcept {
    return g_fastForward.load(std::memory_order_relaxed) && !NetplaySession::Active();
}

void SetVSync(bool enabled) noexcept {
    g_vsync.store(enabled);
    aurora_set_vsync(enabled);
    RuntimeConfigFile::SetVSync(enabled);
}

bool VSync() noexcept {
    return g_vsync.load(std::memory_order_relaxed);
}

void SetFrameGraph(bool show) noexcept {
    g_frameGraph = show;
    RuntimeConfigFile::SetFrameGraph(show);
}

bool FrameGraph() noexcept {
    return g_frameGraph;
}

void SetRenderScale(float scale) noexcept {
    SetResolutionScale(scale);
}

float RenderScale() noexcept {
    return g_resolutionScale;
}

void SetAspect(int32_t mode) noexcept {
    SetAspectMode(mode);
}

int32_t Aspect() noexcept {
    return DisplaySettings::Latest().aspect;
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

// New Super Paper Bros.: the game's Controls screen (- button) shows a 3D Wii Remote with bubbles pointing at its
// buttons. With a controller in use, this draws a controls panel over it instead: a picture of that controller with a
// callout for each action the game's screen is showing, in the game's own font. The remote's textures are hidden and
// Aurora reports which of that screen's parts are drawn (UserData/button_icons/<style>/marker<N>,
// tools/make-button-icons.py): marker 1 = the remote (the screen is up), 2-6 = the Move, Flip, Jump, Use a Pixl and
// Use Tippi bubbles.
void DrawControlsPanel() {
    const int style = aurora_button_icon_style();
    static double shownSince = -1.0;
    static bool seen[8] = {};  // actions shown since the screen opened (bubbles slide in one after another)
    // The screen is up while the remote's body (marker 1) and its Move bubble (marker 2, on every version of it) are
    // drawn: either alone also appears elsewhere, and a panel opened there could not be closed (issue #7).
    const bool screenUp = aurora_button_icon_marker(1) && (seen[2] || aurora_button_icon_marker(2));
    if (style < 0 || style > 2 || !screenUp) {
        if (shownSince >= 0.0)  // closed: what it showed (diagnoses a missing callout)
            std::fprintf(stderr, "[controls panel] style %d showed move %d flip %d jump %d pixl %d tippi %d\n", style, seen[2],
                         seen[3], seen[4], seen[5], seen[6]);
        shownSince = -1.0;
        std::fill(std::begin(seen), std::end(seen), false);
        return;
    }
    if (shownSince < 0.0) shownSince = ImGui::GetTime();
    for (int m = 2; m <= 6; ++m) seen[m] = seen[m] || aurora_button_icon_marker(m);
    seen[7] = true;  // Stylish moves (shake): not on the game's screen, always worth showing
    const float alpha = std::min(1.0f, static_cast<float>(ImGui::GetTime() - shownSince) / 0.2f);  // fade in

    struct Sprite { ImTextureID id{}; float aspect = 1.0f; };
    struct Style {
        bool loaded = false;
        Sprite pad, icons[8];
        ImVec2 spots[8];  // panel.txt: where each action's button is on the picture (fractions)
    };
    static Style styles[3];
    static const char* const kActions[8] = {"", "", "move", "flip", "jump", "pixl", "tippi", "stylish"};
    Style& st = styles[style];
    if (!st.loaded) {
        st.loaded = true;
        static const char* const kNames[3] = {"xbox", "playstation", "nintendo"};
        const auto dir = RuntimeConfigFile::ApplicationDataDirectory() / "button_icons" / kNames[style];
        const auto load = [](const std::filesystem::path& path) {
            Sprite sp;
            uint32_t w = 0, h = 0;
            if (uint8_t* rgba = aurora_load_png_rgba(RuntimeConfigFile::PathToUtf8(path).c_str(), &w, &h)) {
                sp.id = aurora_imgui_add_texture(w, h, rgba);
                sp.aspect = static_cast<float>(w) / static_cast<float>(h);
                std::free(rgba);
            }
            return sp;
        };
        st.pad = load(dir / "controller.png");
        for (int m = 2; m <= 7; ++m) st.icons[m] = load(dir / "ui" / (std::string(kActions[m]) + ".png"));
        std::ifstream panel(dir / "panel.txt");
        std::string name;
        float fx = 0.0f, fy = 0.0f;
        while (panel >> name >> fx >> fy)
            for (int m = 2; m <= 7; ++m)
                if (name == kActions[m]) st.spots[m] = ImVec2(fx, fy);
    }
    if (!st.pad.id) return;

    static const std::shared_ptr<GameLayout::Font> font = GameLayout::LoadFont("papermarioset_US.bfn");
    const bool gameFont = GameLayout::FontReady(font.get());
    const GameLayout::View view = GameLayout::GameView();
    // Layout units: the game's 832 x 456 canvas, origin at the centre, y up.
    const auto at = [&](float x, float y) { return ImVec2(view.centerX + x * view.gameScaleX, view.centerY - y * view.scale); };
    const auto a = [&](int v) { return static_cast<int>(v * alpha); };
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const auto text = [&](const char* s, float x, float y, float size, uint32_t rgba, int originH) {
        if (gameFont) {
            const uint32_t c = (rgba & 0xFFFFFF00u) | static_cast<uint32_t>(a(rgba & 0xFF));
            GameLayout::DrawText(list, view, *font, GameLayout::Utf16(s), x, y, size, c, c, 1.0f, originH, 1);
        } else {
            const ImVec2 p = at(x, y);
            const float w = ImGui::CalcTextSize(s).x;
            list->AddText(ImVec2(p.x - (originH == 1 ? w / 2 : originH == 2 ? w : 0.0f), p.y - ImGui::GetTextLineHeight() / 2),
                          IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, a(rgba & 0xFF)), s);
        }
    };
    const auto textWidth = [&](const char* s, float size) {
        return gameFont ? GameLayout::MeasureText(*font, GameLayout::Utf16(s), size) : ImGui::CalcTextSize(s).x / view.scale;
    };

    // The card, over the whole of the game's screen (its bubbles and remote), with a header.
    const ImVec2 c0 = at(-392.0f, 205.0f), c1 = at(392.0f, -218.0f);
    list->AddRectFilled(c0, c1, IM_COL32(16, 18, 26, a(255)), 22.0f * view.scale);
    list->AddRect(c0, c1, IM_COL32(255, 255, 255, a(46)), 22.0f * view.scale, 0, 2.0f * view.scale);
    static const char* const kPadNames[3] = {"Xbox controller", "PlayStation controller", "Nintendo Switch controller"};
    text("Controls", -360.0f, 176.0f, 26.0f, 0xFFFFFFFFu, 0);
    text(kPadNames[style], 360.0f, 176.0f, 15.0f, 0xA8AEBDFFu, 2);
    list->AddLine(at(-360.0f, 152.0f), at(360.0f, 152.0f), IM_COL32(255, 255, 255, a(30)), 1.5f * view.scale);

    // The controller.
    constexpr float kPadW = 300.0f, kPadCX = -28.0f, kPadCY = -18.0f;
    const float padH = kPadW / st.pad.aspect;
    const ImVec2 p0 = at(kPadCX - kPadW / 2, kPadCY + padH / 2), p1 = at(kPadCX + kPadW / 2, kPadCY - padH / 2);
    list->AddImage(st.pad.id, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a(255)));

    // Callouts: Move on the left, the face buttons and the right stick on the right, each in the order of its button
    // from top to bottom, with a leader line to the button.
    struct Callout { int m; const char* title; const char* sub; };
    static const Callout kLeft[] = {{2, "Move", "Speak, interact"}};
    static const char* const kShakeButtons[3] = {"RB or RT, after a stomp", "R1 or R2, after a stomp", "R or ZR, after a stomp"};
    const Callout kRight[] = {{7, "Stylish move", kShakeButtons[style]}, {3, "Flip", ""}, {5, "Use a Pixl", "Cancel"},
                              {4, "Jump", "Confirm"}, {6, "Use Tippi", ""}};
    const auto callouts = [&](const Callout* items, size_t count, bool right) {
        std::vector<const Callout*> shown;
        for (size_t i = 0; i < count; ++i)
            if (seen[items[i].m]) shown.push_back(&items[i]);
        std::sort(shown.begin(), shown.end(), [&](const Callout* l, const Callout* r) { return st.spots[l->m].y < st.spots[r->m].y; });
        // Each label level with its button, pushed apart where they would touch, so the leaders run nearly level.
        std::vector<float> ys;
        for (const Callout* co : shown) ys.push_back(kPadCY + padH / 2 - st.spots[co->m].y * padH);
        constexpr float kSpacing = 50.0f, kTop = 120.0f, kBottom = -150.0f;
        for (size_t i = 0; i < ys.size(); ++i) ys[i] = std::min(ys[i], i == 0 ? kTop : ys[i - 1] - kSpacing);
        for (size_t i = ys.size(); i-- > 0;) ys[i] = std::max(ys[i], i + 1 == ys.size() ? kBottom : ys[i + 1] + kSpacing);
        for (size_t i = 0; i < shown.size(); ++i) {
            const Callout& co = *shown[i];
            const float y = ys[i];
            const float edge = right ? 360.0f : -360.0f, icon = 30.0f;
            // icon at the outer edge, text inside it
            const float ix = right ? edge - icon / 2 : edge + icon / 2;
            if (st.icons[co.m].id)
                list->AddImage(st.icons[co.m].id, at(ix - icon / 2, y + icon / 2), at(ix + icon / 2, y - icon / 2), ImVec2(0, 0),
                               ImVec2(1, 1), IM_COL32(255, 255, 255, a(255)));
            const float tx = right ? edge - icon - 8.0f : edge + icon + 8.0f;
            const bool two = co.sub[0] != '\0';
            text(co.title, tx, two ? y + 8.0f : y, 17.0f, 0xFFFFFFFFu, right ? 2 : 0);
            if (two) text(co.sub, tx, y - 11.0f, 13.0f, 0xA8AEBDFFu, right ? 2 : 0);
            // leader: from just past the text toward the button, ending on it with a dot
            const float tw = std::max(textWidth(co.title, 17.0f), two ? textWidth(co.sub, 13.0f) : 0.0f);
            const ImVec2 from = at(right ? tx - tw - 10.0f : tx + tw + 10.0f, y);
            const ImVec2 to(p0.x + st.spots[co.m].x * (p1.x - p0.x), p0.y + st.spots[co.m].y * (p1.y - p0.y));
            list->AddLine(from, to, IM_COL32(255, 255, 255, a(150)), 1.6f * view.scale);
            list->AddCircleFilled(to, 4.0f * view.scale, IM_COL32(255, 255, 255, a(235)));
            list->AddCircleFilled(from, 2.2f * view.scale, IM_COL32(255, 255, 255, a(150)));
        }
    };
    callouts(kLeft, std::size(kLeft), false);
    callouts(kRight, std::size(kRight), true);

    // Footer, centred: [1] + [2] Quick menu (the game's 1 + 2 combination).
    const float fy = -190.0f, icon = 24.0f, plus = 16.0f, gap = 10.0f;
    float x = -(icon * 2 + plus + gap + textWidth("Quick menu", 15.0f)) / 2;
    const auto footerIcon = [&](int m) {
        if (st.icons[m].id)
            list->AddImage(st.icons[m].id, at(x, fy + icon / 2), at(x + icon, fy - icon / 2), ImVec2(0, 0), ImVec2(1, 1),
                           IM_COL32(255, 255, 255, a(255)));
        x += icon;
    };
    footerIcon(5);
    text("+", x + plus / 2, fy, 15.0f, 0xA8AEBDFFu, 1);
    x += plus;
    footerIcon(4);
    text("Quick menu", x + gap, fy, 15.0f, 0xFFFFFFFFu, 0);
}

// New Super Paper Bros.: the boot screen "Hold the Wii Remote sideways." takes its line from a table in main.dol (one
// per language: the USA build has English, French and Japanese, the PAL build all five plus Japanese). Each one reads
// "Enjoy New Super Paper Bros!" instead, rewritten in guest memory once, by content, so it works for both regions;
// the controller in use is drawn on that screen by the button_icons texture sets.
void PatchBootStrings() {
    static bool done = false;
    if (done) return;
    done = true;
    static const char* const kLines[] = {
        "Hold the Wii Remote sideways.",
        "Hold the t\xe9l\xe9" "commande Wii sideways.",
        "Halte die Wii-Fernbedienung waagrecht.",
        "Tenez la t\xe9l\xe9" "commande Wii \xe0 l'horizontale.",
        "Sujeta el mando de Wii en posici\xf3n horizontal.",
        "Afferra il telecomando Wii alle estremit\xe0.",
        "\x82\x76\x82\x89\x82\x89\x83\x8a\x83\x82\x83\x52\x83\x93\x82\xf0\x81\x77\x82\xe6\x82\xb1\x82\xe0\x82\xbf\x81\x78"
        "\x82\xb5\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",  // Japanese, Shift-JIS
    };
    static constexpr std::string_view kText = "Enjoy New Super Paper Bros!";
    constexpr uint32_t kStart = 0x80004338u, kSize = 0x00600000u;  // main.dol's sections
    uint8_t* const mem = Memory::Contains(kStart, kSize) ? Memory::GetPointer(kStart, kSize) : nullptr;
    if (mem == nullptr) return;
    int patched = 0;
    for (const char* text : kLines) {
        const auto* line = reinterpret_cast<const uint8_t*>(text);  // bytes, not signed chars: the accented lines
        const size_t len = std::strlen(text) + 1;  // with its NUL: whole strings only
        for (uint8_t* p = mem; (p = std::search(p, mem + kSize, line, line + len)) != mem + kSize; p += len) {
            if (len - 1 < kText.size()) break;
            std::fill(p, p + len, uint8_t{0});
            std::copy(kText.begin(), kText.end(), p);
            ++patched;
        }
    }
    RT_LOGF(RT_TAG_RUNTIME, "boot screen: %d line(s) now read \"Enjoy New Super Paper Bros!\"\n", patched);
}

void Draw() noexcept {
    // PatchBootStrings() is Super Paper Mario's boot screen line; Brawl has none.
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    UpdateStartupPress();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    NetplayUi::Draw();
    DrawFpsOverlay();
    PollMenuButton();
    DrawOptionsMenu();
    UpdateGameSpeed();
    if (std::string message = SaveStates::TakeMessage(); !message.empty()) {
        g_notice = message;
        g_noticeFrames = 60 * 3;
    }
    DrawExitPrompt();
    controller_mapping_wizard::Draw();
    DrawNotice();
    ApplyInputBlockState();
    DrawControlsPanel();
    DrawStartupScreen();
    OnlineMenu::Draw();  // above the title card: the launcher can open it at start
}

void SetBusyProgress(const char* label, float fraction) noexcept {
    g_busyLabel = label != nullptr ? label : "";
    g_busyFraction = fraction;
}

bool StartupScreenVisible() noexcept {
    return !g_busyLabel.empty() || !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
