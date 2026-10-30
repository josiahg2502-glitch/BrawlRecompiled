BRAWL RECOMPILED {VERSION}
==========================
Start: open "Brawl Recompiled.exe". The game starts straight away.

First time only: a window asks for your Super Smash Bros. Brawl disc image (.iso / .ciso / .wbfs, USA RSBE01). It is
extracted into the Disc folder here (about 7 GB), then the game starts.

Options: F10, or Home (or Back and Start together) on a controller, opens the options menu in the game: graphics,
audio, controls, gameplay, cheats, mods, online play, updates and quitting. Other keys:
  F5 / F7  save / load state (F6 picks the slot)    F8  fast-forward    F9  online play    F11  fullscreen / window
Save states save the game exactly as it is, anywhere (mid-fight, mid-level). They last until you close the game, and
load in the same match or level they were saved in.

Cheats: Gecko codes in Dolphin's format in engine\UserData\Cheats\RSBE01.ini (paste a Dolphin cheat file in as it
is); switch them on and off in Options > Cheats. Comes with 9 codes tested on this port: tournament
settings (everything unlocked), bigger fighters (2x/3x/4x), invisible or emerald fighters, infinite shield,
infinite jumps and no "Ready to fight" bar. Not in online play.

Mods (Options > Mods):
- SD card mods, Project+ included: engine\UserData\SD is the game's SD card. "Add a mod" copies a mod's extracted Wii
  download onto it, or copy its SD card contents in yourself (for Project+: its Project+, private and apps folders).
  Several builds can share the card: "Build to play" picks the one that runs (or vanilla Brawl). Builds that work
  here: Project+ (Project+), Project+Ex (P+Ex), REX (rex_), Brawl Minus (Minusery), Legacy XP (LegacyXP).
- Mods that replace game files or textures: one folder per mod in engine\UserData\mods.
- HD texture packs made for Dolphin (REX and Project+ ship them): copy the pack's RSBE01 folder (inside Dolphin's
  Load\Textures) into engine\UserData\texture_replacements.

Ultrawide: Options > Graphics > Aspect ratio > Fill Window uses the whole screen up to 32:9, with the HUD kept in the
middle 16:9.

Updates: the game checks for new versions; Options > System installs one (your saves, settings and mods stay).

Older processors (no AVX2, before Intel Haswell / AMD Excavator) run automatically on a slower build of the engine.

Online (F9, or Options > Online): host a room or join one on your network, over Tailscale, or by the host's address
(the host forwards UDP port 52130). Everyone needs the same build: vanilla, or the same mod.

Your saves, settings and mods live in engine\UserData. Keep this folder together; it needs nothing else.
"Brawl Recompiled.exe --settings" opens the old settings window (Wii save import, keyboard keys).

Credits and licenses: CREDITS.txt and LICENSE.txt.
