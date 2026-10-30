#include "timebase_contract.h"
#include "det_clock.h"
#include "memory.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "audio_backend.h"
#include "ax_dsp.h"
#include "music_attenuation.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>

namespace {
constexpr uint32_t kDefaultSampleRate = 32000u;
constexpr uint32_t kAudioChannels = 2u;
constexpr uint32_t kBytesPerSample = 2u;
constexpr uint32_t kAIInitializedAddr = 0x805A09E0u;          // __AI_init_flag (NSMBW)
constexpr uint32_t kAICallbackBusyAddr = 0x805A09E4u;         // __AID_Active (NSMBW)
constexpr uint32_t kAICallbackStackSwitchAddr = 0x805A0A14u;  // __CallbackStack (NSMBW)
constexpr uint32_t kAIDmaCallbackAddr = 0x805A0A18u;          // __AID_Callback (NSMBW)

// Max completed 3 ms DMA blocks delivered per tick. Draining several at once catches up
// backlog from a long frame without letting a large stall spiral into an unbounded loop.
constexpr int kMaxBlocksPerTick = 4;

struct AIDmaState {
    std::mutex mutex;
    uint32_t startAddr = 0;
    uint32_t registerStartAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    bool enabled = false;
    uint32_t sampleRate = kDefaultSampleRate;
    uint32_t bytesLeft = 0;
    double accumulatorSeconds = 0.0;
    // Deterministic mode: the virtual tick at which the DMA block in flight completes (0 = no
    // block started yet). Integer ticks, not the floating accumulator, so every device delivers each
    // block at exactly the same point.
    uint64_t detBlockDueTicks = 0;
    bool tickActive = false;
    bool loggedBackendFailure = false;
    bool loggedMissingCallback = false;
    bool loggedAccessFailure = false;
};

AIDmaState g_ai{};

// Audio degradation is invisible to the player except as silence, so every
// notice below reaches stderr unconditionally. The ones that sit on the
// per-DMA-frame path keep their one-shot latch in g_ai.
void ReportAudioProblem(const char* who, const char* what) {
    RT_LOGF(RT_TAG_AUDIO, "%s: %s\n", who, what);
    std::fflush(stderr);
}

bool EnsureAudioBackend(uint32_t sampleRate) {
    return AudioBackend::Instance().Init(sampleRate, kAudioChannels);
}

uint32_t EncodeAIDmaStartRegister(uint32_t startAddr) {
    return startAddr & 0x1fffffe0u;
}

uint32_t EncodeAIDmaLengthRegister(uint32_t length) {
    return length & 0x000fffe0u;
}

// NSMBW bring-up: the loudest sample of the AI output, reported every ~15 s of 32 kHz stereo for the
// first minute, so a game that runs silently shows up in the log of an automated run.
void MeterAudioBlock(const uint8_t* src, uint32_t bytes) {
    constexpr uint64_t kReportBytes = 32000ull * 2 * kBytesPerSample * 15;  // frames * channels
    static uint64_t s_bytes = 0;
    static int s_peak = 0;
    static int s_reports = 0;
    if (s_reports >= 4) {
        return;
    }
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        const int value = static_cast<int16_t>(static_cast<uint16_t>((src[i] << 8) | src[i + 1]));
        s_peak = std::max(s_peak, value < 0 ? -value : value);
    }
    s_bytes += bytes;
    if (s_bytes >= kReportBytes) {
        RT_LOGF(RT_TAG_AUDIO, "[meter] output peak %d of 32767 over the last ~15 s\n", s_peak);
        s_bytes = 0;
        s_peak = 0;
        ++s_reports;
    }
}

// WIICOMPILED_AUDIO_DUMP=<file>: the AI output as raw 32 kHz stereo 16-bit little-endian PCM (left, right), for
// checking what the game plays apart from the host audio device.
void DumpAudioBlock(const uint8_t* src, uint32_t bytes) {
    static FILE* file = [] {
        const char* path = std::getenv("WIICOMPILED_AUDIO_DUMP");
        return path != nullptr && *path != '\0' ? std::fopen(path, "wb") : nullptr;
    }();
    if (file == nullptr) {
        return;
    }
    std::vector<uint8_t> out(bytes & ~3u);
    for (uint32_t i = 0; i + 3 < bytes; i += 4) {  // frames arrive right, left, big-endian
        out[i] = src[i + 3];
        out[i + 1] = src[i + 2];
        out[i + 2] = src[i + 1];
        out[i + 3] = src[i];
    }
    std::fwrite(out.data(), 1, out.size(), file);
    std::fflush(file);
}

bool PushAudioBlock(uint32_t startAddr, uint32_t length) {
    if (startAddr == 0 || length == 0) {
        return false;
    }
    const uint32_t bytes = length;
    const uint8_t* src = nullptr;
    try {
        src = static_cast<const uint8_t*>(Memory::GetPointer(startAddr, bytes));
    } catch (const Memory::AccessViolation&) {
        src = nullptr;
    }

    if (src) {
        MeterAudioBlock(src, bytes);
        DumpAudioBlock(src, bytes);
        return AudioBackend::Instance().PushWiiAiSamplesBE16(src, bytes);
    }

    const uint32_t sampleCount = bytes / kBytesPerSample;
    if (sampleCount == 0) {
        return false;
    }
    std::vector<int16_t> samples(sampleCount);
    try {
        for (uint32_t i = 0; i < sampleCount; ++i) {
            const uint32_t addr = startAddr + i * kBytesPerSample;
            samples[i] = static_cast<int16_t>(Memory::Read16(addr));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    // Memory::Read16 has converted endianness, but the Wii AI frame order is
    // still right, left. Convert it to the host's left, right convention.
    for (uint32_t i = 0; i + 1 < sampleCount; i += 2) {
        std::swap(samples[i], samples[i + 1]);
    }
    return AudioBackend::Instance().PushSamplesLE16(samples.data(), samples.size());
}

} // namespace

extern "C" void AIClockInit_801A1138(uint32_t clock_mode)
{
    // clock_mode is unused: AID/DSP rate is controlled separately by AI state, and
    // treating it as a sample-rate switch would break Wii AX's normal 32 kHz cadence.
    (void)clock_mode;
    uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        rate = g_ai.sampleRate;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("__AIClockInit", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801D71F4, AIClockInit_801A1138, (uint32_t clock_mode), (clock_mode));

extern "C" void OSInitAudioSystem_801A1358()
{
    AIClockInit_801A1138(1);
    AxDspHle::InitAram();
    AxDspHle::Init();
    if (!EnsureAudioBackend(kDefaultSampleRate)) {
        ReportAudioProblem("__OSInitAudioSystem", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801D7414, OSInitAudioSystem_801A1358, (), ());

extern "C" void OSStopAudioSystem_801A1520()
{
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.enabled = false;
        g_ai.registerStartAddr = 0;
        g_ai.bytesLeft = 0;
        g_ai.accumulatorSeconds = 0.0;
        g_ai.detBlockDueTicks = 0;
    }
    AxDspHle::Stop();
}

PPC_NATIVE_OVERRIDE_VOID(801D75DC, OSStopAudioSystem_801A1520, (), ());



// Do NOT stub Audio__Manager__Init_80717150 / Audio__Manager__InitSelf_8071724c: they must
// run translated to init AudioHandleHolder::sInstance, or createSceneSoundManager NULL-vtable crashes.


extern "C" void AIInit_801240b0(uint32_t callback_stack_switch)
{
    const uint32_t rate = kDefaultSampleRate;
    uint32_t initialized = 0;
    const bool alreadyInitialized = Memory::TryRead32(kAIInitializedAddr, initialized) && initialized == 1u;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        if (!alreadyInitialized) {
            g_ai.callback = 0;
            g_ai.loggedMissingCallback = false;
            g_ai.loggedAccessFailure = false;
        }
    }
    if (!alreadyInitialized) {
        Memory::TryWrite32(kAIDmaCallbackAddr, 0);
        Memory::TryWrite32(kAICallbackBusyAddr, 0);
        Memory::TryWrite32(kAICallbackStackSwitchAddr, callback_stack_switch);
        Memory::TryWrite32(kAIInitializedAddr, 1);
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIInit", "audio backend init failed");
    } else {
        RT_LOG(RT_TAG_AUDIO) << "AIInit_801240b0 called: Audio subsystem initialized (HLE)" << std::endl;
    }
}

PPC_NATIVE_OVERRIDE_VOID(801fece8, AIInit_801240b0, (uint32_t callback_stack_switch), (callback_stack_switch));

extern "C" uint32_t AICheckInit_80124094()
{
    uint32_t initialized = 0;
    Memory::TryRead32(kAIInitializedAddr, initialized);
    return initialized;
}
#if 0  // SPM: AICheckInit: name not in SPM
#if 0  // BRAWL: no matching function in the Brawl dol
REGISTER_NATIVE_FUNCTION(0x8019F1E0, AICheckInit_80124094);
#endif
#endif



extern "C" void DSPInit_8015d444()
{
    AxDspHle::Init();
    RT_LOG(RT_TAG_AUDIO) << "DSPInit_8015d444 called: DSP hardware boundary initialized (HLE)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(80209864, DSPInit_8015d444, (), ());

extern "C" uint32_t DSPCheckInit_8015d504()
{
    return AxDspHle::CheckInit();
}
REGISTER_NATIVE_FUNCTION(0x80209924, DSPCheckInit_8015d504);

extern "C" uint32_t DSPAddTask_8015d50c(uint32_t task_ptr)
{
    return AxDspHle::AddTask(task_ptr);
}
REGISTER_NATIVE_FUNCTION(0x8020992C, DSPAddTask_8015d50c);

extern "C" void __DSP_boot_task_8015dc60(uint32_t task_ptr)
{
    AxDspHle::AssertTask(task_ptr);
    RT_LOG(RT_TAG_AUDIO) << "__DSP_boot_task called: booted DSP task at 0x"
              << std::hex << task_ptr << std::dec << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(8020A0C0, __DSP_boot_task_8015dc60, (uint32_t task_ptr), (task_ptr));


extern "C" void __AXOutInitDSP_801269bc(CpuContext* ctx)
{
    AxDspHle::InitForAXOut(ctx);
    RT_LOG(RT_TAG_AUDIO) << "__AXOutInitDSP called: native AX/DSP HLE initialized." << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(80200d90, __AXOutInitDSP_801269bc, (CpuContext* ctx), (ctx));



extern "C" void AIInitDMA_80123fcc(uint32_t start_addr, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    g_ai.startAddr = start_addr;
    g_ai.registerStartAddr = EncodeAIDmaStartRegister(start_addr);
    g_ai.length = EncodeAIDmaLengthRegister(length);
    g_ai.bytesLeft = g_ai.length;
}

PPC_NATIVE_OVERRIDE_VOID(801fec04, AIInitDMA_80123fcc, (uint32_t start_addr, uint32_t length), (start_addr, length));



// AIRegisterDMACallback stores the callback in the guest global __AID_Callback (0x8056DE08 in NSMBW).
// Returns the old callback pointer.
extern "C" uint32_t AIRegisterDMACallback_80123f88(uint32_t callback)
{
    uint32_t old_callback = 0;
    Memory::TryRead32(kAIDmaCallbackAddr, old_callback);
    Memory::TryWrite32(kAIDmaCallbackAddr, callback);
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.callback = callback;
        g_ai.loggedMissingCallback = false;
    }
    return old_callback;
}
PPC_NATIVE_OVERRIDE(801febc0, AIRegisterDMACallback_80123f88, uint32_t, (uint32_t callback), (callback));

// AIStartDMA toggles the AI DMA control register on hardware. Keep the guest-visible
// DMA state here and let the VI tick advance the hardware boundary.
extern "C" void AIStartDMA_80124048()
{
    const uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        g_ai.enabled = true;
        g_ai.bytesLeft = g_ai.length;
        g_ai.detBlockDueTicks = 0;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIStartDMA", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801FEC80, AIStartDMA_80124048, (), ());

extern "C" uint32_t AIGetDMABytesLeft_8012405c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.bytesLeft;
}
PPC_NATIVE_OVERRIDE(801FECA8, AIGetDMABytesLeft_8012405c, uint32_t, (), ());

extern "C" uint32_t AIGetDMAStartAddr_8012406c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.registerStartAddr;
}
#if 0  // SPM: AIGetDMAStartAddr: name not in SPM
#if 0  // BRAWL: no matching function in the Brawl dol
PPC_NATIVE_OVERRIDE(8019F1B0, AIGetDMAStartAddr_8012406c, uint32_t, (), ());
#endif
#endif

extern "C" uint32_t AIGetDMALength_80124084()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.length;
}
#if 0  // SPM: AIGetDMALength: name not in SPM
#if 0  // BRAWL: no matching function in the Brawl dol
PPC_NATIVE_OVERRIDE(8019F1D0, AIGetDMALength_80124084, uint32_t, (), ());
#endif
#endif

extern "C" uint32_t AIGetDSPSampleRate_8012409c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    // SDK AIGetDSPSampleRate returns AIDFR^1: 0 for 32 kHz, 1 for 48 kHz.
    return (g_ai.sampleRate == 48000u) ? 1u : 0u;
}
#if 0  // NSMBW: AIGetDSPSampleRate is not linked into NSMBW (was MKW-only)
#if 0  // SPM: 0x8012409c: no NSMBW symbol at this address
#if 0  // BRAWL: no matching function in the Brawl dol
PPC_NATIVE_OVERRIDE(8012409C, AIGetDSPSampleRate_8012409c, uint32_t, (), ());
#endif
#endif
#endif

extern "C" void DSPSendMailToDSP_8015d430(uint32_t mail)
{
    AxDspHle::SendMailToDSP(mail);
}
PPC_NATIVE_OVERRIDE_VOID(80209850, DSPSendMailToDSP_8015d430, (uint32_t mail), (mail));

extern "C" void SoundPlayerSetVolume_800a35e0(uint32_t soundPlayer, float volume)
{
    MusicAttenuation::SetSoundPlayerVolume(soundPlayer, volume);
}

#if 0  // NSMBW: music ducking hooks MKW nw4r::snd::SoundPlayer::SetVolume via an MKW sound-manager pointer; not ported to NSMBW yet
#if 0  // SPM: 0x800a35e0: no NSMBW symbol at this address
#if 0  // BRAWL: no matching function in the Brawl dol
PPC_NATIVE_OVERRIDE_VOID(800A35E0, SoundPlayerSetVolume_800a35e0,
              (uint32_t soundPlayer, float volume), (soundPlayer, volume));
#endif
#endif
#endif

extern "C" uint32_t DSPCheckMailToDSP_8015d3fc()
{
    return AxDspHle::CheckMailToDSP();
}
PPC_NATIVE_OVERRIDE(8020981C, DSPCheckMailToDSP_8015d3fc, uint32_t, (), ());

extern "C" uint32_t DSPCheckMailFromDSP_8015d40c()
{
    return AxDspHle::CheckMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x8020982C, DSPCheckMailFromDSP_8015d40c);

extern "C" uint32_t DSPReadMailFromDSP_8015d41c()
{
    return AxDspHle::ReadMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x8020983C, DSPReadMailFromDSP_8015d41c);

extern "C" uint32_t DSPAssertTask_8015d57c(uint32_t taskPtr)
{
    return AxDspHle::AssertTask(taskPtr);
}
PPC_NATIVE_OVERRIDE(802099DC, DSPAssertTask_8015d57c, uint32_t, (uint32_t taskPtr), (taskPtr));

// Each delivered block runs the AI DMA callback and deferred AX task callbacks before the
// next block, preserving the SoundThread/DSP interleave order real hardware provides.
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros)
{
    uint32_t startAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t sampleRate = kDefaultSampleRate;
    bool enabled = false;

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        enabled = g_ai.enabled;
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;

        if (enabled && startAddr != 0 && length != 0 && sampleRate != 0) {
            // SoundThread can hit the idle scheduler before the outer AXOut frame finishes;
            // retain elapsed time here rather than recursively entering the singleton AI/AX device.
            g_ai.accumulatorSeconds += static_cast<double>(deltaMicros) / 1'000'000.0;
            if (g_ai.tickActive) {
                return;
            }
            g_ai.tickActive = true;
        }
    }

    if (!enabled || startAddr == 0 || length == 0 || sampleRate == 0) {
        return;
    }

    // Resets tickActive on early return; the normal exit path disarms this and clears the
    // flag itself while already holding the mutex, avoiding a redundant lock acquisition.
    struct ActiveTickReset {
        bool armed = true;
        ~ActiveTickReset()
        {
            if (!armed) {
                return;
            }
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } activeTickReset;

    const double bytesPerSecond = static_cast<double>(sampleRate) * kAudioChannels * kBytesPerSample;
    const double blockDuration = static_cast<double>(length) / bytesPerSecond;
    if (blockDuration <= 0.0) {
        return;
    }

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);

    int blocksCompleted = 0;
    while (true) {
        // Claim the block in one critical section; sample-then-consume separately gains
        // nothing since the callback below (the only reentrancy point) runs mutex-released.
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (g_ai.accumulatorSeconds < blockDuration) {
                break;
            }
            g_ai.accumulatorSeconds -= blockDuration;
            g_ai.bytesLeft = 0;
        }

        // Everything below reads what the AX mix wrote (PushAudioBlock) or runs guest code
        // that reads its PB write-back and aux buffers (__AXOutNewFrame via the AI DMA
        // callback), so the mix worker must finish first; the join also publishes its
        // aux-out shadow.
        AxDspHle::JoinMixWorker();

        if (!EnsureAudioBackend(sampleRate)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedBackendFailure) {
                g_ai.loggedBackendFailure = true;
                ReportAudioProblem("Audio", "audio backend unavailable; dropping samples");
            }
        } else {
            const bool pushed = PushAudioBlock(startAddr, length);
            if (!pushed) {
                std::lock_guard<std::mutex> lock(g_ai.mutex);
                if (!g_ai.loggedAccessFailure) {
                    g_ai.loggedAccessFailure = true;
                    ReportAudioProblem("Audio", "failed to read DMA buffer; disabling audio DMA");
                }
                g_ai.enabled = false;
                return;
            }
        }

        if (callback != 0) {
            // Pointer lookup avoids copying the registry record per audio block.
            const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(callback);
            if (!info || !info->rawCpuInvoker) {
                std::lock_guard<std::mutex> lock(g_ai.mutex);
                if (!g_ai.loggedMissingCallback) {
                    g_ai.loggedMissingCallback = true;
                    ReportAudioProblem("Audio", "AI DMA callback not registered; skipping");
                }
            } else {
                Memory::TryWrite32(kAICallbackBusyAddr, 1);
                InvokeIndirectCpu(callback, cpu);
                Memory::TryWrite32(kAICallbackBusyAddr, 0);
                AxDspHle::ServiceDeferredCallbacks();
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            startAddr = g_ai.startAddr;
            length = g_ai.length;
            callback = g_ai.callback;
            sampleRate = g_ai.sampleRate;
            g_ai.bytesLeft = g_ai.length;
        }

        if (++blocksCompleted >= kMaxBlocksPerTick) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (g_ai.length != 0) {
            const double bytesRemaining = g_ai.length * (g_ai.accumulatorSeconds / blockDuration);
            if (bytesRemaining < static_cast<double>(g_ai.length)) {
                g_ai.bytesLeft = g_ai.length - static_cast<uint32_t>(bytesRemaining);
            }
        }
        g_ai.tickActive = false;
        activeTickReset.armed = false;
    }
}

namespace {

// Wall-clock delta since the previous poll, from whichever pump ran it. All
// pumps live on the guest thread, so the one thread_local cursor is shared and
// no interval is ever counted twice or dropped between them.
int64_t ConsumeAudioPollDeltaMicros()
{
    using Clock = std::chrono::steady_clock;
    static thread_local Clock::time_point lastPoll = Clock::now();

    const Clock::time_point now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastPoll).count();
    lastPoll = now;

    if (elapsed < 0) {
        elapsed = 0;
    }

    // Cap the catch-up interval after a debugger pause or host stall; backlog drains via
    // kMaxBlocksPerTick per pass. Never return early on a zero delta: two scheduler passes
    // can land in the same microsecond and the backlog still needs servicing.
    constexpr int64_t kMaxPollDeltaMicros = 100'000;
    // microseconds::rep is long long; int64_t is long on LP64 Linux/Android.
    return std::min<int64_t>(elapsed, kMaxPollDeltaMicros);
}

} // namespace

namespace {

// One DMA block's length on the virtual timeline: 0x180 bytes of 32 kHz stereo (3 ms) is exactly
// 182250 ticks.
uint64_t DetBlockTicks(uint32_t length, uint32_t sampleRate)
{
    const uint64_t bytesPerSecond = static_cast<uint64_t>(sampleRate) * kAudioChannels * kBytesPerSample;
    return bytesPerSecond == 0 ? 0 : static_cast<uint64_t>(length) * TimeBaseContract::kTicksPerSecond / bytesPerSecond;
}

// Deterministic counterpart of Audio_HLE_Tick: delivers every block whose virtual completion time
// has passed, in the same order (mix join, output push, AI DMA callback, deferred AX callbacks).
void AudioTickDeterministic(CpuContext* ctx)
{
    uint32_t startAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t sampleRate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (!g_ai.enabled || g_ai.startAddr == 0 || g_ai.length == 0 || g_ai.sampleRate == 0 || g_ai.tickActive) {
            return;
        }
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;
        const uint64_t blockTicks = DetBlockTicks(length, sampleRate);
        if (blockTicks == 0) {
            return;
        }
        if (g_ai.detBlockDueTicks == 0) {
            g_ai.detBlockDueTicks = DetClock::Now() + blockTicks;
        }
        if (DetClock::Now() < g_ai.detBlockDueTicks) {
            return;
        }
        g_ai.tickActive = true;
    }

    struct ActiveTickReset {
        ~ActiveTickReset()
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } activeTickReset;

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);

    for (int blocksCompleted = 0; blocksCompleted < kMaxBlocksPerTick; ++blocksCompleted) {
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (DetClock::Now() < g_ai.detBlockDueTicks) {
                break;
            }
            g_ai.detBlockDueTicks += DetBlockTicks(length, sampleRate);
            g_ai.bytesLeft = 0;
        }

        AxDspHle::JoinMixWorker();
        if (EnsureAudioBackend(sampleRate) && !PushAudioBlock(startAddr, length)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedAccessFailure) {
                g_ai.loggedAccessFailure = true;
                ReportAudioProblem("Audio", "failed to read DMA buffer; disabling audio DMA");
            }
            g_ai.enabled = false;
            return;
        }

        if (callback != 0) {
            const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(callback);
            if (info && info->rawCpuInvoker) {
                Memory::TryWrite32(kAICallbackBusyAddr, 1);
                InvokeIndirectCpu(callback, cpu);
                Memory::TryWrite32(kAICallbackBusyAddr, 0);
                AxDspHle::ServiceDeferredCallbacks();
            }
        }

        std::lock_guard<std::mutex> lock(g_ai.mutex);
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;
        g_ai.bytesLeft = g_ai.length;
        if (!g_ai.enabled || startAddr == 0 || length == 0 || DetBlockTicks(length, sampleRate) == 0) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_ai.mutex);
    const uint64_t blockTicks = DetBlockTicks(g_ai.length, g_ai.sampleRate);
    const uint64_t now = DetClock::Now();
    if (blockTicks != 0 && g_ai.detBlockDueTicks > now) {
        // The guest polls AIGetDMABytesLeft to size its work; report what is left of this block.
        const uint64_t remaining = std::min<uint64_t>(g_ai.detBlockDueTicks - now, blockTicks);
        g_ai.bytesLeft = static_cast<uint32_t>(remaining * g_ai.length / blockTicks) & ~0x1Fu;
    }
}

} // namespace

uint64_t Audio_HLE_DetNextEventTicks()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    if (!g_ai.enabled || g_ai.startAddr == 0 || g_ai.length == 0 || g_ai.sampleRate == 0) {
        return DetClock::kNever;
    }
    if (g_ai.detBlockDueTicks == 0) {
        return DetClock::Now();  // not armed yet: the next poll arms it
    }
    return g_ai.detBlockDueTicks;
}

void Audio_HLE_Poll(CpuContext* ctx)
{
    MusicAttenuation::TickGuest();
    if (DetClock::Enabled()) {
        AudioTickDeterministic(ctx);
        return;
    }
    Audio_HLE_Tick(ctx, static_cast<uint32_t>(ConsumeAudioPollDeltaMicros()));
}

void Audio_HLE_PollDeferred()
{
    // A wall-clock pump; deterministic mode delivers audio blocks only from the scheduler.
    if (DetClock::Enabled()) {
        return;
    }
    if (!OS_HLE_InterruptsEnabled()) {
        // Leave the elapsed interval unconsumed so the next poll still sees it.
        return;
    }

    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        Audio_HLE_Tick(cpu, static_cast<uint32_t>(ConsumeAudioPollDeltaMicros()));
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}
