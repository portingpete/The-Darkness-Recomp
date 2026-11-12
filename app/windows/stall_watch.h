#pragma once
#include <cstdint>

struct PPCContext;

// Opt-in engine-stall watchdog (DARKRECOMP_STALL_WATCH=<milliseconds>; unset or
// 0 leaves it off). When the engine thread has not finished a frame for that
// long, a watcher thread samples every process thread's Windows scheduling
// state (running / ready / waiting and why) and CPU time, plus the guest call
// stack of each registered guest thread, until the next engine frame. It then
// prints one [StallWatch] report. Sampling runs at high host priority and can
// perturb scheduling; with the variable unset each hook is a relaxed atomic
// load.
namespace DarkRecomp::Native::StallWatch {
bool enabled() noexcept;
void initializeFromEnvironment(uint8_t* base) noexcept;
// Called on the registering thread. ctx may be null (host-only threads).
void registerThread(const PPCContext* ctx, const char* role, uint32_t entry = 0, uint32_t argument = 0) noexcept;
void unregisterThread() noexcept;
// Engine (guest) frame boundary.
void engineFrame() noexcept;
void shutdown() noexcept;

// Guest stack walk used by the watcher: back chain at [sp], saved LR at
// [backChain - 8] (the PPC prologue's stw r12,-8(r1) before stwu). Returns the
// number of return addresses written. Exposed for tests.
unsigned walkGuestStack(const uint8_t* base, uint32_t sp, uint32_t* out, unsigned capacity) noexcept;
// Big-endian guest word read using the process memory API.
bool readGuestWord(const uint8_t* base, uint32_t address, uint32_t& value) noexcept;
// Exercises the same registration and metadata snapshot paths with a failed
// handle and a recycled slot; only call while the watcher is disabled.
bool testSlotLifecycle(const PPCContext* ctx) noexcept;
bool testFrameThreshold() noexcept;
bool testTrackAccounting() noexcept;
}
