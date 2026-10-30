// SuperPaperMario.exe: the one engine executable. Each disc region is a static recompilation of its own dol, so
// the engine proper is a module per region (engine_usa.dll for RSBE01, engine_pal.dll for R8PP01, both built from
// runtime/ by tools/build-spm.ps1 with MKW_ENGINE_MODULE). This stub reads the extracted disc's id and runs the
// matching module's SDL_main (main.cpp). SPM_ENGINE=usa|pal overrides the choice.
#include <windows.h>
#include <cpuid.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path ExeDirectory() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// [paths] dvd_root from UserData\Config.toml, relative paths resolved against UserData (runtime_config.h does the same).
fs::path ConfiguredDvdRoot(const fs::path& exeDir) {
    std::ifstream config(exeDir / "UserData" / "Config.toml");
    std::string line;
    bool inPaths = false;
    while (std::getline(config, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line.erase(0, first);
        if (line[0] == '[') { inPaths = line.rfind("[paths]", 0) == 0; continue; }
        if (!inPaths || line.rfind("dvd_root", 0) != 0) continue;
        std::string value = line.substr(line.find('=') + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.substr(1, value.size() - 2);
        fs::path root = fs::path(value);
        return root.is_absolute() ? root : exeDir / "UserData" / root;
    }
    return {};
}

std::string DiscId(const fs::path& root) {
    for (const fs::path& candidate : {root, root / "DATA"}) {
        std::ifstream boot(candidate / "sys" / "boot.bin", std::ios::binary);
        char id[6] = {};
        if (boot.read(id, 6)) return std::string(id, 6);
    }
    return {};
}

} // namespace

// The usual engine needs x86-64-v3 (AVX2, FMA, BMI2...); older processors get engine_<region>_v2.dll when it is there.
bool HostHasV3() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(1, 0, a, b, c, d);
    const bool fma = (c >> 12) & 1, osxsave = (c >> 27) & 1, avx = (c >> 28) & 1, movbe = (c >> 22) & 1, f16c = (c >> 29) & 1;
    if (!(fma && osxsave && avx && movbe && f16c)) return false;
    unsigned lo = 0, hi = 0;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6u) != 6u) return false;
    __cpuid_count(7, 0, a, b, c, d);
    return ((b >> 5) & 1) && ((b >> 3) & 1) && ((b >> 8) & 1);  // AVX2, BMI1, BMI2
}

int main(int argc, char** argv) {
    const fs::path exeDir = ExeDirectory();
    std::string region = "usa";
    if (const char* forced = std::getenv("SPM_ENGINE"); forced && *forced) {
        region = forced;
    } else {
        std::string id = DiscId(exeDir / "game");  // a "game" folder beside the program wins (game_folder.cpp)
        if (id.empty()) id = DiscId(ConfiguredDvdRoot(exeDir));
        if (id == "R8PP01") region = "pal";
    }
    fs::path module = exeDir / ("engine_" + region + ".dll");
    const fs::path oldCpu = exeDir / ("engine_" + region + "_v2.dll");
    std::error_code ec;
    if ((std::getenv("BRAWL_ENGINE_V2") != nullptr || !HostHasV3()) && fs::exists(oldCpu, ec)) module = oldCpu;
    HMODULE engine = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto entry = engine ? reinterpret_cast<int (*)(int, char**)>(GetProcAddress(engine, "SDL_main")) : nullptr;
    if (!entry) {
        const std::wstring message = L"The game engine could not be loaded:\n" + module.wstring() +
                                     L"\n(error " + std::to_wstring(GetLastError()) + L")\n\nUnzip the whole download again.";
        MessageBoxW(nullptr, message.c_str(), L"Super Smash Bros. Brawl", MB_OK | MB_ICONERROR);
        return 1;
    }
    return entry(argc, argv);
}
