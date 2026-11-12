#include "stall_watch.h"
#include "ppc_recomp_shared.h"
#include "runtime/native/thread_topology.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace DarkRecomp::Native::StallWatch {
namespace {
// Layouts of SYSTEM_PROCESS_INFORMATION / SYSTEM_THREAD_INFORMATION on x64
// (the documented winternl.h structures with their reserved fields named).
struct SystemThread {
    LARGE_INTEGER kernelTime, userTime, createTime;
    ULONG waitTime;
    PVOID startAddress;
    HANDLE processId, threadId;
    LONG priority, basePriority;
    ULONG contextSwitches, state, waitReason;
};
static_assert(sizeof(SystemThread) == 0x50);
constexpr size_t processThreadsOffset = 0x100, processIdOffset = 0x50;
using QuerySystemInformation = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);

constexpr unsigned slotLimit = 160, stackDepth = 14, trackLimit = 256, waitReasons = 48;
struct Slot {
    // 0 free, 1 registered, 2 being read by the watcher.
    std::atomic<int> state{0};
    std::atomic<uint32_t> tid{0};
    const PPCContext* ctx = nullptr;
    const char* role = nullptr;
    uint32_t entry = 0, argument = 0;
    HANDLE handle = nullptr;
};
std::array<Slot, slotLimit> slots;
thread_local Slot* ownSlot = nullptr;

std::atomic<bool> enabledFlag{false};
std::atomic<uint64_t> lastFrame{0}, frames{0};
uint8_t* guestBase = nullptr;
uint64_t thresholdTicks = 0, qpcFrequency = 0;
HANDLE stopEvent = nullptr;
// A raw thread handle, not std::thread: process exit paths that skip
// shutdown() must never reach a joinable std::thread destructor.
HANDLE watcherThread = nullptr;

uint64_t qpc() noexcept { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return uint64_t(v.QuadPart); }
double ms(uint64_t ticks) noexcept { return double(ticks) * 1000.0 / double(qpcFrequency); }

// clang-cl's __try catches faults raised in called functions, not in loads it
// has inlined into the guarded body; keep the load out of line.
__declspec(noinline) uint32_t loadWord(const volatile uint32_t* address) noexcept { return *address; }
bool guardedLoad(const uint8_t* host, uint32_t& value) noexcept {
    __try {
        value = _byteswap_ulong(loadWord(reinterpret_cast<const volatile uint32_t*>(host)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Check current protection first: reading a PAGE_GUARD page would consume the
// title's guard, and uncommitted guest pages are common past a broken chain.
struct ReadableRange { const uint8_t* begin = nullptr; const uint8_t* end = nullptr; };
bool read32(const uint8_t* base, uint32_t address, uint32_t& value, ReadableRange* cache = nullptr) noexcept {
    if (!base || (address & 3) || address > 0xFFFFFFF0u) return false;
    const uint8_t* host = base + address;
    if (!cache || host < cache->begin || host + 4 > cache->end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(host, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & 0xEE)) return false;
        const auto* begin = static_cast<const uint8_t*>(info.BaseAddress);
        if (host + 4 > begin + info.RegionSize) return false;
        if (cache) *cache = {begin, begin + info.RegionSize};
    }
    return guardedLoad(host, value);
}

struct Stack { uint32_t lastFunction = 0, lr = 0, sp = 0; unsigned depth = 0; std::array<uint32_t, stackDepth> frames{}; };
Stack captureStack(const PPCContext* ctx) noexcept {
    Stack s;
    if (!ctx) return s;
    // Racy by design for a running thread; for a blocked thread these are the
    // values at its host call. Only guarded guest-memory reads follow.
    const auto* volatileCtx = reinterpret_cast<const volatile PPCContext*>(ctx);
    s.lastFunction = volatileCtx->lastFunction;
    s.lr = uint32_t(volatileCtx->lr);
    s.sp = volatileCtx->r1.u32;
    s.depth = walkGuestStack(guestBase, s.sp, s.frames.data(), unsigned(s.frames.size()));
    return s;
}

struct Track {
    uint32_t tid = 0;
    int slot = -1;
    bool seen = false;
    std::array<uint16_t, 8> states{};
    std::array<uint16_t, waitReasons> waiting{}, ready{};
    uint64_t time0 = 0, time1 = 0, cycles0 = 0, cycles1 = 0;
    ULONG switches0 = 0, switches1 = 0;
    LONG priority = 0, basePriority = 0;
    void* start = nullptr;
    Stack first, last;
};

// KWAIT_REASON names. For a Ready thread the field keeps the reason it last
// left the processor (WrDispatchInt / WrPreempted: a higher-priority thread
// took it; WrQuantumEnd: round robin), which is what separates starvation
// from waiting.
const char* waitName(unsigned reason) noexcept {
    static const char* names[] = {"Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution",
        "Suspended", "UserRequest", "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation",
        "WrDelayExecution", "WrSuspended", "WrUserRequest", "WrEventPair", "WrQueue", "WrLpcReceive",
        "WrLpcReply", "WrVirtualMemory", "WrPageOut", "WrRendezvous", "WrKeyedEvent", "WrTerminated",
        "WrProcessInSwap", "WrCpuRateControl", "WrCalloutStack", "WrKernel", "WrResource", "WrPushLock",
        "WrMutex", "WrQuantumEnd", "WrDispatchInt", "WrPreempted", "WrYieldExecution", "WrFastMutex",
        "WrGuardedMutex", "WrRundown", "WrAlertByThreadId", "WrDeferredPreempt", "WrPhysicalFault",
        "WrIoRing", "WrMdlCache", "WrRcu"};
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "Wr?";
}

class Watcher {
public:
    Watcher() : query(reinterpret_cast<QuerySystemInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"))) {
        buffer.resize(4u << 20);
        tracks.reserve(trackLimit);
    }
    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        tscStartQpc = qpc(); tscStart = __rdtsc();
        bool stalled = false;
        uint64_t stallFrames = 0, stallBegin = 0, nextSample = 0, serial = 0;
        unsigned samples = 0;
        for (;;) {
            if (!sleepFor(timer, stalled ? 4 : 5)) break;
            const uint64_t now = qpc();
            const uint64_t frameCount = frames.load(std::memory_order_acquire);
            const uint64_t last = lastFrame.load(std::memory_order_acquire);
            if (!stalled) {
                if (!last || now - last < thresholdTicks) continue;
                stalled = true; stallFrames = frameCount; stallBegin = last; samples = 0;
                tracks.clear();
                sample(true); ++samples;
                nextSample = now + qpcFrequency / 100;
                continue;
            }
            if (frameCount != stallFrames) {
                const uint64_t end = lastFrame.load(std::memory_order_acquire);
                sample(false); ++samples;
                report(++serial, end - stallBegin, now - stallBegin, samples);
                stalled = false;
                continue;
            }
            if (now >= nextSample) {
                sample(false); ++samples;
                nextSample = now + qpcFrequency / 100;
            }
        }
        if (timer) CloseHandle(timer);
    }
private:
    QuerySystemInformation query;
    std::vector<uint8_t> buffer;
    std::vector<Track> tracks;
    uint64_t tscStartQpc = 0, tscStart = 0;

    static bool sleepFor(HANDLE timer, unsigned milliseconds) noexcept {
        if (timer) {
            LARGE_INTEGER due{}; due.QuadPart = -int64_t(milliseconds) * 10000;
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                HANDLE handles[2] = {stopEvent, timer};
                return WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0;
            }
        }
        return WaitForSingleObject(stopEvent, milliseconds) != WAIT_OBJECT_0;
    }
    Track& track(uint32_t tid) {
        for (auto& t : tracks) if (t.tid == tid) return t;
        if (tracks.size() == trackLimit) return tracks.back();
        tracks.push_back({});
        tracks.back().tid = tid;
        return tracks.back();
    }
    const SystemThread* processThreads(ULONG& count) {
        if (!query) return nullptr;
        for (int attempt = 0; attempt < 3; ++attempt) {
            ULONG needed = 0;
            const LONG status = query(5, buffer.data(), ULONG(buffer.size()), &needed);
            if (status == LONG(0xC0000004)) { buffer.resize((std::max)(size_t(needed) + (1u << 20), buffer.size() * 2)); continue; }
            if (status < 0) return nullptr;
            const auto self = uintptr_t(GetCurrentProcessId());
            for (size_t offset = 0;;) {
                const uint8_t* process = buffer.data() + offset;
                const auto id = *reinterpret_cast<const uintptr_t*>(process + processIdOffset);
                if (id == self) {
                    count = *reinterpret_cast<const ULONG*>(process + 4);
                    return reinterpret_cast<const SystemThread*>(process + processThreadsOffset);
                }
                const ULONG next = *reinterpret_cast<const ULONG*>(process);
                if (!next) return nullptr;
                offset += next;
            }
        }
        return nullptr;
    }
    void sample(bool first) {
        ULONG count = 0;
        const SystemThread* threads = processThreads(count);
        for (ULONG i = 0; threads && i < count; ++i) {
            const auto& info = threads[i];
            const auto tid = uint32_t(uintptr_t(info.threadId));
            if (tid == GetCurrentThreadId()) continue;
            Track& t = track(tid);
            const uint64_t time = uint64_t(info.kernelTime.QuadPart + info.userTime.QuadPart);
            if (!t.seen) { t.seen = true; t.time0 = time; t.switches0 = info.contextSwitches; t.start = info.startAddress; }
            t.time1 = time; t.switches1 = info.contextSwitches;
            t.priority = info.priority; t.basePriority = info.basePriority;
            const unsigned state = info.state < 8 ? info.state : 0;
            ++t.states[state];
            const unsigned reason = info.waitReason < waitReasons ? info.waitReason : waitReasons - 1;
            if (state == 5) ++t.waiting[reason];
            else if (state == 1 || state == 7 || state == 3) ++t.ready[reason];
        }
        for (unsigned n = 0; n < slotLimit; ++n) {
            auto& slot = slots[n];
            int expected = 1;
            if (!slot.state.compare_exchange_strong(expected, 2, std::memory_order_acquire)) continue;
            // A thread that ended without unregistering (an exit path that
            // skipped its cleanup) owns no live context: retire the slot.
            if (!slot.handle || WaitForSingleObject(slot.handle, 0) != WAIT_TIMEOUT) {
                if (slot.handle) CloseHandle(slot.handle);
                slot.handle = nullptr; slot.ctx = nullptr;
                slot.tid.store(0, std::memory_order_relaxed);
                slot.state.store(0, std::memory_order_release);
                continue;
            }
            Track& t = track(slot.tid.load(std::memory_order_relaxed));
            t.slot = int(n);
            ULONG64 cycles = 0;
            if (slot.handle) QueryThreadCycleTime(slot.handle, &cycles);
            if (first || !t.cycles0) t.cycles0 = cycles;
            t.cycles1 = cycles;
            const Stack stack = captureStack(slot.ctx);
            if (first || !t.first.sp) t.first = stack;
            t.last = stack;
            slot.state.store(1, std::memory_order_release);
        }
    }
    static std::string stackText(const Stack& s) {
        char text[512]; int used = std::snprintf(text, sizeof(text), "fn=%08X lr=%08X sp=%08X [", s.lastFunction, s.lr, s.sp);
        for (unsigned i = 0; i < s.depth && used > 0 && used < int(sizeof(text)) - 12; ++i)
            used += std::snprintf(text + used, sizeof(text) - size_t(used), i ? " %08X" : "%08X", s.frames[i]);
        std::string result(text);
        result += "]";
        return result;
    }
    static std::string histogram(const std::array<uint16_t, waitReasons>& counts) {
        std::string text;
        for (unsigned r = 0; r < waitReasons; ++r) if (counts[r]) {
            char item[64]; std::snprintf(item, sizeof(item), "%s%s:%u", text.empty() ? "" : ",", waitName(r), counts[r]);
            text += item;
        }
        return text.empty() ? "-" : text;
    }
    void report(uint64_t serial, uint64_t engineTicks, uint64_t observedTicks, unsigned samples) {
        const double tscPerQpc = double(__rdtsc() - tscStart) / double((std::max)(uint64_t(1), qpc() - tscStartQpc));
        auto cycleMs = [&](uint64_t cycles) { return tscPerQpc > 0 ? ms(uint64_t(double(cycles) / tscPerQpc)) : 0.0; };
        std::sort(tracks.begin(), tracks.end(), [&](const Track& a, const Track& b) {
            const double ca = a.slot >= 0 ? cycleMs(a.cycles1 - a.cycles0) : (a.time1 - a.time0) / 1e4;
            const double cb = b.slot >= 0 ? cycleMs(b.cycles1 - b.cycles0) : (b.time1 - b.time0) / 1e4;
            return ca > cb;
        });
        double totalMs = 0;
        for (const auto& t : tracks) totalMs += double(t.time1 - t.time0) / 1e4;
        std::string out;
        char line[1024];
        std::snprintf(line, sizeof(line), "[StallWatch] stall=%llu engine_frame_ms=%.1f observed_ms=%.1f threshold_ms=%.1f samples=%u process_threads=%zu process_cpu_ms_ticks=%.1f\n",
            static_cast<unsigned long long>(serial), ms(engineTicks), ms(observedTicks), ms(thresholdTicks), samples, tracks.size(), totalMs);
        out += line;
        for (const auto& t : tracks) {
            const double tickMs = double(t.time1 - t.time0) / 1e4;
            const double cpuMs = t.slot >= 0 ? cycleMs(t.cycles1 - t.cycles0) : tickMs;
            const bool registered = t.slot >= 0;
            if (!registered && tickMs < 15.0) continue;
            const Slot* slot = registered ? &slots[size_t(t.slot)] : nullptr;
            std::snprintf(line, sizeof(line), "[StallWatch] stall=%llu tid=%u role=%s callback=%08X arg=%08X prio=%ld/%ld cpu_ms=%.1f switches=%lu run=%u ready=%u wait=%u other=%u ready_why=%s wait_why=%s start=%p\n",
                static_cast<unsigned long long>(serial), t.tid, slot && slot->role ? slot->role : "host",
                slot ? slot->entry : 0, slot ? slot->argument : 0, t.priority, t.basePriority, cpuMs,
                t.switches1 - t.switches0, t.states[2], t.states[1] + t.states[7] + t.states[3], t.states[5],
                t.states[0] + t.states[4] + t.states[6], histogram(t.ready).c_str(), histogram(t.waiting).c_str(), t.start);
            out += line;
            if (registered && slot->ctx) {
                out += "[StallWatch]   first " + stackText(t.first) + "\n";
                if (t.last.lastFunction != t.first.lastFunction || t.last.frames != t.first.frames)
                    out += "[StallWatch]   last  " + stackText(t.last) + "\n";
            }
        }
        std::fwrite(out.data(), 1, out.size(), stderr);
        std::fflush(stderr);
    }
};
}

bool readGuestWord(const uint8_t* base, uint32_t address, uint32_t& value) noexcept { return read32(base, address, value); }

unsigned walkGuestStack(const uint8_t* base, uint32_t sp, uint32_t* out, unsigned capacity) noexcept {
    unsigned depth = 0;
    ReadableRange cache;
    while (depth < capacity) {
        uint32_t back = 0, lr = 0;
        if (!read32(base, sp, back, &cache) || back <= sp || back - sp > 0x100000u || back < 8) break;
        if (!read32(base, back - 8, lr, &cache)) break;
        out[depth++] = lr;
        sp = back;
    }
    return depth;
}

bool enabled() noexcept { return enabledFlag.load(std::memory_order_relaxed); }

DWORD WINAPI watcherMain(void*) {
    try { Watcher w; w.run(); } catch (...) {}
    return 0;
}

void initializeFromEnvironment(uint8_t* base) noexcept {
    if (enabled() || watcherThread) return;
    const char* value = std::getenv("DARKRECOMP_STALL_WATCH");
    if (!value) return;
    const double threshold = std::atof(value);
    if (!(threshold > 0)) return;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    qpcFrequency = uint64_t(frequency.QuadPart);
    if (!qpcFrequency) return;
    thresholdTicks = uint64_t(threshold * double(qpcFrequency) / 1000.0);
    guestBase = base;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return;
    watcherThread = CreateThread(nullptr, 0, watcherMain, nullptr, 0, nullptr);
    if (!watcherThread) { CloseHandle(stopEvent); stopEvent = nullptr; return; }
    std::fprintf(stderr, "[StallWatch] enabled threshold_ms=%.1f sample_ms=10\n", threshold);
    enabledFlag.store(true, std::memory_order_release);
}

void registerThread(const PPCContext* ctx, const char* role, uint32_t entry, uint32_t argument) noexcept {
    if (!enabled() || ownSlot) return;
    for (auto& slot : slots) {
        int expected = 0;
        if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) continue;
        slot.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        slot.ctx = ctx; slot.role = role; slot.entry = entry; slot.argument = argument;
        slot.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, GetCurrentThreadId());
        slot.state.store(1, std::memory_order_release);
        ownSlot = &slot;
        return;
    }
}

void unregisterThread() noexcept {
    Slot* slot = ownSlot;
    if (!slot) return;
    ownSlot = nullptr;
    for (;;) {
        int expected = 1;
        if (slot->state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) break;
        SwitchToThread();
    }
    if (slot->handle) CloseHandle(slot->handle);
    slot->handle = nullptr; slot->ctx = nullptr;
    slot->tid.store(0, std::memory_order_relaxed);
    slot->state.store(0, std::memory_order_release);
}

void engineFrame() noexcept {
    if (!enabled()) return;
    lastFrame.store(qpc(), std::memory_order_release);
    frames.fetch_add(1, std::memory_order_acq_rel);
}

void shutdown() noexcept {
    if (!watcherThread) return;
    enabledFlag.store(false, std::memory_order_release);
    SetEvent(stopEvent);
    WaitForSingleObject(watcherThread, 2000);
    CloseHandle(watcherThread);
    watcherThread = nullptr;
}
}

// Every title worker enters through 821FBB00 (ExCreateThread entry, argument =
// thread object in r3). Registration happens here rather than in the kernel's
// thread start so the AOT inputs under runtime/native stay unchanged.
extern "C" PPC_FUNC(__imp__sub_821FBB00);
PPC_FUNC(sub_821FBB00) {
    using namespace DarkRecomp::Native;
    if (!StallWatch::enabled()) { __imp__sub_821FBB00(ctx, base); return; }
    const uint32_t argument = ctx.r3.u32;
    const char* role = "worker";
    uint32_t vtable = 0, callback = 0;
    if (StallWatch::readGuestWord(base, argument, vtable) && StallWatch::readGuestWord(base, vtable + 48, callback)) {
        if (resourcePoolThreadRole(0x821FBB00, vtable, callback)) role = "resource-pool";
        else if (applicationSystemThreadRole(0x821FBB00, vtable, callback)) role = "app-system";
    }
    StallWatch::registerThread(&ctx, role, callback, argument);
    struct Unregister { ~Unregister() { StallWatch::unregisterThread(); } } unregister;
    // Workers leave through ExTerminateThread's ThreadExit exception. Under
    // /EHsc a direct call to an extern "C" function is assumed not to throw,
    // which drops this cleanup and leaves the slot pointing at a freed
    // context; an opaque pointer keeps the unwind path.
    static void (*volatile original)(PPCContext&, uint8_t*) = __imp__sub_821FBB00;
    original(ctx, base);
}
