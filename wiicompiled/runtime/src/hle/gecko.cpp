// gecko.cpp - Gecko/Ocarina codes (.gct) for mods, installed as Project+'s Brawl Mod Loader (a USB Loader GX
// derivative) installs them: its code handler at 0x80001800 (translated with the game: projects/brawl/gecko), the
// code list at the address gc.txt names (default 0x800022A8), gc.txt's pokes, and a branch to the handler where
// gc.txt's hooktype says. Codes then patch the game freely; patched functions run in the interpreter
// (guest_code_patch.cpp).
//
// The codes are the first <folder>/RSBE01.GCT on the virtual SD card (UserData/SD/<folder>, Project+'s layout:
// sd:/Project+/RSBE01.GCT and gc.txt), else UserData/Codes/RSBE01.gct with an optional gc.txt beside it.
//
// Cheats come after them in the same list: UserData/Cheats/RSBE01.ini in Dolphin's format ([Gecko] holds "$Name" lines
// each followed by its "XXXXXXXX YYYYYYYY" code lines, [Gecko_Enabled] the names switched on), so a Dolphin cheat file
// works as it is. The options menu switches them while the game runs: the list is rewritten and the handler picks it
// up on its next frame. Never in online play (every player must run the same game).

#include "gecko.h"

#include "abi_bridge.h"
#include "det_clock.h"
#include "guest_code_patch.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <xxhash.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>


namespace {

namespace fs = std::filesystem;

constexpr uint32_t kHandlerEntry = 0x800018A8u;
constexpr uint32_t kBootStack = 0x805B5100u;  // the main thread's stack (its top is 0x805B5150), unused before __start
constexpr uint32_t kCodeListSlot = 0x80001804u;  // where the shipped handler reads the code list address
constexpr uint32_t kVbiHookSite = 0x801E9A2Cu;   // VISetNextFrameBuffer's blr
constexpr uint32_t kAxHookSite = 0x80200984u;    // __AXOutNewFrame's blr (hooktype 7, AXNextFrame)

struct Poke {
    uint32_t address, value;
    std::optional<uint32_t> ifAddress, ifValue;  // pokeifequal
};

struct GameConfig {
    uint32_t codeListStart = 0x800022A8u;
    uint32_t codeListEnd = 0x80003000u;
    int hookType = 1;
    std::vector<Poke> pokes;
};

uint32_t BranchTo(uint32_t at, uint32_t target) { return 0x48000000u | ((target - at) & 0x03FFFFFCu); }

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<fs::path> FindFile(const fs::path& directory, const std::string& name) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && Lower(entry.path().filename().string()) == Lower(name)) return entry.path();
    }
    return std::nullopt;
}

std::string g_setupProblem;  // FindCodes' advice when the card's layout hides a mod

std::optional<fs::path> FindCodes() {
    const fs::path userData = RuntimeConfigFile::ApplicationDataDirectory();
    // Config.toml [mods] sd_codes: the card folder whose codes load when several builds share the card (the
    // launcher's "Codes to load"); otherwise the first folder with codes. "vanilla": none.
    if (RuntimeConfigFile::ModsVanilla()) return std::nullopt;
    const std::string chosen = RuntimeConfigFile::ModsSdCodes();
    if (!chosen.empty()) {
        if (auto codes = FindFile(userData / "SD" / chosen, "RSBE01.GCT")) return codes;
        RT_LOGF(RT_TAG_MOD, "[gecko] the chosen SD folder '%s' has no RSBE01.GCT; using the first found\n", chosen.c_str());
    }
    std::error_code ec;
    std::vector<fs::path> folders;
    for (const auto& entry : fs::directory_iterator(userData / "SD", ec)) {
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        if (auto codes = FindFile(folder, "RSBE01.GCT")) return codes;
    }
    // A download unpacked into a folder of its own (SD/Project+ v3.2.1 (Wii Lite)/Project+/RSBE01.GCT): its codes
    // look for their files at the card's root, so say what to move instead of quietly starting the vanilla game.
    for (const auto& folder : folders) {
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            if (!entry.is_directory(ec) || !FindFile(entry.path(), "RSBE01.GCT")) continue;
            const std::string outer = reinterpret_cast<const char*>(folder.filename().u8string().c_str());
            g_setupProblem = "Your mod is one folder too deep: move everything inside SD\\" + outer +
                             " straight into the SD folder. Starting Brawl without it.";
            RT_LOGF(RT_TAG_MOD, "[gecko] %s\n", g_setupProblem.c_str());
            return FindFile(userData / "Codes", "RSBE01.gct");
        }
    }
    return FindFile(userData / "Codes", "RSBE01.gct");
}

bool MatchesGameId(const std::string& pattern) {
    static constexpr char kId[] = "RSBE01";
    for (size_t i = 0; i < 6; ++i) {
        if (i >= pattern.size() || (pattern[i] != '?' && std::toupper(static_cast<unsigned char>(pattern[i])) != kId[i])) return false;
    }
    return pattern.size() == 6;
}

std::vector<uint32_t> HexArguments(const std::string& line) {
    std::vector<uint32_t> out;
    const size_t open = line.find('('), close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    std::stringstream args(line.substr(open + 1, close - open - 1));
    for (std::string token; std::getline(args, token, ',');) out.push_back(static_cast<uint32_t>(std::stoul(token, nullptr, 16)));
    return out;
}

// USB Loader GX's gameconfig.txt format: sections headed by a game id (? matches any character) ending in ':'.
GameConfig ReadGameConfig(const fs::path& path) {
    GameConfig config;
    std::ifstream in(path);
    bool active = false;
    for (std::string line; std::getline(in, line);) {
        line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == ':') {
            active = MatchesGameId(line.substr(0, line.size() - 1));
            continue;
        }
        if (!active) continue;
        try {
            const std::string key = Lower(line.substr(0, line.find_first_of("=(")));
            const auto value = [&] { return static_cast<uint32_t>(std::stoul(line.substr(line.find('=') + 1), nullptr, 16)); };
            if (key == "codeliststart") config.codeListStart = value();
            else if (key == "codelistend") config.codeListEnd = value();
            else if (key == "hooktype") config.hookType = static_cast<int>(std::stoul(line.substr(line.find('=') + 1)));
            else if (key == "poke") {
                const auto args = HexArguments(line);
                if (args.size() == 2) config.pokes.push_back({args[0], args[1], std::nullopt, std::nullopt});
            } else if (key == "pokeifequal") {
                const auto args = HexArguments(line);
                if (args.size() == 4) config.pokes.push_back({args[2], args[3], args[0], args[1]});
            }
        } catch (const std::exception&) {
            RT_LOGF(RT_TAG_MOD, "[gecko] gc.txt: cannot read '%s'\n", line.c_str());
        }
    }
    return config;
}

// The installed list: where it lives, and the mods' codes (header included, terminator dropped) that come first.
uint32_t g_listStart = 0, g_listEnd = 0;
std::vector<uint8_t> g_modCodes;
std::vector<Gecko::Cheat> g_cheats;
std::vector<std::string> g_cheatFileLines;  // the file as read, minus [Gecko_Enabled], written back with it
std::string g_cheatProblem;

fs::path CheatFilePath() { return RuntimeConfigFile::ApplicationDataDirectory() / "Cheats" / "RSBE01.ini"; }

bool ParseCodeLine(const std::string& line, uint32_t& hi, uint32_t& lo) {
    if (line.size() < 17 || line[8] != ' ') return false;
    for (size_t i : {size_t{0}, size_t{9}}) {
        for (size_t j = i; j < i + 8; ++j) if (!std::isxdigit(static_cast<unsigned char>(line[j]))) return false;
    }
    hi = static_cast<uint32_t>(std::stoul(line.substr(0, 8), nullptr, 16));
    lo = static_cast<uint32_t>(std::stoul(line.substr(9, 8), nullptr, 16));
    return true;
}

// One cheat file: its lines minus [Gecko_Enabled], its cheats with the lines each one spans, the names switched on.
struct CheatFile {
    std::vector<std::string> lines;
    std::vector<Gecko::Cheat> cheats;
    std::vector<std::pair<size_t, size_t>> spans;
    std::vector<std::string> enabled;
};

CheatFile ReadCheatFile(const fs::path& path) {
    CheatFile file;
    std::ifstream in(path);
    std::string section;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        t.erase(0, std::min(t.size(), t.find_first_not_of(" \t")));
        if (!t.empty() && t[0] == '[') section = Lower(t.substr(0, t.find(']') + 1));
        if (section == "[gecko_enabled]") {
            if (!t.empty() && t[0] == '$') file.enabled.push_back(t.substr(1));
            continue;
        }
        file.lines.push_back(line);
        if (section != "[gecko]" || t.empty() || t[0] == '[') continue;
        if (t[0] == '$') {
            file.cheats.push_back({t.substr(1), {}, {}, false, true});
            file.spans.push_back({file.lines.size() - 1, file.lines.size()});
            continue;
        }
        if (file.cheats.empty()) continue;
        file.spans.back().second = file.lines.size();
        Gecko::Cheat& cheat = file.cheats.back();
        if (t[0] == '*') {
            cheat.notes += (cheat.notes.empty() ? "" : " ") + t.substr(1);
        } else {
            uint32_t hi = 0, lo = 0;
            if (ParseCodeLine(t, hi, lo)) {
                cheat.words.push_back(hi);
                cheat.words.push_back(lo);
            } else {
                cheat.usable = false;  // Dolphin's "XXXXXXXX" placeholders: values to fill in first
            }
        }
    }
    for (auto& cheat : file.cheats) {
        cheat.usable = cheat.usable && !cheat.words.empty();
        cheat.enabled = cheat.usable && std::find(file.enabled.begin(), file.enabled.end(), cheat.name) != file.enabled.end();
    }
    return file;
}

void SaveCheats();

void LoadCheats() {
    CheatFile mine = ReadCheatFile(CheatFilePath());
    // The list shipped beside the game (engine/default-cheats.ini): codes the player's file doesn't have yet are added
    // to it, all off, on the first start and after an update that brings new ones.
    const CheatFile shipped = ReadCheatFile(CheatFilePath().parent_path().parent_path().parent_path() / "default-cheats.ini");
    bool added = false;
    for (size_t i = 0; i < shipped.cheats.size(); ++i) {
        const auto have = [&](const Gecko::Cheat& c) { return c.name == shipped.cheats[i].name; };
        if (std::any_of(mine.cheats.begin(), mine.cheats.end(), have)) continue;
        if (!added) mine.lines.push_back("[Gecko]");
        added = true;
        mine.lines.insert(mine.lines.end(), shipped.lines.begin() + static_cast<std::ptrdiff_t>(shipped.spans[i].first),
                          shipped.lines.begin() + static_cast<std::ptrdiff_t>(shipped.spans[i].second));
    }
    g_cheatFileLines = std::move(mine.lines);
    g_cheats = std::move(mine.cheats);
    if (added) {
        SaveCheats();
        const CheatFile merged = ReadCheatFile(CheatFilePath());
        g_cheatFileLines = merged.lines;
        g_cheats = merged.cheats;
    }
}

void SaveCheats() {
    std::error_code ec;
    fs::create_directories(CheatFilePath().parent_path(), ec);
    std::ofstream out(CheatFilePath(), std::ios::binary | std::ios::trunc);
    for (const auto& line : g_cheatFileLines) out << line << "\n";
    out << "[Gecko_Enabled]\n";
    for (const auto& cheat : g_cheats) if (cheat.enabled) out << "$" << cheat.name << "\n";
}

// The mods' codes then the switched-on cheats, ended; false when they do not fit (the cheats are left out then).
bool WriteCodeList() {
    std::vector<uint8_t> list = g_modCodes;
    auto push = [&](uint32_t w) { for (int sh = 24; sh >= 0; sh -= 8) list.push_back(static_cast<uint8_t>(w >> sh)); };
    for (const auto& cheat : g_cheats) if (cheat.enabled) for (uint32_t w : cheat.words) push(w);
    push(0xF0000000u);
    push(0);
    g_cheatProblem.clear();
    if (list.size() > g_listEnd - g_listStart) {
        g_cheatProblem = "Too many cheats on at once: they don't fit in the code list. Switch some off.";
        RT_LOGF(RT_TAG_MOD, "[gecko] %zu bytes of codes and cheats do not fit 0x%08X..0x%08X\n", list.size(), g_listStart, g_listEnd);
        list.resize(g_modCodes.size());
        push(0xF0000000u);
        push(0);
    }
    std::memcpy(Memory::GetPointer(g_listStart, list.size()), list.data(), list.size());
    return g_cheatProblem.empty();
}

} // namespace

void BrawlModulesRecordBootPatches();  // hle/brawl_modules.cpp

namespace Gecko {

const std::string& SetupProblem() {
    static const bool searched = (FindCodes(), true);
    (void)searched;
    return g_setupProblem;
}

std::string BuildIdentity() {
    static const std::string identity = [] {
        const auto codesPath = FindCodes();
        if (!codesPath) return std::string("vanilla");
        std::ifstream in(*codesPath, std::ios::binary);
        const std::vector<uint8_t> codes((std::istreambuf_iterator<char>(in)), {});
        char hash[24];
        std::snprintf(hash, sizeof(hash), " %016llx", static_cast<unsigned long long>(XXH3_64bits(codes.data(), codes.size())));
        return std::string(reinterpret_cast<const char*>(codesPath->parent_path().filename().u8string().c_str())).substr(0, 40) + hash;
    }();
    return identity;
}

void Install() {
    static constexpr uint8_t kMagic[8] = {0x00, 0xD0, 0xC0, 0xDE, 0x00, 0xD0, 0xC0, 0xDE};
    // Online play runs every player's game in step: no cheats there.
    if (!DetClock::Enabled()) LoadCheats();
    const auto codesPath = FindCodes();
    std::vector<uint8_t> codes(kMagic, kMagic + 8);
    GameConfig config;
    if (codesPath) {
        std::ifstream in(*codesPath, std::ios::binary);
        codes.assign(std::istreambuf_iterator<char>(in), {});
        if (codes.size() < 16 || std::memcmp(codes.data(), kMagic, 8) != 0) {
            RT_LOGF(RT_TAG_MOD, "[gecko] '%s' is not a Gecko code list\n", reinterpret_cast<const char*>(codesPath->u8string().c_str()));
            return;
        }
        if (const auto gameConfig = FindFile(codesPath->parent_path(), "gc.txt")) config = ReadGameConfig(*gameConfig);
        if (codes.size() > config.codeListEnd - config.codeListStart || !Memory::Contains(config.codeListStart, codes.size())) {
            RT_LOGF(RT_TAG_MOD, "[gecko] %zu bytes of codes do not fit 0x%08X..0x%08X\n", codes.size(), config.codeListStart,
                    config.codeListEnd);
            return;
        }
        static constexpr uint8_t kEnd[8] = {0xF0, 0, 0, 0, 0, 0, 0, 0};
        if (codes.size() >= 24 && std::memcmp(codes.data() + codes.size() - 8, kEnd, 8) == 0) codes.resize(codes.size() - 8);
    } else if (g_cheats.empty()) {
        return;  // no mod and no cheat file: the game as it is
    }
    g_listStart = config.codeListStart;
    g_listEnd = config.codeListEnd;
    g_modCodes = codes;

    // Guard main.dol's code first, so the pokes and the hook below are seen as the code patches they are.
    GuestCodePatch::Protect(0x80004000u, 0x800064E0u, "main.dol .init");
    GuestCodePatch::Protect(0x8000C860u, 0x804064E0u, "main.dol .text");

    Memory::Write32(kCodeListSlot, config.codeListStart);
    WriteCodeList();
    for (const Poke& poke : config.pokes) {
        if (!poke.ifAddress || Memory::Read32(*poke.ifAddress) == *poke.ifValue) Memory::Write32(poke.address, poke.value);
    }
    switch (config.hookType) {
    case 1: Memory::Write32(kVbiHookSite, BranchTo(kVbiHookSite, kHandlerEntry)); break;
    case 7: Memory::Write32(kAxHookSite, BranchTo(kAxHookSite, kHandlerEntry)); break;
    default: RT_LOGF(RT_TAG_MOD, "[gecko] hooktype %d is not supported; using the VI hook\n", config.hookType);
             Memory::Write32(kVbiHookSite, BranchTo(kVbiHookSite, kHandlerEntry));
    }
    GuestCodePatch::Sync();
    RT_LOGF(RT_TAG_MOD, "[gecko] %zu code lines from '%s' at 0x%08X, hooktype %d, %zu poke(s), %zu cheat(s)\n", codes.size() / 8 - 1,
            codesPath ? reinterpret_cast<const char*>(codesPath->u8string().c_str()) : "(cheats only)", config.codeListStart,
            config.hookType, config.pokes.size(), g_cheats.size());
    // The loaders (Gecko OS, Project+'s, REX's) run the handler once before the game's first instruction, so every
    // hook is in place from the start: REX creates its Syriinge heap from a hook inside the heap set-up loop, which
    // the first hooked frame comes too late for.
    CpuContext boot{};
    boot.gpr[1] = kBootStack;
    InvokeIndirectCpu(kHandlerEntry, &boot);
    GuestCodePatch::Sync();
    BrawlModulesRecordBootPatches();
}

std::vector<Cheat>& Cheats() { return g_cheats; }

bool ModCodesInstalled() { return g_modCodes.size() > 8; }  // more than the header

bool CheatsAvailable() { return g_listStart != 0 && !DetClock::Enabled(); }

bool SetCheatEnabled(size_t index, bool enabled) {
    if (index >= g_cheats.size() || !CheatsAvailable() || !g_cheats[index].usable) return false;
    g_cheats[index].enabled = enabled;
    SaveCheats();
    return WriteCodeList();
}

const std::string& CheatProblem() { return g_cheatProblem; }

std::string CheatFile() { return reinterpret_cast<const char*>(CheatFilePath().u8string().c_str()); }

void RunVbiHook(CpuContext* ctx) {
    if (Memory::Read32(kVbiHookSite) != BranchTo(kVbiHookSite, kHandlerEntry)) return;
    InvokeIndirectCpu(kHandlerEntry, ctx);
    GuestCodePatch::Sync();
}

} // namespace Gecko
