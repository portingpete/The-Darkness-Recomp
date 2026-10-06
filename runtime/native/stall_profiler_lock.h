#pragma once
#include "stall_profiler.h"
#include <mutex>

namespace DarkRecomp::Native::StallProfiler {
// Observe the existing acquisition once; never poll, try-lock or change its
// scheduling. The returned lock has the same ownership/unlock lifetime.
template<class Mutex>
std::unique_lock<Mutex> lock(Mutex& mutex, const char* function) {
    std::unique_lock<Mutex> held(mutex, std::defer_lock);
    {
        Scope wait(Section::Wait, function, 0, 0,
            reinterpret_cast<uintptr_t>(&mutex), "host-mutex");
        held.lock();
    }
    return held;
}
}
