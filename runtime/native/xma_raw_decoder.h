#pragma once
#include <cstdint>

struct AVCodecContext;
struct AVFrame;

namespace DarkRecomp::Native {
// One independent XMA stream. Push owns its packet on acceptance; read retains
// any frame remainder. Neither temporary input exhaustion nor read sends EOF.
class XmaRawDecoder {
public:
    static constexpr int needDrain = -2;
    XmaRawDecoder() = default;
    ~XmaRawDecoder();
    XmaRawDecoder(const XmaRawDecoder&) = delete;
    XmaRawDecoder& operator=(const XmaRawDecoder&) = delete;
    bool open(int rate, int channels);
    int push(const uint8_t* packet); // exactly 2048 bytes; 0 accepted, -2 retry
    int read(float* interleaved, uint32_t capacityPerChannel);
    int error() const { return error_; }
    int channels() const { return channels_; }
    int rate() const { return rate_; }
private:
    AVCodecContext* codec_ = nullptr;
    AVFrame* frame_ = nullptr;
    uint32_t offset_ = 0;
    int channels_ = 0, rate_ = 0, error_ = 0;
};
}
