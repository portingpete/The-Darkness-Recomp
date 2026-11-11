#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

// Opt-in guest file-read totals for the frame timeline (--frame-timeline).
// Disabled runs only test one startup-constant flag per NtReadFile call.
// Wall time covers the host read submission and its completion wait, so a
// read that blocks its calling thread is charged in full to that thread.
namespace DarkRecomp::Native::FrameIoCounters {
inline bool enabled = false; // Set before the guest thread starts.
inline std::atomic<uint32_t> engineThreadId{0};
struct Totals { uint64_t reads = 0, bytes = 0, nanoseconds = 0, engineReads = 0, engineNanoseconds = 0; };
inline std::atomic<uint64_t> reads{0}, bytes{0}, nanoseconds{0}, engineReads{0}, engineNanoseconds{0};
inline void record(uint32_t threadId, uint64_t readBytes, uint64_t elapsedNs) noexcept {
    reads.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(readBytes, std::memory_order_relaxed);
    nanoseconds.fetch_add(elapsedNs, std::memory_order_relaxed);
    if (threadId && threadId == engineThreadId.load(std::memory_order_relaxed)) {
        engineReads.fetch_add(1, std::memory_order_relaxed);
        engineNanoseconds.fetch_add(elapsedNs, std::memory_order_relaxed);
    }
}
inline Totals snapshot() noexcept {
    return {reads.load(std::memory_order_relaxed), bytes.load(std::memory_order_relaxed),
            nanoseconds.load(std::memory_order_relaxed), engineReads.load(std::memory_order_relaxed),
            engineNanoseconds.load(std::memory_order_relaxed)};
}
inline int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
