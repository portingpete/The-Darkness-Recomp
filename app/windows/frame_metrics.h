#pragma once
#include <windows.h>
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

// Count new engine frames accepted with S_OK separately from repeated DXGI
// presents. Timestamps are Present returns, not measured display scanout.
// Fast presentation of a stale backbuffer must never be reported as game FPS.
// frameP95ms/frameP99ms/frameMaxms cover completed accepted-frame intervals
// only; 0 means no completed samples in the window. An unfinished trailing
// gap is reported separately as lastAcceptedAgeMs and never as a sample.
class FrameMetrics {
public:
    using Clock = std::chrono::steady_clock;
private:
    Clock::time_point start_ = Clock::now(), origin_ = start_, previous_{};
    bool haveAccepted_ = false;
    unsigned frames_ = 0, world_ = 0, presents_ = 0, rendered_ = 0;
    unsigned presentOk_ = 0, presentOccluded_ = 0, presentFailed_ = 0, presentOther_ = 0;
    double renderMs_ = 0, presentMs_ = 0;
    std::vector<double> intervals_;
    uint64_t guestCpu_=0,windowCpu_=0;
    bool haveThreadCpu_=false;
    Clock::time_point cpuStart_=Clock::now();
    static double pick(const std::vector<double>& sorted, double frac) {
        return sorted[size_t((sorted.size() - 1) * frac)];
    }
public:
    void threadCpu(HANDLE guest) {
        const auto sampleNow=Clock::now();
        const double seconds=ms(cpuStart_,sampleNow)/1000;cpuStart_=sampleNow;
        auto sample=[](HANDLE thread) {FILETIME creation{},exit{},kernel{},user{};
            if(!GetThreadTimes(thread,&creation,&exit,&kernel,&user))return uint64_t(0);
            return (uint64_t(kernel.dwHighDateTime)<<32|kernel.dwLowDateTime)+(uint64_t(user.dwHighDateTime)<<32|user.dwLowDateTime);};
        const auto game=sample(guest),window=sample(GetCurrentThread());
        if(haveThreadCpu_ && seconds > 1e-9)std::fprintf(stderr,"[ThreadCPU] engineCorePercent=%.1f windowCorePercent=%.1f\n",double(game-guestCpu_)/1e5/seconds,double(window-windowCpu_)/1e5/seconds);
        haveThreadCpu_=true;
        guestCpu_=game;windowCpu_=window;
    }
    static auto now() { return Clock::now(); }
    static double ms(Clock::time_point begin, Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    }
    static double ms(Clock::time_point begin) {
        return ms(begin, Clock::now());
    }
    void frame(bool world, double renderMs, Clock::time_point current) {
        if (haveAccepted_)
            intervals_.push_back(std::chrono::duration<double, std::milli>(current - previous_).count());
        previous_ = current; haveAccepted_ = true;
        ++frames_; world_ += world; renderMs_ += renderMs;
    }
    void rendered() { ++rendered_; }
    void present(HRESULT status, double presentMs) {
        ++presents_; presentMs_ += presentMs;
        if (status == S_OK) ++presentOk_;
        else if (status == DXGI_STATUS_OCCLUDED) ++presentOccluded_;
        else if (FAILED(status)) ++presentFailed_;
        else ++presentOther_;
    }
    struct Snapshot {
        double elapsed=0, fps=0, worldFps=0, presentsFps=0, renderedFps=0, renderCpuMs=0, presentCpuMs=0, p95=0, p99=0, maximum=0, lastAcceptedAgeMs=0;
        unsigned frames=0, presents=0, rendered=0, presentOk=0, presentOccluded=0, presentFailed=0, presentOther=0, intervalSamples=0;
        bool haveAcceptedFrame=false;
    };
    void reset(Clock::time_point now) {
        start_=now; origin_=now; previous_=Clock::time_point{}; haveAccepted_=false;
        frames_=world_=presents_=rendered_=0;
        presentOk_=presentOccluded_=presentFailed_=presentOther_=0;
        renderMs_=presentMs_=0; intervals_.clear();
    }
    double lastAcceptedAgeMs(Clock::time_point now) const {
        if (haveAccepted_)
            return now > previous_ ? ms(previous_, now) : 0;
        return now > origin_ ? ms(origin_, now) : 0;
    }
    Snapshot snapshot(Clock::time_point now) {
        Snapshot out;
        out.elapsed = ms(start_, now) / 1000.0;
        out.frames = frames_; out.presents = presents_; out.rendered = rendered_;
        out.presentOk = presentOk_; out.presentOccluded = presentOccluded_;
        out.presentFailed = presentFailed_; out.presentOther = presentOther_;
        out.haveAcceptedFrame = haveAccepted_;
        out.lastAcceptedAgeMs = lastAcceptedAgeMs(now);
        if (out.elapsed <= 0) return out;
        out.fps = frames_ / out.elapsed;
        out.worldFps = world_ / out.elapsed;
        out.presentsFps = presents_ / out.elapsed;
        out.renderedFps = rendered_ / out.elapsed;
        out.renderCpuMs = frames_ ? renderMs_ / frames_ : 0;
        out.presentCpuMs = presents_ ? presentMs_ / presents_ : 0;
        std::sort(intervals_.begin(), intervals_.end());
        out.intervalSamples = unsigned(intervals_.size());
        if (intervals_.empty()) return out;
        out.p95 = pick(intervals_, .95);
        out.p99 = pick(intervals_, .99);
        out.maximum = intervals_.back();
        return out;
    }
    bool report(double& fps, Clock::time_point now) {
        const double elapsed = ms(start_, now) / 1000.0;
        if (elapsed < 5.0) return false;
        Snapshot view = snapshot(now);
        fps = view.fps;
        std::fprintf(stderr, "[Performance] seconds=%.3f engineFPS=%.2f worldFPS=%.2f presentsFPS=%.2f renderedFPS=%.2f renderCPUms=%.3f presentCPUms=%.3f frameP95ms=%.3f frameP99ms=%.3f frameMaxms=%.3f presentOk=%u presentOccluded=%u presentFailed=%u presentOther=%u intervalSamples=%u lastAcceptedAgeMs=%.3f haveAcceptedFrame=%u\n",
                     view.elapsed, view.fps, view.worldFps, view.presentsFps, view.renderedFps,
                     view.renderCpuMs, view.presentCpuMs, view.p95, view.p99, view.maximum,
                     view.presentOk, view.presentOccluded, view.presentFailed, view.presentOther,
                     view.intervalSamples, view.lastAcceptedAgeMs, view.haveAcceptedFrame ? 1u : 0u);
        start_ = now; frames_ = world_ = presents_ = rendered_ = 0;
        presentOk_ = presentOccluded_ = presentFailed_ = presentOther_ = 0;
        renderMs_ = presentMs_ = 0; intervals_.clear();
        return true;
    }
    bool report(double& fps) {
        return report(fps, Clock::now());
    }
};

class FrameOutlierTrace {
public:
    using Clock = FrameMetrics::Clock;
    enum Stage { Pump = 0, Take, Render, Post, Pacer, Present, Tail, Count };
    static constexpr double kThresholdMs = 34.0;
    static constexpr unsigned kMaxLines = 256;
    static constexpr double kMinEmitGapMs = 1000.0;
    struct LoopStages {
        double pump = 0, take = 0, render = 0, post = 0, pacer = 0, present = 0;
    };
    struct Report {
        bool isOutlier = false;
        bool shouldLog = false;
        double intervalMs = 0, runElapsedMs = 0;
        unsigned serial = 0, loops = 0, miss = 0;
        bool world = false;
        unsigned presentOk = 0, presentOccluded = 0, presentFailed = 0, presentOther = 0;
        double sum[Count]{}, max[Count]{}, residualMs = 0;
        unsigned emitted = 0, suppressed = 0;
    };
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }
    void reset(Clock::time_point runOrigin) {
        runOrigin_ = runOrigin;
        havePrevAccepted_ = false;
        haveOpenPresent_ = false;
        haveEmit_ = false;
        prevAcceptedEnd_ = Clock::time_point{};
        lastPresentEnd_ = Clock::time_point{};
        lastEmit_ = Clock::time_point{};
        acceptedSerial_ = 0;
        emitted_ = 0;
        suppressed_ = 0;
        clearGap();
    }
    void onLoopTop(Clock::time_point loopTop) {
        if (!enabled_) return;
        if (!haveOpenPresent_) return;
        double tailMs = FrameMetrics::ms(lastPresentEnd_, loopTop);
        if (tailMs < 0) tailMs = 0;
        sum_[Tail] += tailMs;
        if (tailMs > max_[Tail]) max_[Tail] = tailMs;
        haveOpenPresent_ = false;
    }
    // Streaming can execute work without a DXGI call. Account for that loop
    // exactly once without inventing a presentation result or a frame.
    void onUnpresentedLoop(Clock::time_point loopEnd, const LoopStages& stages) {
        if (!enabled_) return;
        addStages(stages, false);
        ++loops_;
        lastPresentEnd_ = loopEnd;
        haveOpenPresent_ = true;
    }
    Report onPresent(Clock::time_point presentEnd, HRESULT status, const LoopStages& stages, bool isAccepted, bool world) {
        Report out;
        if (!enabled_) return out;
        addStages(stages, true);
        ++loops_;
        if (status == S_OK) ++presentOk_;
        else if (status == DXGI_STATUS_OCCLUDED) ++presentOccluded_;
        else if (FAILED(status)) ++presentFailed_;
        else ++presentOther_;
        lastPresentEnd_ = presentEnd;
        haveOpenPresent_ = true;
        if (!isAccepted) return out;
        ++acceptedSerial_;
        if (!havePrevAccepted_) {
            havePrevAccepted_ = true;
            prevAcceptedEnd_ = presentEnd;
            out.serial = acceptedSerial_;
            clearGap();
            return out;
        }
        double intervalMs = FrameMetrics::ms(prevAcceptedEnd_, presentEnd);
        if (intervalMs < 0) intervalMs = 0;
        double total = 0;
        for (int i = 0; i < int(Count); ++i) total += sum_[i];
        out.isOutlier = intervalMs > kThresholdMs;
        out.intervalMs = intervalMs;
        out.runElapsedMs = FrameMetrics::ms(runOrigin_, presentEnd);
        if (out.runElapsedMs < 0) out.runElapsedMs = 0;
        out.serial = acceptedSerial_;
        out.loops = loops_;
        out.miss = loops_ > 0 ? loops_ - 1 : 0;
        out.world = world;
        out.presentOk = presentOk_;
        out.presentOccluded = presentOccluded_;
        out.presentFailed = presentFailed_;
        out.presentOther = presentOther_;
        for (int i = 0; i < int(Count); ++i) {
            out.sum[i] = sum_[i];
            out.max[i] = max_[i];
        }
        out.residualMs = intervalMs - total;
        bool rateOk = !haveEmit_ || FrameMetrics::ms(lastEmit_, presentEnd) >= kMinEmitGapMs;
        if (out.isOutlier) {
            if (emitted_ < kMaxLines && rateOk) {
                out.shouldLog = true;
                ++emitted_;
                lastEmit_ = presentEnd;
                haveEmit_ = true;
            } else {
                ++suppressed_;
            }
        }
        out.emitted = emitted_;
        out.suppressed = suppressed_;
        prevAcceptedEnd_ = presentEnd;
        clearGap();
        return out;
    }
    unsigned emitted() const { return emitted_; }
    unsigned suppressed() const { return suppressed_; }
private:
    void addStages(const LoopStages& stages, bool presented) {
        const double values[]{stages.pump,stages.take,stages.render,stages.post,stages.pacer,
                              presented ? stages.present : 0};
        for (int i = 0; i <= int(Present); ++i) {
            const double value = (std::max)(0.0,values[i]);
            sum_[i] += value;
            max_[i] = (std::max)(max_[i],value);
        }
    }
    void clearGap() {
        loops_ = 0;
        presentOk_ = 0;
        presentOccluded_ = 0;
        presentFailed_ = 0;
        presentOther_ = 0;
        for (int i = 0; i < int(Count); ++i) sum_[i] = max_[i] = 0;
    }
    bool enabled_ = false;
    bool havePrevAccepted_ = false;
    bool haveOpenPresent_ = false;
    bool haveEmit_ = false;
    Clock::time_point runOrigin_{};
    Clock::time_point prevAcceptedEnd_{};
    Clock::time_point lastPresentEnd_{};
    Clock::time_point lastEmit_{};
    unsigned acceptedSerial_ = 0;
    unsigned loops_ = 0;
    unsigned presentOk_ = 0, presentOccluded_ = 0, presentFailed_ = 0, presentOther_ = 0;
    double sum_[Count]{}, max_[Count]{};
    unsigned emitted_ = 0, suppressed_ = 0;
};
