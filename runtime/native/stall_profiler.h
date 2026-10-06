#pragma once
#include <atomic>
#include <cstdint>

#ifndef DARK_STALL_PROFILER
#define DARK_STALL_PROFILER 0
#endif

// Host wall-clock observations only. No guest clocks, scheduling, fences or
// frame pacing are changed. Names/objectKind must have static lifetime.
namespace DarkRecomp::Native::StallProfiler {
enum class Section : uint8_t { Guest, Rendering, Audio, FileIO, Wait, Present, Other, Count };

#if DARK_STALL_PROFILER
extern std::atomic<bool> enabledFlag;
inline bool enabled() noexcept { return enabledFlag.load(std::memory_order_acquire); }
class Scope {
public:
    Scope(Section section, const char* function, uint32_t guestPc = 0,
          uint32_t caller = 0, uint64_t object = 0, const char* objectKind = nullptr) noexcept {
        if (enabled()) begin(section, function, guestPc, caller, object, objectKind);
    }
    ~Scope() noexcept { if (buffer_) end(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    void begin(Section, const char*, uint32_t, uint32_t, uint64_t, const char*) noexcept;
    void end() noexcept;
    void* buffer_ = nullptr;
    Scope* parent_ = nullptr;
    const char* function_ = nullptr;
    const char* objectKind_ = nullptr;
    uint64_t start_ = 0, children_ = 0, object_ = 0;
    uint32_t guestPc_ = 0, caller_ = 0;
    Section section_ = Section::Other;
};
void initializeFromEnvironment() noexcept;
// One stream per calling thread. First boundary starts an interval; subsequent
// boundaries measure start-to-start wall time, including original pacing.
void frameBoundary(const char* name = "display") noexcept;
void shutdown() noexcept;
#else
inline bool enabled() noexcept { return false; }
class Scope {
public:
    constexpr Scope(Section, const char*, uint32_t = 0, uint32_t = 0,
                    uint64_t = 0, const char* = nullptr) noexcept {}
    constexpr ~Scope() noexcept {}
};
inline void initializeFromEnvironment() noexcept {}
inline void frameBoundary(const char* = "display") noexcept {}
inline void shutdown() noexcept {}
#endif
}
