// settings_menu.inc - the options menu: every setting in one full-screen menu drawn like the game's own (and like the
// online menu, netplay/menu_kit.h), worked with a controller, the keyboard or the mouse. F10, a controller's Home
// (guide) button, or Back+Start together open it. Included by settings_overlay.cpp inside its anonymous namespace, so it
// works on the same state as the rest of the overlay.

namespace MK = MenuKit;

struct MenuRow {
    std::string label, value, hint;
    std::function<void(int)> change;  // -1/+1 from left/right, 0 from A/Enter or a click
    bool enabled = true;
    int bar = -1;  // 0..100: drawn as a volume bar
};

enum MenuTab { kTabResume, kTabGraphics, kTabAudio, kTabControls, kTabGameplay, kTabCheats, kTabMods, kTabOnline,
               kTabSystem, kTabQuit, kTabCount };
constexpr const char* kTabNames[kTabCount] = {"RESUME", "GRAPHICS", "AUDIO", "CONTROLS", "GAMEPLAY", "CHEATS", "MODS",
                                              "ONLINE", "SYSTEM", "QUIT GAME"};
constexpr int kVisibleRows = 8;

struct MenuState {
    int tab = kTabGraphics;
    bool inRows = false;
    int row = 0;
    int first = 0;     // first row shown (the list scrolls)
    bool quitArmed = false;
    std::string flash;  // a line over the footer for a few seconds
    double flashUntil = 0;
    std::string pendingMod;  // a folder picked in the system's dialog, copied onto the SD card next frame
    std::mutex pendingMutex;
    std::atomic<int> modCopy{-1};  // files copied while a mod is added, -1 when idle
    std::string modResult;
    bool restartNeeded = false;
} g_menu;

int Cycle(int value, int step, int count) { return ((value + (step == 0 ? 1 : step)) % count + count) % count; }
const char* OnOff(bool on) { return on ? "ON" : "OFF"; }
void Flash(std::string text) {
    g_menu.flash = std::move(text);
    g_menu.flashUntil = ImGui::GetTime() + 4.0;
}
void OpenFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::string url = "file:///" + RuntimeConfigFile::PathToUtf8(folder);
    std::replace(url.begin(), url.end(), '\\', '/');
    if (!SDL_OpenURL(url.c_str())) Flash("Couldn't open " + RuntimeConfigFile::PathToUtf8(folder));
}

// ---- the rows of each tab ----
void GraphicsRows(std::vector<MenuRow>& rows) {
    static constexpr const char* kModes[] = {"WINDOW", "BORDERLESS", "FULLSCREEN"};
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    rows.push_back({"Display", kModes[std::clamp(g_displayMode, 0, 2)], "F11 also switches between a window and fullscreen.",
                    [](int step) {
                        aurora_set_display_mode(static_cast<AuroraDisplayMode>(Cycle(g_displayMode, step, 3)));
                        g_displayMode = static_cast<int>(aurora_get_display_mode());
                        RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
                    }});
    int res = 0;
    for (size_t i = 0; i < kResolutions.size(); ++i) if (std::fabs(kResolutions[i].scale - g_resolutionScale) < 0.001f) res = static_cast<int>(i);
    uint32_t pw = 0, ph = 0;
    AuroraGetPictureSize(&pw, &ph);
    rows.push_back({"Resolution", kResolutions[static_cast<size_t>(res)].label,
                    "The game renders at " + std::to_string(pw) + "x" + std::to_string(ph) + ". Higher is sharper and needs a faster GPU.",
                    [res](int step) {
                        int i = res;
                        do i = Cycle(i, step, static_cast<int>(kResolutions.size()));
                        while (IsHighFrameRateMode() && IsHighResolutionScale(kResolutions[static_cast<size_t>(i)].scale));
                        SetResolutionScale(kResolutions[static_cast<size_t>(i)].scale);
                    }});
    const DisplaySettings::Settings display = DisplaySettings::Latest();
    int aspect = 0;
    for (size_t i = 0; i < kAspects.size(); ++i) if (kAspects[i].mode == display.aspect) aspect = static_cast<int>(i);
    const bool restart = (display.aspect != DisplaySettings::kAspect4x3) != DisplaySettings::GameWidescreen();
    rows.push_back({"Aspect ratio", kAspects[static_cast<size_t>(aspect)].label,
                    restart ? "Switching between 4:3 and widescreen takes effect after a restart."
                            : "Fill Window uses the whole screen, ultrawide included (up to 32:9): more of the stage to the sides, the HUD kept in the middle 16:9, never stretched.",
                    [aspect](int step) { SetAspectMode(kAspects[static_cast<size_t>(Cycle(aspect, step, static_cast<int>(kAspects.size())))].mode); }});
    static constexpr const char* kFps[] = {"60 FPS", "120 FPS", "180 FPS", "240 FPS"};
    rows.push_back({"Frame rate", kFps[g_frameInterpolationMode],
                    "Above 60 adds frames in between for screens faster than 60 Hz (experimental). The game itself runs at 60.",
                    [](int step) {
                        g_frameInterpolationMode = Cycle(g_frameInterpolationMode, step, 4);
                        const uint32_t target = kFrameInterpolationTargetFps[static_cast<size_t>(g_frameInterpolationMode)];
                        aurora_set_frame_interpolation_fps(target);
                        RuntimeConfigFile::SetFrameInterpolationFps(target);
                        LimitResolutionForFrameRate();
                        if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_EXCLUSIVE) aurora_set_display_mode(AURORA_DISPLAY_MODE_EXCLUSIVE);
                    }});
    rows.push_back({"V-Sync", OnOff(g_vsync.load()), "No screen tearing; the game follows your screen's refresh.", [](int) {
                        const bool on = !g_vsync.load();
                        SetVSync(on);
                        RuntimeConfigFile::SetVSync(on);
                    }});
    const bool bloom = (g_disabledPostProcessingPaths & 0x10u) == 0;
    rows.push_back({"Bloom", OnOff(bloom), "The glow around bright light. Applies from the next scene.", [bloom](int) {
                        g_disabledPostProcessingPaths = bloom ? (g_disabledPostProcessingPaths | 0x10u) : (g_disabledPostProcessingPaths & ~0x10u);
                        RuntimeGameGraphicsOptions::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
                        RuntimeConfigFile::SetDisabledPostProcessingPaths(g_disabledPostProcessingPaths);
                    }});
    rows.push_back({"Show FPS", OnOff(g_showFps), "Frames per second in the corner.", [](int) {
                        g_showFps = !g_showFps;
                        RuntimeConfigFile::SetShowFps(g_showFps);
                    }});
    rows.push_back({"Hide first-time shader hitches", OnOff(g_skipUnreadyPipelines),
                    "Skips a draw for a frame while its shader is still being built, instead of pausing.", [](int) {
                        g_skipUnreadyPipelines = !g_skipUnreadyPipelines;
                        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
                        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
                    }});
    // Takes effect at the next start: the renderer is chosen once.
#if defined(_WIN32)
    static const std::vector<std::pair<const char*, const char*>> kApis = {{"auto", "AUTOMATIC"}, {"d3d12", "DIRECT3D 12"}, {"vulkan", "VULKAN"}};
#else
    static const std::vector<std::pair<const char*, const char*>> kApis = {{"auto", "AUTOMATIC"}, {"vulkan", "VULKAN"}};
#endif
    const std::string api = RuntimeConfigFile::GraphicsApi("auto");
    int apiIndex = 0;
    for (size_t i = 0; i < kApis.size(); ++i) if (api == kApis[i].first) apiIndex = static_cast<int>(i);
    rows.push_back({"Graphics API", kApis[static_cast<size_t>(apiIndex)].second,
                    std::string("Now running on ") + GraphicsApiDisplayName() +
                        ". If the game shows a black screen or won't start, try the other one. Takes effect after a restart.",
                    [apiIndex](int step) {
                        RuntimeConfigFile::SetGraphicsApi(kApis[static_cast<size_t>(Cycle(apiIndex, step, static_cast<int>(kApis.size())))].first);
                        g_menu.restartNeeded = true;
                    }});
    static constexpr const char* kCompat[] = {"AUTOMATIC", "ON", "OFF"};
    const int compat = std::clamp(RuntimeConfigFile::GpuCompat(), 0, 2);
    rows.push_back({"GPU compatibility mode", kCompat[compat],
                    "Graphics driver workarounds, for a GPU that draws the game wrong; Automatic picks them by GPU. Takes effect after a restart.",
                    [compat](int step) {
                        RuntimeConfigFile::SetGpuCompat(Cycle(compat, step, 3));
                        g_menu.restartNeeded = true;
                    }});
    rows.push_back({"HD texture packs", "OPEN FOLDER", "Put a pack's RSBE01 folder (Dolphin's layout) in here, then restart.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "texture_replacements"); }});
}

void VolumeRow(std::vector<MenuRow>& rows, const char* label, int& percent, void (*apply)(float), bool (*save)(float)) {
    rows.push_back({label, std::to_string(percent) + "%", "Left and right change it.", [&percent, apply, save](int step) {
                        if (step == 0) return;
                        percent = std::clamp(percent + step * 5, 0, 100);
                        apply(percent / 100.0f);
                        save(percent / 100.0f);
                    }, true, percent});
}

void AudioRows(std::vector<MenuRow>& rows) {
    VolumeRow(rows, "Master volume", g_audioVolumePercent, [](float v) { AudioBackend::Instance().SetMasterVolume(v); },
              RuntimeConfigFile::SetAudioVolume);
    VolumeRow(rows, "Music", g_musicVolumePercent, MusicAttenuation::SetMusicVolume, RuntimeConfigFile::SetMusicVolume);
    VolumeRow(rows, "Sound effects", g_soundEffectsVolumePercent, MusicAttenuation::SetSoundEffectsVolume,
              RuntimeConfigFile::SetSoundEffectsVolume);
    VolumeRow(rows, "Voices", g_voicesVolumePercent, MusicAttenuation::SetVoicesVolume, RuntimeConfigFile::SetVoicesVolume);
    VolumeRow(rows, "Menu sounds", g_uiVolumePercent, MusicAttenuation::SetUiVolume, RuntimeConfigFile::SetUiVolume);
    rows.push_back({"Mute", OnOff(g_audioMuted), std::string("Also on the ") + KeyBindingName(g_muteHotkey) + " key.", [](int) {
                        g_audioMuted = !g_audioMuted;
                        AudioBackend::Instance().SetMuted(g_audioMuted);
                        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
                    }});
    rows.push_back({"Quiet music while other media plays", OnOff(g_attenuateMusicWhenMediaPlays),
                    "Mutes the game's music while a video or song plays in another app.", [](int) {
                        g_attenuateMusicWhenMediaPlays = !g_attenuateMusicWhenMediaPlays;
                        MusicAttenuation::SetEnabled(g_attenuateMusicWhenMediaPlays);
                        RuntimeConfigFile::SetAttenuateMusicWhenMediaPlays(g_attenuateMusicWhenMediaPlays);
                    }});
}

void ControlsRows(std::vector<MenuRow>& rows) {
    const uint32_t port = static_cast<uint32_t>(g_controllerPort);
    const char* name = PADGetName(port);
    rows.push_back({"Player", "PLAYER " + std::to_string(port + 1),
                    std::string("Controller on this port: ") + (name != nullptr ? name : "none") + ". Every pad is set up automatically.",
                    [](int step) { g_controllerPort = Cycle(g_controllerPort, step, PAD_MAX_CONTROLLERS); }});
    rows.push_back({"Vibration", OnOff(g_rumbleEnabled), "Rumble on every controller.", [](int) {
                        g_rumbleEnabled = !g_rumbleEnabled;
                        PAD_HLE_SetRumbleEnabled(g_rumbleEnabled);
                        RuntimeConfigFile::SetRumbleEnabled(g_rumbleEnabled);
                        if (!g_rumbleEnabled) {
                            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
                                                                                         PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
                            PADControlAllMotors(stopAll.data());
                        }
                    }});
    uint32_t keyCount = 0;
    auto* keys = PADGetKeyButtonBindings(port, &keyCount);
    rows.push_back({"Keyboard and mouse", OnOff(keys != nullptr), "Use the keyboard as this player's controller instead of a pad.",
                    [port, keys](int) {
                        PADSetKeyboardActive(port, keys == nullptr);
                        uint32_t count = 0;
                        auto* now = PADGetKeyButtonBindings(port, &count);
                        if (now != nullptr && std::all_of(now, now + count, [](const auto& b) { return b.scancode == PAD_KEY_INVALID; })) {
                            const std::array<int, PAD_BUTTON_COUNT> preset = {
                                PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q, SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE,
                                SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
                            for (size_t i = 0; i < preset.size(); ++i) PADSetKeyButtonBinding(port, {preset[i], kControllerButtons[i].padButton});
                            const std::array<int, PAD_AXIS_COUNT> axes = {SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
                                                                          SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
                                                                          SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT};
                            uint32_t axisCount = 0;
                            auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
                            for (uint32_t i = 0; i < axisCount; ++i) PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
                        }
                        PADSerializeMappings();
                    }});
    if (keys != nullptr) {
        for (uint32_t i = 0; i < keyCount; ++i) {
            const auto binding = keys[i];
            rows.push_back({std::string("Key: ") + PADGetButtonName(binding.padButton), KeyBindingName(binding.scancode),
                            "A or Enter, then press a key or click a mouse button. Backspace clears it.", [binding](int step) {
                                if (step == 0) BeginRebind(RebindKind::KeyboardButton, binding.padButton, PADGetButtonName(binding.padButton));
                            }});
        }
        uint32_t axisCount = 0;
        auto* axes = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i) {
            const auto binding = axes[i];
            const char* direction = PADGetAxisDirectionLabel(binding.padAxis);
            const std::string label = std::string("Key: ") + PADGetAxisName(binding.padAxis) + " " + (direction ? direction : "");
            rows.push_back({label, KeyBindingName(binding.scancode), "A or Enter, then press a key. Backspace clears it.",
                            [binding, label](int step) { if (step == 0) BeginRebind(RebindKind::KeyboardAxis, binding.padAxis, label.c_str()); }});
        }
        return;
    }
    // The controller's buttons: which of its buttons is each GameCube button. A layout swaps the face buttons at once.
    const bool padHere = PADGetIndexForPort(port) >= 0;
    uint32_t count = 0;
    const PADButtonMapping* mappings = PADGetButtonMappings(port, &count);
    const auto nativeFor = [&](uint16_t padButton) {
        for (uint32_t i = 0; mappings != nullptr && i < count; ++i)
            if (mappings[i].padButton == padButton) return mappings[i].nativeButton;
        return static_cast<uint32_t>(PAD_NATIVE_BUTTON_INVALID);
    };
    const bool nintendo = nativeFor(kControllerButtons[0].padButton) == FindNativeButton("east")->nativeButton;
    rows.push_back({"Button layout", nintendo ? "NINTENDO" : "BY POSITION",
                    "By position: A is the bottom face button, as on a GameCube pad. Nintendo: A on the right, B at the bottom, "
                    "as printed on Switch and Wii U pads.",
                    [port, nintendo](int) {
                        const auto& preset = nintendo ? kPlayStationPreset : kClassicProPreset;
                        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                                PADSetAltButtonMapping(port, PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                                RuntimeConfigFile::SetControllerButton(i, preset[i]);
                            }
                        }
                        PADSerializeMappings();
                    }, padHere});
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        const uint16_t padButton = kControllerButtons[i].padButton;
        const bool waiting = g_rebind.active && g_rebind.kind == RebindKind::Controller && g_rebind.target == padButton;
        std::string value = waiting ? "PRESS A BUTTON..." : NativeButtonForValue(nativeFor(padButton)).label;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        rows.push_back({std::string("Button: ") + kControllerButtons[i].label, value,
                        padHere ? "A or Enter, then press the controller button to use for it."
                                : "Connect a controller for this player to change its buttons.",
                        [padButton, i](int step) {
                            if (step == 0) BeginRebind(RebindKind::Controller, padButton, kControllerButtons[i].label);
                        }, padHere});
    }
    rows.push_back({"Wii Remotes", OnOff(g_wiiRemotesEnabled),
                    "Wii Remotes and Wii U Pro Controllers paired over Bluetooth (press 1+2 to connect). Takes effect after a restart.",
                    [](int) {
                        g_wiiRemotesEnabled = !g_wiiRemotesEnabled;
                        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
                        g_menu.restartNeeded = true;
                    }});
}

void GameplayRows(std::vector<MenuRow>& rows) {
    rows.push_back({"Fast-forward", OnOff(g_fastForward.load()), "Also F8. Off during online play.", [](int) { g_fastForward.store(!g_fastForward.load()); }});
    const int speed = RuntimeConfigFile::FastForwardSpeed();
    rows.push_back({"Fast-forward speed", std::to_string(speed) + "X", "How fast fast-forward runs the game.",
                    [speed](int step) { RuntimeConfigFile::SetFastForwardSpeed(2 + Cycle(speed - 2, step, 3)); }});
    rows.push_back({"Fast menus", OnOff(RuntimeConfigFile::FastMenus()),
                    "Screen changes, loading and info screens run 3x as fast. Menus you move a cursor in, fights and online play don't.",
                    [](int) { RuntimeConfigFile::SetFastMenus(!RuntimeConfigFile::FastMenus()); }});
    const int slot = SaveStates::Slot();
    rows.push_back({"Save state slot", "SLOT " + std::to_string(slot + 1), SaveStates::Describe(slot),
                    [slot](int step) { SaveStates::SetSlot(Cycle(slot, step, SaveStates::kSlots)); }});
    rows.push_back({"Save state", "F5", "Saves the game exactly as it is now into the slot: anywhere, mid-fight or mid-level.",
                    [](int step) { if (step == 0) { SaveStates::RequestSave(); Flash("Saving to slot " + std::to_string(SaveStates::Slot() + 1) + "..."); } },
                    SaveStates::Available()});
    rows.push_back({"Load state", "F7", "Goes back to what the slot holds.",
                    [](int step) { if (step == 0) { SaveStates::RequestLoad(); SetTopBarVisible(false); } },
                    SaveStates::Available() && SaveStates::Exists(slot)});
}

void CheatRows(std::vector<MenuRow>& rows) {
    auto& cheats = Gecko::Cheats();
    const bool available = Gecko::CheatsAvailable();
    int needValues = 0;
    // Hundreds of codes: A switches one, left / right jump a page. Codes still holding XXXXXXXX placeholders stay in
    // the file only.
    for (size_t i = 0; i < cheats.size(); ++i) {
        const auto& cheat = cheats[i];
        if (!cheat.usable) {
            ++needValues;
            continue;
        }
        std::string hint = !available ? "Cheats are off in online play."
                           : cheat.notes.empty() ? "A switches it. Some cheats change the game's code: those stay on until you restart."
                                                 : cheat.notes;
        rows.push_back({cheat.name, OnOff(cheat.enabled), hint, [i](int step) {
                            if (step != 0) {
                                g_menu.row += step * kVisibleRows;
                                return;
                            }
                            auto& list = Gecko::Cheats();
                            if (!Gecko::SetCheatEnabled(i, !list[i].enabled)) Flash(Gecko::CheatProblem());
                        }, available});
    }
    rows.push_back({"Add cheats", "OPEN FILE",
                    (needValues > 0 ? std::to_string(needValues) + " more codes in the file need values filled in (XXXXXXXX). " : std::string()) +
                    "Gecko codes in Dolphin's format: a $Name line, then its code lines; a Dolphin RSBE01.ini can be pasted in as it is. "
                    "Restart after editing.",
                    [](int step) { if (step == 0) OpenFolder(std::filesystem::u8path(Gecko::CheatFile()).parent_path()); }});
}

std::vector<std::string> SdBuilds() {
    std::vector<std::string> builds;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(RuntimeConfigFile::ApplicationDataDirectory() / "SD", ec)) {
        if (!entry.is_directory(ec)) continue;
        for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
            std::string n = file.path().filename().string();
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (n == "RSBE01.GCT") builds.push_back(RuntimeConfigFile::PathToUtf8(entry.path().filename()));
        }
    }
    std::sort(builds.begin(), builds.end());
    return builds;
}

// "Add a mod": the shallowest folder holding RSBE01.GCT inside the one picked is the mod's build; it is copied onto the
// SD card (launcher/ModInstall.cs does the same for a .zip).
void AddMod(std::filesystem::path source) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> level{source};
    fs::path build;
    for (int depth = 0; depth < 5 && build.empty() && !level.empty(); ++depth) {
        std::vector<fs::path> next;
        for (const auto& dir : level) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                std::string n = entry.path().filename().string();
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (entry.is_regular_file(ec) && n == "RSBE01.GCT" && build.empty()) build = dir;
                if (entry.is_directory(ec)) next.push_back(entry.path());
            }
        }
        level.swap(next);
    }
    if (build.empty()) {
        g_menu.modResult = "No Wii SD card build in there (no RSBE01.GCT). Use the mod's Wii download, extracted.";
        g_menu.modCopy = -1;
        return;
    }
    const fs::path target = RuntimeConfigFile::ApplicationDataDirectory() / "SD" / build.filename();
    int copied = 0;
    for (const auto& entry : fs::recursive_directory_iterator(build, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path dest = target / fs::relative(entry.path(), build, ec);
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
        g_menu.modCopy = ++copied;
    }
    const std::string name = RuntimeConfigFile::PathToUtf8(build.filename());
    RuntimeConfigFile::SetModsSdCodes(name);
    g_menu.modResult = "Added " + name + " (" + std::to_string(copied) + " files). Restart to play it.";
    g_menu.restartNeeded = true;
    g_menu.modCopy = -1;
}

void ModRows(std::vector<MenuRow>& rows) {
    const auto builds = SdBuilds();
    std::vector<std::string> choices{"", "vanilla"};
    choices.insert(choices.end(), builds.begin(), builds.end());
    const std::string chosen = RuntimeConfigFile::ModsSdCodes();
    int index = 0;
    for (size_t i = 0; i < choices.size(); ++i) if (choices[i] == chosen) index = static_cast<int>(i);
    const std::string value = index == 0 ? "FIRST ON THE CARD" : index == 1 ? "VANILLA BRAWL" : choices[static_cast<size_t>(index)];
    rows.push_back({"Build to play", value, "Which mod's codes load (REX, Project+...). Takes effect after a restart.",
                    [choices, index](int step) {
                        RuntimeConfigFile::SetModsSdCodes(choices[static_cast<size_t>(Cycle(index, step, static_cast<int>(choices.size())))]);
                        g_menu.restartNeeded = true;
                    }});
    const int copying = g_menu.modCopy.load();
    rows.push_back({"Add a mod", copying >= 0 ? "COPYING " + std::to_string(copying) : "CHOOSE FOLDER",
                    g_menu.modResult.empty() ? "Pick the folder a mod's Wii download was extracted to. It goes onto the SD card."
                                             : g_menu.modResult,
                    [](int step) {
                        if (step != 0 || g_menu.modCopy >= 0) return;
                        SDL_ShowOpenFolderDialog([](void*, const char* const* list, int) {
                            if (list == nullptr || list[0] == nullptr) return;
                            std::lock_guard<std::mutex> lock(g_menu.pendingMutex);
                            g_menu.pendingMod = list[0];
                        }, nullptr, nullptr, nullptr, false);
                    }, copying < 0});
    rows.push_back({"SD card folder", "OPEN", "The mods' files (UserData\\SD), read straight from here.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "SD"); }});
    rows.push_back({"File and texture mods", "OPEN", "Files that replace the disc's (UserData\\mods): fighters, stages, music.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "mods"); }});
    rows.push_back({"Code mods", "OPEN", "Recompiled code mods (UserData\\recomp_mods): new behaviour, not just files.",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "recomp_mods"); }});
}

void SystemRows(std::vector<MenuRow>& rows) {
    const std::string update = Updates::Available();
    rows.push_back({"Version", BRAWL_VERSION,
                    update.empty() ? "You have the latest version." : "Version " + update + " is out: Update below installs it.",
                    [](int) {}});
    if (!update.empty()) {
        rows.push_back({"Update to " + update, "INSTALL", "Downloads it and restarts the game. Your saves, settings, mods and disc stay.",
                        [](int step) { if (step == 0 && !Updates::Install()) Flash("Couldn't start the updater."); }});
    }
    rows.push_back({"Saves", "OPEN FOLDER", "Your Brawl save data (UserData\\Saves).",
                    [](int step) { if (step == 0) OpenFolder(RuntimeConfigFile::ApplicationDataDirectory() / "Saves"); }});
    rows.push_back({"Restart the game", "RESTART", "Starts Brawl again, to apply changes that need it.",
                    [](int step) { if (step == 0) NetplayStart::RestartGame(); }});
}

std::vector<MenuRow> TabRows(int tab) {
    std::vector<MenuRow> rows;
    switch (tab) {
    case kTabGraphics: GraphicsRows(rows); break;
    case kTabAudio: AudioRows(rows); break;
    case kTabControls: ControlsRows(rows); break;
    case kTabGameplay: GameplayRows(rows); break;
    case kTabCheats: CheatRows(rows); break;
    case kTabMods: ModRows(rows); break;
    case kTabSystem: SystemRows(rows); break;
    default: break;
    }
    if (g_menu.restartNeeded && (tab == kTabMods || tab == kTabControls || tab == kTabGraphics))
        rows.push_back({"Restart now", "RESTART", "Some changes here take effect after a restart.",
                        [](int step) { if (step == 0) NetplayStart::RestartGame(); }});
    return rows;
}

// ---- drawing ----
void MenuRowDraw(const MK::Canvas& c, const MenuRow& row, float y, bool focused) {
    const float x0 = -150, x1 = 460;
    MK::Plate(c, x0, y - 17, x1, y + 17, focused ? 0x2B3350FFu : 0x1C2133FFu, 7.0f, row.enabled ? 1.0f : 0.55f);
    if (focused) c.list->AddRect(c.P(x0 - 4, y + 19), c.P(x1 + 4, y - 19), MK::Col(MK::kOrange, 0.6f + 0.4f * std::sin(c.t * 6.0f)), 0, 0, 2.0f);
    MK::Body(c, row.label, x0 + 18, y, 16, row.enabled ? MK::kSnow : MK::kDim, 0, 300);
    const float vx = 330;
    std::string value = row.value;  // Brawl's title font has no parentheses
    value.erase(std::remove_if(value.begin(), value.end(), [](char ch) { return ch == '(' || ch == ')'; }), value.end());
    if (row.bar >= 0) {
        c.list->AddRectFilled(c.P(vx - 100, y + 5), c.P(vx + 60, y - 5), MK::Col(0x0B0D16FFu));
        c.list->AddRectFilled(c.P(vx - 100, y + 5), c.P(vx - 100 + 160.0f * row.bar / 100.0f, y - 5), MK::Col(MK::kOrange));
        MK::Text(c, *MK::TitleFont(), value, vx + 98, y, 17, MK::kSnow, 0xFFE7B0FFu, 1, 70, true);
    } else if (!value.empty()) {
        MK::Text(c, *MK::TitleFont(), value, vx, y, 18, row.value == "ON" ? 0xB8FFB0FFu : MK::kSnow,
                 row.value == "OFF" ? MK::kDim : 0xFFE7B0FFu, 1, 230, true);
    }
    if (focused && row.enabled && row.change && row.bar >= 0) {
        MK::Arrow(c, vx - 115, y, 14, 2, MK::kOrange);
        MK::Arrow(c, vx + 140, y, 14, 3, MK::kOrange);
    }
}

void DrawOptionsMenu() {
    if (!g_topBarVisible) {
        g_menu.inRows = false;
        g_menu.quitArmed = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_menu.pendingMutex);
        if (!g_menu.pendingMod.empty()) {
            g_menu.modCopy = 0;
            g_menu.modResult.clear();
            std::thread(AddMod, std::filesystem::u8path(g_menu.pendingMod)).detach();
            g_menu.pendingMod.clear();
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##options", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse)) {
        MK::Canvas c{ImGui::GetWindowDrawList(), GameLayout::GameView(), static_cast<float>(ImGui::GetTime())};
        c.view.centerX = vp->Pos.x + vp->Size.x * 0.5f;
        c.view.centerY = vp->Pos.y + vp->Size.y * 0.5f;
        c.view.scale = std::min(vp->Size.y / 470.0f, vp->Size.x / 960.0f);
        ImGui::SetWindowFocus();
        // The game stays visible behind a dark veil, as in its own pause menu.
        c.list->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(10, 12, 26, 225),
                                        IM_COL32(10, 12, 26, 225), IM_COL32(34, 12, 8, 235), IM_COL32(34, 12, 8, 235));
        const bool capturing = g_rebind.active;
        const MK::Nav nav = capturing ? MK::Nav{} : MK::ReadNav();

        // Banner.
        MK::Plate(c, -470, 176, 40, 222, 0x14182AFFu, 16.0f);
        c.list->AddRectFilled(c.P(-470, 178), c.P(60, 174), MK::Col(MK::kOrange));
        MK::Text(c, *MK::TitleFont(), "OPTIONS", -320, 200, 44, 0xFFF3B0FFu, MK::kOrange, 1, 0, true);
        MK::Text(c, *MK::TitleFont(), kTabNames[g_menu.tab], -140, 197, 24, MK::kSnow, 0xFFC86AFFu, 0, 200, true);
        MK::SmashBall(c, 380, 194, 56, c.t * 0.35f);

        // Tabs.
        int tabFocus = g_menu.inRows ? -1 : g_menu.tab;
        for (int i = 0; i < kTabCount; ++i) {
            const float y = 146 - i * 37.0f;
            const uint32_t color = i == kTabQuit ? MK::kPlayer[0] : i == g_menu.tab ? 0x3A4670FFu : 0x232A42FFu;
            const std::string label = i == kTabQuit && g_menu.quitArmed ? "QUIT? PRESS AGAIN" : kTabNames[i];
            int focus = tabFocus;
            if (MK::Button(c, i, focus, false, -455, y - 15, -205, y + 15, label, color, true, 18.0f)) {
                g_menu.tab = i;
                g_menu.inRows = i != kTabResume && i != kTabOnline && i != kTabQuit;
                g_menu.row = 0;
                g_menu.first = 0;
                if (i == kTabResume) SetTopBarVisible(false);
                if (i == kTabOnline) { OnlineMenu::SetOpen(true); SetTopBarVisible(false); }
                if (i == kTabQuit) { if (g_menu.quitArmed) ExitForAuroraWindowClose(); g_menu.quitArmed = true; }
            }
            if (focus != tabFocus && focus == i && !g_menu.inRows) g_menu.tab = i;  // hovered
            if (i == g_menu.tab && !g_menu.inRows) {
                MK::Arrow(c, -195, y, 14, 3, MK::kOrange);
            }
        }

        // Rows of the chosen tab.
        std::vector<MenuRow> rows = TabRows(g_menu.tab);
        const int count = static_cast<int>(rows.size());
        g_menu.row = std::clamp(g_menu.row, 0, std::max(0, count - 1));
        if (g_menu.row < g_menu.first) g_menu.first = g_menu.row;
        if (g_menu.row >= g_menu.first + kVisibleRows) g_menu.first = g_menu.row - kVisibleRows + 1;
        g_menu.first = std::clamp(g_menu.first, 0, std::max(0, count - kVisibleRows));
        std::string hint;
        if (count == 0) {
            const char* text = g_menu.tab == kTabResume ? "Back to the game." : g_menu.tab == kTabOnline ? "Play with friends or anyone online."
                                                                                                         : "Close Brawl Recompiled. Your settings are saved.";
            MK::Body(c, text, 155, 60, 20, MK::kDim, 1, 560);
        }
        for (int i = g_menu.first; i < std::min(count, g_menu.first + kVisibleRows); ++i) {
            const float y = 146 - (i - g_menu.first) * 40.0f;
            const MK::Canvas& cc = c;
            const ImVec2 lo = cc.P(-150, y + 17), hi = cc.P(460, y - 17);
            const bool hovered = ImGui::IsMouseHoveringRect(lo, hi);
            if (hovered && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) {
                g_menu.inRows = true;
                g_menu.row = i;
            }
            const bool focused = g_menu.inRows && g_menu.row == i;
            MenuRowDraw(c, rows[static_cast<size_t>(i)], y, focused);
            if (hovered && !capturing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && rows[static_cast<size_t>(i)].enabled &&
                rows[static_cast<size_t>(i)].change) {
                const bool right = ImGui::GetIO().MousePos.x > c.P(330, 0).x;
                rows[static_cast<size_t>(i)].change(rows[static_cast<size_t>(i)].bar >= 0 ? (right ? 1 : -1) : 0);
            }
            if (focused) hint = rows[static_cast<size_t>(i)].hint;
        }
        if (g_menu.first > 0) MK::Arrow(c, 478, 146, 12, 0, MK::kOrange);
        if (g_menu.first + kVisibleRows < count) MK::Arrow(c, 478, 146 - (kVisibleRows - 1) * 40.0f, 12, 1, MK::kOrange);
        if (ImGui::GetIO().MouseWheel != 0 && count > 0) {
            g_menu.inRows = true;
            g_menu.row = std::clamp(g_menu.row - static_cast<int>(ImGui::GetIO().MouseWheel), 0, count - 1);
        }

        // Navigation.
        if (!g_menu.inRows) {
            if (nav.up) g_menu.tab = (g_menu.tab + kTabCount - 1) % kTabCount, g_menu.quitArmed = false;
            if (nav.down) g_menu.tab = (g_menu.tab + 1) % kTabCount, g_menu.quitArmed = false;
            if (nav.confirm || nav.right) {
                if (g_menu.tab == kTabResume) SetTopBarVisible(false);
                else if (g_menu.tab == kTabOnline) { OnlineMenu::SetOpen(true); SetTopBarVisible(false); }
                else if (g_menu.tab == kTabQuit) { if (nav.confirm && g_menu.quitArmed) ExitForAuroraWindowClose(); g_menu.quitArmed = nav.confirm; }
                else if (count > 0) g_menu.inRows = true, g_menu.row = 0, g_menu.first = 0;
            }
            if (nav.back) SetTopBarVisible(false);
        } else if (count > 0) {
            MenuRow& row = rows[static_cast<size_t>(g_menu.row)];
            if (nav.up) g_menu.row = (g_menu.row + count - 1) % count;
            if (nav.down) g_menu.row = (g_menu.row + 1) % count;
            if (row.enabled && row.change) {
                if (nav.confirm) row.change(row.bar >= 0 ? 1 : 0);
                if (nav.right) row.change(1);
                if (nav.left) {
                    // Left on an action (nothing to go left through) leads back to the tabs instead.
                    static const std::set<std::string> kActions = {"", "OPEN", "OPEN FOLDER", "OPEN FILE", "RESTART", "INSTALL",
                                                                   "CHOOSE FOLDER", "F5", "F7"};
                    if (kActions.count(row.value) != 0) g_menu.inRows = false;
                    else row.change(-1);
                }
            } else if (nav.left) {
                g_menu.inRows = false;
            }
            if (nav.back) g_menu.inRows = false;
        } else if (nav.back || nav.left) {
            g_menu.inRows = false;
        }

        // Footer: the focused row's explanation, then the buttons.
        MK::Plate(c, -470, -232, 470, -200, 0x10131FFFu, 8.0f);
        const bool flashing = ImGui::GetTime() < g_menu.flashUntil && !g_menu.flash.empty();
        if (g_rebind.active) {
            hint = g_rebind.kind == RebindKind::Controller ? "Press the controller button for " + g_rebind.label + "."
                                                           : "Press a key or click a mouse button for " + g_rebind.label + ". Backspace clears it.";
        }
        // Up to two lines under the rows (a cheat's notes can be long).
        const auto lines = GameLayout::WrapText(*MK::MenuFont(), GameLayout::Utf16(flashing ? g_menu.flash : hint), 15, 600);
        for (size_t i = 0; i < std::min<size_t>(lines.size(), 2); ++i) {
            GameLayout::DrawText(c.list, c.view, *MK::MenuFont(), lines[i], -150, -164 - 17.0f * i, 15,
                                 flashing ? MK::kGold : MK::kDim, flashing ? MK::kGold : MK::kDim, 1.0f, 0, 1);
        }
        MK::Body(c, "A / Enter: choose    Left / Right: change    B / Esc: back    F10 / Home: close", 0, -216, 15, MK::kDim, 1, 900);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (g_rebind.active) PollRebind(false);
}

// Home (guide) on any controller, or Back and Start held together, opens and closes the menu.
void PollMenuButton() {
    static bool wasDown = false;
    bool down = false;
    for (u32 i = 0; i < 8 && SDL_GetKeyboardFocus() != nullptr; ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        if (pad == nullptr) continue;
        down = down || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE) ||
               (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START));
    }
    if (down && !wasDown && !OnlineMenu::IsOpen() && !StartupScreenVisible()) SetTopBarVisible(!g_topBarVisible);
    wasDown = down;
}

// ---- game speed: fast-forward, or the fast menus ----
std::atomic<int> g_gameSpeed{1};

// A scene change is under way (gfSceneManager's next scene set: fading out, loading, fading in), or a screen that only
// shows something (the strap and info screens). Menus a player moves a cursor in keep their speed.
bool TransitionOrInfo(const std::string& scene) {
    constexpr uint32_t kInstanceSlot = RuntimeConfig::SDA1_BASE - 0x43C0u;
    const uint32_t manager = Memory::Read32(kInstanceSlot);
    const bool changing = Memory::Contains(manager, 0x290) && Memory::Read32(manager + 8) != 0;
    return changing || scene == "scStrap" || scene == "scPrizeInfo";
}

// Tests: BRAWL_OPTIONS_TAB=<n> opens the options menu on tab n once the game is up; BRAWL_STATE_TEST=1 saves 5 s
// into the first match, then loads it at 9 s and 14 s.
void RunTestHooks(const std::string& scene, double sceneSeconds) {
    // A list ("3,5,6") shows each tab for 4 s.
    static const char* tabs = std::getenv("BRAWL_OPTIONS_TAB");
    static double openedAt = -1;
    if (tabs != nullptr && !StartupScreenVisible() && scene == "muMenuMain") {
        if (openedAt < 0) openedAt = ImGui::GetTime();
        std::vector<int> list;
        for (const char* p = tabs; *p != '\0';) {
            list.push_back(std::atoi(p));
            while (*p != '\0' && *p != ',') ++p;
            if (*p == ',') ++p;
        }
        const size_t i = static_cast<size_t>((ImGui::GetTime() - openedAt) / 4.0);
        if (i < list.size() && g_menu.tab != list[i]) {
            g_menu.tab = std::clamp(list[i], 0, kTabCount - 1);
            g_menu.inRows = true;
            g_menu.row = 0;
            SetTopBarVisible(true);
            RT_LOGF(RT_TAG_RUNTIME, "[options test] tab %d\n", g_menu.tab);
        }
    }
    static const bool stateTest = std::getenv("BRAWL_STATE_TEST") != nullptr;
    static int step = 0;
    if (!stateTest || scene != "scMelee") return;
    const double at[3] = {5.0, 9.0, 14.0};
    if (step < 3 && sceneSeconds >= at[step]) {
        RT_LOGF(RT_TAG_RUNTIME, "[savestate test] %s at %.1f s\n", step == 0 ? "save" : "load", sceneSeconds);
        if (step == 0) SaveStates::RequestSave(); else SaveStates::RequestLoad();
        ++step;
    }
}

void UpdateGameSpeed() {
    static std::string lastScene;
    static double sceneStart = 0;
    const std::string scene = SaveStates::SceneName();
    if (scene != lastScene) {
        RT_LOGF(RT_TAG_RUNTIME, "[scene] %s\n", scene.c_str());
        lastScene = scene;
        sceneStart = ImGui::GetTime();
    }
    RunTestHooks(scene, ImGui::GetTime() - sceneStart);
    // #26: a new release is announced once, past the title card (the check runs in the background).
    static bool updateTold = false;
    if (!updateTold && !StartupScreenVisible()) {
        if (const std::string newer = Updates::Available(); !newer.empty()) {
            updateTold = true;
            g_notice = "Brawl Recompiled " + newer + " is out: Options (F10 or Home) > System installs it.";
            g_noticeFrames = 60 * 10;
        }
    }
    // Loading and scene changes run long frames by nature: they don't count against the frame rate (aurora's pacing).
    // Menus count neither: the stage select reads the saved custom stages as it opens, a one-time hitch that should cost
    // its own frames only, not seconds at 60.
    const bool menu = scene.rfind("mu", 0) == 0 || scene.rfind("scSel", 0) == 0 || scene == "scTitle" || scene == "scBoot" ||
                      scene == "scMemoryChange" || scene.empty();
    aurora_set_frame_pacing_exempt(StartupScreenVisible() || TransitionOrInfo(scene) || menu);
    int speed = 1;
    if (NetplaySession::Active()) speed = 1;
    else if (g_fastForward.load()) speed = RuntimeConfigFile::FastForwardSpeed();
    else if (RuntimeConfigFile::FastMenus() && TransitionOrInfo(scene)) speed = 3;
    g_gameSpeed.store(speed, std::memory_order_relaxed);
}
