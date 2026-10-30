// save_states.cpp - see save_states.h.

#include "save_states.h"

#include "abi_bridge.h"
#include "det_clock.h"
#include "fiber_manager.h"
#include "generated/RuntimeConfig.h"
#include "guest_code_patch.h"
#include "hle/os/os_internal.h"
#include "host_context.h"
#include "memory.h"
#include "runtime_log.h"

#if __has_include(<zstd.h>)
#include <zstd.h>
#define SAVE_STATES_ZSTD 1
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

size_t DvdPendingCallbackCount();  // hle/storage/dvd.cpp

namespace OsHleInternal {
extern std::mutex gSleepTimerMutex;
extern std::vector<SleepTimerEntry> gSleepTimers;
extern std::mutex gOutstandingParkMutex;
extern std::unordered_set<uint32_t> gOutstandingParks;
} // namespace OsHleInternal

namespace SaveStates {
namespace {

constexpr uint32_t kMem1 = 0x80000000u, kMem1Size = 24u << 20;
constexpr uint32_t kMem2 = 0x90000000u, kMem2Size = 128u << 20;  // the modules' pre-linked homes live above 64 MB
constexpr size_t kPad = 64u << 10;    // see SwitchFiber
constexpr size_t kBelow = 512;        // bytes kept below a thread's resume point (its switch frame's spills)

// Where each fiber resumes: written by SwitchFiber, read back when the fiber runs again.
struct FiberSlot {
    void* jmp[5] = {};
    char* top = nullptr;
};
std::unordered_map<void*, FiberSlot> g_fibers;  // only touched on the game's host thread

struct StackCopy {
    void* fiber = nullptr;
    FiberSlot slot;
    char* from = nullptr;
    std::vector<uint8_t> bytes;
};

struct Snapshot {
    bool valid = false;
    std::atomic<bool> packing{false};  // MEM1/MEM2 being compressed on a worker
    std::vector<uint8_t> mem1, mem2;   // zstd frames (or raw without zstd)
    CpuContext cpu{};
    std::vector<std::pair<uint32_t, Fiber::GuestFiber>> threads;
    uint32_t currentThread = 0;
    std::vector<StackCopy> stacks;
    std::vector<OsHleInternal::SleepTimerEntry> sleepTimers;
    std::unordered_set<uint32_t> parks;
    std::string scene;
    std::chrono::steady_clock::time_point when;
};
Snapshot g_states[kSlots];
std::thread g_packer;

std::atomic<int> g_slot{0};
std::atomic<int> g_request{0};  // 1 save, 2 load
std::mutex g_messageMutex;
std::string g_message;

void* g_mainFiber = nullptr;
void* g_helper = nullptr;
int g_op = 0;
int g_opSlot = 0;
bool g_justLoaded = false;

void Say(std::string text) {
    std::lock_guard<std::mutex> lock(g_messageMutex);
    g_message = std::move(text);
}

std::vector<uint8_t> Pack(const uint8_t* data, size_t size) {
#if SAVE_STATES_ZSTD
    std::vector<uint8_t> out(ZSTD_compressBound(size));
    const size_t n = ZSTD_compress(out.data(), out.size(), data, size, 1);
    if (!ZSTD_isError(n)) {
        out.resize(n);
        out.shrink_to_fit();
        return out;
    }
#endif
    return {data, data + size};
}

void Unpack(const std::vector<uint8_t>& packed, uint8_t* out, size_t size) {
#if SAVE_STATES_ZSTD
    if (packed.size() != size) {
        ZSTD_decompress(out, size, packed.data(), packed.size());
        return;
    }
#endif
    std::memcpy(out, packed.data(), size);
}

// Runs on the helper fiber: every guest fiber (the main one included) is switched out here.
void TakeSnapshot(Snapshot& s) {
    if (g_packer.joinable()) g_packer.join();
    s.valid = false;
    s.cpu = GetPersistentCpuContext();
    s.threads = Fiber::GuestFiberManager::CopyFibers(s.currentThread);
    {
        std::lock_guard<std::mutex> lock(OsHleInternal::gSleepTimerMutex);
        s.sleepTimers = OsHleInternal::gSleepTimers;
    }
    {
        std::lock_guard<std::mutex> lock(OsHleInternal::gOutstandingParkMutex);
        s.parks = OsHleInternal::gOutstandingParks;
    }
    s.stacks.clear();
    for (const auto& [fiber, slot] : g_fibers) {
        if (slot.top == nullptr || slot.jmp[2] == nullptr) continue;  // never ran, or never switched out
        StackCopy copy;
        copy.fiber = fiber;
        copy.slot = slot;
        copy.from = static_cast<char*>(slot.jmp[2]) - kBelow;  // __builtin_setjmp keeps the stack pointer in [2]
        copy.bytes.assign(copy.from, slot.top);
        s.stacks.push_back(std::move(copy));
    }
    // Copy guest memory raw now (the game moves on), compress it on a worker.
    auto mem1 = std::make_shared<std::vector<uint8_t>>(Memory::GetPointer(kMem1, kMem1Size), Memory::GetPointer(kMem1, kMem1Size) + kMem1Size);
    auto mem2 = std::make_shared<std::vector<uint8_t>>(Memory::GetPointer(kMem2, kMem2Size), Memory::GetPointer(kMem2, kMem2Size) + kMem2Size);
    s.packing = true;
    g_packer = std::thread([&s, mem1, mem2] {
        s.mem1 = Pack(mem1->data(), mem1->size());
        s.mem2 = Pack(mem2->data(), mem2->size());
        s.packing = false;
    });
    s.scene = SceneName();
    s.when = std::chrono::steady_clock::now();
    s.valid = true;
}

void PutBack(const Snapshot& s) {
    if (g_packer.joinable()) g_packer.join();
    Unpack(s.mem1, Memory::GetPointer(kMem1, kMem1Size), kMem1Size);
    Unpack(s.mem2, Memory::GetPointer(kMem2, kMem2Size), kMem2Size);
    for (const StackCopy& copy : s.stacks) {
        std::memcpy(copy.from, copy.bytes.data(), copy.bytes.size());
        g_fibers[copy.fiber] = copy.slot;
    }
    GetPersistentCpuContext() = s.cpu;
    Fiber::GuestFiberManager::RestoreFibers(s.threads, s.currentThread);
    {
        std::lock_guard<std::mutex> lock(OsHleInternal::gSleepTimerMutex);
        OsHleInternal::gSleepTimers = s.sleepTimers;
    }
    {
        std::lock_guard<std::mutex> lock(OsHleInternal::gOutstandingParkMutex);
        OsHleInternal::gOutstandingParks = s.parks;
    }
    g_justLoaded = true;
}

void HelperMain(void*) {
    for (;;) {
        if (g_op == 1) TakeSnapshot(g_states[g_opSlot]);
        else if (g_op == 2) PutBack(g_states[g_opSlot]);
        g_op = 0;
        HostContext::Switch(g_mainFiber);
    }
}

// The same guest threads on the same fibers, each stack started where it was: only then do the saved stacks fit.
std::string LoadProblem(const Snapshot& s) {
    if (!s.valid) return "That slot is empty.";
    uint32_t current = 0;
    const auto threads = Fiber::GuestFiberManager::CopyFibers(current);
    const size_t live = std::count_if(g_fibers.begin(), g_fibers.end(), [](const auto& f) { return f.second.top && f.second.jmp[2]; });
    bool same = threads.size() == s.threads.size() && s.stacks.size() == live;
    for (const auto& [thread, fiber] : threads) {
        const auto it = std::find_if(s.threads.begin(), s.threads.end(), [&](const auto& t) { return t.first == thread; });
        same = same && it != s.threads.end() && it->second.fiber == fiber.fiber && it->second.terminated == fiber.terminated;
    }
    for (const StackCopy& copy : s.stacks) {
        const auto it = g_fibers.find(copy.fiber);
        same = same && it != g_fibers.end() && it->second.top == copy.slot.top;
    }
    return same ? "" : "This state is from another match or level. Load it from where you saved it.";
}

[[gnu::noinline]] void ResumeSaved(void* fiber) {
    FiberSlot& slot = g_fibers[fiber];
    if (slot.jmp[1] == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "[savestate] fiber %p resumed with no saved point (%zu fibers)\n", fiber, g_fibers.size());
        std::abort();
    }
    __builtin_longjmp(slot.jmp, 1);
}

} // namespace

int Slot() { return g_slot; }
void SetSlot(int slot) { g_slot = std::clamp(slot, 0, kSlots - 1); }
bool Exists(int slot) { return slot >= 0 && slot < kSlots && g_states[slot].valid; }
bool Available() { return !DetClock::Enabled(); }
void RequestSave() { if (Available()) g_request = 1; }
void RequestLoad() { if (Available()) g_request = 2; }

std::string Describe(int slot) {
    if (!Exists(slot)) return "Empty. States last until you close the game.";
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - g_states[slot].when).count();
    return "Saved " + (minutes < 1 ? std::string("just now") : std::to_string(minutes) + " min ago") +
           (g_states[slot].scene.empty() ? "" : " (" + g_states[slot].scene + ")") + ". States last until you close the game.";
}

std::string TakeMessage() {
    std::lock_guard<std::mutex> lock(g_messageMutex);
    return std::exchange(g_message, {});
}

void ForgetFiber(void* fiber) { g_fibers.erase(fiber); }

void MarkStackTop(void* top) { g_fibers[HostContext::Current()].top = static_cast<char*>(top); }

// The resume point goes into the fiber's slot, its registers onto its own stack (the compiler spills every callee-saved
// register around __builtin_setjmp). kPad keeps this switch's own frames below where a load writes the saved stack, as
// long as the thread is no more than kPad shallower than when it was saved; after the switch nothing in this frame is
// read again: ResumeSaved jumps through the slot, which a load has replaced.
[[gnu::noinline]] void SwitchBelow(void* target) {
    volatile char pad[kPad];  // a fixed frame: a dynamic alloca would need a base pointer longjmp doesn't restore
    pad[0] = 0;
    HostContext::Switch(target);
    ResumeSaved(HostContext::Current());
}

[[gnu::noinline]] void SwitchFiber(void* target) {
    void** jmp = g_fibers[HostContext::Current()].jmp;
    if (__builtin_setjmp(jmp) == 0) {
#if defined(__x86_64__) && defined(_WIN32)
        // Win64: the frame address __builtin_setjmp keeps is rbp minus the SEH frame offset, but the resume path reloads
        // xmm6-15 relative to rbp, so it read them from below this frame and every resumed thread lost them (the
        // x86-64-v2 build kept guest r14-r29 there across native OS calls). Resume with the real rbp.
        asm volatile("movq %%rbp, %0" : "=m"(jmp[0]));
#endif
        SwitchBelow(target);
    }
}

void FrameBoundary() {
    const int request = g_request.load();
    if (request == 0) return;
    void* self = HostContext::Current();
    if (g_mainFiber == nullptr) g_mainFiber = self;
    if (self != g_mainFiber || g_fibers[self].top == nullptr) return;  // not the main thread: next frame
    if (DvdPendingCallbackCount() != 0) return;                        // a disc read still finishing
    const int slot = g_slot;
    if (request == 2) {
        // The game starts short-lived threads (sound and file loading): wait up to 3 s for the saved set to come back.
        static int waited = 0;
        if (const std::string problem = LoadProblem(g_states[slot]); !problem.empty()) {
            if (g_states[slot].valid && ++waited < 180) return;
            waited = 0;
            g_request = 0;
            Say(problem);
            RT_LOGF(RT_TAG_RUNTIME, "[savestate] load refused: %s\n", problem.c_str());
            return;
        }
        waited = 0;
    }
    g_request = 0;
    if (g_helper == nullptr) g_helper = HostContext::Create(256u << 10, HelperMain, nullptr);
    g_op = request;
    g_opSlot = slot;
    SwitchFiber(g_helper);
    // Here after the save, and again (through the saved stack) after every load of it.
    if (g_justLoaded) {
        g_justLoaded = false;
        GuestCodePatch::Sync();
        Say("Loaded slot " + std::to_string(g_opSlot + 1));
        RT_LOGF(RT_TAG_RUNTIME, "[savestate] loaded slot %d\n", g_opSlot + 1);
    } else if (request == 1) {
        Say("Saved to slot " + std::to_string(slot + 1));
        RT_LOGF(RT_TAG_RUNTIME, "[savestate] saved slot %d (%zu threads, scene %s)\n", slot + 1, g_states[slot].stacks.size(),
                g_states[slot].scene.c_str());
    }
}

std::string SceneName() {
    // gfSceneManager::getInstance() is lwz r3, -0x43C0(r13); m_currentScene at +4, its name at +0.
    constexpr uint32_t kInstanceSlot = RuntimeConfig::SDA1_BASE - 0x43C0u;
    const uint32_t manager = Memory::Read32(kInstanceSlot);
    if (!Memory::Contains(manager, 8)) return {};
    const uint32_t scene = Memory::Read32(manager + 4);
    if (!Memory::Contains(scene, 4)) return {};
    const uint32_t name = Memory::Read32(scene);
    if (!Memory::Contains(name, 32)) return {};
    const char* text = reinterpret_cast<const char*>(Memory::GetPointer(name, 32));
    return std::string(text, strnlen(text, 32));
}

} // namespace SaveStates
