// sd_card.cpp - the virtual SD card image behind /dev/sdio/slot0 (sdio.cpp).
//
// UserData/SD (a folder) is the user-facing card: the image UserData/Cache/sd_card.raw is built from it with
// FatFs whenever the folder changed since the last build (a manifest of sizes and times decides), and files the
// game created or changed on the card are copied back into the folder at shutdown. Without that folder a
// Dolphin-style UserData/sd.raw image is used directly.
//
// The image holds only the card's structure (FAT, folders): each folder file gets a contiguous run of clusters
// that is left unwritten (a sparse file) and read from the folder file itself (sd_card.raw.extents lists them), so
// a mod's gigabytes are not stored twice. A run the game writes into is copied into the image first.

#include "sd_card.h"

#include "det_clock.h"
#include "hle/gecko.h"
#include "runtime_config.h"
#include "runtime_log.h"

extern "C" {
#include "ff.h"      // before diskio.h, which uses its LBA_t
#include "diskio.h"
}

#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kSector = 512;
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kMaxStandardCard = 2048 * kMiB;  // bigger is an SDHC card (sdio.cpp)
constexpr uint64_t kMaxCard = 32767 * kMiB;
constexpr uint64_t kCluster = 32 * 1024;

std::mutex g_mutex;
FILE* g_image = nullptr;
uint64_t g_size = 0;
bool g_tried = false;
fs::path g_imagePath;
fs::path g_folder;  // empty: a plain image, nothing to sync back

SdCard::ProgressHandler g_progress = nullptr;
uint64_t g_progressDone = 0, g_progressTotal = 0;

void ReportProgress(uint64_t bytes) {
    g_progressDone += bytes;
    if (g_progress) g_progress(g_progressDone, g_progressTotal);
}

// The image FatFs works on while building or syncing.
FILE* g_fatfsDisk = nullptr;
uint64_t g_fatfsSectors = 0;

bool SeekTo(FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

FILE* OpenFile(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(path.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path FromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

// Card bytes that are a folder file's contents, by card offset (the folder file's relative path and length).
struct Extent {
    uint64_t length = 0;
    std::string path;
};
std::map<uint64_t, Extent> g_extents;
std::map<std::string, FILE*> g_hostFiles;  // open folder files behind extents

fs::path ExtentsPath() { return fs::path(g_imagePath).concat(".extents"); }

bool ReadExtents() {
    g_extents.clear();
    std::ifstream in(ExtentsPath(), std::ios::binary);
    if (!in) return false;
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        uint64_t offset = 0;
        Extent extent;
        if (fields >> offset >> extent.length && std::getline(fields >> std::ws, extent.path)) g_extents[offset] = extent;
    }
    return true;
}

// Written aside and renamed over, like the manifest.
void WriteExtents() {
    const fs::path temp = fs::path(ExtentsPath()).concat(".new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        for (const auto& [offset, extent] : g_extents) out << offset << ' ' << extent.length << ' ' << extent.path << '\n';
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(temp, ExtentsPath(), ec);
}

void CloseHostFiles() {
    for (auto& [path, file] : g_hostFiles) std::fclose(file);
    g_hostFiles.clear();
}

FILE* HostFile(const std::string& path) {
    if (auto it = g_hostFiles.find(path); it != g_hostFiles.end()) return it->second;
    if (g_hostFiles.size() >= 64) CloseHostFiles();
    FILE* file = OpenFile(g_folder / FromUtf8(path), "rb");
    if (file) g_hostFiles[path] = file;
    return file;
}

// The first extent that may overlap `offset` onwards.
std::map<uint64_t, Extent>::iterator FirstExtentFrom(uint64_t offset) {
    auto it = g_extents.upper_bound(offset);
    if (it != g_extents.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second.length > offset) return previous;
    }
    return it;
}

// Card reads: extents from their folder files, the rest from the image.
bool ReadCard(FILE* image, uint64_t offset, void* destination, uint64_t length) {
    auto* out = static_cast<uint8_t*>(destination);
    while (length > 0) {
        auto it = FirstExtentFrom(offset);
        uint64_t chunk = length;
        if (it != g_extents.end() && it->first <= offset) {
            chunk = std::min(chunk, it->first + it->second.length - offset);
            FILE* host = HostFile(it->second.path);
            if (!host || !SeekTo(host, offset - it->first) || std::fread(out, 1, chunk, host) != chunk) {
                RT_LOGF(RT_TAG_NAND, "[sd] reading '%s' for the card failed\n", it->second.path.c_str());
                return false;
            }
        } else {
            if (it != g_extents.end()) chunk = std::min(chunk, it->first - offset);
            if (!SeekTo(image, offset) || std::fread(out, 1, chunk, image) != chunk) return false;
        }
        out += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

// Before the game (or FatFs) writes into extents: their folder files' bytes go into the image, which holds them
// from then on.
bool DetachExtents(FILE* image, uint64_t offset, uint64_t length) {
    bool changed = false;
    std::vector<char> buffer(kMiB);
    for (auto it = FirstExtentFrom(offset); it != g_extents.end() && it->first < offset + length;) {
        FILE* host = HostFile(it->second.path);
        for (uint64_t done = 0; done < it->second.length;) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), it->second.length - done));
            if (!host || !SeekTo(host, done) || std::fread(buffer.data(), 1, chunk, host) != chunk ||
                !SeekTo(image, it->first + done) || std::fwrite(buffer.data(), 1, chunk, image) != chunk) {
                return false;
            }
            done += chunk;
        }
        it = g_extents.erase(it);
        changed = true;
    }
    if (changed) WriteExtents();
    return true;
}

bool WriteCard(FILE* image, uint64_t offset, const void* source, uint64_t length) {
    return DetachExtents(image, offset, length) && SeekTo(image, offset) &&
           std::fwrite(source, 1, length, image) == length;
}

void ForgetExtents(const std::string& path) {
    for (auto it = g_extents.begin(); it != g_extents.end();) it = it->second.path == path ? g_extents.erase(it) : std::next(it);
    if (auto open = g_hostFiles.find(path); open != g_hostFiles.end()) {
        std::fclose(open->second);
        g_hostFiles.erase(open);
    }
}

// The image's unwritten runs take no disk space (NTFS needs to be told; Linux file systems do it anyway).
void MakeSparse(FILE* file) {
#ifdef _WIN32
    constexpr DWORD kFsctlSetSparse = 0x000900C4;
    DWORD returned = 0;
    DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))), kFsctlSetSparse, nullptr, 0, nullptr, 0,
                    &returned, nullptr);
#else
    (void)file;
#endif
}

// A new file's contents on the card: a contiguous run read from the folder file, or (no contiguous room left)
// copied into the image.
bool FillFile(FIL& file, const std::string& path, uint64_t size) {
    if (size == 0) return true;
    if (f_expand(&file, static_cast<FSIZE_t>(size), 1) == FR_OK) {
        const FATFS* volume = file.obj.fs;
        const uint64_t sector = volume->database + static_cast<uint64_t>(file.obj.sclust - 2) * volume->csize;
        g_extents[sector * kSector] = {size, path};
        ReportProgress(size);
        return true;
    }
    FILE* source = OpenFile(g_folder / FromUtf8(path), "rb");
    if (!source) return false;
    bool ok = true;
    std::vector<char> buffer(4 * kMiB);
    for (size_t got; ok && (got = std::fread(buffer.data(), 1, buffer.size(), source)) > 0;) {
        UINT written = 0;
        ok = f_write(&file, buffer.data(), static_cast<UINT>(got), &written) == FR_OK && written == got;
        ReportProgress(got);
    }
    std::fclose(source);
    return ok;
}

// FAT timestamps are local time with 2-second resolution.
DWORD FatTime(std::time_t t) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    if (local.tm_year < 80) return (1u << 21) | (1u << 16);  // 1980-01-01
    return (static_cast<DWORD>(local.tm_year - 80) << 25) | (static_cast<DWORD>(local.tm_mon + 1) << 21) |
           (static_cast<DWORD>(local.tm_mday) << 16) | (static_cast<DWORD>(local.tm_hour) << 11) |
           (static_cast<DWORD>(local.tm_min) << 5) | (static_cast<DWORD>(local.tm_sec) / 2);
}

std::time_t FromFatTime(WORD date, WORD time) {
    std::tm local{};
    local.tm_year = ((date >> 9) & 0x7F) + 80;
    local.tm_mon = ((date >> 5) & 0xF) - 1;
    local.tm_mday = date & 0x1F;
    local.tm_hour = (time >> 11) & 0x1F;
    local.tm_min = (time >> 5) & 0x3F;
    local.tm_sec = (time & 0x1F) * 2;
    local.tm_isdst = -1;
    return std::mktime(&local);
}

// File times as whole seconds through the C runtime, the same on every scan (the manifest's 2-second FAT times must
// not drift): std::filesystem's file_time_type has no exact conversion to time_t on this toolchain.
std::time_t ModifiedTime(const fs::path& path) {
#ifdef _WIN32
    struct _stat64 st {};
    return _wstat64(path.c_str(), &st) == 0 ? st.st_mtime : 0;
#else
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
#endif
}

void SetModifiedTime(const fs::path& path, std::time_t t) {
#ifdef _WIN32
    struct __utimbuf64 times { t, t };
    _wutime64(path.c_str(), &times);
#else
    struct utimbuf times { t, t };
    ::utime(path.c_str(), &times);
#endif
}

// What the image was built from: relative path -> size and FAT time (directories have size UINT64_MAX).
struct Entry {
    uint64_t size = 0;
    DWORD fatTime = 0;
    bool operator==(const Entry&) const = default;
};
using Manifest = std::map<std::string, Entry>;
constexpr uint64_t kDirectory = ~0ull;

Manifest ScanFolder(const fs::path& folder) {
    Manifest manifest;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path relative = it->path().lexically_relative(folder);
        std::error_code entryEc;
        if (it->is_directory(entryEc)) {
            manifest[Utf8(relative)] = {kDirectory, 0};
        } else if (it->is_regular_file(entryEc)) {
            const auto size = it->file_size(entryEc);
            if (!entryEc) manifest[Utf8(relative)] = {size, FatTime(ModifiedTime(it->path()))};
        }
    }
    return manifest;
}

// Renamed whenever the image layout changes, so an older cached image is rebuilt.
fs::path ManifestPath() { return fs::path(g_imagePath).concat(".fat32b.manifest"); }

Manifest ReadManifest() {
    Manifest manifest;
    std::ifstream in(ManifestPath(), std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        Entry entry;
        std::string path;
        if (fields >> entry.size >> entry.fatTime && std::getline(fields >> std::ws, path)) manifest[path] = entry;
    }
    return manifest;
}

// Written aside and renamed over: a process killed while writing it must not leave a half manifest (a full rebuild).
void WriteManifest(const Manifest& manifest) {
    const fs::path temp = fs::path(ManifestPath()).concat(".new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        for (const auto& [path, entry] : manifest) out << entry.size << ' ' << entry.fatTime << ' ' << path << '\n';
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(temp, ManifestPath(), ec);
}

// FatFs paths are UTF-8, rooted at the volume.
std::string VolumePath(const std::string& relative) { return "/" + relative; }

bool BuildImage(const Manifest& manifest) {
    uint64_t content = 0;
    for (const auto& [path, entry] : manifest) {
        content += entry.size == kDirectory ? kCluster : (entry.size + kCluster - 1) / kCluster * kCluster;
    }
    // Dolphin's card from a folder (Common/FatFsUtil.cpp), the one Brawl mods are made on: the contents plus half
    // as much free space, at least 512 MiB and at most 8 GiB, as FAT32 without a partition table.
    uint64_t size = content + std::clamp(content / 2, 512 * kMiB, 8192 * kMiB);
    size = (size + kCluster - 1) / kCluster * kCluster;
    if (size > kMaxCard) {
        RT_LOGF(RT_TAG_NAND, "[sd] '%s' holds %llu MiB, more than an SD card can: not building a card\n",
                Utf8(g_folder).c_str(), static_cast<unsigned long long>(content / kMiB));
        return false;
    }
    if (size > kMaxStandardCard) {
        RT_LOGF(RT_TAG_NAND, "[sd] the card is %llu MiB, an SDHC card: Brawl reads it only with a mod's SDHC support "
                "(Project+ has it)\n", static_cast<unsigned long long>(size / kMiB));
    }

    std::error_code ec;
    fs::create_directories(g_imagePath.parent_path(), ec);
    const fs::path temp = fs::path(g_imagePath).concat(".building");
    FILE* disk = OpenFile(temp, "w+b");
    if (!disk) return false;
    MakeSparse(disk);
    g_extents.clear();
    CloseHostFiles();
    // Size the image without writing every byte.
    if (!SeekTo(disk, size - 1) || std::fputc(0, disk) == EOF) {
        std::fclose(disk);
        return false;
    }
    g_fatfsDisk = disk;
    g_fatfsSectors = size / kSector;

    RT_LOGF(RT_TAG_NAND, "[sd] building the SD card image (%llu MiB) from '%s'...\n",
            static_cast<unsigned long long>(size / kMiB), Utf8(g_folder).c_str());
    const auto start = std::chrono::steady_clock::now();
    bool ok = true;
    std::vector<BYTE> work(64 * 1024);
    MKFS_PARM options{};
    options.fmt = FM_FAT32 | FM_SFD;
    options.align = 1;
    FRESULT fr = f_mkfs("", &options, work.data(), static_cast<UINT>(work.size()));
    auto volume = std::make_unique<FATFS>();
    if (fr != FR_OK || f_mount(volume.get(), "", 1) != FR_OK) {
        RT_LOGF(RT_TAG_NAND, "[sd] formatting the image failed (FatFs %d)\n", static_cast<int>(fr));
        ok = false;
    }

    g_progressDone = 0;
    g_progressTotal = content;
    for (auto it = manifest.begin(); ok && it != manifest.end(); ++it) {
        const auto& [path, entry] = *it;
        const std::string target = VolumePath(path);
        if (entry.size == kDirectory) {
            fr = f_mkdir(target.c_str());
            if (fr != FR_OK && fr != FR_EXIST) {
                RT_LOGF(RT_TAG_NAND, "[sd] mkdir '%s' failed (FatFs %d)\n", target.c_str(), static_cast<int>(fr));
                ok = false;
            }
            continue;
        }
        FIL file;
        if (f_open(&file, target.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK || !FillFile(file, path, entry.size)) {
            RT_LOGF(RT_TAG_NAND, "[sd] putting '%s' on the card failed\n", path.c_str());
            ok = false;
            break;
        }
        ok = f_close(&file) == FR_OK && ok;
        FILINFO info{};
        info.fdate = static_cast<WORD>(entry.fatTime >> 16);
        info.ftime = static_cast<WORD>(entry.fatTime);
        f_utime(target.c_str(), &info);
    }
    f_mount(nullptr, "", 0);
    std::fclose(disk);
    g_fatfsDisk = nullptr;

    if (!ok) {
        fs::remove(temp, ec);
        return false;
    }
    fs::rename(temp, g_imagePath, ec);
    if (ec) {
        fs::remove(g_imagePath, ec);
        fs::rename(temp, g_imagePath, ec);
    }
    if (ec) return false;
    WriteExtents();
    WriteManifest(manifest);
    RT_LOGF(RT_TAG_NAND, "[sd] SD card image built in %.1f s (%zu entries)\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), manifest.size());
    return true;
}

// Writes one folder file onto the card at its path, with the folder's FAT time.
bool PutFile(const std::string& path, const Entry& entry) {
    const std::string target = VolumePath(path);
    if (entry.size == kDirectory) {
        const FRESULT fr = f_mkdir(target.c_str());
        return fr == FR_OK || fr == FR_EXIST;
    }
    ForgetExtents(path);  // its old clusters are freed
    FIL file;
    if (f_open(&file, target.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    bool ok = FillFile(file, path, entry.size);
    ok = f_close(&file) == FR_OK && ok;
    FILINFO info{};
    info.fdate = static_cast<WORD>(entry.fatTime >> 16);
    info.ftime = static_cast<WORD>(entry.fatTime);
    f_utime(target.c_str(), &info);
    return ok;
}

// Brings the existing card from what it was built from (`built`) to the folder as it is now (`current`): files
// changed or added are written, files gone are removed, folders gone last. False when the card is better rebuilt
// (too much changed, or no room).
bool UpdateImage(const Manifest& built, const Manifest& current) {
    std::vector<std::pair<std::string, Entry>> put;
    std::vector<std::string> gone;
    uint64_t bytes = 0;
    for (const auto& [path, entry] : current) {
        const auto was = built.find(path);
        if (was != built.end() && was->second == entry) continue;
        put.emplace_back(path, entry);
        if (entry.size != kDirectory) bytes += entry.size;
    }
    for (const auto& [path, entry] : built) {
        if (!current.count(path)) gone.push_back(path);
    }
    if (put.size() + gone.size() > 256) return false;  // a new mod: a fresh card sized for it
    g_progressDone = 0;
    g_progressTotal = bytes;
    FILE* disk = OpenFile(g_imagePath, "r+b");
    if (!disk) return false;
    g_fatfsDisk = disk;
    std::error_code ec;
    g_fatfsSectors = fs::file_size(g_imagePath, ec) / kSector;
    auto volume = std::make_unique<FATFS>();
    bool ok = f_mount(volume.get(), "", 1) == FR_OK;
    DWORD freeClusters = 0;
    FATFS* mounted = nullptr;
    if (ok && f_getfree("", &freeClusters, &mounted) == FR_OK) {
        const uint64_t room = static_cast<uint64_t>(freeClusters) * mounted->csize * kSector;
        ok = bytes + 16 * kMiB < room;  // the game's own writes need room too
    }
    // Files first, deepest folders last, so a folder is empty when removed.
    std::sort(gone.begin(), gone.end(), [&](const std::string& a, const std::string& b) {
        const bool da = built.at(a).size == kDirectory, db = built.at(b).size == kDirectory;
        return da != db ? !da : a.size() > b.size();
    });
    for (auto it = gone.begin(); ok && it != gone.end(); ++it) {
        ForgetExtents(*it);
        ok = f_unlink(VolumePath(*it).c_str()) == FR_OK;
    }
    for (auto it = put.begin(); ok && it != put.end(); ++it) ok = PutFile(it->first, it->second);
    f_mount(nullptr, "", 0);
    std::fclose(disk);
    g_fatfsDisk = nullptr;
    if (!ok) {
        RT_LOGF(RT_TAG_NAND, "[sd] the card could not be updated in place: building it again\n");
        return false;
    }
    WriteExtents();
    WriteManifest(current);
    RT_LOGF(RT_TAG_NAND, "[sd] card updated: %zu file(s) written, %zu removed\n", put.size(), gone.size());
    return true;
}

// Copies what the game created or changed on the card back into the folder. Deletions are not mirrored.
void SyncBack(const fs::path& relative, const Manifest& built, int& copied) {
    const std::string dirPath = relative.empty() ? "/" : VolumePath(Utf8(relative));
    DIR dir;
    if (f_opendir(&dir, dirPath.c_str()) != FR_OK) return;
    std::vector<char> buffer(4 * kMiB);
    for (FILINFO info; f_readdir(&dir, &info) == FR_OK && info.fname[0];) {
        const fs::path childRelative = relative / FromUtf8(info.fname);
        const std::string key = Utf8(childRelative);
        const fs::path host = g_folder / childRelative;
        std::error_code ec;
        if (info.fattrib & AM_DIR) {
            fs::create_directories(host, ec);
            SyncBack(childRelative, built, copied);
            continue;
        }
        const DWORD fatTime = (static_cast<DWORD>(info.fdate) << 16) | info.ftime;
        const auto known = built.find(key);
        if (known != built.end() && known->second.size == info.fsize && known->second.fatTime == fatTime) continue;
        FIL file;
        FILE* out = nullptr;
        if (f_open(&file, VolumePath(key).c_str(), FA_READ) != FR_OK) continue;
        fs::create_directories(host.parent_path(), ec);
        if ((out = OpenFile(host, "wb")) != nullptr) {
            UINT got = 0;
            while (f_read(&file, buffer.data(), static_cast<UINT>(buffer.size()), &got) == FR_OK && got > 0) {
                std::fwrite(buffer.data(), 1, got, out);
            }
            std::fclose(out);
            SetModifiedTime(host, FromFatTime(info.fdate, info.ftime));
            ++copied;
        }
        f_close(&file);
    }
    f_closedir(&dir);
}

bool OpenLocked() {
    if (g_tried) return g_image != nullptr;
    g_tried = true;
    // Online (deterministic mode) a vanilla game has no card at all, so an empty SD folder on one player's PC and none
    // on another's can't make their games differ.
    if (RuntimeConfigFile::ModsVanilla() || (DetClock::Enabled() && Gecko::BuildIdentity() == "vanilla")) {
        RT_LOGF(RT_TAG_NAND, "[sd] vanilla (no mods) chosen: no SD card\n");
        return false;
    }
    const fs::path userData = RuntimeConfigFile::ApplicationDataDirectory();
    std::error_code ec;
    if (fs::is_directory(userData / "SD", ec)) {
        g_folder = userData / "SD";
        g_imagePath = userData / "Cache" / "sd_card.raw";
        fs::remove(fs::path(g_imagePath).concat(".fat32.manifest"), ec);  // a full-copy card from before 0.5.1: rebuilt
        const Manifest current = ScanFolder(g_folder);
        const Manifest built = ReadManifest();
        const bool haveExtents = ReadExtents();
        if (!fs::exists(g_imagePath, ec) || built != current || !haveExtents) {
            const auto [now, then] = std::mismatch(current.begin(), current.end(), built.begin(), built.end());
            const std::string& changed = now != current.end() ? now->first : then != built.end() ? then->first : "";
            RT_LOGF(RT_TAG_NAND, "[sd] '%s' changed since the card was built%s%s\n", Utf8(g_folder).c_str(),
                    changed.empty() ? "" : ": ", changed.c_str());
            // A few files changed (the game's own writes copied back, a mod's update): change them on the card rather
            // than build the whole card again.
            if (!(fs::exists(g_imagePath, ec) && !built.empty() && haveExtents && UpdateImage(built, current)) &&
                !BuildImage(current)) {
                return false;
            }
        }
    } else if (fs::exists(userData / "sd.raw", ec)) {
        g_imagePath = userData / "sd.raw";
    } else {
        return false;
    }
    g_image = OpenFile(g_imagePath, "r+b");
    if (!g_image) {
        RT_LOGF(RT_TAG_NAND, "[sd] cannot open '%s'\n", Utf8(g_imagePath).c_str());
        return false;
    }
    g_size = fs::file_size(g_imagePath, ec) / kSector * kSector;
    RT_LOGF(RT_TAG_NAND, "[sd] SD card inserted: '%s' (%llu MiB)\n", Utf8(g_imagePath).c_str(),
            static_cast<unsigned long long>(g_size / kMiB));
    return true;
}

} // namespace

extern "C" DSTATUS disk_status(BYTE) { return g_fatfsDisk ? 0 : STA_NOINIT; }
extern "C" DSTATUS disk_initialize(BYTE) { return g_fatfsDisk ? 0 : STA_NOINIT; }

extern "C" DRESULT disk_read(BYTE, BYTE* buffer, LBA_t sector, UINT count) {
    return g_fatfsDisk && ReadCard(g_fatfsDisk, static_cast<uint64_t>(sector) * kSector, buffer, count * kSector)
               ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_write(BYTE, const BYTE* buffer, LBA_t sector, UINT count) {
    return g_fatfsDisk && WriteCard(g_fatfsDisk, static_cast<uint64_t>(sector) * kSector, buffer, count * kSector)
               ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_ioctl(BYTE, BYTE command, void* buffer) {
    switch (command) {
    case CTRL_SYNC: return g_fatfsDisk && std::fflush(g_fatfsDisk) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buffer) = static_cast<LBA_t>(g_fatfsSectors); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buffer) = static_cast<WORD>(kSector); return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buffer) = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

extern "C" DWORD get_fattime() { return FatTime(std::time(nullptr)); }

namespace SdCard {

void SetProgressHandler(ProgressHandler handler) { g_progress = handler; }

bool Present() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool open = OpenLocked();
    if (g_progressTotal != 0) {  // a build or update ran (or failed part way): take its progress off the screen
        if (g_progress) g_progress(g_progressTotal, g_progressTotal);
        g_progressTotal = 0;
    }
    return open;
}

uint64_t SizeBytes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return OpenLocked() ? g_size : 0;
}

bool Read(uint64_t offset, void* destination, size_t length) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return OpenLocked() && offset + length <= g_size && ReadCard(g_image, offset, destination, length);
}

bool Write(uint64_t offset, const void* source, size_t length) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return OpenLocked() && offset + length <= g_size && WriteCard(g_image, offset, source, length);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_image) return;
    std::fclose(g_image);
    g_image = nullptr;
    if (g_folder.empty()) return;
    g_fatfsDisk = OpenFile(g_imagePath, "r+b");
    if (!g_fatfsDisk) return;
    g_fatfsSectors = g_size / kSector;
    auto volume = std::make_unique<FATFS>();
    int copied = 0;
    if (f_mount(volume.get(), "", 1) == FR_OK) {
        SyncBack({}, ReadManifest(), copied);
        f_mount(nullptr, "", 0);
    }
    std::fclose(g_fatfsDisk);
    g_fatfsDisk = nullptr;
    CloseHostFiles();
    // The folder now matches the card, so the next start reuses this image instead of rebuilding it.
    WriteManifest(ScanFolder(g_folder));
    if (copied) RT_LOGF(RT_TAG_NAND, "[sd] copied %d file(s) the game wrote back to '%s'\n", copied, Utf8(g_folder).c_str());
}

} // namespace SdCard
