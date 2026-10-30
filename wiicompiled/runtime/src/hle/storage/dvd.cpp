#include "hle_stubs.h"
#include "isa/big_endian.h"
#include "hle/dvd_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"

// Defined in hle/gx/gx_objects.cpp. DVD reads are DMA-class writes to guest
// RAM: the game brackets them with DCInvalidateRange (not a flush), so the GX
// layer's caches must be notified explicitly that these bytes changed.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);
#include "hle/storage/riivolution.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <unordered_set>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <deque>
#include <utility>

namespace fs = std::filesystem;

// ============================================================================
// Configuration
// ============================================================================

// The extracted ISO "DATA" folder. We map its "files" subfolder to the DVD
// root "/" and its "sys" subfolder to "/sys/". The root is user-owned input:
// it is never embedded into or copied by the public runtime.
static fs::path g_dvdRoot;
static std::once_flag g_dvdRootOnce;

static uint32_t CurrentDiscGameCode() {
    return RuntimeHle::CurrentGameCode(0x52534245u); // SMNE (NSMBW) fallback
}

// ============================================================================
// DVD Constants & Structures
// ============================================================================

#define DVD_STATE_FATAL_ERROR  -1
#define DVD_STATE_END           0
#define DVD_STATE_BUSY          1
#define DVD_STATE_WAITING       2
#define DVD_STATE_COVER_CLOSED  3
#define DVD_STATE_NO_DISK       4
#define DVD_STATE_COVER_OPEN    5

// Offsets in DVD Command Block (OS standard)
#define DVD_CB_OFFSET_STATE       0x0C
#define DVD_CB_OFFSET_TRANSFERRED 0x20
#define DVD_FILEINFO_OFFSET_ADDR  0x30
#define DVD_FILEINFO_OFFSET_LEN   0x34

struct DVDFileEntry {
    fs::path hostPath;
    std::string dvdPath;  // Virtual Wii path (e.g., "/Race/Course.szs")
    uint32_t size;
    uint32_t discOffsetWords = 0;
    bool isDirectory = false;
};

struct FstFileEntry {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    std::string dvdPath;
    fs::path hostPath;
};

// Global State
static std::vector<DVDFileEntry> g_fileEntries;

// ============================================================================
// Host read cache
// ============================================================================

// The game's reads (course and object archives, and the music it streams while playing) run on the
// one host thread every guest thread shares, so a slow read stalls the frame. Each read used to open
// the file again, which on a busy PC (antivirus and sync filters, a hard disk) took long enough to
// drop frames during play. On Windows the whole disc (about 340 MB) is read into memory once, on a
// background thread at the lowest I/O priority, and reads are then served from it. Phones keep
// reading from flash, which is fast and has less memory to spare.
namespace {

using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;
std::mutex g_readCacheMutex;
std::unordered_map<fs::path::string_type, FileBytes> g_readCache;
std::atomic<uint64_t> g_dvdReadNanos{0};
std::atomic<uint32_t> g_dvdReads{0};

FileBytes CachedFile(const fs::path& path) {
    std::lock_guard lock(g_readCacheMutex);
    const auto it = g_readCache.find(path.native());
    return it != g_readCache.end() ? it->second : nullptr;
}

bool ReadHostFile(const fs::path& path, uint64_t offset, uint32_t length, std::vector<uint8_t>& destination,
                  DvdReadContract::HostReadFailure& failure) {
    const auto start = std::chrono::steady_clock::now();
    bool ok = false;
    if (const FileBytes bytes = CachedFile(path)) {
        failure = DvdReadContract::HostReadFailure::None;
        if (offset >= bytes->size()) {
            failure = DvdReadContract::HostReadFailure::BadOffset;
        } else if (length > bytes->size() - offset) {
            failure = DvdReadContract::HostReadFailure::ShortRead;
        } else {
            destination.assign(bytes->begin() + static_cast<std::ptrdiff_t>(offset),
                               bytes->begin() + static_cast<std::ptrdiff_t>(offset + length));
            ok = true;
        }
    } else {
        ok = DvdReadContract::ReadExact(path, offset, length, destination, failure);
    }
    g_dvdReadNanos.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - start)
                                                       .count()),
                             std::memory_order_relaxed);
    g_dvdReads.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void StartReadCachePreload() {
#if defined(_WIN32)
    constexpr uint64_t kCacheLimitBytes = 1ull << 30;  // mods can be larger than the disc; stop at 1 GiB
    std::vector<fs::path> paths;
    for (const DVDFileEntry& entry : g_fileEntries) {
        if (!entry.isDirectory && entry.size > 0) {
            paths.push_back(entry.hostPath);
        }
    }
    std::thread([paths = std::move(paths)] {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto start = std::chrono::steady_clock::now();
        uint64_t total = 0;
        size_t files = 0;
        for (const fs::path& path : paths) {
            std::error_code ec;
            const uint64_t size = fs::file_size(path, ec);
            if (ec || size == 0 || total + size > kCacheLimitBytes || CachedFile(path)) {
                continue;
            }
            std::ifstream file(path, std::ios::binary);
            auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size))) {
                continue;
            }
            {
                std::lock_guard lock(g_readCacheMutex);
                g_readCache.emplace(path.native(), std::move(bytes));
            }
            total += size;
            ++files;
        }
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        RT_LOG(RT_TAG_DVD) << "read cache: " << files << " file(s), " << (total >> 20) << " MiB in "
                           << std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count()
                           << " ms" << std::endl;
    }).detach();
#endif
}

}  // namespace

// Time spent in the game's disc reads since start, and how many (slow-frame breakdowns take deltas).
uint64_t DvdReadNanosTotal() { return g_dvdReadNanos.load(std::memory_order_relaxed); }
uint32_t DvdReadCount() { return g_dvdReads.load(std::memory_order_relaxed); }
static std::map<std::string, int32_t> g_pathToEntry;
// Maps each published file's disc range back to its runtime entry.
struct PublishedExtent {
    uint64_t startBytes = 0;
    uint64_t endBytes = 0;
    int32_t entryIndex = -1;
};
static std::vector<PublishedExtent> g_publishedExtents;
static bool g_dvdInitialized = false;
static std::vector<FstFileEntry> g_fstFiles;
static bool g_fstLoaded = false;
static std::unordered_set<std::string> g_loggedReadErrors;
static constexpr int32_t kDvdFatalError = -3;

extern "C" void DVDInit_8015EA1C();

// This MEM2 region is reserved at startup for the published FST.
extern "C" uint32_t g_dvdFstReservedBase;
extern "C" uint32_t g_dvdFstReservedSize;

// Byte-wise copy into guest RAM plus the DMA notification the GX caches need.
static void CopyToGuestAsDma(uint32_t dest, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        Memory::Write8(dest + static_cast<uint32_t>(i), data[i]);
    }
    GxNotifyGuestRamDmaWrite(dest, static_cast<uint32_t>(size));
}

// Host path strings only ever leave this module as UTF-8 display text.
static std::string HostPathText(const fs::path& path) {
    return RuntimeConfigFile::PathToUtf8(path);
}

static bool IsDvdDataRoot(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec &&
           fs::is_directory(path / "files", ec) && !ec &&
           fs::is_regular_file(path / "sys" / "fst.bin", ec) && !ec;
}

// Single fatal idiom for this module: crash artifacts under `reason`, the same
// text in the popup, a non-zero exit code, and no second generic report from
// the atexit handler.
[[noreturn]] static void FailDvd(const char* reason, const char* category,
                                 const std::string& details) {
    RuntimeCrash::WriteCrashArtifacts(reason, details);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup(category, details);
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

[[noreturn]] static void FailDvdRoot(const char* source, const fs::path& path = {}) {
    RT_LOGF(RT_TAG_DVD, "ERROR: %s", source);
    if (!path.empty()) {
        std::fprintf(stderr, ": %s", HostPathText(path).c_str());
    }
    std::fprintf(stderr,
                 "\n[dvd] Set [paths] dvd_root in Config.toml to your Super Smash Bros. Brawl, "
                 "extracted with Dolphin's Extract Entire Disc.\n");
    std::string details = source ? source : "The configured DVD root could not be opened.";
    if (!path.empty()) {
        details += "\n\nPath: ";
        details += HostPathText(path);
    }
    details += "\n\nSet [paths] dvd_root in Config.toml to your Super Smash Bros. Brawl "
               "(RSBE01 revision 1), extracted with Dolphin's Extract Entire Disc.";
    FailDvd("dvd_root", "DVD data is unavailable", details);
}

static const fs::path& GetDvdRoot() {
    std::call_once(g_dvdRootOnce, []() {
        const fs::path path = RuntimeConfigFile::ResolvedDvdRoot();
        if (path.empty()) {
            FailDvdRoot("No DVD root is configured");
        }
        if (!IsDvdDataRoot(path)) {
            FailDvdRoot("Configured DVD root is not an extracted DATA directory", path);
        }
        g_dvdRoot = path;
    });

    return g_dvdRoot;
}

static std::string NormalizePath(const std::string& path);
static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath);

[[maybe_unused]] static void InvokeDvdCallback(uint32_t callbackPtr, int32_t result, uint32_t fileInfoPtr) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    cpu.gpr[4] = fileInfoPtr;
    InvokeIndirectCpu(callbackPtr, &cpu);
}

// DVD*Async completions are queued and run from the scheduler's callback drain, as the DI interrupt
// does on hardware. Calling them inside DVDReadAsync re-entered SPM's chunked loaders (fontmgrInit ->
// DVDMgrRead: the callback issues the next chunk before the caller has advanced its offset) without end.
struct PendingDvdCallback { uint32_t callbackPtr; int32_t result; uint32_t commandBlock; };
static std::mutex g_pendingDvdCallbackMutex;
static std::deque<PendingDvdCallback> g_pendingDvdCallbacks;
static void QueueDvdCallback(uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) return;
    std::lock_guard<std::mutex> lock(g_pendingDvdCallbackMutex);
    g_pendingDvdCallbacks.push_back({callbackPtr, result, commandBlock});
}
// Save states wait for none to be queued (save_states.cpp).
size_t DvdPendingCallbackCount() {
    std::lock_guard<std::mutex> lock(g_pendingDvdCallbackMutex);
    return g_pendingDvdCallbacks.size();
}
bool DvdProcessPendingCallbacks(CpuContext* cpu) {
    static thread_local int depth = 0;
    if (depth != 0) return false;   // a callback that queues another read drains on the outer loop
    ++depth;
    bool any = false;
    for (;;) {
        PendingDvdCallback cb{};
        {
            std::lock_guard<std::mutex> lock(g_pendingDvdCallbackMutex);
            if (g_pendingDvdCallbacks.empty()) break;
            cb = g_pendingDvdCallbacks.front();
            g_pendingDvdCallbacks.pop_front();
        }
        any = true;
        if (!TranslatedFunctionRegistry::FindByAddressPtr(cb.callbackPtr)) continue;
        CpuContext callbackCpu = cpu ? *cpu : GetPersistentCpuContext();
        callbackCpu.gpr[3] = static_cast<uint32_t>(cb.result);
        callbackCpu.gpr[4] = cb.commandBlock;
        CpuContextScope scope(&callbackCpu);
        InvokeIndirectCpu(cb.callbackPtr, &callbackCpu);
    }
    --depth;
    return any;
}

// Same shape as InvokeDvdCallback minus the command block: the DVDLow callbacks
// take only a result, so r4 is deliberately left untouched here.
static void InvokeDvdLowCallback(uint32_t callbackPtr, int32_t result) {
    if (callbackPtr == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callbackPtr, &cpu);
}

static void ReportDvdReadError(const std::string& hostPath,
                               int64_t offset,
                               uint32_t length,
                               const char* reason) {
    const std::string key = hostPath + "\n" + std::to_string(offset);
    if (!g_loggedReadErrors.insert(key).second) {
        return;
    }

    RT_LOGF(RT_TAG_DVD,
                 "ERROR: DVD read failed: host=\"%s\" offset=%lld (0x%llx) "
                 "length=%u: %s\n",
                 hostPath.c_str(),
                 static_cast<long long>(offset),
                 static_cast<unsigned long long>(offset),
                 length,
                 reason ? reason : "unknown read error");
}

static int32_t DvdReadFatal(uint32_t fileInfoPtr,
                            const std::string& hostPath,
                            int64_t offset,
                            uint32_t length,
                            const char* reason) {
    ReportDvdReadError(hostPath, offset, length, reason);
    try {
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE,
                        static_cast<uint32_t>(DVD_STATE_FATAL_ERROR));
        Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, 0);
    } catch (const Memory::AccessViolation&) {
    }
    return kDvdFatalError;
}

static void LoadFstIndex() {
    if (g_fstLoaded) {
        return;
    }
    g_fstLoaded = true;

    const fs::path fstPath = GetDvdRoot() / "sys" / "fst.bin";
    std::ifstream fstFile(fstPath, std::ios::binary);
    if (!fstFile.is_open()) {
        return;
    }

    fstFile.seekg(0, std::ios::end);
    const std::streamsize size = fstFile.tellg();
    fstFile.seekg(0, std::ios::beg);
    if (size < 12) {
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    fstFile.read(reinterpret_cast<char*>(data.data()), size);
    if (!fstFile) {
        return;
    }

    const uint32_t entryCount = BigEndian::Read32(&data[8]);
    if (entryCount == 0 || entryCount > 0x10000) {
        return;
    }

    const size_t entriesSize = static_cast<size_t>(entryCount) * 12;
    if (entriesSize >= data.size()) {
        return;
    }

    const size_t stringBase = entriesSize;

    struct DirFrame {
        uint32_t endIndex;
        std::string path;
    };

    std::vector<DirFrame> stack;
    stack.push_back({entryCount, std::string()});

    g_fstFiles.clear();
    g_fstFiles.reserve(entryCount / 2);

    for (uint32_t i = 1; i < entryCount; ++i) {
        while (!stack.empty() && i >= stack.back().endIndex) {
            stack.pop_back();
        }
        if (stack.empty()) {
            break;
        }

        const size_t entryOff = static_cast<size_t>(i) * 12;
        const uint32_t nameWord = BigEndian::Read32(&data[entryOff]);
        const uint8_t type = static_cast<uint8_t>(nameWord >> 24);
        const uint32_t nameOffset = nameWord & 0x00FFFFFFu;
        if (stringBase + nameOffset >= data.size()) {
            break;
        }

        const char* namePtr = reinterpret_cast<const char*>(&data[stringBase + nameOffset]);
        std::string name(namePtr);

        if (type != 0) {
            const uint32_t nextIndex = BigEndian::Read32(&data[entryOff + 8]);
            std::string dirPath = stack.back().path;
            if (!dirPath.empty()) {
                dirPath += "/";
            }
            dirPath += name;
            stack.push_back({nextIndex, dirPath});
            continue;
        }

        const uint32_t fileOffset = BigEndian::Read32(&data[entryOff + 4]);
        const uint32_t fileSize = BigEndian::Read32(&data[entryOff + 8]);

        std::string relPath = stack.back().path;
        if (!relPath.empty()) {
            relPath += "/";
        }
        relPath += name;

        const uint64_t startBytes64 = static_cast<uint64_t>(fileOffset) * 4ull;
        if (startBytes64 > 0xFFFFFFFFu) {
            continue;
        }
        const uint32_t startBytes = static_cast<uint32_t>(startBytes64);
        const uint32_t endBytes = startBytes + fileSize;

        FstFileEntry entry;
        entry.start = startBytes;
        entry.end = endBytes;
        entry.size = fileSize;
        entry.dvdPath = "/" + relPath;
        const fs::path baseHostPath = GetDvdRoot() / "files" / fs::path(relPath);
        entry.hostPath = ResolveDvdMappedHostPath(entry.dvdPath, baseHostPath);
        g_fstFiles.push_back(std::move(entry));
    }

    std::sort(g_fstFiles.begin(), g_fstFiles.end(),
              [](const FstFileEntry& a, const FstFileEntry& b) { return a.start < b.start; });

}

static const PublishedExtent* FindPublishedExtentForByteOffset(uint64_t offset) {
    if (g_publishedExtents.empty()) {
        return nullptr;
    }

    auto it = std::upper_bound(
        g_publishedExtents.begin(), g_publishedExtents.end(), offset,
        [](uint64_t value, const PublishedExtent& extent) { return value < extent.startBytes; });
    if (it == g_publishedExtents.begin()) {
        return nullptr;
    }
    --it;
    if (offset >= it->startBytes && offset < it->endBytes) {
        return &(*it);
    }
    return nullptr;
}

struct AbsReadResult {
    const DVDFileEntry* entry = nullptr;
    uint32_t fileOffset = 0;
    uint32_t readLength = 0;
};

static bool ResolveAbsRead(uint32_t offset, uint32_t length, AbsReadResult& out) {
    // The extent table is available after DVDInit publishes the FST.
    DVDInit_8015EA1C();
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(offset);
    uint64_t effectiveOffset = offset;

    if (!extent) {
        // Accept both byte offsets and the FST's four-byte word offsets.
        const uint64_t scaledOffset = static_cast<uint64_t>(offset) * 4ull;
        extent = FindPublishedExtentForByteOffset(scaledOffset);
        if (extent) {
            effectiveOffset = scaledOffset;
        }
    }

    if (!extent) {
        return false;
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];
    const uint32_t fileOffset = static_cast<uint32_t>(effectiveOffset - extent->startBytes);
    uint32_t readLength = length;
    if (readLength == 0 || fileOffset + readLength > entry.size) {
        readLength = entry.size - fileOffset;
    }

    out.entry = &entry;
    out.fileOffset = fileOffset;
    out.readLength = readLength;
    return true;
}

// Helper: Normalize paths (Windows backslash -> forward slash, lowercase for lookup)
static std::string NormalizePath(const std::string& path) {
    return DvdFstContract::NormalizeLookupPath(path);
}

static void RegisterFileEntry(std::string dvdPath, const fs::path& hostPath, uint32_t size) {
    dvdPath = DvdFstContract::CanonicalizePath(dvdPath);

    DVDFileEntry fileEntry;
    fileEntry.hostPath = hostPath;
    fileEntry.dvdPath = dvdPath;
    fileEntry.size = size;

    const int32_t entryNum = static_cast<int32_t>(g_fileEntries.size());
    g_fileEntries.push_back(std::move(fileEntry));
    g_pathToEntry[NormalizePath(dvdPath)] = entryNum;
}

// True when a disc path already has an entry (vanilla disc or an earlier
// overlay registration). Uses the same canonicalization as RegisterFileEntry.
static bool DvdEntryExists(const std::string& dvdPath) {
    return g_pathToEntry.find(NormalizePath(DvdFstContract::CanonicalizePath(dvdPath))) !=
           g_pathToEntry.end();
}

// Walk `root` and hand every entry to `visit`. Uses the error_code iterator forms so an
// unreadable entry doesn't abort the rest of the overlay. `announceErrors` keeps the
// disc-index scan noisy and the by-name mapping quiet, matching each caller's old behavior.
template <typename Visit>
static void WalkDirectory(const fs::path& root, bool recursive, bool announceErrors, Visit&& visit) {
    std::error_code ec;

    if (!recursive) {
        for (const auto& entry : fs::directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            visit(entry);
        }
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (announceErrors) {
            RT_LOG(RT_TAG_DVD) << "WARNING: cannot enumerate " << HostPathText(root) << ": "
                      << ec.message() << std::endl;
        }
        return;
    }

    const fs::recursive_directory_iterator end;
    while (it != end) {
        const fs::directory_entry entry = *it;

        it.increment(ec);
        if (ec) {
            if (announceErrors) {
                RT_LOG(RT_TAG_DVD) << "WARNING: stopped enumerating " << HostPathText(root) << ": "
                          << ec.message() << std::endl;
                return;
            }
            visit(entry);
            return;
        }

        visit(entry);
    }
}

// Helper: Scan directory and build file index.
// addNewFiles=false is Riivolution's create="false": only files already on
// the disc may be replaced, nothing new is added.
static void ScanDirectory(const fs::path& root, const std::string& virtualPrefix,
                          bool recursive = true, bool addNewFiles = true) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }

    const auto registerEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }

        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            RT_LOG(RT_TAG_DVD) << "WARNING: skipping " << HostPathText(entry.path()) << ": "
                      << entryEc.message() << std::endl;
            return;
        }

        const fs::path relative = fs::relative(entry.path(), root, entryEc);
        if (entryEc) {
            return;
        }

        std::string prefix = virtualPrefix.empty() ? "/" : virtualPrefix;
        if (prefix.back() != '/' && prefix.back() != '\\') {
            prefix += "/";
        }
        std::string dvdPath = prefix + HostPathText(relative);
        if (!addNewFiles && !DvdEntryExists(dvdPath)) {
            return;
        }
        RegisterFileEntry(std::move(dvdPath), entry.path(), static_cast<uint32_t>(size));
    };

    WalkDirectory(root, recursive, /*announceErrors=*/true, registerEntry);
}

// Riivolution <folder> without a disc path: replace disc files that share a
// filename with a file in the external folder (Dolphin's
// FindFilenameNodeInFST behavior, applied across the whole index).
static void ApplyFolderByNameMapping(const RuntimeRiivolution::Mapping& mapping) {
    // Snapshot the current index: matches by filename may only target entries
    // that exist before this mapping, and RegisterFileEntry appends.
    std::map<std::string, std::vector<std::string>> discPathsByName;
    const size_t entryCount = g_fileEntries.size();
    for (size_t i = 0; i < entryCount; ++i) {
        const std::string& dvdPath = g_fileEntries[i].dvdPath;
        const size_t lastSlash = dvdPath.rfind('/');
        std::string name = lastSlash == std::string::npos ? dvdPath : dvdPath.substr(lastSlash + 1);
        RuntimeHle::LowerInPlace(name);
        if (!name.empty()) {
            discPathsByName[name].push_back(dvdPath);
        }
    }

    uint32_t replaced = 0;
    const auto applyEntry = [&](const fs::directory_entry& entry) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            return;
        }
        std::string name = HostPathText(entry.path().filename());
        RuntimeHle::LowerInPlace(name);
        const auto matches = discPathsByName.find(name);
        if (matches == discPathsByName.end()) {
            return;
        }
        const std::uintmax_t size = fs::file_size(entry.path(), entryEc);
        if (entryEc) {
            return;
        }
        for (const std::string& dvdPath : matches->second) {
            RegisterFileEntry(dvdPath, entry.path(), static_cast<uint32_t>(size));
            ++replaced;
        }
    };

    WalkDirectory(mapping.hostPath, mapping.recursive, /*announceErrors=*/false, applyEntry);

    RT_LOG(RT_TAG_DVD) << HostPathText(mapping.hostPath) << ": replaced " << replaced
              << " disc file(s) by filename" << std::endl;
}

static void ScanOverlayRoot(const RuntimeRiivolution::Overlay& overlay) {
    if (!overlay.patches) {
        // Fallback for mod roots that mirror the disc filesystem directly (not a
        // Riivolution pack, which wouldn't map anything useful this way).
        RT_LOG(RT_TAG_DVD) << HostPathText(overlay.root)
                  << ": no Riivolution XML found, treating the root as a disc-shaped overlay"
                  << std::endl;
        ScanDirectory(overlay.root, "/");
        return;
    }

    for (const auto& mapping : overlay.patches->mappings) {
        switch (mapping.kind) {
        case RuntimeRiivolution::Mapping::Kind::File: {
            if (!mapping.create && !DvdEntryExists(mapping.discPath)) {
                break;
            }
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(mapping.hostPath, ec);
            if (!ec) {
                RegisterFileEntry(mapping.discPath, mapping.hostPath, static_cast<uint32_t>(size));
            }
            break;
        }
        case RuntimeRiivolution::Mapping::Kind::Folder:
            ScanDirectory(mapping.hostPath, mapping.discPath, mapping.recursive, mapping.create);
            break;
        case RuntimeRiivolution::Mapping::Kind::FolderByName:
            ApplyFolderByNameMapping(mapping);
            break;
        }
    }
}

static fs::path ResolveDvdMappedHostPath(const std::string& dvdPath, const fs::path& fallbackHostPath) {
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it != g_pathToEntry.end() && it->second >= 0 &&
        it->second < static_cast<int32_t>(g_fileEntries.size())) {
        return g_fileEntries[it->second].hostPath;
    }

    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        const fs::path candidate = overlay.root / fs::path(normalized.substr(1));
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    return fallbackHostPath;
}

[[noreturn]] static void FailRuntimeFst(const std::string& reason) {
    RT_LOGF(RT_TAG_DVD, "ERROR: cannot publish the runtime FST: %s\n", reason.c_str());
    FailDvd("dvd_fst", "DVD file table is unavailable", reason);
}

static void BuildAndPublishRuntimeFst() {
    // Preserve real disc offsets where a runtime entry replaces a file from the
    // extracted image. Created Riivolution paths have no physical disc extent;
    // they are opened by their shared FST/SDK entry number instead.
    for (const FstFileEntry& fstFile : g_fstFiles) {
        const auto mapped = g_pathToEntry.find(NormalizePath(fstFile.dvdPath));
        if (mapped == g_pathToEntry.end() || mapped->second < 0 ||
            mapped->second >= static_cast<int32_t>(g_fileEntries.size())) {
            continue;
        }
        g_fileEntries[mapped->second].discOffsetWords = fstFile.start / 4u;
    }

    // Give runtime-added files unique synthetic disc ranges so reads can find them.
    uint64_t nextFreeBytes = 0;
    for (const FstFileEntry& fstFile : g_fstFiles) {
        nextFreeBytes = std::max(nextFreeBytes, static_cast<uint64_t>(fstFile.end));
    }
    for (const DVDFileEntry& entry : g_fileEntries) {
        if (entry.discOffsetWords != 0) {
            nextFreeBytes = std::max(
                nextFreeBytes, static_cast<uint64_t>(entry.discOffsetWords) * UINT64_C(4) +
                                   static_cast<uint64_t>(entry.size));
        }
    }
    for (DVDFileEntry& entry : g_fileEntries) {
        if (entry.isDirectory || entry.discOffsetWords != 0) {
            continue;
        }
        nextFreeBytes = (nextFreeBytes + 31ull) & ~31ull;
        if (nextFreeBytes / 4ull > 0xFFFFFFFFull) {
            FailRuntimeFst("synthetic disc extents exceed the 32-bit FST word offset range");
        }
        entry.discOffsetWords = static_cast<uint32_t>(nextFreeBytes / 4ull);
        nextFreeBytes += std::max<uint32_t>(entry.size, 4u);
    }

    std::vector<DvdFstContract::RegisteredFile> registrations;
    registrations.reserve(g_fileEntries.size());
    for (const DVDFileEntry& entry : g_fileEntries) {
        registrations.push_back({entry.hostPath, entry.dvdPath, entry.size, entry.discOffsetWords});
    }

    DvdFstContract::Image image;
    try {
        image = DvdFstContract::BuildImage(registrations);
    } catch (const std::exception& error) {
        FailRuntimeFst(error.what());
    }

    std::vector<DVDFileEntry> indexedEntries;
    indexedEntries.reserve(image.entries.size());
    for (const DvdFstContract::IndexedEntry& entry : image.entries) {
        indexedEntries.push_back({entry.hostPath, entry.dvdPath, entry.size,
                                  entry.discOffsetWords, entry.isDirectory});
    }
    g_fileEntries = std::move(indexedEntries);
    g_pathToEntry = std::move(image.pathToEntry);

    g_publishedExtents.clear();
    g_publishedExtents.reserve(g_fileEntries.size());
    for (size_t i = 0; i < g_fileEntries.size(); ++i) {
        const DVDFileEntry& entry = g_fileEntries[i];
        if (entry.isDirectory || entry.discOffsetWords == 0) {
            continue;
        }
        const uint64_t startBytes = static_cast<uint64_t>(entry.discOffsetWords) * 4ull;
        // Give empty files a one-byte range so their handles still resolve.
        const uint64_t endBytes = startBytes + std::max<uint32_t>(entry.size, 1u);
        g_publishedExtents.push_back({startBytes, endBytes, static_cast<int32_t>(i)});
    }
    std::sort(g_publishedExtents.begin(), g_publishedExtents.end(),
              [](const PublishedExtent& a, const PublishedExtent& b) {
                  return a.startBytes < b.startBytes;
              });

    // Use the FST memory reserved before guest code can allocate over it.
    if (g_dvdFstReservedBase == 0 || image.bytes.size() > g_dvdFstReservedSize ||
        !Memory::Contains(g_dvdFstReservedBase, g_dvdFstReservedSize)) {
        std::ostringstream reason;
        reason << "The " << image.bytes.size() << "-byte runtime FST does not fit the "
               << g_dvdFstReservedSize << "-byte guest reservation at 0x" << std::hex
               << g_dvdFstReservedBase << ".";
        FailRuntimeFst(reason.str());
    }

    // Where the console's apploader puts it, the top of MEM1 just above the game's arena (mods and game code find it
    // there); only an FST grown past that room (files a mod adds) uses the MEM2 reservation.
    const uint32_t mem1Fst = (0x81800000u - static_cast<uint32_t>(image.bytes.size())) & ~31u;
    const uint32_t fstAddress = mem1Fst >= Memory::Read32(0x80003110u) ? mem1Fst : g_dvdFstReservedBase;
    for (size_t i = 0; i < image.bytes.size(); ++i) {
        Memory::Write8(fstAddress + static_cast<uint32_t>(i), image.bytes[i]);
    }

    Memory::Write32(0x80000038u, fstAddress);
    Memory::Write32(0x8000003Cu, static_cast<uint32_t>(image.bytes.size()));

    RT_LOG(RT_TAG_DVD) << "published " << image.entries.size() << " FST entries ("
              << image.bytes.size() << " bytes) at 0x" << std::hex << fstAddress
              << std::dec << std::endl;

    if (const char* dumpPath = std::getenv("MKW_DUMP_FST")) {
        std::ofstream dump(dumpPath, std::ios::binary);
        dump.write(reinterpret_cast<const char*>(image.bytes.data()),
                   static_cast<std::streamsize>(image.bytes.size()));
        RT_LOG(RT_TAG_DVD) << "dumped published FST to " << dumpPath << std::endl;
    }
}

extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath)
{
    static std::string resolved;
    resolved.clear();

    if (!dvdPath || dvdPath[0] == '\0') {
        return nullptr;
    }

    DVDInit_8015EA1C();
    const std::string normalized = NormalizePath(dvdPath);
    const auto it = g_pathToEntry.find(normalized);
    if (it == g_pathToEntry.end() ||
        it->second < 0 ||
        it->second >= static_cast<int32_t>(g_fileEntries.size()) ||
        g_fileEntries[it->second].isDirectory) {
        return nullptr;
    }

    resolved = HostPathText(g_fileEntries[it->second].hostPath);
    return resolved.c_str();
}

// ============================================================================
// High-Level DVD API
// ============================================================================

static void InitDvdWaitingQueues()
{
    constexpr uint32_t kQueueBase = 0x804E43F0;  // WaitingQueue[4] (NSMBW)
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t queue = kQueueBase + (i * 8);
        Memory::Write32(queue + 0, queue);
        Memory::Write32(queue + 4, queue);
    }
}

static void CompleteDvdCancelState()
{
    InitDvdWaitingQueues();

    // These are the SDK DVD globals used by DVDCancelAll/__DVDPrepareReset.
    // The actual drive work is HLE'd, so complete pending cancel/reset waits.
    Memory::Write32(0x805A0910, 0); // Canceling (NSMBW)
    Memory::Write32(0x805A0914, 0); // ResumeFromHere (NSMBW)
    Memory::Write32(0x805A0908, 0); // PausingFlag (NSMBW)
    // (MKW also set CancelAllSyncComplete; NSMBW does not link DVDCancelAll, so it has no such flag.)
    Memory::Write32(0x805A0948, 1); // Prepared: __DVDPrepareReset polls it (NSMBW)
    Memory::Write32(0x805A0990, 0); // executing (NSMBW)
}

// 0x801CAD30 -> DVDInit
extern "C" void DVDInit_8015EA1C()
{
    if (g_dvdInitialized) return;
    // The scan below builds g_fileEntries incrementally, so a re-entrant call
    // must not start a second scan on top of a half-built index and duplicate
    // every entry.
    static bool initializing = false;
    if (initializing) return;
    initializing = true;


    // 1. Initialize Global Flags (Emulate OS state)
    // These addresses are standard OS globals for DVD context
    Memory::Write8(0x805A09C4, 1);   // dvdContextsInited (NSMBW)
    Memory::Write8(0x805A09C5, 1);   // DVDLowInitCalled (NSMBW)
    Memory::Write32(0x805A09C0, 0);  // freeDvdContext (NSMBW)
    Memory::Write8(0x805A0940, 1);   // DVDInitialized (NSMBW)
    CompleteDvdCancelState();

    // 2. Initialize DVD Context structures (prevent crashes in callbacks)
    constexpr uint32_t kContextBase = 0x804E46A0;  // dvdContexts[4] (NSMBW)
    constexpr uint32_t kMagicValue = 0xFEEBDAED;
    for (int i = 0; i < 4; i++) {
        uint32_t ctx = kContextBase + i * 0x20;
        Memory::Write32(ctx + 0x0C, kMagicValue);
        Memory::Write32(ctx + 0x10, i);
    }

    // 3. Set Low Memory Globals (The "Magic" Identification)
    // This tells the game "Yes, I am Mario Kart Wii"
    // 4. Scan Files
    const fs::path& rootPath = GetDvdRoot();
    // The disc ID (game code, maker, disc number, revision, ...) from the extracted boot.bin: games compare it with
    // their own copy (Brawl's gfErrorCheck shows "Please insert the ... Game Disc." on any difference).
    uint32_t diskHeader = 0x80000000;
    if (std::ifstream boot(rootPath / "sys" / "boot.bin", std::ios::binary); boot) {
        char id[0x20] = {};
        if (boot.read(id, sizeof(id))) {
            for (uint32_t i = 0; i < sizeof(id); ++i) Memory::Write8(diskHeader + i, static_cast<uint8_t>(id[i]));
        }
    }
    Memory::Write32(diskHeader + 0x00, CurrentDiscGameCode());
    
    // Map "<dvd_root>/files" -> "/". The disc's sys files are not in its file system, so not in the FST either:
    // the FST stays the disc's own, at the console's address (BuildAndPublishRuntimeFst).
    ScanDirectory(rootPath / "files", "/");

    const auto& overlays = RuntimeRiivolution::Overlays();
    if (overlays.empty() && RuntimeProduct::IsRetroRewind()) {
        RT_LOG(RT_TAG_DVD) << "WARNING: no Retro Rewind overlay root was found. "
                     "File replacements (menu archives, karts, drivers, courses) will not apply "
                     "and the game will look and play like the unmodded disc. Set "
                     "[paths] retro_rewind_root in Config.toml "
                     "with the RetroRewind6 folder."
                  << std::endl;
    }
    // RegisterFileEntry lets the last registration win, so apply the roots in
    // reverse discovery order: the explicitly configured root outranks the mod
    // manifest.
    const size_t vanillaEntryCount = g_fileEntries.size();
    for (auto overlay = overlays.rbegin(); overlay != overlays.rend(); ++overlay) {
        ScanOverlayRoot(*overlay);
    }
    RT_LOG(RT_TAG_DVD) << "disc index: " << vanillaEntryCount << " disc file(s), "
              << (g_fileEntries.size() - vanillaEntryCount) << " overlay registration(s) from "
              << overlays.size() << " root(s)" << std::endl;

    StartReadCachePreload();

    // Load FST mapping so real files keep their physical disc extents.
    LoadFstIndex();
    BuildAndPublishRuntimeFst();

    // Initialize the translated DVD filesystem so it can use the published FST.
    constexpr uint32_t kDvdFsInitAddress = 0x801F5ED8;
    if (TranslatedFunctionRegistry::FindByAddressPtr(kDvdFsInitAddress)) {
        InvokeIndirectCpu(kDvdFsInitAddress, &GetPersistentCpuContext());
    }

    g_dvdInitialized = true;
    initializing = false;
}
PPC_NATIVE_OVERRIDE_VOID(801F6A90, DVDInit_8015EA1C, (), ());

// FST-only DVD functions run translated so mods can override them safely.

// 0x801CAB20 -> DVDReadPrio
extern "C" int32_t DVDReadPrio_8015E834(uint32_t fileInfoPtr, uint32_t bufferPtr, int32_t length, int32_t offset, int32_t prio)
{
    (void)prio;

    uint32_t startWords = 0;
    try {
        startWords = Memory::Read32(fileInfoPtr + DVD_FILEINFO_OFFSET_ADDR);
    } catch (const Memory::AccessViolation&) {
        return DvdReadFatal(fileInfoPtr, "<invalid DVD file info>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info is outside guest memory");
    }

    // Treat startAddr as an FST offset, including offsets inside a file.
    const uint64_t startBytes = static_cast<uint64_t>(startWords) * 4ull;
    const PublishedExtent* extent = FindPublishedExtentForByteOffset(startBytes);
    if (!extent) {
        return DvdReadFatal(fileInfoPtr, "<unresolved DVD file handle>", offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "DVD file info does not reference a published FST extent");
    }

    const DVDFileEntry& entry = g_fileEntries[extent->entryIndex];
    {  // SPM bring-up: which disc files the game reads (first chunk of each)
        static int n = 0;
        if (offset == 0 && n++ < 400) RT_LOGF(RT_TAG_RUNTIME, "[dvdread] %s len %d\n", HostPathText(entry.hostPath).c_str(), length);
    }

    if (offset < 0 || length < 0) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset,
                            length > 0 ? static_cast<uint32_t>(length) : 0,
                            "negative DVD read offset or length");
    }

    const uint64_t extentBias = startBytes - extent->startBytes;
    const uint64_t requestedOffset = extentBias + static_cast<uint32_t>(offset);
    uint32_t uLength = (uint32_t)length;

    if (requestedOffset >= entry.size) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "read offset is outside the indexed DVD file");
    }
    const uint32_t uOffset = static_cast<uint32_t>(requestedOffset);

    if (uLength > entry.size - uOffset) {
        uLength = entry.size - uOffset;
    }

    if (uLength != 0 && !Memory::Contains(bufferPtr, uLength)) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            "DVD read destination is outside guest memory");
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!ReadHostFile(entry.hostPath, uOffset, uLength, tempBuf, failure)) {
        return DvdReadFatal(fileInfoPtr, HostPathText(entry.hostPath), offset, uLength,
                            DvdReadContract::Describe(failure));
    }

    CopyToGuestAsDma(bufferPtr, tempBuf.data(), uLength);

    Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_TRANSFERRED, uLength);
    Memory::Write32(fileInfoPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    CompleteDvdCancelState();

    return static_cast<int32_t>(uLength);
}
PPC_NATIVE_OVERRIDE(801F67F0, DVDReadPrio_8015E834, int32_t, (uint32_t f, uint32_t b, int32_t l, int32_t o, int32_t p), (f, b, l, o, p));

// 0x801CAA30 -> DVDReadAsyncPrio (internal)
// HLE: perform synchronous read and invoke callback immediately.
extern "C" int32_t DVD__ReadAsyncPrio_HLE_8015e74c(uint32_t fileInfoPtr,
                                                   uint32_t bufferPtr,
                                                   int32_t length,
                                                   int32_t offset,
                                                   uint32_t callbackPtr,
                                                   int32_t prio)
{
    const int32_t bytesRead = DVDReadPrio_8015E834(fileInfoPtr, bufferPtr, length, offset, prio);

    QueueDvdCallback(callbackPtr, bytesRead, fileInfoPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801F6708, DVD__ReadAsyncPrio_HLE_8015e74c, int32_t,
         (uint32_t f, uint32_t b, int32_t l, int32_t o, uint32_t cb, int32_t p),
         (f, b, l, o, cb, p));

// 0x801CECC0 -> DVDReadAbsAsyncPrio (internal)
// HLE: resolve the absolute disc offset to a host file and read it.
extern "C" int32_t DVD__ReadAbsAsyncPrio_HLE_801628cc(uint32_t cmdBlockPtr,
                                                      uint32_t bufferPtr,
                                                      int32_t length,
                                                      int32_t offset,
                                                      uint32_t callbackPtr,
                                                      int32_t prio)
{
    (void)prio;
    int32_t bytesRead = -1;
    {
        AbsReadResult readInfo;
        const uint32_t requestedLength = length > 0 ? static_cast<uint32_t>(length) : 0;
        const uint32_t absoluteOffset = static_cast<uint32_t>(offset);
        if (length < 0) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset, 0,
                                     "negative absolute DVD read length");
        } else if (!ResolveAbsRead(absoluteOffset, requestedLength, readInfo)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, "<unmapped DVD offset>", absoluteOffset,
                                     requestedLength,
                                     "absolute DVD read offset is not mapped to a host file");
        } else if (requestedLength != 0 && readInfo.readLength != requestedLength) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "requested range extends beyond the indexed DVD file");
        } else if (requestedLength != 0 && !Memory::Contains(bufferPtr, requestedLength)) {
            bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                     readInfo.fileOffset, requestedLength,
                                     "DVD read destination is outside guest memory");
        } else {
            std::vector<uint8_t> tempBuf;
            DvdReadContract::HostReadFailure failure;
            if (!ReadHostFile(readInfo.entry->hostPath,
                                            readInfo.fileOffset,
                                            readInfo.readLength,
                                            tempBuf,
                                            failure)) {
                bytesRead = DvdReadFatal(cmdBlockPtr, HostPathText(readInfo.entry->hostPath),
                                         readInfo.fileOffset, readInfo.readLength,
                                         DvdReadContract::Describe(failure));
            } else {
                CopyToGuestAsDma(bufferPtr, tempBuf.data(), tempBuf.size());
                bytesRead = static_cast<int32_t>(tempBuf.size());
            }
        }

        if (bytesRead >= 0) {
            try {
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
                Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_TRANSFERRED,
                                static_cast<uint32_t>(bytesRead));
            } catch (const Memory::AccessViolation&) {
            }
        }
    }

    QueueDvdCallback(callbackPtr, bytesRead, cmdBlockPtr);
    CompleteDvdCancelState();
    return bytesRead >= 0 ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801FA940, DVD__ReadAbsAsyncPrio_HLE_801628cc, int32_t,
         (uint32_t cb, uint32_t b, int32_t l, int32_t o, uint32_t cbfn, int32_t p),
         (cb, b, l, o, cbfn, p));


// ============================================================================
// Low-Level / Core DVD (The "Magic" Handlers)
// ============================================================================

// 0x801D1230 -> DVDLowInit
extern "C" int32_t DVDLowInit_80164848() {
    // Just ensure init is done
    DVDInit_8015EA1C(); 
    return 1;
}
PPC_NATIVE_OVERRIDE(801FC938, DVDLowInit_80164848, int32_t, (), ());

extern "C" int32_t DVDLowInquiry_80165A30(uint32_t cmdBlockPtr, uint32_t callback)
{
    // HLE: acknowledge the drive is present. Must mark the command block complete
    // (State = 0) or callers polling this address hang waiting for "Busy" to clear.
    if (cmdBlockPtr) {
        Memory::Write32(cmdBlockPtr + DVD_CB_OFFSET_STATE, DVD_STATE_END);
    }
    CompleteDvdCancelState();

    return 1; // Return 1 to indicate the command was successfully issued.
}
PPC_NATIVE_OVERRIDE(801FDB20, DVDLowInquiry_80165A30, int32_t, (uint32_t b, uint32_t c), (b, c));

// 0x801D14A0 -> DVDLowReadDiskID
extern "C" int32_t DVDLowReadDiskID_80164AAC(uint32_t diskIdPtr, uint32_t callback) {
    if (diskIdPtr) {
        // The disc ID as the boot path left it at 0x80000000 (game code, maker, disc number, revision, ...):
        // games compare it with DVDGetCurrentDiskID (Brawl's gfErrorCheck shows "insert the disc" on a mismatch).
        for (uint32_t i = 0; i < 0x20u; i += 4) {
            Memory::Write32(diskIdPtr + i, Memory::Read32(0x80000000u + i));
        }
        Memory::Write32(diskIdPtr + 0x00, CurrentDiscGameCode());
    }
    CompleteDvdCancelState();
    return 1; // Success
}
PPC_NATIVE_OVERRIDE(801FCB9C, DVDLowReadDiskID_80164AAC, int32_t, (uint32_t p, uint32_t c), (p, c));

// 0x801D2F20 -> DVDLowRead (And 0x801D2130 UnencryptedRead)
// The game calls this to read the Disk Header (offset 0) or raw data.
extern "C" int32_t DVDLowRead_80166330(uint32_t buffer, uint32_t length, uint32_t offset, uint32_t callback)
{
    const auto finish = [callback](bool succeeded) {
        const auto completion = DvdReadContract::CompletionFor(succeeded);
        InvokeDvdLowCallback(callback, completion.callbackResult);
        CompleteDvdCancelState();
        return completion.returnValue;
    };

    if (length == 0) {
        return finish(true);
    }

    if (offset == 0 && length >= 0x20) {
        if (!Memory::Contains(buffer, length)) {
            ReportDvdReadError("<synthetic disc header>", offset, length,
                               "DVD read destination is outside guest memory");
            return finish(false);
        }
        Memory::Write32(buffer + 0x00, CurrentDiscGameCode());
        Memory::Write16(buffer + 0x04, 0x3031);     // 01
        GxNotifyGuestRamDmaWrite(buffer, length);
        return finish(true);
    }

    AbsReadResult readInfo;
    if (!ResolveAbsRead(offset, length, readInfo)) {
        ReportDvdReadError("<unmapped DVD offset>", offset, length,
                           "absolute DVD read offset is not mapped to a host file");
        return finish(false);
    }
    if (readInfo.readLength != length) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "requested range extends beyond the indexed DVD file");
        return finish(false);
    }
    if (!Memory::Contains(buffer, length)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset, length,
                           "DVD read destination is outside guest memory");
        return finish(false);
    }

    std::vector<uint8_t> tempBuf;
    DvdReadContract::HostReadFailure failure;
    if (!ReadHostFile(readInfo.entry->hostPath,
                                    readInfo.fileOffset,
                                    readInfo.readLength,
                                    tempBuf,
                                    failure)) {
        ReportDvdReadError(HostPathText(readInfo.entry->hostPath), readInfo.fileOffset,
                           readInfo.readLength, DvdReadContract::Describe(failure));
        return finish(false);
    }

    CopyToGuestAsDma(buffer, tempBuf.data(), tempBuf.size());
    return finish(true);
}
PPC_NATIVE_OVERRIDE(801FE420, DVDLowRead_80166330, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// UnencryptedRead has the same completion and failure contract as DVDLowRead.
extern "C" int32_t DVDLowUnencryptedRead_80165708(uint32_t b, uint32_t l, uint32_t o, uint32_t c) {
    return DVDLowRead_80166330(b, l, o, c);
}
PPC_NATIVE_OVERRIDE(801FD7F8, DVDLowUnencryptedRead_80165708, int32_t, (uint32_t b, uint32_t l, uint32_t o, uint32_t c), (b, l, o, c));

// ============================================================================
// Other Necessary Stubs
// ============================================================================

extern "C" int32_t DVDCheckDevice_801643FC() { return 1; } // Ready
#if 0  // SPM: __DVDCheckDevice: name not in SPM
#if 0  // BRAWL: no matching function in the Brawl dol
PPC_NATIVE_OVERRIDE(801D0980, DVDCheckDevice_801643FC, int32_t, (), ());
#endif
#endif

extern "C" int32_t DVDLowClearCoverInterrupt_80166964(uint32_t cb) { return 1; }
PPC_NATIVE_OVERRIDE(801FEA54, DVDLowClearCoverInterrupt_80166964, int32_t, (uint32_t cb), (cb));

// Brawl's error checker (gfErrorCheck) polls DVDGetDriveStatus every frame and shows "Please insert the ... Game
// Disc." unless the drive reports DVD_STATE_END (0). Reads complete synchronously here, so the drive is always idle
// with the disc in.
extern "C" int32_t DVDGetDriveStatus_HLE() { return 0; }
PPC_NATIVE_OVERRIDE(801FAC98, DVDGetDriveStatus_HLE, int32_t, (), ());

// DVDCompareDiskID as the SDK (dvdidutils.c): the game names are compared only when both start with a non-zero
// byte (Legacy XP zeroes the first byte of the ID at 0x80000000), the company must be set and equal, and 0xFF disk
// numbers and versions match any. Logs the first mismatch: Brawl shows "insert the disc" while they differ.
extern "C" int32_t DVDCompareDiskID_HLE(uint32_t id1, uint32_t id2) {
    const uint32_t name1 = Memory::Read32(id1), name2 = Memory::Read32(id2);
    const uint16_t maker1 = Memory::Read16(id1 + 4), maker2 = Memory::Read16(id2 + 4);
    const uint8_t disk1 = Memory::Read8(id1 + 6), disk2 = Memory::Read8(id2 + 6);
    const uint8_t ver1 = Memory::Read8(id1 + 7), ver2 = Memory::Read8(id2 + 7);
    // strncmp over 4 bytes stops at the first NUL: bytes after it do not count.
    const auto namesEqual = [](uint32_t a, uint32_t b) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            const uint8_t x = static_cast<uint8_t>(a >> shift), y = static_cast<uint8_t>(b >> shift);
            if (x != y) return false;
            if (x == 0) return true;
        }
        return true;
    };
    const bool nameOk = (name1 >> 24) == 0 || (name2 >> 24) == 0 || namesEqual(name1, name2);
    const bool makerOk = (maker1 >> 8) != 0 && (maker2 >> 8) != 0 && maker1 == maker2;
    const bool same = nameOk && makerOk && (disk1 == 0xFF || disk2 == 0xFF || disk1 == disk2) &&
                      (ver1 == 0xFF || ver2 == 0xFF || ver1 == ver2);
    static bool s_logged = false;
    if (!same && !s_logged) {
        s_logged = true;
        RT_LOGF(RT_TAG_DVD, "DVDCompareDiskID mismatch: 0x%08X %08X %04X %02X %02X vs 0x%08X %08X %04X %02X %02X\n",
                id1, name1, maker1, disk1, ver1, id2, name2, maker2, disk2, ver2);
    }
    return same ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(801FC228, DVDCompareDiskID_HLE, int32_t, (uint32_t a, uint32_t b), (a, b));

