// NSMBW_PROFILE=1: samples the game thread's instruction pointer every millisecond and logs the hottest engine
// addresses with each NSMBW_LOG_FPS window. Resolve them with llvm-symbolizer --obj=engine_usa.dll 0x<offset>.
#include "sampling_profiler.h"

#include "runtime_log.h"

#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

std::mutex g_mutex;
// Per thread (id), per address: module-relative offset inside the engine, else absolute | bit 63.
std::unordered_map<uint32_t, std::unordered_map<uint64_t, uint32_t>> g_hits;
uint32_t g_gameThread = 0;

bool Enabled() {
    static const bool on = std::getenv("NSMBW_PROFILE") != nullptr;
    return on;
}

}  // namespace

// Samples every thread of the process (the game thread, Aurora's submission and presentation workers, audio)
// every 2 ms, so a frame that waits on another thread shows where that thread spends its time.
void SamplingProfilerAttachCurrentThread() {
#ifdef _WIN32
    static bool started = false;
    if (!Enabled() || started) return;
    started = true;
    g_gameThread = GetCurrentThreadId();
    const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleA("engine_usa.dll"));
    std::thread([base] {
        const DWORD self = GetCurrentThreadId();
        std::unordered_map<DWORD, HANDLE> threads;
        for (uint32_t tick = 0;; ++tick) {
            if (tick % 250 == 0) {  // pick up new threads
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self ||
                        threads.count(te.th32ThreadID)) continue;
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                    if (h) threads[te.th32ThreadID] = h;
                }
                CloseHandle(snap);
            }
            Sleep(2);
            for (auto it = threads.begin(); it != threads.end();) {
                if (SuspendThread(it->second) == static_cast<DWORD>(-1)) {
                    CloseHandle(it->second);
                    it = threads.erase(it);
                    continue;
                }
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                const bool ok = GetThreadContext(it->second, &ctx) != 0;
                const auto inEngine = [base](uint64_t pc) { return pc >= base && pc - base < 0x40000000ull; };
                uint64_t key = 0;
                if (ok) {
                    const uint64_t rip = ctx.Rip;
                    key = inEngine(rip) ? rip - base : rip | (1ull << 63);
                    // A sample inside a system DLL (a wait, a lock) is charged to the engine function that called it,
                    // unwound while the thread is still suspended. Bit 62 marks those.
                    if (!inEngine(rip)) {
                        CONTEXT c = ctx;
                        int engineFrames = 0;
                        for (int depth = 0; depth < 24; ++depth) {
                            DWORD64 imageBase = 0;
                            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                            if (fn == nullptr) {
                                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                                c.Rsp += 8;
                            } else {
                                PVOID handlerData = nullptr;
                                DWORD64 establisher = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher,
                                                 nullptr);
                            }
                            if (c.Rip == 0) break;
                            // The first engine frame is usually an inlined std wait or lock helper; its caller says why.
                            if (inEngine(c.Rip) && ++engineFrames == 2) {
                                key = (c.Rip - base) | (1ull << 62);
                                break;
                            }
                        }
                    }
                }
                ResumeThread(it->second);
                if (ok) {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    ++g_hits[it->first][key];
                }
                ++it;
            }
        }
    }).detach();
#endif
}

void SamplingProfilerReport() {
    if (!Enabled()) return;
    std::unordered_map<uint32_t, std::unordered_map<uint64_t, uint32_t>> hits;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hits.swap(g_hits);
    }
    for (auto& [tid, map] : hits) {
        std::vector<std::pair<uint64_t, uint32_t>> top(map.begin(), map.end());
        uint32_t total = 0;
        for (const auto& [key, count] : top) total += count;
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string line;
        char item[48];
        for (size_t i = 0; i < top.size() && i < 400; ++i) {
            const uint64_t addr = top[i].first & ~(3ull << 62);
            std::snprintf(item, sizeof(item), "%s%llx", (top[i].first >> 62) == 1 ? "w" : "",
                          static_cast<unsigned long long>(addr));
            std::string where = item;
#ifdef _WIN32
            HMODULE module = nullptr;
            if ((top[i].first >> 63) &&  // outside the engine: name the module and the offset into it
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(addr), &module) && module != nullptr) {
                char path[MAX_PATH] = "?";
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* name = std::strrchr(path, '\\');
                std::snprintf(item, sizeof(item), "+%llx", static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(module)));
                where = std::string(name ? name + 1 : path) + item;
            }
#endif
            line += " " + where + ":" + std::to_string(top[i].second);
        }
        RT_LOGF(RT_TAG_VI, "profile thread %u%s %u samples:%s\n", tid, tid == g_gameThread ? " (game)" : "", total,
                line.c_str());
    }
}
