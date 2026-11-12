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
#include <cstring>
#include <exception>
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
std::atomic<uint64_t> lastFrame{0}, frames{0}; // frames is an even/odd publication sequence
std::atomic_flag frameWriter = ATOMIC_FLAG_INIT;
uint8_t* guestBase = nullptr;
uint64_t thresholdTicks = 0, qpcFrequency = 0;
HANDLE stopEvent = nullptr;
// A raw thread handle, not std::thread: process exit paths that skip
// shutdown() must never reach a joinable std::thread destructor.
HANDLE watcherThread = nullptr;

uint64_t qpc() noexcept { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return uint64_t(v.QuadPart); }
double ms(uint64_t ticks) noexcept { return double(ticks) * 1000.0 / double(qpcFrequency); }

// ReadProcessMemory rejects inaccessible pages without a direct user-mode load.
// A self-process guard-page probe preserves the guard on Windows; concurrent
// protection changes and other Windows implementations still need validation.
bool read32(const uint8_t* base, uint32_t address, uint32_t& value) noexcept {
    if (!base || (address & 3) || address > 0xFFFFFFF0u) return false;
    const uint8_t* host = base + address;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(host, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & 0xEE)) return false;
    uint32_t word = 0;
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), host, &word, sizeof(word), &read) || read != sizeof(word)) return false;
    value = _byteswap_ulong(word);
    return true;
}

struct Stack { uint32_t lastFunction = 0, lr = 0, sp = 0; unsigned depth = 0; std::array<uint32_t, stackDepth> frames{}; };
Stack captureStack(const PPCContext* ctx) noexcept {
    Stack s;
    if (!ctx) return s;
    // The slot lock keeps ctx alive. A self-process copy avoids direct C++
    // loads racing the guest writer, but is not an atomic point-in-time view.
    PPCContext snapshot{};
    SIZE_T bytes = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), ctx, &snapshot, sizeof(snapshot), &bytes) || bytes != sizeof(snapshot)) return s;
    s.lastFunction = snapshot.lastFunction;
    s.lr = uint32_t(snapshot.lr);
    s.sp = snapshot.r1.u32;
    s.depth = walkGuestStack(guestBase, s.sp, s.frames.data(), unsigned(s.frames.size()));
    return s;
}

struct Track {
    uint32_t tid = 0;
    uint64_t creationTime = 0;
    bool registered = false, hasContext = false;
    const char* role = nullptr;
    uint32_t entry = 0, argument = 0;
    bool seen = false, cpuSeen = false;
    std::array<uint16_t, 8> states{};
    std::array<uint16_t, waitReasons> waiting{}, ready{};
    uint64_t time0 = 0, time1 = 0;
    ULONG switches0 = 0, switches1 = 0;
    LONG priority = 0, basePriority = 0;
    void* start = nullptr;
    Stack first, last;
};

uint64_t fileTimeTicks(const FILETIME& value) noexcept {
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

void recordCpu(Track& track, uint64_t time) noexcept {
    // A reset is not a valid unsigned delta, even on unusual host runtimes.
    if (!track.cpuSeen || time < track.time1) {
        track.cpuSeen = true;
        track.time0 = time;
    }
    track.time1 = time;
}

void snapshotMetadata(Track& track, const Slot& slot) noexcept {
    track.registered = true;
    track.hasContext = slot.ctx != nullptr;
    track.role = slot.role;
    track.entry = slot.entry;
    track.argument = slot.argument;
}

struct FrameSnapshot { uint64_t sequence, time; };
bool thresholdReached(uint64_t now, uint64_t frame, uint64_t threshold) noexcept {
    return frame && now >= frame && now - frame >= threshold;
}
bool frameSnapshot(FrameSnapshot& snapshot) noexcept {
    const uint64_t before = frames.load(std::memory_order_acquire);
    // The watcher runs at higher priority than the producer. Never spin while
    // the producer is publishing; let it run and retry on the next poll.
    if (before & 1) return false;
    const uint64_t time = lastFrame.load(std::memory_order_relaxed);
    if (before != frames.load(std::memory_order_acquire)) return false;
    snapshot = {before, time};
    return true;
}

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
    static bool testTrackAccounting() {
        Watcher watcher;
        Track* first = watcher.track(7, 100);
        if (!first) return false;
        recordCpu(*first, 200);
        recordCpu(*first, 250);
        Track* reused = watcher.track(7, 101);
        if (!reused || reused == first || reused->cpuSeen || first->time1 - first->time0 != 50) return false;
        recordCpu(*reused, 5);
        recordCpu(*reused, 3);
        if (reused->time1 - reused->time0 != 0) return false;
        for (unsigned i = 2; i < trackLimit; ++i)
            if (!watcher.track(i + 100, i)) return false;
        watcher.tracks.back().entry = 0x12345678;
        watcher.noteOmitted(3);
        watcher.noteOmitted(1);
        return !watcher.track(99999, 99999) && watcher.tracks.back().entry == 0x12345678 &&
            watcher.findTrack(7, 100) == first && watcher.findTrack(7, 101) == reused &&
            watcher.peakOmittedThreads == 3 && watcher.omittedThreadSamples == 4;
    }
    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        bool stalled = false;
        uint64_t stallFrames = 0, stallBegin = 0, sampleBegin = 0, nextSample = 0, serial = 0;
        unsigned samples = 0;
        for (;;) {
            if (!sleepFor(timer, stalled ? 4 : 5)) break;
            FrameSnapshot frame{};
            if (!frameSnapshot(frame)) continue;
            const uint64_t now = qpc();
            if (!stalled) {
                if (!thresholdReached(now, frame.time, thresholdTicks)) continue;
                stalled = true; stallFrames = frame.sequence; stallBegin = frame.time; sampleBegin = now; samples = 0;
                tracks.clear(); omittedThreadSamples = 0; peakOmittedThreads = 0;
                sample(true); ++samples;
                nextSample = now + qpcFrequency / 100;
                continue;
            }
            if (frame.sequence != stallFrames) {
                sample(false); ++samples;
                report(++serial, frame.time >= stallBegin ? frame.time - stallBegin : 0,
                       qpc() - sampleBegin, samples);
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
    uint64_t omittedThreadSamples = 0;
    ULONG peakOmittedThreads = 0;

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
    Track* findTrack(uint32_t tid, uint64_t creationTime) noexcept {
        for (auto& t : tracks) if (t.tid == tid && t.creationTime == creationTime) return &t;
        return nullptr;
    }
    void noteOmitted(ULONG count) noexcept {
        omittedThreadSamples += count;
        peakOmittedThreads = (std::max)(peakOmittedThreads, count);
    }
    Track* track(uint32_t tid, uint64_t creationTime) {
        if (auto* existing = findTrack(tid, creationTime)) return existing;
        if (tracks.size() == trackLimit) return nullptr;
        tracks.push_back({});
        tracks.back().tid = tid;
        tracks.back().creationTime = creationTime;
        return &tracks.back();
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
        ULONG omitted = 0;
        const SystemThread* threads = processThreads(count);
        for (ULONG i = 0; threads && i < count; ++i) {
            const auto& info = threads[i];
            const auto tid = uint32_t(uintptr_t(info.threadId));
            if (tid == GetCurrentThreadId()) continue;
            Track* found = track(tid, uint64_t(info.createTime.QuadPart));
            if (!found) { ++omitted; continue; }
            Track& t = *found;
            if (!t.seen) { t.seen = true; t.switches0 = info.contextSwitches; t.start = info.startAddress; }
            t.switches1 = info.contextSwitches;
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (thread) {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                    if (fileTimeTicks(created) == t.creationTime)
                        recordCpu(t, fileTimeTicks(kernel) + fileTimeTicks(user));
                }
                CloseHandle(thread);
            }
            t.priority = info.priority; t.basePriority = info.basePriority;
            const unsigned state = info.state < 8 ? info.state : 0;
            ++t.states[state];
            const unsigned reason = info.waitReason < waitReasons ? info.waitReason : waitReasons - 1;
            if (state == 5) ++t.waiting[reason];
            else if (state == 1 || state == 7 || state == 3) ++t.ready[reason];
        }
        noteOmitted(omitted);
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
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetThreadTimes(slot.handle, &created, &exited, &kernel, &user)) {
                slot.state.store(1, std::memory_order_release);
                continue;
            }
            Track* found = findTrack(slot.tid.load(std::memory_order_relaxed), fileTimeTicks(created));
            if (!found) {
                slot.state.store(1, std::memory_order_release);
                continue;
            }
            Track& t = *found;
            snapshotMetadata(t, slot);
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
    void report(uint64_t serial, uint64_t engineTicks, uint64_t sampleTicks, unsigned samples) {
        std::sort(tracks.begin(), tracks.end(), [&](const Track& a, const Track& b) {
            return a.time1 - a.time0 > b.time1 - b.time0;
        });
        double totalMs = 0;
        for (const auto& t : tracks) totalMs += double(t.time1 - t.time0) / 1e4;
        std::string out;
        char line[1024];
        std::snprintf(line, sizeof(line), "[StallWatch] stall=%llu engine_frame_ms=%.1f sample_window_ms=%.1f threshold_ms=%.1f samples=%u tracked_thread_lifetimes=%zu peak_omitted_threads=%lu omitted_thread_samples=%llu sample_process_cpu_ms=%.1f\n",
            static_cast<unsigned long long>(serial), ms(engineTicks), ms(sampleTicks), ms(thresholdTicks), samples, tracks.size(),
            peakOmittedThreads, static_cast<unsigned long long>(omittedThreadSamples), totalMs);
        out += line;
        for (const auto& t : tracks) {
            const double tickMs = double(t.time1 - t.time0) / 1e4;
            const bool registered = t.registered;
            if (!registered && tickMs < 15.0) continue;
            std::snprintf(line, sizeof(line), "[StallWatch] stall=%llu tid=%u role=%s callback=%08X arg=%08X prio=%ld/%ld sample_cpu_ms=%.1f cpu_available=%u switches=%lu run=%u ready=%u wait=%u other=%u ready_why=%s wait_why=%s start=%p\n",
                static_cast<unsigned long long>(serial), t.tid, t.role ? t.role : "host",
                t.entry, t.argument, t.priority, t.basePriority, tickMs, unsigned(t.cpuSeen),
                t.switches1 - t.switches0, t.states[2], t.states[1] + t.states[7] + t.states[3], t.states[5],
                t.states[0] + t.states[4] + t.states[6], histogram(t.ready).c_str(), histogram(t.waiting).c_str(), t.start);
            out += line;
            if (registered && t.hasContext) {
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
    while (depth < capacity) {
        uint32_t back = 0, lr = 0;
        if (!read32(base, sp, back) || back <= sp || back - sp > 0x100000u || back < 8) break;
        if (!read32(base, back - 8, lr)) break;
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
    lastFrame.store(0, std::memory_order_relaxed);
    frames.store(0, std::memory_order_relaxed);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return;
    watcherThread = CreateThread(nullptr, 0, watcherMain, nullptr, 0, nullptr);
    if (!watcherThread) { CloseHandle(stopEvent); stopEvent = nullptr; return; }
    std::fprintf(stderr, "[StallWatch] enabled threshold_ms=%.1f sample_ms=10\n", threshold);
    enabledFlag.store(true, std::memory_order_release);
}

static bool registerWithHandle(HANDLE handle, const PPCContext* ctx, const char* role, uint32_t entry, uint32_t argument) noexcept {
    if (!handle) return false;
    if (ownSlot) { CloseHandle(handle); return false; }
    for (auto& slot : slots) {
        int expected = 0;
        if (!slot.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) continue;
        slot.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        slot.ctx = ctx; slot.role = role; slot.entry = entry; slot.argument = argument;
        slot.handle = handle;
        slot.state.store(1, std::memory_order_release);
        ownSlot = &slot;
        return true;
    }
    CloseHandle(handle);
    return false;
}

void registerThread(const PPCContext* ctx, const char* role, uint32_t entry, uint32_t argument) noexcept {
    if (!enabled() || ownSlot) return;
    registerWithHandle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, GetCurrentThreadId()),
                       ctx, role, entry, argument);
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
    // Render hooks can be reached from multiple guest call paths. A competing
    // writer drops its publication instead of spinning at guest priority.
    if (frameWriter.test_and_set(std::memory_order_acquire)) return;
    frames.fetch_add(1, std::memory_order_acq_rel);
    lastFrame.store(qpc(), std::memory_order_relaxed);
    frames.fetch_add(1, std::memory_order_release);
    frameWriter.clear(std::memory_order_release);
}

void shutdown() noexcept {
    if (!watcherThread) return;
    enabledFlag.store(false, std::memory_order_release);
    SetEvent(stopEvent);
    if (WaitForSingleObject(watcherThread, INFINITE) != WAIT_OBJECT_0) std::terminate();
    CloseHandle(watcherThread);
    watcherThread = nullptr;
    CloseHandle(stopEvent);
    stopEvent = nullptr;
    unregisterThread();
}

bool testSlotLifecycle(const PPCContext* ctx) noexcept {
    if (enabled() || ownSlot || registerWithHandle(nullptr, ctx, "failed", 1, 2) || ownSlot) return false;
    unregisterThread();
    auto openSelf = [] { return OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, GetCurrentThreadId()); };
    if (!registerWithHandle(openSelf(), ctx, "first", 11, 22)) return false;
    Slot* first = ownSlot;
    int expected = 1;
    if (!first->state.compare_exchange_strong(expected, 2, std::memory_order_acquire)) return false;
    Track snapshot;
    snapshotMetadata(snapshot, *first);
    first->state.store(1, std::memory_order_release);
    unregisterThread();
    if (!registerWithHandle(openSelf(), nullptr, "second", 33, 44)) return false;
    const bool ok = ownSlot == first && snapshot.registered && snapshot.hasContext &&
        snapshot.role && std::strcmp(snapshot.role, "first") == 0 && snapshot.entry == 11 && snapshot.argument == 22;
    unregisterThread();
    return ok;
}
bool testFrameThreshold() noexcept {
    if (enabled()) return false;
    FrameSnapshot snapshot{};
    lastFrame.store(123, std::memory_order_relaxed);
    frames.store(1, std::memory_order_relaxed);
    const bool rejectsInProgress = !frameSnapshot(snapshot);
    frames.store(2, std::memory_order_release);
    const bool acceptsPublished = frameSnapshot(snapshot) && snapshot.sequence == 2 && snapshot.time == 123;
    frames.store(0, std::memory_order_relaxed);
    lastFrame.store(0, std::memory_order_relaxed);
    return rejectsInProgress && acceptsPublished && !thresholdReached(99, 100, 10) &&
        !thresholdReached(109, 100, 10) && thresholdReached(110, 100, 10) &&
        !thresholdReached(100, 0, 10);
}
bool testTrackAccounting() noexcept {
    try { return Watcher::testTrackAccounting(); } catch (...) { return false; }
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
