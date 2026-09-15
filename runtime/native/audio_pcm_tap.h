#pragma once

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace DarkRecomp::Native {

// Opt-in capture of ONLY the interleaved game PCM accepted by XAudio.
// Call initializeFromEnvironment at registration, submit after each successful
// submission, and finish at shutdown. Serialize initialize/finish/destruction;
// finish may overlap submit, but callers must stop using the object before its
// destruction. Submits are serialized internally (including their sample clock).
// No allocation, logging, or file I/O occurs in submit. All file operations run
// on the writer, including CREATE_NEW during initialization. finish joins it.
class AudioPcmTap {
public:
    static constexpr uint32_t kChannels = 6;
    static constexpr uint32_t kRateHz = 48000;
    static constexpr uint32_t kFramesPerSubmit = 256;

    AudioPcmTap() noexcept = default;
    ~AudioPcmTap() noexcept { finish(); }
    AudioPcmTap(const AudioPcmTap&) = delete;
    AudioPcmTap& operator=(const AudioPcmTap&) = delete;

    // Missing/empty DARKRECOMP_AUDIO_TAP disables the tap; false also indicates
    // a failed start, logged to stderr. The path must be absolute. Existing WAV
    // files are never overwritten. The optional sidecar is <wav path>.csv and
    // is also CREATE_NEW; its failure does not prevent WAV capture.
    bool initializeFromEnvironment() noexcept {
        finish();
        try {
            const DWORD needed = GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_TAP", nullptr, 0);
            if (!needed) return false;
            std::wstring path(needed, L'\0');
            const DWORD copied = GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_TAP", path.data(), needed);
            if (!copied || copied >= needed) return false;
            path.resize(copied);
            if (!absolutePath(path)) {
                std::fprintf(stderr, "AudioPcmTap: disabled; WAV path must be an absolute drive or UNC file path.\n");
                return false;
            }
            wavPath_ = std::move(path);
            csvPath_ = wavPath_ + L".csv";
            const double start = seconds(L"DARKRECOMP_AUDIO_TAP_START_SECONDS", 30, 0, 300);
            const double duration = seconds(L"DARKRECOMP_AUDIO_TAP_SECONDS", 30, 1, 120);
            startFrame_ = static_cast<uint64_t>(start * kRateHz);
            targetFrames_ = static_cast<uint64_t>(duration * kRateHz);
            submittedFrames_ = capturedFrames_ = metadataCount_ = 0;
            // At 120s: 138,240,000 PCM + at most 900,040 metadata bytes.
            // At 30s: 34,560,000 PCM + at most 225,040 metadata bytes.
            // One extra row covers a capture beginning partway through a block.
            pcm_.resize(static_cast<size_t>(targetFrames_) * kChannels);
            metadata_.resize(static_cast<size_t>((targetFrames_ + 255) / 256 + 1));
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
                std::fprintf(stderr, "AudioPcmTap: disabled; QPC frequency unavailable.\n");
                finish();
                return false;
            }
            qpcFrequency_ = static_cast<uint64_t>(frequency.QuadPart);
            ready_.store(false, std::memory_order_relaxed);
            startupOk_.store(false, std::memory_order_relaxed);
            initializedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            captureEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (initializedEvent_ && captureEvent_)
                writer_ = CreateThread(nullptr, 0, writerEntry, this, 0, nullptr);
            if (!writer_) {
                std::fprintf(stderr, "AudioPcmTap: disabled; writer startup failed (%lu).\n", GetLastError());
                finish();
                return false;
            }
            const DWORD wait = WaitForSingleObject(initializedEvent_, INFINITE);
            if (wait != WAIT_OBJECT_0 || !startupOk_.load(std::memory_order_acquire)) {
                finish();
                return false;
            }
            enabled_.store(true, std::memory_order_release);
            std::fprintf(stderr, "AudioPcmTap: armed start_frame=%llu frames=%llu bytes=%llu WAV=%ls\n",
                static_cast<unsigned long long>(startFrame_),
                static_cast<unsigned long long>(targetFrames_),
                static_cast<unsigned long long>(pcm_.size() * sizeof(float) + metadata_.size() * sizeof(Metadata)),
                wavPath_.c_str());
            return true;
        } catch (...) {
            std::fprintf(stderr, "AudioPcmTap: disabled; initialization allocation failed.\n");
            finish();
            return false;
        }
    }

    // Exactly 256 frames / 1536 floats, in FL FR FC LFE BL BR order. A null
    // pointer is ignored and does not advance the accepted-submission clock.
    // sequence is diagnostic only; timing is driven by submitted frame count.
    void submit(const float* interleaved, uint64_t sequence, uint32_t guestAddress) noexcept {
        if (!interleaved || !enabled_.load(std::memory_order_acquire)) return;
        AcquireSRWLockExclusive(&submitLock_);
        if (!enabled_.load(std::memory_order_relaxed)) {
            ReleaseSRWLockExclusive(&submitLock_);
            return;
        }
        const uint64_t blockStart = submittedFrames_;
        submittedFrames_ += kFramesPerSubmit;
        const uint64_t captureEnd = startFrame_ + targetFrames_;
        const uint64_t first = blockStart > startFrame_ ? blockStart : startFrame_;
        const uint64_t last = submittedFrames_ < captureEnd ? submittedFrames_ : captureEnd;
        if (last > first) {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
            const uint64_t micros = (ticks / qpcFrequency_) * 1000000ull +
                ((ticks % qpcFrequency_) * 1000000ull) / qpcFrequency_;
            const uint32_t frames = static_cast<uint32_t>(last - first);
            std::memcpy(pcm_.data() + static_cast<size_t>(capturedFrames_) * kChannels,
                interleaved + static_cast<size_t>(first - blockStart) * kChannels,
                static_cast<size_t>(frames) * kChannels * sizeof(float));
            metadata_[metadataCount_++] = {sequence, micros, capturedFrames_, first, guestAddress, frames};
            capturedFrames_ += frames;
            if (capturedFrames_ == targetFrames_) {
                enabled_.store(false, std::memory_order_release);
                ready_.store(true, std::memory_order_release);
                SetEvent(captureEvent_);
            }
        }
        ReleaseSRWLockExclusive(&submitLock_);
    }

    // Idempotent, flushes even an empty/partial capture, and waits for persistence
    // and handle closure. After automatic completion, only this join is needed.
    void finish() noexcept {
        enabled_.store(false, std::memory_order_release);
        AcquireSRWLockExclusive(&submitLock_);
        ready_.store(true, std::memory_order_release);
        if (captureEvent_) SetEvent(captureEvent_);
        ReleaseSRWLockExclusive(&submitLock_);
        if (writer_) { WaitForSingleObject(writer_, INFINITE); CloseHandle(writer_); writer_ = nullptr; }
        if (initializedEvent_) { CloseHandle(initializedEvent_); initializedEvent_ = nullptr; }
        if (captureEvent_) { CloseHandle(captureEvent_); captureEvent_ = nullptr; }
        std::vector<float>().swap(pcm_);
        std::vector<Metadata>().swap(metadata_);
    }

private:
    struct Metadata {
        uint64_t sequence, qpcMicros, frameOffset, submittedFrame;
        uint32_t guestAddress, frameCount;
    };
    static_assert(sizeof(float) == 4, "WAV requires float32");
    static_assert(sizeof(Metadata) <= 40, "Keep tap allocation below 140 MB");
    static_assert(120ull * kRateHz * kChannels * sizeof(float) +
        ((120ull * kRateHz + 255) / 256 + 1) * sizeof(Metadata) < 140000000ull);

    static bool absolutePath(const std::wstring& p) noexcept {
        const bool drive = p.size() >= 4 && ((p[0] >= L'A' && p[0] <= L'Z') ||
            (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
        if (drive) return true;
        // Exclude Win32 device namespaces; this is a regular WAV file sink.
        if (p.size() < 5 || p[0] != L'\\' || p[1] != L'\\' || p[2] == L'.' || p[2] == L'?') return false;
        const size_t serverEnd = p.find(L'\\', 2);
        const size_t shareEnd = serverEnd == std::wstring::npos ? serverEnd : p.find(L'\\', serverEnd + 1);
        return serverEnd > 2 && shareEnd != std::wstring::npos && shareEnd > serverEnd + 1 && shareEnd + 1 < p.size();
    }

    static double seconds(const wchar_t* name, double fallback, double low, double high) noexcept {
        wchar_t buffer[128]{};
        const DWORD count = GetEnvironmentVariableW(name, buffer, 128);
        if (!count) return fallback;
        wchar_t* end = nullptr;
        const double value = count < 128 ? std::wcstod(buffer, &end) : fallback;
        if (end) while (std::iswspace(*end)) ++end;
        if (count >= 128 || end == buffer || !end || *end || !std::isfinite(value)) {
            std::fprintf(stderr, "AudioPcmTap: invalid %ls; using %.0f seconds.\n", name, fallback);
            return fallback;
        }
        return value < low ? low : value > high ? high : value;
    }

    static bool writeAll(HANDLE file, const void* data, size_t bytes) noexcept {
        const auto* cursor = static_cast<const uint8_t*>(data);
        while (bytes) {
            const DWORD chunk = static_cast<DWORD>(bytes > 1048576 ? 1048576 : bytes);
            DWORD written = 0;
            if (!WriteFile(file, cursor, chunk, &written, nullptr) || !written) return false;
            bytes -= written;
            cursor += written;
        }
        return true;
    }

    static void put16(uint8_t* p, uint16_t n) noexcept { p[0] = uint8_t(n); p[1] = uint8_t(n >> 8); }
    static void put32(uint8_t* p, uint32_t n) noexcept {
        for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (i * 8));
    }
    bool writeWav(HANDLE file) noexcept {
        // RIFF(12), fmt extensible(48), fact(12), data header(8): PCM at 80.
        const uint32_t bytes = static_cast<uint32_t>(capturedFrames_ * kChannels * sizeof(float));
        uint8_t header[80]{};
        std::memcpy(header, "RIFF", 4); put32(header + 4, 72 + bytes);
        std::memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 40);
        put16(header + 20, 0xFFFE); put16(header + 22, kChannels);
        put32(header + 24, kRateHz); put32(header + 28, kRateHz * kChannels * 4);
        put16(header + 32, kChannels * 4); put16(header + 34, 32);
        put16(header + 36, 22); put16(header + 38, 32); put32(header + 40, 0x3F);
        const uint8_t ieeeFloat[16] = {3,0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71};
        std::memcpy(header + 44, ieeeFloat, 16);
        std::memcpy(header + 60, "fact", 4); put32(header + 64, 4);
        put32(header + 68, static_cast<uint32_t>(capturedFrames_));
        std::memcpy(header + 72, "data", 4); put32(header + 76, bytes);
        return writeAll(file, header, sizeof(header)) && writeAll(file, pcm_.data(), bytes);
    }

    bool writeCsv(HANDLE file) noexcept {
        const char columns[] = "sequence,qpc_us,guest_address,frame_offset,frame_count,submitted_frame\n";
        if (!writeAll(file, columns, sizeof(columns) - 1)) return false;
        // Batch rows on the writer's stack instead of issuing one write per row.
        char buffer[16384];
        size_t used = 0;
        for (size_t i = 0; i < metadataCount_; ++i) {
            const Metadata& m = metadata_[i];
            char row[192];
            const int count = std::snprintf(row, sizeof(row), "%llu,%llu,0x%08X,%llu,%u,%llu\n",
                static_cast<unsigned long long>(m.sequence), static_cast<unsigned long long>(m.qpcMicros),
                static_cast<unsigned>(m.guestAddress), static_cast<unsigned long long>(m.frameOffset),
                static_cast<unsigned>(m.frameCount), static_cast<unsigned long long>(m.submittedFrame));
            if (count < 0 || static_cast<size_t>(count) >= sizeof(row)) return false;
            if (used + static_cast<size_t>(count) > sizeof(buffer)) {
                if (!writeAll(file, buffer, used)) return false;
                used = 0;
            }
            std::memcpy(buffer + used, row, static_cast<size_t>(count));
            used += static_cast<size_t>(count);
        }
        return writeAll(file, buffer, used);
    }

    static DWORD WINAPI writerEntry(void* context) noexcept {
        static_cast<AudioPcmTap*>(context)->writeCapture();
        return 0;
    }
    void writeCapture() noexcept {
        const HANDLE wav = CreateFileW(wavPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (wav == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "AudioPcmTap: disabled; CREATE_NEW WAV failed (%lu), path=%ls\n", GetLastError(), wavPath_.c_str());
            startupOk_.store(false, std::memory_order_release);
            SetEvent(initializedEvent_);
            return;
        }
        const HANDLE csv = CreateFileW(csvPath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (csv == INVALID_HANDLE_VALUE)
            std::fprintf(stderr, "AudioPcmTap: optional CSV unavailable (%lu), path=%ls\n", GetLastError(), csvPath_.c_str());
        startupOk_.store(true, std::memory_order_release);
        SetEvent(initializedEvent_);
        const DWORD wait = WaitForSingleObject(captureEvent_, INFINITE);
        bool wavOk = wait == WAIT_OBJECT_0 && ready_.load(std::memory_order_acquire);
        bool csvOk = false;
        if (wavOk) {
            std::fprintf(stderr, "AudioPcmTap: writing %s capture, frames=%llu blocks=%llu\n",
                capturedFrames_ == targetFrames_ ? "complete" : "partial",
                static_cast<unsigned long long>(capturedFrames_), static_cast<unsigned long long>(metadataCount_));
            wavOk = writeWav(wav);
            if (csv != INVALID_HANDLE_VALUE) csvOk = writeCsv(csv);
        }
        // Try flushing even after write errors; every handle is closed before
        // the completion log or join returns. This also runs at duration end.
        if (!FlushFileBuffers(wav)) wavOk = false;
        if (csv != INVALID_HANDLE_VALUE) {
            if (!FlushFileBuffers(csv)) csvOk = false;
            if (!CloseHandle(csv)) csvOk = false;
        }
        if (!CloseHandle(wav)) wavOk = false;
        std::fprintf(stderr, "AudioPcmTap: WAV %s; CSV %s; frames=%llu path=%ls\n",
            wavOk ? "persisted" : "WRITE/FLUSH FAILED", csv == INVALID_HANDLE_VALUE ? "unavailable" : csvOk ? "persisted" : "WRITE/FLUSH FAILED",
            static_cast<unsigned long long>(capturedFrames_), wavPath_.c_str());
    }

    std::atomic<bool> enabled_{false}, ready_{false}, startupOk_{false};
    SRWLOCK submitLock_ = SRWLOCK_INIT;
    HANDLE writer_ = nullptr, initializedEvent_ = nullptr, captureEvent_ = nullptr;
    std::wstring wavPath_, csvPath_;
    std::vector<float> pcm_;
    std::vector<Metadata> metadata_;
    uint64_t startFrame_ = 0, targetFrames_ = 0, submittedFrames_ = 0, capturedFrames_ = 0;
    uint64_t qpcFrequency_ = 1;
    size_t metadataCount_ = 0;
};

} // namespace DarkRecomp::Native
