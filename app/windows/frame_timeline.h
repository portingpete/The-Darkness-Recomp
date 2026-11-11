#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#endif

// Explicit --frame-timeline diagnostics: one CSV row per accepted frame, so a
// short run shows exactly which stage, thread or resource made a frame late.
// Rows are buffered on the display thread and written by a background thread
// roughly once per second; no per-frame disk I/O happens on a game thread.
namespace DarkRecomp::FrameTimeline {
struct Row {
    uint64_t frame = 0;          // accepted-frame serial ([FrameOutlier] serial)
    uint64_t gpuId = 0;          // GPU timer id, 0 when this frame was not measured
    uint64_t stallSerial = 0;    // [STALL] frame=N stream=display, when profiling
    double tMs = 0, intervalMs = 0;
    unsigned targetFps = 0;
    bool vsync = false, world = false, report = false;
    unsigned loops = 0, presentNotOk = 0, markers = 0;
    // Display-thread wall time, summed over every loop iteration in the interval.
    double pumpMs = 0, takeMs = 0, renderMs = 0, postMs = 0, pacerMs = 0, presentMs = 0, tailMs = 0, residualMs = 0;
    double takeMaxMs = 0;
    double displayCpuMs = -1, processCpuMs = -1;
    uint64_t pageFaults = 0;
    // Engine (guest producer) frame shown by this present.
    int64_t engineFrame = -1;
    double engineWallMs = -1, engineCpuMs = -1, enginePublishWaitMs = -1, engineBoundaryMs = -1, engineLatencyMs = -1;
    uint64_t engineParts = 0, engineCommands = 0, engineWorldDraws = 0;
    // Guest file reads completed during the interval (all threads / engine thread).
    uint64_t ioReads = 0, ioBytes = 0;
    double ioMs = 0, engineIoMs = 0;
    // Render-thread work during the interval.
    uint64_t draws = 0, imageUploads = 0, imageUploadBytes = 0, geometryUploads = 0, geometryUploadBytes = 0;
    uint64_t transientUploadBytes = 0, surfaceCreates = 0, newVertexShaders = 0, vertexShaderCompiles = 0, uploadDeferred = 0;
    double imageUploadMs = 0, geometryUploadMs = 0, transientUploadMs = 0, surfaceMs = 0, decodeMs = 0;
    double gpuMs = -1, gpuIdleBeforeMs = -1;
};

inline const char* csvHeader() noexcept {
    return "frame,t_ms,interval_ms,target_fps,vsync,world,loops,present_not_ok,markers,report,"
           "pump_ms,take_ms,take_max_ms,render_ms,post_ms,pacer_ms,present_ms,tail_ms,residual_ms,"
           "gpu_ms,gpu_idle_before_ms,display_cpu_ms,process_cpu_ms,page_faults,"
           "engine_frame,engine_wall_ms,engine_cpu_ms,engine_publish_wait_ms,engine_boundary_ms,engine_latency_ms,"
           "engine_parts,engine_commands,engine_world_draws,"
           "io_reads,io_kib,io_ms,engine_io_ms,"
           "draws,image_uploads,image_kib,image_ms,geometry_uploads,geometry_kib,geometry_ms,transient_kib,transient_ms,"
           "surface_creates,surface_ms,decode_ms,new_vs,vs_compiles,upload_deferred,stall_serial\n";
}

inline std::string formatRow(const Row& r) {
    char line[1536];
    const int count = std::snprintf(line, sizeof(line),
        "%llu,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%u,"
        "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
        "%.3f,%.3f,%.3f,%.3f,%llu,"
        "%lld,%.3f,%.3f,%.3f,%.3f,%.3f,"
        "%llu,%llu,%llu,"
        "%llu,%.1f,%.3f,%.3f,"
        "%llu,%llu,%.1f,%.3f,%llu,%.1f,%.3f,%.1f,%.3f,"
        "%llu,%.3f,%.3f,%llu,%llu,%llu,%llu\n",
        (unsigned long long)r.frame, r.tMs, r.intervalMs, r.targetFps, unsigned(r.vsync), unsigned(r.world),
        r.loops, r.presentNotOk, r.markers, unsigned(r.report),
        r.pumpMs, r.takeMs, r.takeMaxMs, r.renderMs, r.postMs, r.pacerMs, r.presentMs, r.tailMs, r.residualMs,
        r.gpuMs, r.gpuIdleBeforeMs, r.displayCpuMs, r.processCpuMs, (unsigned long long)r.pageFaults,
        (long long)r.engineFrame, r.engineWallMs, r.engineCpuMs, r.enginePublishWaitMs, r.engineBoundaryMs, r.engineLatencyMs,
        (unsigned long long)r.engineParts, (unsigned long long)r.engineCommands, (unsigned long long)r.engineWorldDraws,
        (unsigned long long)r.ioReads, double(r.ioBytes) / 1024.0, r.ioMs, r.engineIoMs,
        (unsigned long long)r.draws, (unsigned long long)r.imageUploads, double(r.imageUploadBytes) / 1024.0, r.imageUploadMs,
        (unsigned long long)r.geometryUploads, double(r.geometryUploadBytes) / 1024.0, r.geometryUploadMs,
        double(r.transientUploadBytes) / 1024.0, r.transientUploadMs,
        (unsigned long long)r.surfaceCreates, r.surfaceMs, r.decodeMs, (unsigned long long)r.newVertexShaders,
        (unsigned long long)r.vertexShaderCompiles, (unsigned long long)r.uploadDeferred, (unsigned long long)r.stallSerial);
    if (count <= 0 || size_t(count) >= sizeof(line)) return {};
    return std::string(line, size_t(count));
}

// Frame budget used for slow-frame counts: the active frame cap, else 60 Hz.
inline double budgetMs(unsigned targetFps) noexcept { return targetFps ? 1000.0 / targetFps : 1000.0 / 60.0; }
// A frame is "slow" once it costs at least half a budget extra (one dropped
// refresh at a fixed cap), and never below budget + 4 ms on very high caps.
inline double slowThresholdMs(unsigned targetFps) noexcept {
    const double budget = budgetMs(targetFps);
    return (std::max)(budget * 1.5, budget + 4.0);
}

// Running totals for the end-of-run [FrameDropSummary]. Pure; unit tested.
class SummaryAccumulator {
public:
    enum Stage { Take, Render, Present, Pacer, Tail, Pump, Gpu, StageCount };
    void add(const Row& r) {
        ++frames_; seconds_ = r.tMs / 1000.0;
        markers_ += r.markers;
        if (!r.world) return;
        intervals_.push_back(r.intervalMs);
        const double slow = slowThresholdMs(r.targetFps);
        if (r.intervalMs <= slow) return;
        ++slow_;
        if (r.intervalMs > 2.0 * budgetMs(r.targetFps)) ++over2x_;
        if (r.intervalMs > 50.0) ++over50_;
        if (r.intervalMs > 100.0) ++over100_;
        excessMs_ += r.intervalMs - budgetMs(r.targetFps);
        stage_[Take] += r.takeMs; stage_[Render] += r.renderMs + r.postMs; stage_[Present] += r.presentMs;
        stage_[Pacer] += r.pacerMs; stage_[Tail] += r.tailMs; stage_[Pump] += r.pumpMs;
        if (r.gpuMs >= 0) stage_[Gpu] += r.gpuMs;
        // Dominant display stage for this slow frame (GPU is reported separately).
        const double parts[] = {r.takeMs, r.renderMs + r.postMs, r.presentMs, r.pacerMs, r.tailMs, r.pumpMs};
        ++dominant_[size_t(std::max_element(std::begin(parts), std::end(parts)) - std::begin(parts))];
    }
    struct Result {
        uint64_t frames = 0, worldFrames = 0, slow = 0, over2x = 0, over50 = 0, over100 = 0, markers = 0;
        double seconds = 0, p50 = 0, p95 = 0, p99 = 0, maximum = 0, worldAverageMs = 0, excessMs = 0;
        double stage[StageCount]{};
        uint64_t dominant[StageCount - 1]{};
    };
    Result result() const {
        Result out;
        out.frames = frames_; out.worldFrames = intervals_.size(); out.slow = slow_; out.over2x = over2x_;
        out.over50 = over50_; out.over100 = over100_; out.markers = markers_; out.seconds = seconds_;
        out.excessMs = excessMs_;
        for (int i = 0; i < StageCount; ++i) out.stage[i] = stage_[i];
        for (int i = 0; i < StageCount - 1; ++i) out.dominant[i] = dominant_[i];
        if (intervals_.empty()) return out;
        auto sorted = intervals_;
        std::sort(sorted.begin(), sorted.end());
        auto pick = [&](double fraction) { return sorted[size_t(double(sorted.size() - 1) * fraction)]; };
        out.p50 = pick(.50); out.p95 = pick(.95); out.p99 = pick(.99); out.maximum = sorted.back();
        double total = 0; for (double v : sorted) total += v;
        out.worldAverageMs = total / double(sorted.size());
        return out;
    }
private:
    uint64_t frames_ = 0, slow_ = 0, over2x_ = 0, over50_ = 0, over100_ = 0, markers_ = 0;
    double seconds_ = 0, excessMs_ = 0;
    std::vector<double> intervals_;
    double stage_[StageCount]{};
    uint64_t dominant_[StageCount - 1]{};
};

inline std::string formatSummary(const SummaryAccumulator::Result& s, const char* prefix) {
    const double fps = s.worldAverageMs > 0 ? 1000.0 / s.worldAverageMs : 0;
    char first[768], second[768];
    std::snprintf(first, sizeof(first), "%s frames=%llu worldFrames=%llu seconds=%.1f worldAvgFps=%.2f p50Ms=%.3f p95Ms=%.3f p99Ms=%.3f maxMs=%.3f "
        "slowFrames=%llu over2xBudget=%llu over50ms=%llu over100ms=%llu excessMs=%.1f markers=%llu\n",
        prefix, (unsigned long long)s.frames, (unsigned long long)s.worldFrames, s.seconds, fps, s.p50, s.p95, s.p99, s.maximum,
        (unsigned long long)s.slow, (unsigned long long)s.over2x, (unsigned long long)s.over50, (unsigned long long)s.over100,
        s.excessMs, (unsigned long long)s.markers);
    using A = SummaryAccumulator;
    std::snprintf(second, sizeof(second), "%s slowFrameStageMs engineWait=%.1f render=%.1f present=%.1f pacer=%.1f tail=%.1f pump=%.1f gpu=%.1f "
        "dominant engineWait=%llu render=%llu present=%llu pacer=%llu tail=%llu pump=%llu\n",
        prefix, s.stage[A::Take], s.stage[A::Render], s.stage[A::Present], s.stage[A::Pacer], s.stage[A::Tail], s.stage[A::Pump],
        s.stage[A::Gpu], (unsigned long long)s.dominant[A::Take], (unsigned long long)s.dominant[A::Render],
        (unsigned long long)s.dominant[A::Present], (unsigned long long)s.dominant[A::Pacer],
        (unsigned long long)s.dominant[A::Tail], (unsigned long long)s.dominant[A::Pump]);
    return std::string(first) + second;
}

// Owns the CSV and its writer thread. append()/gpuResult()/close() are
// display-thread only; the writer thread only formats rows it was handed.
class Recorder {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t kHoldRows = 16;      // GPU results normally arrive within 2-3 frames.
    static constexpr size_t kBatchRows = 240;
    static constexpr double kHandoffMs = 1000.0;
    static constexpr size_t kMaxRows = 2000000;  // ~9 hours at 60 FPS; bounded memory and disk.

    explicit Recorder(const std::filesystem::path& path) : path_(path), origin_(Clock::now()) {
#ifdef _WIN32
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
        originQpc_ = uint64_t(counter.QuadPart); qpcFrequency_ = uint64_t(frequency.QuadPart);
        file_ = _wfopen(path.c_str(), L"wbx");
#else
        file_ = std::fopen(path.c_str(), "wbx");
#endif
        if (!file_) throw std::runtime_error("Frame timeline requires a new writable CSV path");
        std::setvbuf(file_, nullptr, _IOFBF, 1 << 16);
    }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder() { close(); }

    Clock::time_point origin() const { return origin_; }
    int64_t originSteadyNs() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(origin_.time_since_epoch()).count();
    }
    uint64_t originQpc() const { return originQpc_; }
    uint64_t qpcFrequency() const { return qpcFrequency_; }
    const std::filesystem::path& path() const { return path_; }

    // Free-form "# key=value" lines written before the column header.
    void comment(const std::string& text) {
        std::lock_guard lock(mutex_);
        if (headerWritten_) return;
        preamble_ += "# " + text + "\n";
    }
    void append(Row row) {
        if (closed_) return;
        if (accepted_ >= kMaxRows) { ++dropped_; return; }
        ++accepted_;
        pending_.push_back(std::move(row));
        while (pending_.size() > kHoldRows) retireFront();
        const double now = pending_.back().tMs;
        if (outgoing_.size() >= kBatchRows || (!outgoing_.empty() && now - lastHandoffMs_ >= kHandoffMs)) {
            handoff(); lastHandoffMs_ = now;
        }
    }
    // Late GPU timestamps for a row still held on the display thread. Results
    // for frames that were never accepted (occluded presents) stay unmatched.
    void gpuResult(uint64_t id, double gpuMs, double idleBeforeMs) {
        if (!id) return;
        for (auto it = pending_.rbegin(); it != pending_.rend(); ++it)
            if (it->gpuId == id) { it->gpuMs = gpuMs; it->gpuIdleBeforeMs = idleBeforeMs; return; }
        ++unmatchedGpu_;
    }
    SummaryAccumulator::Result summary() const { return summary_.result(); }
    // Writes every row, the summary, and joins the writer. Idempotent.
    void close() noexcept {
        if (closed_) return;
        closed_ = true;
        try {
            while (!pending_.empty()) retireFront();
            handoff();
            const auto result = summary_.result();
            char extra[256];
            std::snprintf(extra, sizeof(extra), "# rows=%llu dropped=%llu unmatched_gpu_results=%llu\n",
                (unsigned long long)accepted_, (unsigned long long)dropped_, (unsigned long long)unmatchedGpu_);
            ensureWriter();
            {
                std::lock_guard lock(mutex_);
                trailer_ = extra;
                trailerSummary_ = result;
                haveTrailer_ = true;
                stopping_ = true;
            }
            ready_.notify_one();
            if (writer_.joinable()) writer_.join();
            std::fputs(formatSummary(result, "[FrameDropSummary]").c_str(), stderr);
            std::fprintf(stderr, "[FrameTimeline] saved=%llu dropped=%llu unmatchedGpuResults=%llu writeFailures=%llu csv=%s\n",
                (unsigned long long)accepted_, (unsigned long long)dropped_, (unsigned long long)unmatchedGpu_,
                (unsigned long long)writeFailures_, reinterpret_cast<const char*>(path_.u8string().c_str()));
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[FrameTimeline] close failed: %s\n", error.what());
        }
        std::fflush(stderr);
    }

private:
    void retireFront() {
        summary_.add(pending_.front());
        outgoing_.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    // Started on the first handoff, after startup comments are complete.
    void ensureWriter() {
        if (writerStarted_) return;
        writerStarted_ = true;
        writer_ = std::thread([this] { writerLoop(); });
    }
    void handoff() {
        if (outgoing_.empty()) return;
        {
            std::lock_guard lock(mutex_);
            if (batch_.empty()) batch_.swap(outgoing_);
            else { batch_.insert(batch_.end(), outgoing_.begin(), outgoing_.end()); outgoing_.clear(); }
        }
        ensureWriter();
        ready_.notify_one();
    }
    void writerLoop() noexcept {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        std::vector<Row> work;
        std::string text;
        for (;;) {
            bool stop = false, trailer = false;
            SummaryAccumulator::Result trailerSummary;
            std::string trailerText;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !batch_.empty(); });
                if (!headerWritten_) { text = preamble_; text += csvHeader(); headerWritten_ = true; }
                work.swap(batch_);
                stop = stopping_;
                if (stop && haveTrailer_) { trailer = true; trailerText = trailer_; trailerSummary = trailerSummary_; }
            }
            try {
                for (const auto& row : work) text += formatRow(row);
                work.clear();
                if (trailer) {
                    // Repeat the summary in the CSV so the file stands alone.
                    text += trailerText;
                    text += formatSummary(trailerSummary, "# summary");
                }
                if (!text.empty()) {
                    if (std::fwrite(text.data(), 1, text.size(), file_) != text.size()) ++writeFailures_;
                    std::fflush(file_);
                }
            } catch (...) { ++writeFailures_; }
            text.clear();
            if (stop) break;
        }
        std::fclose(file_);
        file_ = nullptr;
    }

    std::filesystem::path path_;
    Clock::time_point origin_;
    uint64_t originQpc_ = 0, qpcFrequency_ = 0;
    std::FILE* file_ = nullptr;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<Row> batch_;
    std::string preamble_, trailer_;
    SummaryAccumulator::Result trailerSummary_;
    bool headerWritten_ = false, stopping_ = false, haveTrailer_ = false;
    // Display-thread state.
    std::deque<Row> pending_;
    std::vector<Row> outgoing_;
    SummaryAccumulator summary_;
    double lastHandoffMs_ = 0;
    uint64_t accepted_ = 0, dropped_ = 0, unmatchedGpu_ = 0;
    bool closed_ = false, writerStarted_ = false;
    uint64_t writeFailures_ = 0; // writer thread; read after join
};

#ifdef _WIN32
// Time-stamp counter cycles per 100 ns, measured against the steady clock
// over the first second of sampling; 0 until then. Thread and process cycle
// times count these cycles exactly, while GetThreadTimes/GetProcessTimes only
// advance in scheduler ticks (15.6 ms), too coarse for one frame.
inline double cyclesPer100ns() noexcept {
    static std::atomic<double> rate{0};
    static const uint64_t startCycles = __rdtsc();
    static const auto startTime = std::chrono::steady_clock::now();
    if (const double known = rate.load(std::memory_order_relaxed); known > 0) return known;
    const uint64_t cycles = __rdtsc();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    if (elapsed < 1'000'000'000 || cycles <= startCycles) return 0;
    const double measured = double(cycles - startCycles) / (double(elapsed) / 100.0);
    rate.store(measured, std::memory_order_relaxed);
    return measured;
}
// Cumulative host samples differenced between accepted frames.
struct HostSample {
    uint64_t displayCpu100ns = 0, processCpu100ns = 0, pageFaults = 0;
    bool valid = false;
    static HostSample take() noexcept {
        HostSample out;
        const double rate = cyclesPer100ns();
        ULONG64 threadCycles = 0, processCycles = 0;
        bool ok = rate > 0 && QueryThreadCycleTime(GetCurrentThread(), &threadCycles) != FALSE;
        out.displayCpu100ns = ok ? uint64_t(double(threadCycles) / rate) : 0;
        ok = ok && QueryProcessCycleTime(GetCurrentProcess(), &processCycles) != FALSE;
        out.processCpu100ns = ok ? uint64_t(double(processCycles) / rate) : 0;
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        ok = ok && GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) != FALSE;
        out.pageFaults = memory.PageFaultCount;
        out.valid = ok;
        return out;
    }
};
#endif
}
