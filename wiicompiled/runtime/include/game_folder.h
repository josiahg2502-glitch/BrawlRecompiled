#pragma once

#include <filesystem>
#include <string>

// The player's own copy of Super Smash Bros. Brawl, extracted with Dolphin ("Extract Entire Disc"):
// a folder holding files/ and sys/. This port runs the USA disc, revision 1 or 2 (RSBE01): revision 2's
// modules are revision 1's byte for byte, and the translation has revision 1's main.dol built in.
namespace GameFolder {

// The extracted game at `path` (or its DATA subfolder); empty when there is none.
std::filesystem::path Find(const std::filesystem::path& path);

// Why the game at `root` can't be used ("" when it can): not an extracted disc, or another game or
// revision, read from sys/boot.bin.
std::string Problem(const std::filesystem::path& root);

// At startup, before anything reads the disc. Windows: a game folder that is missing or wrong is
// asked for, with the Dolphin steps and a folder picker (a "game" folder next to the program is
// picked up by itself), and saved to Config.toml. Elsewhere the app supplies the folder.
// Returns false when the player gave up (the program then quits).
bool EnsureConfigured();

} // namespace GameFolder
