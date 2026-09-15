#pragma once
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "ppc_context.h"

namespace DarkRecomp { class CDisplayContextD3D11; }

namespace DarkRecomp::Native {
class XmaBridge;
constexpr uint64_t kTimebaseFrequency = 49875000;
class Memory {
public:
    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    uint8_t* base() const { return base_; }
    const std::filesystem::path& gameDirectory() const { return gameDir_; }
    bool commit(uint32_t address, uint32_t size);
    uint32_t allocate(uint32_t size, uint32_t alignment = 4096,
                      uint32_t regionBase = 0x10000000, uint64_t regionEnd = 0x60000000);
    uint32_t allocationSize(uint32_t address);
    uint32_t allocatedBytes();
    bool release(uint32_t address);
    bool releaseContaining(uint32_t address);
    static uint32_t physicalAddress(uint32_t address);
    static constexpr uint32_t cAliasBegin = 0xc0000000u;
    static constexpr uint32_t cAliasEnd = 0xe0000000u;
    static constexpr uint32_t cViewBytes = 0x01000000u;
    static constexpr uint32_t cViewEnd(uint32_t address) {
        return (address & ~(cViewBytes - 1)) + cViewBytes;
    }
    // C uses separate views of the same backing. Native callers of raw
    // VirtualProtect must split C spans at cViewEnd; page-local calls remain
    // valid. Memory's commit/release split internally; A/E are unchanged.
    void load(const std::filesystem::path& gameDir);
    void initThread(PPCContext& ctx);
    void initThreadStorage(uint32_t pcr, uint32_t stackLow, uint32_t stackSize, uint32_t id);
    uint32_t dynamicTls(const PPCContext& ctx) const;
    void clearDynamicTls(uint32_t index);
    static constexpr uint32_t threadObjectOffset = 0xd00;
    uint32_t headerField(uint32_t key) const;
    static constexpr uint32_t xexHeader = 0x81000000;
    uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    void startTimestamp(uint32_t storage);
    void stopTimestamp();
    uint32_t xmaCreate();
    bool xmaFree(uint32_t context);
    uint32_t xmaPoolBase();
    const char* xmaDecode(uint32_t context);
    // Ordered decoding under the memory lock. First error names its context;
    // earlier progress is retained and later contexts are not decoded.
    const char* xmaDecodeBatch(std::span<const uint32_t> contexts, uint32_t& failedContext);
    // Caller has validated the staged record span. Read each +64 context ID
    // immediately before decoding it, since earlier PCM may alias later IDs.
    const char* xmaDecodeRecords(uint32_t records, uint32_t count, uint32_t& failedContext);
    bool xmaOwned(uint32_t context);
    void xmaReset(uint32_t context);
private:
    static uint32_t millisSince(uint64_t startQpc, uint64_t frequency, uint64_t nowQpc);
    void writeTimestamp(uint32_t storage, uint32_t millis);
    void stopTimestampLocked();
    bool protectPhysical(uint32_t offset, uint32_t size, DWORD protection);
    void releaseAddressSpace();
    void forgetThreadStorage(uint32_t address, uint32_t size);
    uint8_t* base_ = nullptr;
    std::vector<std::pair<uint32_t, bool>> memoryViews_;
    std::filesystem::path gameDir_;
    uint32_t headerSize_ = 0;
    std::vector<uint8_t> tlsInitial_;
    std::map<uint32_t, uint32_t> allocations_;
    std::map<uint32_t, uint32_t> threadTls_;
    std::recursive_mutex mutex_;
    uint32_t xmaPool_ = 0;
    bool xmaUsed_[320] = {};
    std::unique_ptr<XmaBridge> xmaBridge_;
    std::thread timestampThread_;
    std::atomic<bool> timestampStop_{false};
    uint64_t timestampEpochQpc_ = 0;
    uint64_t timestampFreq_ = 0;
    bool timestampArmed_ = false;
};
extern Memory* memory;
extern thread_local PPCContext* currentContext;
// Host teardown unwinds a blocked guest callback without returning guest success.
struct DispatcherCancellation { bool requested = false; }; // guarded by dispatcher mutex
struct DispatcherWaitCancelled {};
void setDispatcherCancellation(DispatcherCancellation* cancellation);
void cancelDispatcherWaits(DispatcherCancellation& cancellation);
// Read-only inspection of registered, unsatisfied waits on a guest object.
uint32_t dispatcherWaiterCount(uint8_t* base, uint32_t address);
// Test synchronization after a blocked wait wakes, with the dispatcher unlocked.
// Thread-local and unset during normal execution; callbacks must not throw.
using DispatcherWaitResumeHook = void (*)(void*) noexcept;
void setDispatcherWaitResumeHook(DispatcherWaitResumeHook hook, void* context);
void initializeKernel();
// Optional worker profiler snapshot: at most 64 live registered guest threads.
// Caller owns/closes the duplicate handles; no registry lock survives this call.
// Threads can exit after the snapshot. Does not suspend or modify guest state.
std::vector<HANDLE> nativeThreadSampleHandles();
int runGuest(PPCContext& ctx, uint8_t* base);
int runGuestWithEntry(PPCContext& ctx, uint8_t* base, PPCFunc* entry);
void setNativeDisplayContext(DarkRecomp::CDisplayContextD3D11* display);
void nativeDisplayPresent();
void printContext(const PPCContext& ctx);
LONG exceptionFilter(EXCEPTION_POINTERS* exception);
}
