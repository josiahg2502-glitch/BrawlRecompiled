// Gecko/Ocarina codes (.gct), installed the way Project+'s Brawl Mod Loader (USB Loader GX) installs them.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CpuContext;

namespace Gecko {

// Before the game's entry point runs: the code list, gc.txt's pokes and the handler hook. No-op without codes.
void Install();
// Which build this game runs, for network play to check that everyone runs the same one: "vanilla" without codes,
// otherwise the codes' SD folder and a hash of the code list.
std::string BuildIdentity();
// A problem with how the mods are laid out on the SD card, for the player (empty when there is none).
const std::string& SetupProblem();
// Cheats (UserData/Cheats/RSBE01.ini, Dolphin's format), after the mods' codes in the same list.
struct Cheat {
    std::string name, notes;
    std::vector<uint32_t> words;
    bool enabled = false;
    bool usable = true;  // false: no code lines, or Dolphin's XXXXXXXX placeholders still in them
};
std::vector<Cheat>& Cheats();
// A mod's code list (RSBE01.GCT) is installed, not just cheats.
bool ModCodesInstalled();
// False in online play, or with neither mod codes nor a cheat file at start (the handler is not hooked then).
bool CheatsAvailable();
// Switches one on or off now and saves the file; false if the list is full (the reason in CheatProblem).
// A cheat that changed the game's code stays in effect until a restart.
bool SetCheatEnabled(size_t index, bool enabled);
const std::string& CheatProblem();
std::string CheatFile();
// VISetNextFrameBuffer_HLE stands in for the guest function whose blr the VBI hook replaces.
void RunVbiHook(CpuContext* ctx);

} // namespace Gecko
