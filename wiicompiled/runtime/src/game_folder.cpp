#include "game_folder.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace GameFolder {
namespace {

namespace fs = std::filesystem;

bool IsExtractedDisc(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path / "files", ec) && fs::is_regular_file(path / "sys" / "fst.bin", ec);
}

#if defined(_WIN32)
constexpr const wchar_t* kTitle = L"Super Smash Bros. Brawl";

constexpr const wchar_t* kSteps =
    L"This port runs your own copy of Super Smash Bros. Brawl (USA, RSBE01, revision 1 or 2). "
    L"Extract it once with Dolphin:\n\n"
    L"1. Open Dolphin on this computer and find Super Smash Bros. Brawl in the game list.\n"
    L"2. Right-click it, choose Properties, then the Filesystem tab.\n"
    L"3. Right-click the disc at the top and choose Extract Entire Disc, then pick an empty folder.\n"
    L"4. Click OK here and choose that folder. Keep it: the game reads it every time.\n\n"
    L"(Dolphin shows the revision under Properties > Info. Or put the extracted folder next to this "
    L"program and name it \"game\".)";

fs::path ProgramDirectory() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// The Windows folder picker; empty when cancelled.
fs::path PickFolder() {
    fs::path result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose the folder Dolphin extracted Super Smash Bros. Brawl to");
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    result = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return result;
}

std::wstring Widen(const std::string& utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}
#endif

} // namespace

fs::path Find(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    if (IsExtractedDisc(path)) {
        return path;
    }
    if (IsExtractedDisc(path / "DATA")) {
        return path / "DATA";
    }
    return {};
}

std::string Problem(const fs::path& root) {
    if (!IsExtractedDisc(root)) {
        return "That folder isn't an extracted game: it needs the files and sys folders Dolphin's Extract "
               "Entire Disc makes.";
    }
    std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
    char header[8] = {};
    if (!boot.read(header, sizeof(header))) {
        return "That folder has no sys/boot.bin, so the game can't be identified.";
    }
    const std::string id(header, 6);
    const int revision = static_cast<unsigned char>(header[7]);
    // projects/spm/pal/tools/make_pal_runtime.py rewrites these two for the PAL engine (R8PP01, any revision).
    constexpr const char* kGameId = "RSBE01";
    constexpr int kRevision = 1;
    // Revision 2 runs the same: its modules are revision 1's byte for byte, and its main.dol is never read (the
    // translation has revision 1's built in).
    constexpr int kAlsoRevision = 2;
    if (id != kGameId) {
        return "That is " + id + ", not Super Smash Bros. Brawl " + kGameId + ". This engine runs only that disc.";
    }
    if (kRevision >= 0 && revision != kRevision && revision != kAlsoRevision) {
        return "That is Super Smash Bros. Brawl revision " + std::to_string(revision) +
               ". This port runs revision 1 or 2 (Dolphin shows it under Properties > Info).";
    }
    return {};
}

bool EnsureConfigured() {
#if defined(_WIN32)
    // A "game" folder next to the program needs no questions, and wins over the saved folder: every
    // install shares Documents\MarioWiiSaveData, so this is how two installs play two games (say the
    // original and a mod), each with its own saves (nand_path.h keeps saves per game).
    if (const fs::path beside = Find(ProgramDirectory() / "game"); !beside.empty() && Problem(beside).empty()) {
        if (RuntimeConfigFile::DvdRoot().empty()) {
            RuntimeConfigFile::SetDvdRoot(beside);
        } else {
            RuntimeConfigFile::UseDvdRootForThisRun(beside);
        }
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(beside).c_str());
        return true;
    }
#endif
    const fs::path configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
    if (!configured.empty() && Problem(configured).empty()) {
        return true;
    }
#if defined(_WIN32)
    std::wstring message = kSteps;
    if (!configured.empty()) {
        message = Widen(Problem(configured)) + L"\n\n" + message;
    }
    for (;;) {
        if (MessageBoxW(nullptr, message.c_str(), kTitle, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
            return false;
        }
        const fs::path picked = PickFolder();
        if (picked.empty()) {
            continue;
        }
        const fs::path root = Find(picked);
        const std::string problem = Problem(root.empty() ? picked : root);
        if (!problem.empty()) {
            message = Widen(problem) + L"\n\n" + kSteps;
            continue;
        }
        RuntimeConfigFile::SetDvdRoot(root);
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(root).c_str());
        return true;
    }
#else
    return true;  // the app chose and checked the folder
#endif
}

} // namespace GameFolder
