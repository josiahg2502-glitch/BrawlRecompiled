# Builds Brawl Recompiled's release folder and updates the player's game folder (-Install, default
# Desktop\My recomps\BrawlRecompiled).
#   powershell -ExecutionPolicy Bypass -File tools\package-brawl.ps1 [-Build] [-Zip] [-Install <folder>]
# -Build first rebuilds the game (build-brawl.ps1 -SkipTools) and the launcher. -Zip also writes dist\BrawlRecompiled.zip.
# Layout: Brawl Recompiled.exe (launcher), README.txt, engine\ (brawl-engine.exe = Play\SuperSmashBrosBrawl.exe
# renamed, engine_usa.dll = the recompiled game, DLLs, wii_bootstrap\, UserData\ = saves, settings, the virtual SD
# card (UserData\SD: Project+ and other SD mods) and UserData\mods (file and texture replacement mods)).
# The disc is never copied in: the launcher extracts the player's image into Disc\ inside the folder.
# A clean copy is staged in dist\_stage (deleted afterwards) and copied over the game folder, so the saves, Config.toml,
# mods and Disc\ already there are kept. The version is <Version> in launcher\BrawlRecompiled.Launcher.csproj.
[CmdletBinding()] param([switch]$Build, [switch]$Zip, [string]$Install = "$env:USERPROFILE\Desktop\My recomps\BrawlRecompiled")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$launcher = "$root\launcher\publish\Brawl Recompiled.exe"
if ($Build) { & "$PSScriptRoot\build-brawl.ps1" -SkipTools; & "$PSScriptRoot\build-launcher.ps1" }
elseif (-not (Test-Path -LiteralPath $launcher)) { & "$PSScriptRoot\build-launcher.ps1" }
$version = 'v' + ([xml](Get-Content -LiteralPath "$root\launcher\BrawlRecompiled.Launcher.csproj")).Project.PropertyGroup.Version
function Mirror([string]$From, [string]$To) {
    robocopy $From $To /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null  # few retries: a running game locks its files
    if ($LASTEXITCODE -ge 8) { throw "robocopy $From -> $To failed ($LASTEXITCODE)" }
}
function Remove-Tree([string]$Path) {
    # Explorer or antivirus briefly holds files in a big tree after it is opened: retry the delete.
    for ($try = 1; Test-Path -LiteralPath $Path; $try++) {
        try { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop }
        catch { if ($try -ge 5) { throw }; Start-Sleep -Seconds 3 }
    }
}

$stage = "$root\dist\_stage\BrawlRecompiled"
Remove-Tree "$root\dist\_stage"
$engine = "$stage\engine"
foreach ($d in 'UserData\mods', 'UserData\SD') { New-Item -ItemType Directory -Force "$engine\$d" | Out-Null }
Copy-Item -LiteralPath $launcher -Destination $stage
# The game: runtime files only (no saves, logs, caches, SD card images or debug switches).
Copy-Item -LiteralPath "$root\Play\SuperSmashBrosBrawl.exe" -Destination "$engine\brawl-engine.exe"
# The exe is the loader; the engine proper is engine_usa.dll (build-brawl.ps1), copied with the other DLLs below.
if (-not (Test-Path -LiteralPath "$root\Play\engine_usa.dll")) { throw 'missing engine_usa.dll: run build-brawl.ps1' }
foreach ($f in '*.dll', 'dsp_coef.bin', 'portable.txt') { Copy-Item "$root\Play\$f" $engine }
# Dawn (webgpu_dawn.dll) and dxcompiler.dll need the Visual C++ 2015-2022 runtime; ship it app-local (the files of
# Microsoft's redistributable) so a PC without the VC++ Redistributable still starts the game.
foreach ($f in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item "$env:SystemRoot\System32\$f" $engine }
Mirror "$root\Play\wii_bootstrap" "$engine\wii_bootstrap"
# The starter cheat list, copied into UserData\Cheats\RSBE01.ini on first start (never over a player's own).
Copy-Item -LiteralPath "$PSScriptRoot\cheats-RSBE01.ini" -Destination "$engine\default-cheats.ini"
# The title card's Smash Ball (tools/make_icon.py, from the game's own Smash Ball textures).
New-Item -ItemType Directory -Force "$engine\startup" | Out-Null
Copy-Item "$root\launcher\art\smash_ball.png" "$engine\startup"
# Credits and licenses: GPLv3 for the engine, the bundled libraries' own license files.
Copy-Item -LiteralPath "$PSScriptRoot\CREDITS.txt" -Destination "$stage\CREDITS.txt"
Copy-Item -LiteralPath "$root\wiicompiled\LICENSE" -Destination "$stage\LICENSE.txt"
New-Item -ItemType Directory -Force "$engine\licenses" | Out-Null
Copy-Item -LiteralPath "$root\wiicompiled\THIRD-PARTY-NOTICES.md" -Destination "$engine\licenses"
Copy-Item -LiteralPath "$root\wiicompiled\runtime\third_party\fatfs\LICENSE.txt" -Destination "$engine\licenses\FatFs-LICENSE.txt"
foreach ($dir in Get-ChildItem -LiteralPath "$root\wiicompiled\Dependencies" -Directory) {
    foreach ($f in Get-ChildItem -LiteralPath $dir.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|COPYING|NOTICE)' }) {
        Copy-Item -LiteralPath $f.FullName -Destination "$engine\licenses\$($dir.Name)-$($f.BaseName).txt"
    }
}
(Get-Content -LiteralPath "$PSScriptRoot\package-README.txt" -Raw).Replace('{VERSION}', $version) | Set-Content -LiteralPath "$stage\README.txt" -Encoding utf8

# Guard: no disc data and no SD card contents come from the dev tree (the player makes both inside the folder).
if (Get-ChildItem -LiteralPath $stage -Recurse -Directory | Where-Object { $_.Name -in 'Disc', 'DATA', 'sys' }) { throw 'disc folders in the build' }
if (Get-ChildItem -LiteralPath "$engine\UserData\SD" -Force) { throw 'SD card contents in the build' }

if ($Zip) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zipPath = "$root\dist\BrawlRecompiled.zip"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    # Entry by entry with '/' separators: Windows PowerShell's CreateFromDirectory writes '\', which 7-Zip, WinRAR,
    # Linux and macOS unpack as flat files named "BrawlRecompiled\engine\..." (only Explorer copes).
    $base = Split-Path -Parent $stage
    $archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in Get-ChildItem -LiteralPath $stage -Recurse -Force) {
            $name = $item.FullName.Substring($base.Length + 1).Replace('\', '/')
            if (-not $item.PSIsContainer) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $item.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
            } elseif (-not (Get-ChildItem -LiteralPath $item.FullName -Force)) {
                [void]$archive.CreateEntry("$name/")  # keep empty folders (UserData\mods, UserData\SD)
            }
        }
    } finally { $archive.Dispose() }
    Write-Host "ZIP: $zipPath"
}

# Copy over the game folder, keeping what is not in the build (saves, Config.toml, mods, the SD card, Disc\).
$dist = $Install
Mirror $stage $dist
Remove-Tree "$root\dist\_stage"
# Only the launcher should be visible: hide the engine folder (the launcher finds it regardless).
$eng = Get-Item -LiteralPath "$dist\engine" -Force; $eng.Attributes = $eng.Attributes -bor [IO.FileAttributes]::Hidden
$size = [math]::Round((Get-ChildItem -LiteralPath $dist -Recurse -File -Force | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "DONE: $dist ($version, $size MB)"
