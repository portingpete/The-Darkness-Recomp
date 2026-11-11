#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace DarkRecomp {
// Opt-in frame-timeline diagnostics only. Brackets one display frame's
// immediate-context commands with D3D11 timestamp queries and reads results
// later without flushing or waiting. gpuMs is the GPU span from the frame's
// first command to its last: when the GPU is saturated it is the frame's GPU
// cost; when the GPU waits on CPU submission it approaches submission time.
// idleBeforeMs is the GPU gap since the previous measured frame ended; a gap
// near zero on consecutive frames means the GPU never ran out of work.
class GpuFrameTimer {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    static constexpr size_t kSlots = 8;
    struct Slot { Ptr<ID3D11Query> disjoint, start, stop; uint64_t id = 0; bool inFlight = false; };
    std::array<Slot, kSlots> slots_{};
    Ptr<ID3D11DeviceContext> context_;
    uint64_t next_ = 0, oldest_ = 0, skipped_ = 0, disjoint_ = 0, completed_ = 0;
    Slot* open_ = nullptr;
    bool ready_ = false, havePrevious_ = false;
    uint64_t previousId_ = 0, previousStop_ = 0, previousFrequency_ = 0;
public:
    struct Result { uint64_t id = 0; double gpuMs = -1, idleBeforeMs = -1; };
    bool init(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (!device || !context) return false;
        D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& slot : slots_)
            if (FAILED(device->CreateQuery(&disjoint, &slot.disjoint)) || FAILED(device->CreateQuery(&stamp, &slot.start)) ||
                FAILED(device->CreateQuery(&stamp, &slot.stop))) return false;
        context_ = context;
        return ready_ = true;
    }
    bool ready() const { return ready_; }
    bool open() const { return open_ != nullptr; }
    uint64_t skipped() const { return skipped_; }
    uint64_t disjointFrames() const { return disjoint_; }
    uint64_t completed() const { return completed_; }
    // A full ring means the GPU is more than kSlots frames behind: skip rather
    // than reuse a pending query, which would make GetData wait or mix frames.
    void begin(uint64_t id) {
        if (!ready_ || open_) return;
        auto& slot = slots_[next_ % kSlots];
        if (slot.inFlight) { ++skipped_; return; }
        context_->Begin(slot.disjoint.Get());
        context_->End(slot.start.Get());
        slot.id = id; open_ = &slot;
    }
    void end() {
        if (!open_) return;
        context_->End(open_->stop.Get());
        context_->End(open_->disjoint.Get());
        open_->inFlight = true; open_ = nullptr; ++next_;
    }
    // Results arrive in submission order. Stops at the first unfinished frame.
    template<class Callback> void poll(Callback&& callback) {
        if (!ready_) return;
        while (oldest_ < next_) {
            auto& slot = slots_[oldest_ % kSlots];
            if (!slot.inFlight) { ++oldest_; continue; }
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
            UINT64 start = 0, stop = 0;
            constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (context_->GetData(slot.disjoint.Get(), &timing, sizeof(timing), flags) != S_OK) return;
            if (context_->GetData(slot.start.Get(), &start, sizeof(start), flags) != S_OK) return;
            if (context_->GetData(slot.stop.Get(), &stop, sizeof(stop), flags) != S_OK) return;
            Result result; result.id = slot.id;
            const bool valid = !timing.Disjoint && timing.Frequency && stop >= start;
            if (valid) {
                result.gpuMs = double(stop - start) * 1000.0 / double(timing.Frequency);
                if (havePrevious_ && previousId_ + 1 == slot.id && previousFrequency_ == timing.Frequency && start >= previousStop_)
                    result.idleBeforeMs = double(start - previousStop_) * 1000.0 / double(timing.Frequency);
                havePrevious_ = true; previousId_ = slot.id; previousStop_ = stop; previousFrequency_ = timing.Frequency;
            } else {
                ++disjoint_; havePrevious_ = false;
            }
            slot.inFlight = false; ++oldest_; ++completed_;
            callback(result);
        }
    }
};
}
