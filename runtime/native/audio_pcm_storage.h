#pragma once
#include "audio_driver.h"
#include <array>
#include <cassert>

namespace DarkRecomp::Native {
using AudioPcmFrame = std::array<float, size_t(kAudioFramesPerSubmit) * kAudioChannels>;

// Only the producer under the driver mutex changes this ring. XAudio2 reads
// accepted frames until OnBufferEnd publishes completion; it never edits it.
class AudioPcmQueue {
    std::array<AudioPcmFrame, kAudioMaxQueuedBuffers> frames_;
    size_t front_ = 0;
    size_t count_ = 0;
public:
    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
    AudioPcmFrame& next() {
        assert(count_ < frames_.size());
        return frames_[(front_ + count_) % frames_.size()];
    }
    void push() { assert(count_ < frames_.size()); ++count_; }
    void pop_back() { assert(count_); --count_; }
    void pop_front() {
        assert(count_);
        front_ = (front_ + 1) % frames_.size();
        --count_;
    }
    void clear() { front_ = count_ = 0; }
};
}
