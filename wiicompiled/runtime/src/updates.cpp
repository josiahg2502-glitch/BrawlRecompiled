// updates.cpp - see updates.h.

#include "updates.h"

#include "brawl_version.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Updates {
namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::string g_newer;
std::once_flag g_started;

fs::path ExePath() {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe);
#else
    char exe[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    return n > 0 ? fs::path(std::string(exe, static_cast<size_t>(n))) : fs::path();
#endif
}

// The launcher beside engine/, in a packaged install; empty in the dev tree (Play/).
fs::path Launcher() {
    const fs::path engine = ExePath().parent_path();
    if (engine.filename() != "engine") return {};
#if defined(_WIN32)
    const fs::path launcher = engine.parent_path() / "Brawl Recompiled.exe";
#else
    const fs::path launcher = engine.parent_path() / "Brawl Recompiled";
#endif
    std::error_code ec;
    return fs::exists(launcher, ec) ? launcher : fs::path();
}

bool Run(const std::string& command) {
#if defined(_WIN32)
    std::wstring wide(command.begin(), command.end());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
#else
    return std::system(command.c_str()) == 0;
#endif
}

// "0.5.2" > "0.5.1", numerically per part.
bool Newer(const std::string& a, const std::string& b) {
    std::istringstream x(a), y(b);
    for (int i = 0; i < 3; ++i) {
        int p = 0, q = 0;
        char dot;
        x >> p;
        y >> q;
        if (p != q) return p > q;
        x >> dot;
        y >> dot;
    }
    return false;
}

void Check() {
    if (Launcher().empty()) return;
    const fs::path out = RuntimeConfigFile::ApplicationDataDirectory() / "update-check.json";
    const std::string command = "curl -s -L -m 15 -H \"User-Agent: BrawlRecompiled\" -o \"" + RuntimeConfigFile::PathToUtf8(out) +
                                "\" https://api.github.com/repos/josiahg2502-glitch/BrawlRecompiled/releases/latest";
    if (!Run(command)) {
        RT_LOGF(RT_TAG_RUNTIME, "[update] could not reach GitHub\n");
        return;
    }
    std::ifstream in(out);
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    in.close();
    std::error_code ec;
    fs::remove(out, ec);
    std::smatch match;
    if (!std::regex_search(json, match, std::regex("\"tag_name\"\\s*:\\s*\"v?([0-9]+\\.[0-9]+\\.[0-9]+)\""))) return;
    const std::string latest = match[1];
    RT_LOGF(RT_TAG_RUNTIME, "[update] latest release %s, this is %s\n", latest.c_str(), BRAWL_VERSION);
    if (!Newer(latest, BRAWL_VERSION)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_newer = latest;
}

} // namespace

std::string Available() {
    std::call_once(g_started, [] { std::thread(Check).detach(); });
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_newer;
}

bool Install() {
    const fs::path launcher = Launcher();
    if (launcher.empty()) return false;
#if defined(_WIN32)
    std::wstring command = L"\"" + launcher.wstring() + L"\" --update";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                        launcher.parent_path().wstring().c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    return std::system(("\"" + launcher.string() + "\" --update &").c_str()) == 0;
#endif
}

} // namespace Updates
