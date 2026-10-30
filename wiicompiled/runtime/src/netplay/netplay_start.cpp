#include "netplay_start.h"

#include "aurora_events.h"
#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
void RuntimeShutdownProcessTranscript();  // main.cpp
#endif

#if defined(__ANDROID__)
namespace AndroidBridge {
bool RestartGame();
}
#endif

namespace SdCard { void Shutdown(); }  // hle/storage/sd_card.h

namespace NetplayStart {
namespace {

std::filesystem::path NetplayDirectory() {
    return RuntimeConfigFile::ApplicationDataDirectory() / "Netplay";
}
std::filesystem::path SessionFile() {
    return NetplayDirectory() / "session.txt";
}
std::filesystem::path SessionNand() {
    return NetplayDirectory() / "SessionNAND";
}

std::vector<std::string> g_names;
bool g_isHost = false;

// Brawl's save, both copies of each part: what the host keeps from a session.
constexpr const char* kSaveFiles[] = {
    "title/00010000/52534245/data/autosv0.bin",
    "title/00010000/52534245/data/autosv1.bin",
    "title/00010000/52534245/data/advsv0.bin",
    "title/00010000/52534245/data/advsv1.bin",
    "title/00010000/52534245/data/collect.vff",
};

// Copies the session's save over the player's own. Call with the NAND override cleared.
void KeepSessionSave(const char* what) {
    for (const char* file : kSaveFiles) {
        std::error_code ec;
        const auto played = SessionNand() / std::filesystem::u8path(file);
        const auto mine = RuntimeNandPath::ResolveNandRootPath() / std::filesystem::u8path(file);
        if (!std::filesystem::exists(played, ec)) continue;
        std::filesystem::create_directories(mine.parent_path(), ec);
        std::filesystem::copy_file(played, mine, std::filesystem::copy_options::overwrite_existing, ec);
        RT_LOGF(RT_TAG_RUNTIME, "netplay: kept %s %s (%s)\n", what, file, ec ? ec.message().c_str() : "ok");
    }
}

[[noreturn]] void RestartProcess() {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "netplay: could not start the game again (error %lu)\n", GetLastError());
    }
    TerminateProcess(GetCurrentProcess(), 0);
#elif defined(__ANDROID__)
    AndroidBridge::RestartGame();
#else
    // Linux: the same executable again, in place of this process, with the console's own output back and none of
    // this process's other descriptors (the guest RAM, sockets, the window) carried over.
    RuntimeShutdownProcessTranscript();
    for (int fd = 3; fd < 4096; ++fd) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    char exe[4096] = {};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length > 0) {
        char* argv[] = {exe, nullptr};
        execv(exe, argv);
    }
    RT_LOGF(RT_TAG_RUNTIME, "netplay: could not start the game again\n");
#endif
    std::_Exit(0);
}

} // namespace

bool Restart(const NetplayLobby::Plan& plan) {
    std::error_code ec;
    // A fresh NAND: the runtime's standard seeds (written at boot) plus the host's files.
    std::filesystem::remove_all(SessionNand(), ec);
    std::filesystem::create_directories(SessionNand(), ec);
    for (const auto& [path, bytes] : plan.nandFiles) {
        const auto target = SessionNand() / std::filesystem::u8path(path);
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            RT_LOGF(RT_TAG_RUNTIME, "netplay: could not write %s\n", path.c_str());
            return false;
        }
    }
    std::ofstream session(SessionFile(), std::ios::trunc);
    session << "created=" << static_cast<long long>(std::time(nullptr)) << "\n";
    session << "id=" << plan.sessionId << "\n";
    session << "slot=" << static_cast<int>(plan.localSlot) << "\n";
    session << "players=" << static_cast<int>(plan.players) << "\n";
    session << "delay=" << plan.inputDelay << "\n";
    session << "host=" << plan.host << "\n";
    session << "file=" << plan.saveFile << "\n";
    for (const auto& name : plan.names) {
        session << "name=" << name << "\n";
    }
    session.close();
    if (!session) {
        return false;
    }
    RT_LOGF(RT_TAG_RUNTIME, "netplay: restarting into session %llu as player %u of %u\n",
            static_cast<unsigned long long>(plan.sessionId), plan.localSlot + 1, plan.players);
    WindowPlacementPersistence::Flush(true);
    RestartProcess();
}

std::filesystem::path HostMarker() {
    return SessionNand() / ".host";
}

// A host that closed the game during a session never went through EndSession: bring the session's
// save back now, before anything reads the NAND.
void RecoverHostSave() {
    std::error_code ec;
    if (!std::filesystem::exists(HostMarker(), ec)) {
        return;
    }
    KeepSessionSave("from the last session you hosted:");
    std::filesystem::remove(HostMarker(), ec);
}

bool TakePendingSession(NetplaySession::Config& config, int& saveFile) {
    std::ifstream in(SessionFile());
    if (!in) {
        RecoverHostSave();
        return false;
    }
    std::map<std::string, std::string> values;
    std::vector<std::string> names;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "name") {
            names.push_back(value);
        } else {
            values[key] = value;
        }
    }
    in.close();
    std::error_code ec;
    std::filesystem::remove(SessionFile(), ec);  // a session is started once
    const long long created = std::atoll(values["created"].c_str());
    const long long now = static_cast<long long>(std::time(nullptr));
    if (created <= 0 || now - created > 120 || now < created - 5) {
        RT_LOGF(RT_TAG_RUNTIME, "netplay: ignoring a stale session file\n");
        return false;
    }
    config = {};
    config.sessionId = std::strtoull(values["id"].c_str(), nullptr, 10);
    config.localSlot = static_cast<uint8_t>(std::atoi(values["slot"].c_str()));
    config.playerCount = static_cast<uint8_t>(std::atoi(values["players"].c_str()));
    config.inputDelay = static_cast<uint32_t>(std::atoi(values["delay"].c_str()));
    config.host = values["host"];
    saveFile = std::atoi(values["file"].c_str());
    if (config.sessionId == 0 || config.playerCount < 2 || config.localSlot >= config.playerCount ||
        config.inputDelay == 0 || (config.localSlot != 0 && config.host.empty())) {
        return false;
    }
    g_names = names;
    g_isHost = config.localSlot == 0;
    if (g_isHost) {
        std::ofstream(HostMarker()) << "1\n";
    }
    RuntimeNandPath::NandRootOverride() = SessionNand();
    return true;
}

const std::vector<std::string>& SessionNames() {
    return g_names;
}

void PrepareToLeave() {
    // The host keeps what was played: the session's save goes back over the player's own.
    if (g_isHost) {
        std::error_code markerError;
        std::filesystem::remove(HostMarker(), markerError);
        RuntimeNandPath::NandRootOverride().clear();
        KeepSessionSave("from the session:");
    }
    std::error_code ec;
    std::filesystem::remove(SessionFile(), ec);
}

void RestartGame() {
    SdCard::Shutdown();  // what the game wrote on the SD card goes back into UserData/SD first
    RestartProcess();
}

void EndSession() {
    PrepareToLeave();
    WindowPlacementPersistence::Flush(true);
    RestartProcess();
}

} // namespace NetplayStart
