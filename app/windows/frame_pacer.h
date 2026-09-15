#pragma once
#include <windows.h>
#include <timeapi.h>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

class NativeTimerResolution {
    bool active_=false;
public:
    NativeTimerResolution():active_(timeBeginPeriod(1)==TIMERR_NOERROR) {}
    NativeTimerResolution(const NativeTimerResolution&)=delete;
    NativeTimerResolution& operator=(const NativeTimerResolution&)=delete;
    NativeTimerResolution(NativeTimerResolution&&)=delete;
    NativeTimerResolution& operator=(NativeTimerResolution&&)=delete;
    ~NativeTimerResolution() {stop();}
    void stop() {if(active_){timeEndPeriod(1);active_=false;}}
};

class NativeFramePacer {
public:
    using Clock = std::chrono::steady_clock;
private:
    HANDLE timer_ = nullptr;
    Clock::duration period_{};
    Clock::time_point deadline_ = Clock::now();
public:
    explicit NativeFramePacer(unsigned fps) { setFrameRate(fps); }
    void setFrameRate(unsigned fps) {
        const auto next = fps ? std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / fps)) : Clock::duration{};
        if (next == period_) return;
        if (fps && !timer_) {
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            if (!timer) throw std::runtime_error("Cannot create native frame timer");
            timer_ = timer;
        }
        if (timer_) CancelWaitableTimer(timer_);
        period_ = next;
        deadline_ = Clock::now();
    }
    NativeFramePacer(const NativeFramePacer&)=delete;
    NativeFramePacer& operator=(const NativeFramePacer&)=delete;
    NativeFramePacer(NativeFramePacer&&)=delete;
    NativeFramePacer& operator=(NativeFramePacer&&)=delete;
    ~NativeFramePacer() { if (timer_) CloseHandle(timer_); }
    bool capped() const { return period_.count() > 0; }
    Clock::duration period() const { return period_; }
    static Clock::time_point resolveOverrun(Clock::time_point ticked, Clock::time_point now, Clock::duration period) {
        if (ticked > now) return ticked;
        if (period.count() <= 0) return now;
        if (now - ticked >= period) return now;
        return ticked;
    }
    void wait() {
        if (!capped()) return;
        deadline_ += period_;
        const auto now = Clock::now();
        if (deadline_ <= now) { deadline_ = resolveOverrun(deadline_, now, period_); return; }
        LARGE_INTEGER due{};
        due.QuadPart = -(std::max)(int64_t(1), std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_ - now).count() / 100);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            throw std::runtime_error("Cannot arm native frame timer");
        if (WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0)
            throw std::runtime_error("Native frame wait failed");
    }
};
