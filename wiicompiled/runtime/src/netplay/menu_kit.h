#pragma once

// Brawl-styled menu drawing shared by the online menu and the options menu: the game's own fonts (read from the disc)
// in its 456-unit-tall layout space, slanted plates for buttons and banners, the Smash Ball, and one input reader for
// every controller, the keyboard and the mouse.

#include "netplay/game_layout.h"

#include <aurora/imgui.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

extern "C" uint8_t* aurora_load_png_rgba(const char* path, uint32_t* width, uint32_t* height);

namespace MenuKit {
namespace GL = GameLayout;

constexpr uint32_t kPlayer[4] = {0xE0382CFFu, 0x2F6FDCFFu, 0xE8B421FFu, 0x2FA84AFFu};
constexpr uint32_t kOrange = 0xFF8C28FFu, kGold = 0xFFD966FFu, kSnow = 0xF4F4F8FFu, kDim = 0xA8ACBEFFu,
                   kInk = 0x0B0D16FFu, kRed = 0xFF6A52FFu, kGreen = 0x6FDB6FFFu;

inline ImU32 Col(uint32_t rgba, float alpha = 1.0f) {
    return IM_COL32(rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, static_cast<int>((rgba & 255) * alpha));
}
inline uint32_t Shade(uint32_t rgba, float f) {
    auto c = [&](int shift) { return static_cast<uint32_t>(std::clamp(((rgba >> shift) & 255) * f, 0.0f, 255.0f)); };
    return (c(24) << 24) | (c(16) << 16) | (c(8) << 8) | (rgba & 255);
}


// ---- input: edges of directions and buttons from every controller, the keyboard and the mouse ----
struct Nav {
    bool up = false, down = false, left = false, right = false, confirm = false, back = false;
};

inline Nav ReadNav(bool typing = false) {
    static uint32_t prevHeld = 0;
    static double nextRepeat = 0.0;
    enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32 };
    uint32_t held = 0;
    // Only the game window in front reads controllers (two games on one PC share them).
    for (u32 i = 0; i < 8 && SDL_GetKeyboardFocus() != nullptr; ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        if (pad == nullptr) continue;
        auto b = [&](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
        const int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (b(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -20000) held |= kUp;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 20000) held |= kDown;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -20000) held |= kLeft;
        if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 20000) held |= kRight;
        if (b(SDL_GAMEPAD_BUTTON_SOUTH) || b(SDL_GAMEPAD_BUTTON_START)) held |= kA;
        if (b(SDL_GAMEPAD_BUTTON_EAST)) held |= kB;
    }
    const double now = ImGui::GetTime();
    uint32_t pressed = held & ~prevHeld;
    // A held direction repeats, as menus do.
    const uint32_t dirs = held & (kUp | kDown | kLeft | kRight);
    if (dirs != 0 && (dirs & prevHeld) == dirs && now >= nextRepeat) {
        pressed |= dirs;
        nextRepeat = now + 0.11;
    } else if ((pressed & (kUp | kDown | kLeft | kRight)) != 0) {
        nextRepeat = now + 0.4;
    }
    prevHeld = held;
    Nav nav;
    nav.up = (pressed & kUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    nav.down = (pressed & kDown) || ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    nav.left = (pressed & kLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    nav.right = (pressed & kRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    nav.confirm = (pressed & kA) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                  (!typing && ImGui::IsKeyPressed(ImGuiKey_Space));
    nav.back = (pressed & kB) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    return nav;
}

// ---- drawing in layout units (456 tall, y up, 0 at the centre) ----
struct Canvas {
    ImDrawList* list = nullptr;
    GL::View view;
    float t = 0.0f;
    ImVec2 P(float x, float y) const { return ImVec2(view.centerX + x * view.scale, view.centerY - y * view.scale); }
};

inline std::shared_ptr<GL::Font> TitleFont() {
    static const auto font = GL::LoadFont("/system/font/font_hira.brfnt");
    return font;
}
inline std::shared_ptr<GL::Font> MenuFont() {
    static const auto font = GL::LoadFont("/system/font/font_melee.brfnt");
    return GL::FontReady(font.get()) ? font : TitleFont();
}

inline void Text(const Canvas& c, const GL::Font& font, const std::string& text, float x, float y, float size, uint32_t top,
          uint32_t bottom, int originH = 1, float maxWidth = 0.0f, bool outline = false) {
    const auto u = GL::Utf16(text);
    if (maxWidth > 0.0f) {
        const float w = GL::MeasureText(font, u, size);
        if (w > maxWidth) size *= maxWidth / w;
    }
    if (outline) {
        for (int j = 0; j < 12; ++j) {
            const float a = j * 0.5236f, d = std::max(1.5f, size * 0.07f);
            GL::DrawText(c.list, c.view, font, u, x + std::cos(a) * d, y + std::sin(a) * d, size, kInk, kInk, 1.0f, originH, 1);
        }
    }
    GL::DrawText(c.list, c.view, font, u, x, y, size, top, bottom, 1.0f, originH, 1);
}

inline void Body(const Canvas& c, const std::string& text, float x, float y, float size, uint32_t color = kSnow, int originH = 1,
          float maxWidth = 0.0f) {
    Text(c, *MenuFont(), text, x, y, size, color, color, originH, maxWidth);
}

// A small arrow (Brawl's fonts have no arrow glyphs): dir 0 up, 1 down, 2 left, 3 right.
inline void Arrow(const Canvas& c, float x, float y, float size, int dir, uint32_t color) {
    const float h = size * 0.5f;
    ImVec2 a, b, d;
    switch (dir) {
    case 0: a = c.P(x, y + h); b = c.P(x + h, y - h); d = c.P(x - h, y - h); break;
    case 1: a = c.P(x, y - h); b = c.P(x - h, y + h); d = c.P(x + h, y + h); break;
    case 2: a = c.P(x - h, y); b = c.P(x + h, y + h); d = c.P(x + h, y - h); break;
    default: a = c.P(x + h, y); b = c.P(x - h, y - h); d = c.P(x - h, y + h); break;
    }
    c.list->AddTriangleFilled(a, b, d, Col(color));
}

// A slanted plate, the shape of Brawl's menu buttons and banners: a lighter top band over the base colour, outlined.
inline void Plate(const Canvas& c, float x0, float y0, float x1, float y1, uint32_t color, float slant = 10.0f, float alpha = 1.0f) {
    const float ym = (y0 + y1) * 0.5f;
    auto Edge = [&](float y) { return slant * (2.0f * (y - y0) / std::max(1.0f, y1 - y0) - 1.0f); };  // -slant..+slant
    const ImVec2 tl = c.P(x0 + Edge(y1), y1), tr = c.P(x1 + Edge(y1), y1), ml = c.P(x0 + Edge(ym), ym),
                 mr = c.P(x1 + Edge(ym), ym), bl = c.P(x0 + Edge(y0), y0), br = c.P(x1 + Edge(y0), y0);
    c.list->AddQuadFilled(tl, tr, mr, ml, Col(Shade(color, 1.22f), alpha));
    c.list->AddQuadFilled(ml, mr, br, bl, Col(color, alpha));
    c.list->AddLine(ml, mr, Col(Shade(color, 1.45f), alpha * 0.5f), 1.0f);
    c.list->AddQuad(tl, tr, br, bl, Col(kInk, alpha), 3.0f);
}

// A button: hovered or focused it glows; true when clicked, or confirmed while focused.
inline bool Button(const Canvas& c, int index, int& focus, bool confirm, float x0, float y0, float x1, float y1,
            const std::string& label, uint32_t color, bool enabled = true, float textSize = 26.0f) {
    const ImVec2 lo = c.P(x0 - 10, y1), hi = c.P(x1 + 10, y0);
    const bool hovered = ImGui::IsMouseHoveringRect(lo, hi);
    if (hovered && enabled && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) focus = index;
    const bool focused = focus == index;
    const float pulse = focused ? 0.5f + 0.5f * std::sin(c.t * 6.0f) : 0.0f;
    const uint32_t body = enabled ? color : 0x3A3F55FFu;
    if (focused && enabled) {
        const float g = 4.0f + pulse * 3.0f;
        c.list->AddQuadFilled(c.P(x0 + 10 - g, y1 + g), c.P(x1 + 10 + g, y1 + g), c.P(x1 - 10 + g, y0 - g),
                              c.P(x0 - 10 - g, y0 - g), Col(0xFFFFFFFFu, 0.55f + 0.35f * pulse));
    }
    Plate(c, x0, y0, x1, y1, body);
    const bool darkText = (color == kPlayer[2] || color == kGold) && enabled;
    Text(c, *TitleFont(), label, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, textSize, darkText ? 0x2A1A00FFu : kSnow,
         darkText ? 0x4A3000FFu : (enabled ? 0xFFE7B0FFu : kDim), 1, (x1 - x0) - 24.0f, !darkText);
    return enabled && ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || (focused && confirm));
}

// Left/right selector: "<  value  >".
inline bool Selector(const Canvas& c, int index, int& focus, const Nav& nav, float y, const std::string& label,
              const std::string& value, int& option, int count) {
    const bool focused = focus == index;
    const ImVec2 lo = c.P(-300, y + 22), hi = c.P(300, y - 22);
    if (ImGui::IsMouseHoveringRect(lo, hi) && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0)) focus = index;
    Plate(c, -300, y - 22, 300, y + 22, focused ? 0x2B3350FFu : 0x1C2133FFu, 8.0f);
    Body(c, label, -280, y, 18, kDim, 0);
    Text(c, *TitleFont(), value, 120, y, 22, kSnow, 0xFFE7B0FFu, 1, 230, true);
    const bool arrows = focused || ImGui::IsMouseHoveringRect(lo, hi);
    if (arrows) {
        Arrow(c, -10, y, 18, 2, kOrange);
        Arrow(c, 250, y, 18, 3, kOrange);
    }
    int step = 0;
    if (focused && nav.left) step = -1;
    if (focused && nav.right) step = 1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(lo, hi)) {
        step = ImGui::GetIO().MousePos.x < c.P(120, 0).x ? -1 : 1;
    }
    if (step != 0) option = (option + step + count) % count;
    return step != 0;
}

// Typed characters this frame (names, codes, addresses).
inline void TypeInto(std::string& text, size_t limit, bool codeOnly) {
    ImGuiIO& io = ImGui::GetIO();
    for (ImWchar ch : io.InputQueueCharacters) {
        if (ch < 32 || ch >= 127 || text.size() >= limit) continue;
        char cc = static_cast<char>(ch);
        if (codeOnly && !std::isalnum(static_cast<unsigned char>(cc)) && cc != '.' && cc != ':' && cc != '-') continue;
        text += codeOnly && text.find('.') == std::string::npos ? static_cast<char>(std::toupper(cc)) : cc;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !text.empty()) text.pop_back();
    if (ImGui::IsKeyDown(ImGuiKey_ModCtrl) && ImGui::IsKeyPressed(ImGuiKey_V)) {
        if (const char* clip = ImGui::GetClipboardText()) {
            for (const char* p = clip; *p && text.size() < limit; ++p) {
                if (*p > 32 && *p < 127) text += codeOnly && std::isalpha(static_cast<unsigned char>(*p)) ? static_cast<char>(std::toupper(*p)) : *p;
            }
        }
    }
}


inline void Backdrop(const Canvas& c) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 lo = vp->Pos, hi(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    c.list->AddRectFilledMultiColor(lo, hi, IM_COL32(16, 18, 34, 255), IM_COL32(16, 18, 34, 255), IM_COL32(40, 14, 10, 255),
                                    IM_COL32(40, 14, 10, 255));
    // Slow light rays from behind the banner, as on the title card.
    const ImVec2 origin = c.P(0, 330);
    const float reach = std::max(vp->Size.x, vp->Size.y) * 1.4f;
    for (int i = 0; i < 14; ++i) {
        const float a = c.t * 0.05f + i * 0.4488f;
        const ImVec2 p1(origin.x + std::cos(a) * reach, origin.y + std::sin(a) * reach),
            p2(origin.x + std::cos(a + 0.16f) * reach, origin.y + std::sin(a + 0.16f) * reach);
        c.list->AddTriangleFilled(origin, p1, p2, IM_COL32(255, 160, 70, 14));
    }
}

inline void SmashBall(const Canvas& c, float x, float y, float size, float angle) {
    static ImTextureID texture{};
    static float aspect = 1.0f;
    static bool tried = false;
    if (!tried) {
        tried = true;
        uint32_t w = 0, h = 0;
        if (uint8_t* rgba = aurora_load_png_rgba("startup/smash_ball.png", &w, &h)) {
            texture = aurora_imgui_add_texture(w, h, rgba);
            aspect = h ? float(w) / float(h) : 1.0f;
            std::free(rgba);
        }
    }
    if (!texture) return;
    const float hw = size * aspect * 0.5f, hh = size * 0.5f, co = std::cos(angle), si = std::sin(angle);
    auto Q = [&](float dx, float dy) { return c.P(x + dx * co - dy * si, y + dx * si + dy * co); };
    c.list->AddImageQuad(texture, Q(-hw, hh), Q(hw, hh), Q(hw, -hh), Q(-hw, -hh));
}


} // namespace MenuKit
