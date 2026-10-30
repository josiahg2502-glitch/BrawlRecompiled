#pragma once

#include <string>

// The game checks GitHub for a newer release by itself (curl, in the background, once per run) and the options menu
// offers it; installing hands over to the launcher's updater (launcher/Updater.cs: download, unpack over the install
// keeping engine/UserData, start the game again). Only in a packaged install (the game in engine/ beside the launcher).
namespace Updates {

// The newer version ("0.5.2"), or "" (none, not known yet, or not a packaged install). The first call starts the check.
std::string Available();
// Starts the launcher's updater; true if it started (the caller then closes the game).
bool Install();

} // namespace Updates
