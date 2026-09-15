#pragma once

#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>

// Run from an isolated NativeTests flag after Memory::load/initThread, before
// starting the title or audio workers. No dependency on native_tests.cpp's check.
namespace CriticalSectionTests {
using namespace DarkRecomp::Native;
using Clock = std::chrono::steady_clock;
constexpr uint32_t globalCS = 0x82A2918C;
constexpr uint32_t titleSpin = 0x82A49AC0;
constexpr uint32_t callbackSlot = uint32_t(PPC_CODE_BASE);
constexpr auto deadlineLength = std::chrono::seconds(3);

struct Results {
    unsigned failures = 0;
    std::string group;
    void expect(bool ok, const char* message) {
        std::printf("CriticalSections[%s] %s: %s\n", group.c_str(), ok ? "PASS" : "FAIL", message);
        if (!ok) ++failures;
    }
    void equal(uint32_t actual, uint32_t expected, const char* message) {
        expect(actual == expected, message);
        if (actual != expected)
            std::printf("  actual=%08X expected=%08X\n", actual, expected);
    }
    template<class F> void run(const char* name, F body) {
        group = name;
        try { body(); }
        catch (const std::exception& e) { expect(false, e.what()); }
        catch (...) { expect(false, "unexpected fixture/worker exception"); }
    }
};

struct Allocation {
    uint32_t address;
    explicit Allocation(uint32_t bytes) : address(memory->allocate(bytes)) {
        if (!address) throw std::runtime_error("critical-section fixture allocation failed");
        std::memset(memory->base() + address, 0, bytes);
    }
    ~Allocation() { memory->release(address); }
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
};

// Each host worker gets a real, distinct guest PCR/KTHREAD and private stack.
struct ThreadStorage {
    Allocation storage{0x6000};
    PPCContext context{};
    void initialize() {
        memory->initThreadStorage(storage.address, storage.address + 0x1000, 0x5000, GetCurrentThreadId());
        context = PPCContext{};
        context.r13.u64 = storage.address;
        context.r1.u64 = storage.address + 0x5F00;
        context.fpscr.loadFromHost();
    }
};

struct CurrentContext {
    PPCContext* saved = currentContext;
    explicit CurrentContext(PPCContext& ctx) { currentContext = &ctx; }
    ~CurrentContext() { currentContext = saved; }
};

template<size_t N> struct SavedBytes {
    uint32_t address;
    std::array<uint8_t, N> bytes{};
    explicit SavedBytes(uint32_t at) : address(at) {
        std::memcpy(bytes.data(), memory->base() + address, N);
    }
    ~SavedBytes() { std::memcpy(memory->base() + address, bytes.data(), N); }
};

struct SavedFunction {
    PPCFunc* original = PPC_LOOKUP_FUNC(memory->base(), callbackSlot);
    explicit SavedFunction(PPCFunc* replacement) { PPC_LOOKUP_FUNC(memory->base(), callbackSlot) = replacement; }
    ~SavedFunction() { PPC_LOOKUP_FUNC(memory->base(), callbackSlot) = original; }
};

// Track successful *calls*, never the (possibly broken) guest recursion field.
// Consequently baseline cleanup still makes exactly the matching Leave calls.
struct HeldSection {
    PPCContext& ctx;
    uint32_t address;
    unsigned depth = 0;
    HeldSection(PPCContext& c, uint32_t at) : ctx(c), address(at) {}
    void enter() {
        ctx.r3.u64 = address;
        __imp__RtlEnterCriticalSection(ctx, memory->base());
        ++depth;
    }
    bool tryEnter() {
        ctx.r3.u64 = address;
        __imp__RtlTryEnterCriticalSection(ctx, memory->base());
        if (!ctx.r3.u32) return false;
        ++depth;
        return true;
    }
    void leave() {
        if (!depth) return;
        ctx.r3.u64 = address;
        __imp__RtlLeaveCriticalSection(ctx, memory->base());
        --depth;
    }
    void release() { while (depth) leave(); }
    ~HeldSection() { release(); }
};

static std::array<uint8_t, 28> snapshot(uint32_t address) {
    std::array<uint8_t, 28> bytes{};
    std::memcpy(bytes.data(), memory->base() + address, bytes.size());
    return bytes;
}

static void idle(Results& out, uint32_t cs) {
    out.equal(memory->read32(cs + 20), 0, "idle recursion is zero");
}

static void ownerDiagnostic(const PPCContext& ctx, uint32_t cs) {
    // Xbox-specific supporting implementation (not a hardware ABI oracle):
    // https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc
    // Xenia uses guest_object()/PKTHREAD, whereas the local Unleashed imports
    // use r13. Do not turn that disagreement into a speculative hard assertion.
    // No LockCount/owner values are asserted. Windows' native LockCount encoding
    // is not the Xbox ABI; guest RecursionCount +20 is proven by title readers.
    std::printf("CriticalSections[owner diagnostic] owner=%08X PCR=%08X KTHREAD=%08X\n",
                memory->read32(cs + 24), ctx.r13.u32, memory->read32(ctx.r13.u32 + 0x100));
}

static bool competingTry(ThreadStorage& worker, uint32_t cs) {
    // Only a single nonblocking Try is executed; future destruction cannot
    // strand a worker in Enter while the calling thread still owns the lock.
    auto future = std::async(std::launch::async, [&] {
        worker.initialize();
        CurrentContext current(worker.context);
        HeldSection held(worker.context, cs);
        return held.tryEnter();
    });
    return future.get();
}

// Cancellation is always requested before a future is joined. Both callback
// gates and acquisition attempts have their own deadlines as a second guard.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool go = false;
    std::atomic<bool> cancel{false};
    void announce() {
        std::lock_guard lock(mutex);
        entered = true;
        changed.notify_all();
    }
    bool waitEntered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, deadlineLength, [&] { return entered; });
    }
    bool waitGo() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, deadlineLength, [&] { return go || cancel.load(); }) && !cancel.load();
    }
    void open() {
        std::lock_guard lock(mutex);
        go = true;
        changed.notify_all();
    }
    void stop() {
        std::lock_guard lock(mutex);
        cancel = true;
        changed.notify_all();
    }
};

struct HoldingWorker {
    Gate gate;
    bool owns = false; // Protected by gate.mutex, including the final Leave.
    std::future<std::array<bool, 2>> future;
    ~HoldingWorker() {
        gate.stop();
        if (future.valid()) future.wait();
    }
};

static void direct(Results& out, PPCContext& ctx, ThreadStorage& worker, uint32_t cs, unsigned mode) {
    if (mode == 0) {
        ctx.r3.u64 = cs;
        __imp__RtlInitializeCriticalSection(ctx, memory->base());
    } else {
        // Fresh static initializer, with no Initialize import. The title/host
        // recognize -1/0/0 at +16/+20/+24. These are initialization writes only.
        std::memset(memory->base() + cs, 0, 28);
        memory->write32(cs, 0x01000400);
        memory->write32(cs + 8, cs + 8);
        memory->write32(cs + 12, cs + 8);
        memory->write32(cs + 16, 0xFFFFFFFFu);
    }
    idle(out, cs);
    HeldSection held(ctx, cs);
    if (mode == 2) {
        if (!held.tryEnter()) { out.expect(false, "first static TryEnter succeeds"); return; }
        out.expect(true, "first static TryEnter succeeds");
    } else {
        held.enter();
    }
    out.equal(memory->read32(cs + 20), 1, "first acquisition publishes recursion 1");
    ownerDiagnostic(ctx, cs);
    const bool recursiveTry = held.tryEnter();
    out.expect(recursiveTry, "owning thread's TryEnter succeeds");
    out.equal(memory->read32(cs + 20), 2, "successful recursive TryEnter publishes recursion 2");

    const auto before = snapshot(cs);
    out.expect(!competingTry(worker, cs), "competing TryEnter fails while recursively owned");
    out.expect(snapshot(cs) == before, "failed competing TryEnter preserves all 28 guest bytes");
    if (recursiveTry) {
        held.leave();
        out.equal(memory->read32(cs + 20), 1, "partial Leave publishes recursion 1");
        const auto partial = snapshot(cs);
        out.expect(!competingTry(worker, cs), "partial Leave retains host mutual exclusion");
        out.expect(snapshot(cs) == partial, "failed Try after partial Leave preserves guest state");
    }
    held.release();
    idle(out, cs);

    // Reverse ownership, holding the worker inside the lock until the main
    // thread has tried it. No sleeps are used to infer that ownership occurred.
    HoldingWorker other;
    other.future = std::async(std::launch::async, [&] {
        worker.initialize();
        CurrentContext current(worker.context);
        HeldSection workerHeld(worker.context, cs);
        const bool acquired = workerHeld.tryEnter();
        std::unique_lock gateLock(other.gate.mutex);
        other.owns = acquired;
        struct ReleaseUnderGate {
            HeldSection& held;
            bool& owns;
            ~ReleaseUnderGate() { held.release(); owns = false; }
        } release{workerHeld, other.owns};
        other.gate.entered = true;
        other.gate.changed.notify_all();
        const bool releasedByGate = acquired && other.gate.changed.wait_for(gateLock, deadlineLength,
            [&] { return other.gate.go || other.gate.cancel.load(); }) && !other.gate.cancel.load();
        return std::array<bool, 2>{acquired, releasedByGate};
    });
    const bool ready = other.gate.waitEntered();
    out.expect(ready, "competing thread completed its post-release TryEnter");
    if (ready) {
        // A timed-out worker publishes its release under this same mutex.
        // Inspect only while it still owns the section, and prevent release
        // during the snapshot even if the gate deadline has already elapsed.
        std::lock_guard gateLock(other.gate.mutex);
        out.expect(other.owns, "competing owner is still gated before inspection");
        if (other.owns) {
            out.equal(memory->read32(cs + 20), 1, "new owner publishes recursion 1");
            const auto otherState = snapshot(cs);
            const bool stolen = held.tryEnter();
            out.expect(!stolen, "main TryEnter fails while competing thread owns host lock");
            out.expect(snapshot(cs) == otherState, "main failed TryEnter preserves competing owner's guest state");
            if (stolen) held.leave();
        }
        other.gate.go = true;
        other.gate.changed.notify_all();
    }
    other.gate.open();
    const auto otherResult = other.future.get();
    out.expect(otherResult[0], "another thread acquires after final Leave");
    out.expect(otherResult[1], "competing owner remained gated until inspection finished");
    idle(out, cs);
}

struct CallbackControl {
    Gate gate;
    std::atomic<bool> acquired{false};
    std::atomic<bool> timedOut{false};
};
static thread_local CallbackControl* activeCallback = nullptr;

static PPC_FUNC(finishCallback) {
    auto& control = *activeCallback;
    control.gate.announce();
    if (!control.gate.waitGo()) { control.timedOut = true; return; }
    HeldSection held(ctx, globalCS);
    const auto deadline = Clock::now() + deadlineLength;
    while (!control.gate.cancel.load() && Clock::now() < deadline) {
        if (held.tryEnter()) {
            control.acquired = true;
            return; // RAII Leave, then the original dispatcher finishes the callback.
        }
        SwitchToThread();
    }
    // Failure escape: return from the test callback, NOT from the original
    // waiter. The original dispatcher clears engine+296 normally. In particular
    // do not manufacture guest recursion or release another thread's host lock.
    control.timedOut = true;
}

struct OriginalFixture {
    SavedBytes<28> savedCS{globalCS};
    SavedBytes<16> savedSpin{titleSpin};
    SavedFunction savedFunction{finishCallback};
    Allocation engine{512};
    ThreadStorage owner;
    ThreadStorage dispatcher;
    CallbackControl callback;
    std::future<void> future;
    HeldSection held{owner.context, globalCS};
    bool spinHeld = false;

    OriginalFixture() {
        owner.initialize();
        std::memset(memory->base() + titleSpin, 0, 16);
        owner.context.r3.u64 = globalCS;
        __imp__RtlInitializeCriticalSection(owner.context, memory->base());
        // One real callback slot in the original dispatcher's eight-slot group.
        memory->write32(engine.address + 168, callbackSlot);
        // Worker exclusions +300/+332.. stay zero: the owner has a nonzero
        // KTHREAD from initThreadStorage and must execute the waiting path.
    }

    void takeSpin() {
        owner.context.r3.u64 = titleSpin;
        __imp__KeAcquireSpinLockAtRaisedIrql(owner.context, memory->base());
        spinHeld = true;
        // This is the title's recursive spin wrapper, separate from the RTL CS.
        memory->write32(titleSpin + 4, 1);
        memory->write32(titleSpin + 8, owner.context.r13.u32);
        memory->base()[titleSpin + 12] = 0;
    }
    void dropSpin() {
        if (!spinHeld) return;
        memory->write32(titleSpin + 4, 0);
        memory->write32(titleSpin + 8, 0);
        memory->base()[titleSpin + 12] = 0;
        owner.context.r3.u64 = titleSpin;
        __imp__KeReleaseSpinLockFromRaisedIrql(owner.context, memory->base());
        spinHeld = false;
    }
    void start() {
        future = std::async(std::launch::async, [&] {
            dispatcher.initialize();
            CurrentContext current(dispatcher.context);
            struct CallbackScope {
                CallbackControl* saved = activeCallback;
                explicit CallbackScope(CallbackControl& control) { activeCallback = &control; }
                ~CallbackScope() { activeCallback = saved; }
            } scope(callback);
            dispatcher.context.r3.u64 = engine.address;
            dispatcher.context.r4.u64 = 0;
            sub_828B4148(dispatcher.context, memory->base());
        });
    }
    void stop() {
        callback.gate.stop();
        // Before joining, release every fixture lock the dispatcher could need.
        // This ordering also handles exceptions before the original waiter ran.
        dropSpin();
        held.release();
    }
    ~OriginalFixture() {
        stop();
        if (future.valid()) future.wait();
        // Member RAII restores function slot/global bytes only AFTER that join.
    }
};

static void originalWait(Results& out, unsigned depth) {
    OriginalFixture fixture;
    auto& ctx = fixture.owner.context;
    CurrentContext current(ctx);
    for (unsigned i = 0; i < depth; ++i) fixture.held.enter();
    out.equal(memory->read32(globalCS + 20), depth, "original wait initial recursion");
    fixture.start();
    const bool entered = fixture.callback.gate.waitEntered();
    out.expect(entered, "original dispatcher entered the gated callback");
    if (!entered) {
        fixture.stop();
        fixture.future.get();
        return;
    }

    // The caller in sub_828B47F8 holds this spin lock once when it invokes
    // sub_828B4408. The callback is gated outside that spin lock by sub_828B4148.
    fixture.takeSpin();
    out.equal(memory->read32(fixture.engine.address + 296), fixture.engine.address + 168,
              "original dispatcher published the active callback slot");
    fixture.callback.gate.open();
    ctx.r3.u64 = fixture.engine.address;
    ctx.r4.u64 = fixture.engine.address + 168;
    sub_828B4408(ctx, memory->base());

    // Inspect before releasing the caller's lock, so an early-out cannot let a
    // late callback acquisition turn a broken waiter into a false positive.
    out.expect(fixture.callback.acquired.load(), "callback acquired global CS while original waiter ran");
    out.expect(!fixture.callback.timedOut.load(), "callback completed without the baseline timeout escape");
    out.equal(memory->read32(globalCS + 20), depth, "original wait restores recursion depth");
    out.equal(memory->read32(titleSpin + 4), 1, "original wait returns holding one title spin recursion");
    out.equal(memory->read32(titleSpin + 8), ctx.r13.u32, "original wait returns title spin ownership");
    fixture.dropSpin();
    // Cancellation makes early-out/setup failures safe as well as the baseline.
    fixture.callback.gate.stop();
    fixture.future.get();
    out.equal(memory->read32(fixture.engine.address + 296), 0, "original dispatcher cleared active callback");

    for (unsigned remaining = depth; remaining; --remaining) {
        out.expect(!competingTry(fixture.dispatcher, globalCS), "restored host recursion still excludes another thread");
        fixture.held.leave();
        out.equal(memory->read32(globalCS + 20), remaining - 1, "matching Leave decrements restored recursion");
    }
    idle(out, globalCS);
    out.expect(competingTry(fixture.dispatcher, globalCS), "final restored Leave releases the host lock");
    idle(out, globalCS);
}
} // namespace CriticalSectionTests

static void testCriticalSections(PPCContext& ctx) {
    using namespace CriticalSectionTests;
    // Preserve the caller's registers and currentContext; helpers use private
    // guest stacks/PCRs. All assertions accumulate, then fail the main flag once.
    (void)ctx;
    Results out;
    out.run("direct fixtures", [&] {
        Allocation sections(4096); // distinct addresses for both static first-use cases
        ThreadStorage owner, worker;
        owner.initialize();
        CurrentContext current(owner.context);
        out.run("initialized", [&] { direct(out, owner.context, worker, sections.address, 0); });
        out.run("static Enter", [&] { direct(out, owner.context, worker, sections.address + 64, 1); });
        out.run("static TryEnter", [&] { direct(out, owner.context, worker, sections.address + 128, 2); });
    });
    out.run("PPC 828B4408 depth 1", [&] { originalWait(out, 1); });
    out.run("PPC 828B4408 depth 2", [&] { originalWait(out, 2); });
    std::printf("CriticalSections: %u failure(s)\n", out.failures);
    if (out.failures) throw std::runtime_error("critical-section regression failures (see individual results)");
}
