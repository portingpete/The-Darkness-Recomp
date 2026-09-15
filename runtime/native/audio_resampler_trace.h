#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace DarkRecomp::Native {

// Opt-in original-resampler evidence. No allocation or file I/O on mixer
// threads. The audio owner arms it before guest startup. A writer persists the
// completed capture during play (the launcher's ExitProcess skips teardown).
// A closed-bit/refcount gate also protects against a still-finishing mixer.
class AudioResamplerTrace {
public:
    ~AudioResamplerTrace() { finish(); }
    struct Record {
        uint64_t qpc, nextSubmit;
        uint32_t thread, descriptor, caller, result;
        // Normal record: exact original BE resampler descriptors. caller==
        // 82828518 tags limiter metadata and BE PCM seam samples (see hook).
        uint8_t before[88], after[88];
        float first[6], last[6];      // Produced planar PCM, host float order.
    };
    static_assert(sizeof(Record) == 256);
    static constexpr uint32_t capacity = 262144; // 64 MiB, hard bound.
    static constexpr uint32_t closed = 0x80000000u;

    struct Call {
        AudioResamplerTrace* owner = nullptr;
        Record* record = nullptr;
        Call() = default;
        Call(const Call&) = delete;
        ~Call() { if (owner) owner->gate_.fetch_sub(1, std::memory_order_release); }
    };

    void initialize() noexcept {
        finish();
        mixStages_ = false;
        mixSources_ = false;
        sparse_ = false;
        refill_ = false;
        startFrame_ = 960000;
        wchar_t path[32768]{};
        DWORD n = GetEnvironmentVariableW(L"DARKRECOMP_RESAMPLER_TRACE", path, 32768);
        if (!n || n >= 32768) return;
        const bool absolute = (n > 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
                              (n > 2 && path[0] == L'\\' && path[1] == L'\\');
        if (!absolute) return;
        wchar_t mode[8]{};
        mixStages_ = GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_MIX_TRACE", mode, 8) == 1 && mode[0] == L'1';
        mixSources_ = GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_MIX_SOURCES", mode, 8) == 1 && mode[0] == L'1';
        sparse_ = GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_TRACE_SPARSE", mode, 8) == 1 && mode[0] == L'1';
        refill_ = GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_REFILL_TRACE", mode, 8) == 1 && mode[0] == L'1';
        // Move the same bounded capture past menu navigation when needed.
        // Parse only at startup, never on an audio thread. Invalid input keeps
        // the default 20..80 second capture and 45..55 second detail window.
        wchar_t start[16]{};
        const DWORD startLength = GetEnvironmentVariableW(L"DARKRECOMP_RESAMPLER_TRACE_START_SECONDS", start, 16);
        if (startLength && startLength <= 3) {
            uint32_t seconds = 0;
            bool valid = true;
            for (DWORD i = 0; i < startLength; ++i) {
                if (start[i] < L'0' || start[i] > L'9') { valid = false; break; }
                seconds = seconds * 10 + uint32_t(start[i] - L'0');
            }
            if (valid && seconds <= 240) startFrame_ = uint64_t(seconds) * 48000;
        }
        try { records_ = std::make_unique<Record[]>(capacity); }
        catch (...) { return; }
        file_ = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            records_.reset();
            std::fprintf(stderr, "[AudioResamplerTrace] CREATE_NEW failed error=%lu\n", GetLastError());
            return;
        }
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        frequency_ = uint64_t(frequency.QuadPart);
        count_.store(0);
        frames_.store(0);
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (done_) writer_ = CreateThread(nullptr, 0, writerEntry, this, 0, nullptr);
        if (!writer_) {
            std::fprintf(stderr, "[AudioResamplerTrace] writer startup failed error=%lu\n", GetLastError());
            finish();
            return;
        }
        gate_.store(0, std::memory_order_release);
        if (sparse_) std::fprintf(stderr, "[AudioResamplerTrace] sparse adjacent pairs: 2/64 blocks across full window\n");
        std::fprintf(stderr, "[AudioResamplerTrace] armed frames=%llu..%llu detail=%llu..%llu maxRecords=%u path=%ls\n",
                     startFrame_, startFrame_ + 2880000, startFrame_ + 1200000,
                     startFrame_ + 1680000, capacity, path);
    }

    void submitted(uint64_t sequence) noexcept {
        if (!(gate_.load(std::memory_order_relaxed) & closed)) {
            frames_.store(sequence * 256, std::memory_order_relaxed);
            if (sequence * 256 >= startFrame_ + 2880000) SetEvent(done_);
        }
    }

    void begin(Call& call, bool refillCall = false) noexcept {
        if (refill_ != refillCall) return;
        uint32_t gate = gate_.load(std::memory_order_acquire);
        do {
            if (gate & closed) return;
        } while (!gate_.compare_exchange_weak(gate, gate + 1, std::memory_order_acquire));
        call.owner = this;
        const uint64_t frame = frames_.load(std::memory_order_relaxed);
        if (frame < startFrame_ || frame >= startFrame_ + 2880000) return;
        if (!refillCall && !selected(frame)) return;
        const uint64_t index = count_.fetch_add(1, std::memory_order_relaxed);
        if (index >= capacity) { SetEvent(done_); return; }
        call.record = &records_[size_t(index)];
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        call.record->qpc = uint64_t(now.QuadPart);
        call.record->nextSubmit = frame / 256 + 1;
        call.record->thread = GetCurrentThreadId();
    }

    bool refillWindow() const noexcept {
        if (!refill_ || (gate_.load(std::memory_order_relaxed) & closed)) return false;
        const auto frame = frames_.load(std::memory_order_relaxed);
        return frame >= startFrame_ && frame < startFrame_ + 2880000;
    }

    // Dense source evidence has a ten-second window. Sparse evidence retains
    // adjacent pairs across the full capture, including nested SRC calls.
    bool engineWindow() const noexcept {
        return (!mixStages_ || sparse_) && detailWindow();
    }

    // Source/filter identity can accompany a mix capture without enabling the
    // much more numerous nested SRC snapshots. Both remain opt-in and bounded.
    bool sourceWindow() const noexcept {
        return (!mixStages_ || mixSources_) && detailWindow();
    }

    // By default the mix pass omits source/filter snapshots. Sparse mode can
    // include them and nested SRC calls in the same selected pair.
    bool mixWindow() const noexcept {
        return mixStages_ && detailWindow();
    }

    bool detailWindow() const noexcept {
        if (refill_ || (gate_.load(std::memory_order_relaxed) & closed)) return false;
        const auto frame = frames_.load(std::memory_order_relaxed);
        // Two adjacent blocks out of 64 preserve one measurable seam while
        // avoiding all per-voice memory queries on the other 62 blocks. Spread
        // these pairs across the full capture so intermittent fights are seen.
        if (sparse_) return frame >= startFrame_ && frame < startFrame_ + 2880000 && selected(frame);
        return frame >= startFrame_ + 1200000 && frame < startFrame_ + 1680000;
    }

    void finish() noexcept {
        gate_.fetch_or(closed, std::memory_order_acq_rel);
        if (writer_) {
            SetEvent(done_);
            WaitForSingleObject(writer_, INFINITE);
            CloseHandle(writer_);
            writer_ = nullptr;
        } else {
            persist();
        }
        if (done_) { CloseHandle(done_); done_ = nullptr; }
        records_.reset();
    }

private:
    bool refill_ = false;
    bool selected(uint64_t frame) const noexcept {
        return !sparse_ || (frame / 256) % 64 < 2;
    }
    static DWORD WINAPI writerEntry(void* value) noexcept {
        auto* self = static_cast<AudioResamplerTrace*>(value);
        WaitForSingleObject(self->done_, INFINITE);
        self->gate_.fetch_or(closed, std::memory_order_acq_rel);
        self->persist();
        return 0;
    }

    void persist() noexcept {
        while (gate_.load(std::memory_order_acquire) != closed) Sleep(1);
        if (file_ != INVALID_HANDLE_VALUE) {
            const uint64_t seen = count_.load();
            const uint64_t saved = seen < capacity ? seen : capacity;
            struct Header {
                char magic[8]; uint32_t version, recordBytes;
                uint64_t count, frequency, startFrame, endFrame, dropped, reserved;
            } header{{'D','R','R','S','M','P','0','1'}, 1, sizeof(Record), saved,
                     frequency_, startFrame_, startFrame_ + 2880000, seen - saved, 0};
            static_assert(sizeof(Header) == 64);
            auto write = [&](const void* data, DWORD size) {
                DWORD written = 0;
                return WriteFile(file_, data, size, &written, nullptr) && written == size;
            };
            const bool ok = write(&header, sizeof(header)) &&
                            (!saved || write(records_.get(), DWORD(saved * sizeof(Record))));
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            std::fprintf(stderr, "[AudioResamplerTrace] %s records=%llu dropped=%llu\n",
                         ok ? "persisted" : "write failed", saved, seen - saved);
        }
    }

    std::atomic<uint32_t> gate_{closed};
    std::atomic<uint64_t> count_{0}, frames_{0};
    uint64_t frequency_ = 0;
    uint64_t startFrame_ = 960000; // Fixed before guest workers start.
    bool mixStages_ = false; // Fixed before guest workers start; owner-only init.
    bool mixSources_ = false;
    bool sparse_ = false;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE done_ = nullptr, writer_ = nullptr;
    std::unique_ptr<Record[]> records_;
};

inline AudioResamplerTrace audioResamplerTrace;
}
