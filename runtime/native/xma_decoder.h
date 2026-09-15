#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct AVCodecContext;
struct AVFrame;

struct XmaStreamDecoder {
    AVCodecContext* codec = nullptr;
    int channels = 0;
    int rateHz = 0;
    bool open = false;
    bool eofSent = false;
    bool eofTerminal = false;
    uint64_t packetsAccepted = 0;
    uint32_t inPtr[2] = {0, 0};
    uint32_t inDone[2] = {0, 0};
    uint64_t inGen[2] = {0, 0};
    bool inSeen[2] = {false, false};
    std::vector<int16_t> spill;
    AVFrame* pending = nullptr;
    uint32_t pendingOff = 0;
};

class XmaDecoderPool {
public:
    static constexpr uint32_t kSlots = 320;
    static constexpr uint32_t kPacketBytes = 2048;
    static constexpr uint32_t kMaxSpillSamplesPerChannel = 32768;
    static constexpr uint32_t kWholePacketReadBit = 32;
    static constexpr int kError = -1;
    static constexpr int kUnsupported = -2;
    static constexpr int kNeedReset = -3;
    int submit(uint32_t slot, int rateHz, int channels, const uint32_t* image,
               const uint8_t* base, int16_t* outBE, uint32_t outCapSamples,
               uint32_t consumed[2], const uint64_t generation[2]);
    int drain(uint32_t slot, int16_t* outBE, uint32_t outCapSamples);
    int drainEofForTest(uint32_t slot, int16_t* outBE, uint32_t outCapSamples);
    uint32_t pendingSamples(uint32_t slot);
    void release(uint32_t slot);
    void clear();
    ~XmaDecoderPool();

private:
    AVCodecContext* openDecoder(int rateHz, int channels);
    std::mutex mutex_;
    std::array<std::unique_ptr<XmaStreamDecoder>, kSlots> decoders_{};
};
