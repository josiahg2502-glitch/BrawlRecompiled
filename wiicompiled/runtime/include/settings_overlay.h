#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Draw the F10 settings bar before each Aurora present.
void HandleEvents(const AuroraEvent* events) noexcept;
void Draw() noexcept;
bool StartupScreenVisible() noexcept;
// A long job before the game can go on (building the mods' SD card): shown on the title card with its progress
// bar (0..1) until cleared with nullptr. Game thread only.
void SetBusyProgress(const char* label, float fraction) noexcept;
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
// Put host controllers back to a neutral state before the process ends.
void ReleaseControllers() noexcept;
// FPS counter on or off (the Android menu sets it through JNI); saved to Config.toml.
void SetShowFps(bool show) noexcept;
bool ShowFps() noexcept;
// A frame-time graph under the FPS counter, off by default. Saved to Config.toml.
void SetFrameGraph(bool show) noexcept;
bool FrameGraph() noexcept;
// V-Sync: presents wait for the display, and the game's clock follows its refresh (hle/vi.cpp).
// Off by default. Saved to Config.toml.
void SetVSync(bool enabled) noexcept;
bool VSync() noexcept;
// Animated tiles (spinning coins, ? blocks...), on by default. Saved to Config.toml.
void SetSpinningCoins(bool enabled) noexcept;
bool SpinningCoins() noexcept;
// Render scale: 1 = 480 lines, 0 = the window's (screen's) own resolution. Saved to Config.toml.
void SetRenderScale(float scale) noexcept;
float RenderScale() noexcept;
// DisplaySettings' aspect (4:3, 16:9, Fill), including a change not yet applied. Saved to Config.toml.
void SetAspect(int32_t mode) noexcept;
int32_t Aspect() noexcept;
// Fast-forward (F8 or the options menu): the game's clock runs [gameplay] fast_forward_speed times as fast. Never
// during online play.
bool FastForward() noexcept;
// How many times as fast as normal the game's clock runs now: fast-forward, or the fast menus (2x in menus and
// loading screens), else 1.
int GameSpeed() noexcept;
} // namespace settings_overlay
