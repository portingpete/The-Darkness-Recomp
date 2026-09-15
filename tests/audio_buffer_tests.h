#pragma once
#include "audio_pcm_tap_tests.h"

// A gated real-sink probe distinguishes prefill and actual source starvation
// from the weaker condition that submitted/completed counters keep increasing.
static HANDLE gAudioContinuityEntered = nullptr;
static HANDLE gAudioContinuityRelease = nullptr;
static std::atomic<uint32_t> gAudioContinuityGateCall{0xffffffffu};
static std::atomic<uint32_t> gAudioContinuityCalls{0};
static std::atomic<uint32_t> gAudioContinuityStatus{0};
static std::atomic<uint64_t> gAudioContinuityFirstSubmitMs{0};
static uint32_t gAudioContinuityOut = 0;
static uint32_t gAudioContinuityPCM = 0;

static PPC_FUNC(audioContinuityProbe) {
    if (gAudioContinuityCalls.load() == gAudioContinuityGateCall.load()) {
        SetEvent(gAudioContinuityEntered);
        WaitForSingleObject(gAudioContinuityRelease, 5000);
    }
    if (gAudioContinuityCalls.load() == 0) gAudioContinuityFirstSubmitMs.store(GetTickCount64());
    const uint32_t status = AudioRenderDriver::instance().submitFrame(
        memory->read32(gAudioContinuityOut), gAudioContinuityPCM);
    if (status) gAudioContinuityStatus.store(status);
    gAudioContinuityCalls.fetch_add(1);
    ctx.r3.u64 = 0;
}

static void testAudioContinuity() {
    auto& driver = AudioRenderDriver::instance();
    auto* base = memory->base();
    auto* original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    uint32_t token = 0;
    gAudioContinuityOut = memory->allocate(4);
    gAudioContinuityPCM = memory->allocate(kAudioSubmitBytes);
    gAudioContinuityEntered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    gAudioContinuityRelease = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    auto cleanup = [&] {
        if (gAudioContinuityRelease) SetEvent(gAudioContinuityRelease);
        if (token) driver.unregisterClient(token);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
        if (gAudioContinuityEntered) CloseHandle(gAudioContinuityEntered);
        if (gAudioContinuityRelease) CloseHandle(gAudioContinuityRelease);
        gAudioContinuityEntered = gAudioContinuityRelease = nullptr;
        if (gAudioContinuityOut) memory->release(gAudioContinuityOut);
        if (gAudioContinuityPCM) memory->release(gAudioContinuityPCM);
        gAudioContinuityOut = gAudioContinuityPCM = 0;
    };
    auto waitFor = [&](auto predicate) {
        const uint64_t deadline = GetTickCount64() + 5000;
        while (!predicate(driver.counters())) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(2);
        }
        return true;
    };
    try {
        check(gAudioContinuityOut && gAudioContinuityPCM && gAudioContinuityEntered && gAudioContinuityRelease,
              "Audio continuity fixture allocation failed");
        memset(base + gAudioContinuityPCM, 0, kAudioSubmitBytes);
        gAudioContinuityCalls.store(0);
        gAudioContinuityStatus.store(0);
        gAudioContinuityFirstSubmitMs.store(0);
        gAudioContinuityGateCall.store(kAudioPrefillBuffers - 1);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = audioContinuityProbe;
        const auto before = driver.counters();
        const uint32_t status = driver.registerClient(PPC_CODE_BASE, 0, gAudioContinuityOut);
        if (status) {
            printf("Audio continuity real-sink probe skipped: device unavailable 0x%08X.\n", status);
            cleanup();
            return;
        }
        token = memory->read32(gAudioContinuityOut);
        check(WaitForSingleObject(gAudioContinuityEntered, 5000) == WAIT_OBJECT_0,
              "Audio prefill callback did not reach gate");
        const auto priming = driver.counters();
        // A very slow test host may already have expired the independent timer.
        // Before its deadline the partial prefill must remain untouched.
        if (GetTickCount64() - gAudioContinuityFirstSubmitMs.load() < kAudioPrefillDeadlineMs) {
            check(!priming.playbackStarted && priming.queuedBuffers == kAudioPrefillBuffers - 1 &&
                  priming.queuedFrames == uint64_t(kAudioPrefillBuffers - 1) * kAudioFramesPerSubmit &&
                  priming.buffersCompleted == before.buffersCompleted,
                  "Audio consumed partial prefill before the startup deadline");
            check(priming.starvationPasses == before.starvationPasses,
                  "Stopped prefill was incorrectly counted as source starvation");
        }
        // Do not release the producer. Sparse queued audio must start/drain on
        // the deadline even though the next guest callback is still blocked.
        check(waitFor([&](const AudioDriverCounters& c) {
                  return c.playbackStarted && c.queuedBuffers == 0 &&
                         c.buffersCompleted == before.buffersCompleted + kAudioPrefillBuffers - 1;
              }), "Sparse prefill deadlocked behind a blocked guest callback");
        SetEvent(gAudioContinuityRelease);
        check(waitFor([&](const AudioDriverCounters& c) {
                  return c.playbackStarted && c.buffersCompleted >= before.buffersCompleted + 16;
              }), "Prefilled audio never started/completed");

        // Hold the producer while the device runs. All queued PCM must complete
        // without any producer-side lock or a new submission to release it.
        ResetEvent(gAudioContinuityEntered);
        gAudioContinuityGateCall.store(gAudioContinuityCalls.load() + kAudioPrefillBuffers);
        check(WaitForSingleObject(gAudioContinuityEntered, 5000) == WAIT_OBJECT_0,
              "Running producer did not reach starvation gate");
        const auto held = driver.counters();
        check(waitFor([&](const AudioDriverCounters& c) {
                  return c.queuedBuffers == 0 && c.starvationPasses > held.starvationPasses &&
                         c.starvationBytes > held.starvationBytes;
              }), "Drained source was not reported as starvation");
        const auto starved = driver.counters();
        check(starved.deviceErrors == held.deviceErrors,
              "Source shortage was incorrectly reported as an HRESULT error");
        SetEvent(gAudioContinuityRelease);
        check(waitFor([&](const AudioDriverCounters& c) {
                  return c.buffersCompleted >= starved.buffersCompleted + 16 && c.queuedBuffers > 0;
              }), "Producer lost batched completion credits after starvation");
        check(gAudioContinuityStatus.load() == 0, "Continuity probe submission failed");
        puts("Audio prefill retained PCM; forced source starvation measured separately from errors; stream resumed.");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

static void testAudioBufferRegions(uint32_t token, uint32_t planar) {
    auto& driver = AudioRenderDriver::instance();
    auto* base = memory->base();
    const uint32_t storage = memory->allocate(3 * 4096);
    check(storage != 0, "Audio span fixture allocation failed");
    const uint32_t samples = storage + 2048;
    memcpy(base + samples, base + planar, kAudioSubmitBytes);
    std::vector<float> expected(kAudioChannels * kAudioFramesPerSubmit);
    audioConvertPlanarBEFloat(base + planar, expected.data());
    try {
        for (DWORD protection : {PAGE_READONLY, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE}) {
            DWORD previous;
            check(VirtualProtect(base + storage + 4096, 4096, protection, &previous) != 0,
                  "Readable audio region setup failed");
            check(driver.submitFrame(token, samples) == 0,
                  "Audio rejected a fully readable PCM buffer spanning memory regions");
            std::vector<float> actual;
            check(audioDriverPeekLastSubmitted(actual) && actual == expected,
                  "Audio region boundary changed submitted PCM samples");
        }
        const auto before = driver.counters();
        for (DWORD protection : {DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
            DWORD previous;
            check(VirtualProtect(base + storage + 4096, 4096, protection, &previous) != 0,
                  "Inaccessible audio region setup failed");
            check(driver.submitFrame(token, samples) == 0xC000000Du,
                  "Audio accepted PCM spanning an inaccessible region");
            MEMORY_BASIC_INFORMATION info{};
            check(VirtualQuery(base + storage + 4096, &info, sizeof(info)) &&
                      info.Protect == protection,
                  "Audio submission touched the inaccessible region");
        }
        check(driver.submitFrame(token, 0xfffff800u) == 0xC000000Du,
              "Audio accepted PCM wrapping the guest address space");
        check(driver.counters().framesSubmitted == before.framesSubmitted,
              "Rejected audio spans queued partial frames");
    } catch (...) {
        memory->release(storage);
        throw;
    }
    check(memory->release(storage), "Audio span fixture cleanup failed");
    puts("Audio accepts complete readable spans across regions and rejects guarded spans before conversion.");
}
