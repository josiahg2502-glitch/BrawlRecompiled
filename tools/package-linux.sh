#!/usr/bin/env bash
# Packs the Linux download: dist/BrawlRecompiled-linux.tar.gz. Run on Linux or in WSL after tools/build-linux.sh and a
# Windows build (Play\ holds the shared files; tools\build-launcher.ps1 makes launcher\linux\publish).
# Same contents as package-brawl.ps1's engine\, with the Linux programs; the disc is never copied in.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=$HOME/brawl-linux
version=v$(grep -oP '(?<=<Version>)[^<]+' "$root/launcher/BrawlRecompiled.Launcher.csproj")
stage=$build/stage/BrawlRecompiled
engine=$stage/engine
rm -rf "$build/stage"
mkdir -p "$engine/UserData/mods" "$engine/UserData/SD" "$engine/licenses"

[[ -x $build/usa/build/WiiCompiled ]] || { echo "missing the build: run tools/build-linux.sh" >&2; exit 1; }
cp "$build/usa/build/WiiCompiled" "$engine/brawl-engine"
cp "$root/launcher/linux/publish/Brawl Recompiled" "$stage/"
cp "$build/usa/build/_deps/png-build/libpng16.so.16" "$engine/"   # the one library the game does not build in
cp -L "$build/sdl-shared/libSDL3.so.0" "$engine/"                 # the launcher's controller list
cp "$build/usa/build/dsp_coef.bin" "$build/usa/build/cacert.pem" "$root/Play/portable.txt" "$engine/"
cp -r "$root/Play/wii_bootstrap" "$engine/"
cp "$root/tools/cheats-RSBE01.ini" "$engine/default-cheats.ini"   # the starter cheat list (gecko.cpp copies it into UserData)
mkdir -p "$engine/startup" && cp "$root/launcher/art/smash_ball.png" "$engine/startup/"   # the title card's Smash Ball
cp "$root/tools/CREDITS.txt" "$stage/CREDITS.txt"
cp "$root/wiicompiled/LICENSE" "$stage/LICENSE.txt"
cp "$root/wiicompiled/THIRD-PARTY-NOTICES.md" "$engine/licenses/"
cp "$root/wiicompiled/runtime/third_party/fatfs/LICENSE.txt" "$engine/licenses/FatFs-LICENSE.txt"
for dir in "$root"/wiicompiled/Dependencies/*/; do
    for f in "$dir"LICEN[CS]E* "$dir"COPYING* "$dir"NOTICE*; do
        [[ -f $f ]] && cp "$f" "$engine/licenses/$(basename "$dir")-$(basename "${f%.*}").txt"
    done
done
sed "s/{VERSION}/$version/" "$root/tools/package-README-linux.txt" >"$stage/README.txt"
# Files copied off /mnt/c come out executable: only the programs should be.
find "$stage" -type f -exec chmod 644 {} +
chmod 755 "$stage/Brawl Recompiled" "$engine/brawl-engine"

# Guard: no disc data and no SD card contents in the download.
if find "$stage" -type d \( -name Disc -o -name DATA -o -name sys \) | grep -q .; then echo 'disc folders in the build' >&2; exit 1; fi
[[ -z $(ls -A "$engine/UserData/SD") ]] || { echo 'SD card contents in the build' >&2; exit 1; }

mkdir -p "$root/dist"
tar -C "$build/stage" -czf "$root/dist/BrawlRecompiled-linux.tar.gz" BrawlRecompiled
echo "DONE: dist/BrawlRecompiled-linux.tar.gz ($version, $(du -sh "$root/dist/BrawlRecompiled-linux.tar.gz" | cut -f1))"
