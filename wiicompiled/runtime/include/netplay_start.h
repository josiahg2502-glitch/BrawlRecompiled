#pragma once

#include "netplay_lobby.h"
#include "netplay_session.h"

// Network play, the hand-over from the lobby to the session: every device restarts the game into
// deterministic mode from a NAND of its own that holds the host's save, so all of them boot into the
// same state (netplay_session.h).
namespace NetplayStart {

// Writes the session NAND and the pending-session file, then restarts the game. Returns only on
// failure.
bool Restart(const NetplayLobby::Plan& plan);

// At startup: a pending session written by Restart in the last couple of minutes. Fills `config`,
// points the NAND at the session's copy and consumes the file.
bool TakePendingSession(NetplaySession::Config& config, int& saveFile);

// The players' names in the running session, by slot.
const std::vector<std::string>& SessionNames();

// Starts the game again (the options menu, after a change that needs it). Does not return.
[[noreturn]] void RestartGame();

// Leaves the session: restarts the game normally (the host keeps the session's save).
void EndSession();
// The part of EndSession that keeps the host's save and forgets the session, for a front end that
// restarts the game itself (the Android menu).
void PrepareToLeave();

} // namespace NetplayStart
