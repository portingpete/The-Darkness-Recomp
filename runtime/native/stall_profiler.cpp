#include "stall_profiler.h"
#if DARK_STALL_PROFILER
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace DarkRecomp::Native::StallProfiler {
std::atomic<bool> enabledFlag{false};
namespace {
constexpr size_t sections = size_t(Section::Count), threadLimit = 128;
constexpr size_t eventCapacity = 128, frameCapacity = 8, topCount = 6;
using Totals = std::array<uint64_t, sections>;
struct Event {
    uint64_t begin = 0, end = 0, self = 0, object = 0;
    const char* function = nullptr;
    const char* objectKind = nullptr;
    uint32_t thread = 0, pc = 0, caller = 0;
    Section section = Section::Other;
    bool pending = false;
    bool segment = false;
};
struct Frame {
    uint64_t begin = 0, end = 0, serial = 0, dropped = 0, incomplete = 0;
    const char* name = nullptr;
    uint32_t thread = 0;
    Totals local{}, workers{};
    // Reserve the frame thread's active leaf separately: idle worker waits
    // must never crowd the critical path out of the contributor report.
    std::array<Event, topCount + 1> active{};
};
template<class T, size_t N> struct Queue {
    std::array<T, N> items{};
    std::atomic<uint64_t> written{0}, read{0};
    size_t pending() const noexcept {
        return size_t((std::min)(uint64_t(N),
            written.load(std::memory_order_acquire) - read.load(std::memory_order_relaxed)));
    }
    bool push(const T& item) noexcept {
        const auto w = written.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) == N) return false;
        items[w % N] = item;
        written.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& item) noexcept {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == written.load(std::memory_order_acquire)) return false;
        item = items[r % N];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
};
struct Buffer {
    // All snapshot fields are atomic: even an unsuccessful seqlock read has
    // no data race. Only the owner writes; the reporter never takes its locks.
    std::atomic<uint32_t> owner{0}, version{0};
    std::array<std::atomic<uint64_t>, sections> totals{};
    std::atomic<uint64_t> since{0}, callBegin{0}, object{0}, dropped{0};
    std::atomic<int> section{-1};
    std::atomic<const char*> function{nullptr}, objectKind{nullptr};
    std::atomic<uint32_t> pc{0}, caller{0};
    Queue<Event, eventCapacity> events;
    Queue<Frame, frameCapacity> frames;
};
std::array<Buffer, threadLimit> buffers;
std::atomic<uint64_t> unregistered{0};
uint64_t frequency = 0, callThreshold = 0, frameThreshold = 0;
HANDLE stopEvent = nullptr;
std::thread reporter;
std::atomic_flag stopping = ATOMIC_FLAG_INIT;
struct Local {
    Buffer* buffer = nullptr;
    bool attempted = false;
    Scope* scope = nullptr;
    uint64_t frameStart = 0, serial = 0;
    std::array<Totals, threadLimit> previous{};
    std::array<bool, threadLimit> gap{};
};
thread_local Local local;
uint64_t now() noexcept {
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
}
Buffer* getBuffer() noexcept {
    if (local.attempted) return local.buffer;
    local.attempted = true;
    const auto tid = GetCurrentThreadId();
    for (auto& buffer : buffers) {
        uint32_t empty = 0;
        if (buffer.owner.compare_exchange_strong(empty, tid, std::memory_order_relaxed))
            return local.buffer = &buffer;
    }
    unregistered.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
void charge(Buffer& buffer, uint64_t time) noexcept {
    const auto category = buffer.section.load(std::memory_order_relaxed);
    const auto since = buffer.since.load(std::memory_order_relaxed);
    if (category >= 0 && time >= since) {
        auto& total = buffer.totals[size_t(category)];
        total.store(total.load(std::memory_order_relaxed) + time - since, std::memory_order_relaxed);
        // Record contiguous exclusive intervals for exact cross-frame ranking.
        // The separately queued completed call retains full inclusive/self time.
        if (time - since > callThreshold) {
            Event segment{since, time, time - since, buffer.object.load(std::memory_order_relaxed),
                buffer.function.load(std::memory_order_relaxed), buffer.objectKind.load(std::memory_order_relaxed),
                buffer.owner.load(std::memory_order_relaxed), buffer.pc.load(std::memory_order_relaxed),
                buffer.caller.load(std::memory_order_relaxed), Section(category), false, true};
            if (!buffer.events.push(segment)) buffer.dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    buffer.since.store(time, std::memory_order_relaxed);
}
struct Snapshot { Totals totals{}; Event active{}; };
bool snapshot(const Buffer& buffer, uint64_t time, Snapshot& result) noexcept {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto v = buffer.version.load(std::memory_order_acquire);
        if (v & 1) continue;
        for (size_t i = 0; i < sections; ++i)
            result.totals[i] = buffer.totals[i].load(std::memory_order_relaxed);
        const int category = buffer.section.load(std::memory_order_relaxed);
        const auto since = buffer.since.load(std::memory_order_relaxed);
        result.active = {};
        if (category >= 0) {
            if (time >= since) result.totals[size_t(category)] += time - since;
            // since marks the active leaf interval, not the parent's lifetime;
            // nested work completed earlier cannot inflate this contributor.
            result.active = {since, time, time >= since ? time - since : 0,
                buffer.object.load(std::memory_order_relaxed),
                buffer.function.load(std::memory_order_relaxed),
                buffer.objectKind.load(std::memory_order_relaxed),
                buffer.owner.load(std::memory_order_relaxed),
                buffer.pc.load(std::memory_order_relaxed), buffer.caller.load(std::memory_order_relaxed),
                Section(category), true};
        }
        // Pair all relaxed snapshot reads with the next version check.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (v == buffer.version.load(std::memory_order_relaxed)) return true;
    }
    return false;
}
const char* sectionName(Section section) noexcept {
    constexpr const char* names[] = {"guest", "rendering", "audio", "file_io", "wait", "present", "other"};
    return names[size_t(section)];
}
double ms(uint64_t ticks) noexcept { return double(ticks) * 1000.0 / double(frequency); }

// The launcher redirects stdout and stderr to the same handle, but their CRT
// buffers and locks are independent. Assemble complete bounded batches before
// acquiring either stream lock; ranking/formatting must never block a guest
// diagnostic writer. Only the background reporter writes these statistics.
struct OutputStatistics {
    uint64_t batches = 0, bytes = 0, maximumBytes = 0;
    uint64_t maximumFlush = 0, maximumLockWait = 0, maximumHold = 0;
    uint64_t oversizedRecords = 0;
} outputStatistics;
class OutputBatch {
    static constexpr size_t limit = 32 * 1024;
    std::array<char, limit> data_;
    size_t bytes_ = 1;
public:
    OutputBatch() noexcept { data_[0] = '\n'; }
    ~OutputBatch() { flush(); }
    void flush() noexcept {
        if (bytes_ == 1) return;
        const auto begin = now();
        _lock_file(stdout);
        _lock_file(stderr);
        const auto acquired = now();
        std::fflush(stdout);
        std::fflush(stderr);
        // The leading newline also separates a legacy diagnostic's unfinished
        // prefix. No partially buffered profiler record survives the unlock.
        std::fwrite(data_.data(), 1, bytes_, stderr);
        std::fflush(stderr);
        const auto finished = now();
        _unlock_file(stderr);
        _unlock_file(stdout);
        ++outputStatistics.batches;
        outputStatistics.bytes += bytes_;
        outputStatistics.maximumBytes = (std::max)(outputStatistics.maximumBytes, uint64_t(bytes_));
        outputStatistics.maximumFlush = (std::max)(outputStatistics.maximumFlush, finished - begin);
        outputStatistics.maximumLockWait = (std::max)(outputStatistics.maximumLockWait, acquired - begin);
        outputStatistics.maximumHold = (std::max)(outputStatistics.maximumHold, finished - acquired);
        bytes_ = 1;
    }
    template<class... Args> void print(const char* format, Args... args) noexcept {
        // Reporter stack storage only; names are static metadata, but handle a
        // malformed oversized name without truncating a record mid-field.
        std::array<char, limit> line;
        const int written = std::snprintf(line.data(), line.size(), format, args...);
        if (written <= 0) return;
        size_t length = size_t(written);
        if (length >= limit) {
            ++outputStatistics.oversizedRecords;
            length = size_t(std::snprintf(line.data(), line.size(),
                "[StallProfiler] oversized_output_record required_bytes=%d\n", written));
        }
        if (bytes_ + length > limit) flush();
        std::memcpy(data_.data() + bytes_, line.data(), length);
        bytes_ += length;
    }
};

void printEvent(OutputBatch& output, const Event& event, const char* prefix, uint64_t frame = 0, uint64_t overlap = 0,
                const char* stream = nullptr, const char* group = nullptr) {
    char suffix[256]{};
    if (frame) std::snprintf(suffix, sizeof(suffix), " frame=%llu stream=%s contributor_group=%s overlap_ms=%.3f",
        frame, stream, group, ms(overlap));
    output.print("%s thread=%u guest_pc=0x%08X caller=0x%08X function=%s section=%s duration_ms=%.3f self_ms=%.3f object_kind=%s object=0x%llX pending=%u%s\n",
        prefix, event.thread, event.pc, event.caller, event.function ? event.function : "unknown",
        sectionName(event.section), ms(event.end - event.begin), ms(event.self),
        event.objectKind ? event.objectKind : "unavailable", event.object, unsigned(event.pending), suffix);
}
uint64_t overlap(const Event& event, const Frame& frame) noexcept {
    const auto first = (std::max)(event.begin, frame.begin), last = (std::min)(event.end, frame.end);
    return last > first ? last - first : 0;
}
void reportFrame(OutputBatch& output, const Frame& frame, const std::array<Event, 1024>& history, size_t historyCount) {
    const auto elapsed = frame.end - frame.begin;
    if (elapsed <= frameThreshold) return;
    output.print("[STALL] frame=%llu stream=%s thread=%u total_ms=%.3f budget_ms=16.670 dropped=%llu incomplete_thread_intervals=%llu unregistered_threads=%llu\n",
        frame.serial, frame.name, frame.thread, ms(elapsed), frame.dropped,
        frame.incomplete,
        unregistered.load(std::memory_order_relaxed));
    for (unsigned group = 0; group < 2; ++group) {
        const auto& totals = group ? frame.workers : frame.local;
        std::array<size_t, sections> order{};
        for (size_t i = 0; i < sections; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return totals[a] > totals[b]; });
        char line[1024]{};
        size_t used = size_t(std::snprintf(line, sizeof(line), "[STALL] frame=%llu stream=%s contributors=%s (exclusive wall time%s)",
            frame.serial, frame.name, group ? "workers" : "frame_thread", group ? "; concurrent threads sum" : ""));
        used = (std::min)(used, sizeof(line) - 1);
        for (auto i : order) {
            const auto added = std::snprintf(line + used, sizeof(line) - used, " %s=%.3fms", sectionName(Section(i)), ms(totals[i]));
            if (added > 0) used = (std::min)(used + size_t(added), sizeof(line) - 1);
        }
        output.print("%s\n", line);
    }
    for (unsigned group = 0; group < 2; ++group) {
    std::array<Event, topCount> best{};
    auto score = [&](const Event& event) {
        return overlap(event, frame); // history contains exact exclusive intervals
    };
    auto offer = [&](const Event& event) {
        if (!event.function || !overlap(event, frame)) return;
        if ((event.thread != frame.thread) != bool(group)) return;
        // The reporter may see a completed interval and its earlier in-flight
        // snapshot in the same batch. Keep only one contributor for that span.
        for (const auto& entry : best)
            if (entry.function && entry.thread == event.thread && entry.begin == event.begin) return;
        for (auto& entry : best) {
            if (!entry.function || score(event) > score(entry)) {
                for (size_t i = topCount - 1; i > size_t(&entry - best.data()); --i) best[i] = best[i - 1];
                entry = event; break;
            }
        }
    };
    // Preserve the in-progress state at the actual frame boundary, even if
    // this interval completed before the background writer read the queues.
    for (const auto& event : frame.active) offer(event);
    for (size_t i = 0; i < historyCount; ++i) if (history[i].segment) offer(history[i]);
    for (const auto& event : best) if (event.function && score(event) > callThreshold)
        printEvent(output, event, event.section == Section::Wait ? "[WAIT] contributor" : "[STALL] contributor", frame.serial, overlap(event, frame), frame.name, group ? "workers" : "frame_thread");
    }
}
void writer() noexcept {
    std::array<Event, 1024> history{};
    size_t cursor = 0, count = 0;
    std::vector<Frame> frames;
    try { frames.reserve(threadLimit * frameCapacity); } catch (...) { return; }
    for (;;) {
        const bool stopping = WaitForSingleObject(stopEvent, 25) == WAIT_OBJECT_0;
        OutputBatch output;
        for (auto& buffer : buffers) {
            Event event;
            const auto budget = buffer.events.pending();
            for (size_t n = 0; n < budget && buffer.events.pop(event); ++n) {
                if (!event.segment) printEvent(output, event, event.section == Section::Wait ? "[WAIT]" : "[STALL] call");
                history[cursor++ % history.size()] = event;
                count = (std::min)(history.size(), count + 1);
            }
        }
        frames.clear();
        for (auto& buffer : buffers) {
            Frame frame;
            const auto budget = buffer.frames.pending();
            for (size_t n = 0; n < budget && buffer.frames.pop(frame); ++n) frames.push_back(frame);
        }
        for (const auto& frame : frames) reportFrame(output, frame, history, count);
        if (stopping) {
            output.flush();
            output.print("[StallProfiler] reporter batches=%llu bytes=%llu max_batch_bytes=%llu max_batch_flush_ms=%.3f max_stdio_wait_ms=%.3f max_stdio_hold_ms=%.3f oversized_output_records=%llu\n",
                outputStatistics.batches, outputStatistics.bytes, outputStatistics.maximumBytes,
                ms(outputStatistics.maximumFlush), ms(outputStatistics.maximumLockWait),
                ms(outputStatistics.maximumHold), outputStatistics.oversizedRecords);
            break;
        }
    }
}
}
void Scope::begin(Section section, const char* function, uint32_t pc,
                  uint32_t caller, uint64_t object, const char* objectKind) noexcept {
    auto* buffer = getBuffer(); if (!buffer) return;
    buffer_ = buffer; parent_ = local.scope;
    section_ = section; function_ = function; object_ = object; objectKind_ = objectKind;
    guestPc_ = pc ? pc : parent_ ? parent_->guestPc_ : 0;
    caller_ = caller ? caller : parent_ ? parent_->caller_ : 0;
    start_ = now();
    buffer->version.fetch_add(1, std::memory_order_acq_rel);
    charge(*buffer, start_);
    buffer->section.store(int(section_), std::memory_order_relaxed);
    buffer->callBegin.store(start_, std::memory_order_relaxed);
    buffer->function.store(function_, std::memory_order_relaxed);
    buffer->objectKind.store(objectKind_, std::memory_order_relaxed);
    buffer->object.store(object_, std::memory_order_relaxed);
    buffer->pc.store(guestPc_, std::memory_order_relaxed);
    buffer->caller.store(caller_, std::memory_order_relaxed);
    local.scope = this;
    buffer->version.fetch_add(1, std::memory_order_release);
}
void Scope::end() noexcept {
    auto& buffer = *static_cast<Buffer*>(buffer_);
    const auto end = now(), elapsed = end - start_;
    buffer.version.fetch_add(1, std::memory_order_acq_rel);
    charge(buffer, end);
    buffer.section.store(parent_ ? int(parent_->section_) : -1, std::memory_order_relaxed);
    buffer.callBegin.store(parent_ ? parent_->start_ : 0, std::memory_order_relaxed);
    buffer.function.store(parent_ ? parent_->function_ : nullptr, std::memory_order_relaxed);
    buffer.objectKind.store(parent_ ? parent_->objectKind_ : nullptr, std::memory_order_relaxed);
    buffer.object.store(parent_ ? parent_->object_ : 0, std::memory_order_relaxed);
    buffer.pc.store(parent_ ? parent_->guestPc_ : 0, std::memory_order_relaxed);
    buffer.caller.store(parent_ ? parent_->caller_ : 0, std::memory_order_relaxed);
    local.scope = parent_;
    if (parent_) parent_->children_ += elapsed;
    buffer.version.fetch_add(1, std::memory_order_release);
    if (elapsed > callThreshold) {
        Event event{start_, end, elapsed >= children_ ? elapsed - children_ : 0, object_,
            function_, objectKind_, buffer.owner.load(std::memory_order_relaxed), guestPc_, caller_, section_};
        if (!buffer.events.push(event)) buffer.dropped.fetch_add(1, std::memory_order_relaxed);
    }
}
void initializeFromEnvironment() noexcept {
    if (enabled() || reporter.joinable()) return;
    wchar_t setting[8]{};
    if (GetEnvironmentVariableW(L"DARKRECOMP_STALL_PROFILE", setting, 8) != 1 || setting[0] != L'1') return;
    LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); frequency = uint64_t(value.QuadPart);
    if (!frequency) return;
    callThreshold = frequency * 2 / 1000;
    frameThreshold = frequency * 1667 / 100000;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return;
    try { reporter = std::thread(writer); }
    catch (...) { CloseHandle(stopEvent); stopEvent = nullptr; return; }
    std::fprintf(stderr, "[StallProfiler] enabled frame_budget_ms=16.670 call_threshold_ms=2.000 threads=128 deferred_log=1\n");
    enabledFlag.store(true, std::memory_order_release);
}
void frameBoundary(const char* name) noexcept {
    if (!enabled()) return;
    auto* buffer = getBuffer(); if (!buffer) return;
    const auto time = now();
    Frame frame; frame.begin = local.frameStart; frame.end = time;
    frame.name = name; frame.serial = ++local.serial;
    frame.thread = buffer->owner.load(std::memory_order_relaxed);
    size_t activeCount = 0;
    for (size_t n = 0; n < buffers.size(); ++n) {
        auto& other = buffers[n];
        if (!other.owner.load(std::memory_order_relaxed)) continue;
        Snapshot state;
        if (!snapshot(other, time, state)) {
            local.gap[n] = true; ++frame.incomplete; continue;
        }
        auto& totals = &other == buffer ? frame.local : frame.workers;
        if (local.gap[n]) {
            // Rebase instead of charging the missed interval to the next frame.
            local.previous[n] = state.totals;
            local.gap[n] = false; ++frame.incomplete;
        }
        for (size_t i = 0; i < sections; ++i) {
            // Reader may previously have extrapolated just beyond a writer's
            // earlier timestamp. Clamp tiny negative deltas, retain watermark.
            if (state.totals[i] >= local.previous[n][i]) {
                totals[i] += state.totals[i] - local.previous[n][i];
                local.previous[n][i] = state.totals[i];
            }
        }
        frame.dropped += other.dropped.load(std::memory_order_relaxed);
        if (state.active.function && state.active.section != Section::Other &&
            time > state.active.begin && time - state.active.begin > callThreshold) {
            auto event = state.active;
            event.self = time - (std::max)(event.begin, frame.begin);
            if (&other == buffer) { frame.active[0] = event; continue; }
            size_t at = (std::min)(++activeCount, topCount);
            if (activeCount > topCount && frame.active[at].self >= event.self) continue;
            frame.active[at] = event;
            while (at > 1 && frame.active[at].self > frame.active[at - 1].self) {
                std::swap(frame.active[at], frame.active[at - 1]); --at;
            }
        }
    }
    local.frameStart = time;
    // Fast guest boundaries can run far more often than presentation. They
    // update the baseline but must not fill the queue needed by slow frames.
    if (frame.begin && time - frame.begin > frameThreshold && !buffer->frames.push(frame))
        buffer->dropped.fetch_add(1, std::memory_order_relaxed);
}
void shutdown() noexcept {
    enabledFlag.store(false, std::memory_order_release);
    // Fatal guest, timeout and display paths can race during process teardown.
    if (stopping.test_and_set(std::memory_order_acq_rel)) return;
    if (reporter.joinable()) {
        SetEvent(stopEvent); reporter.join(); CloseHandle(stopEvent); stopEvent = nullptr;
    }
    stopping.clear(std::memory_order_release);
}
}
#endif
