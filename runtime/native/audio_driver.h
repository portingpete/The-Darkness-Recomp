#pragma once
#include <cstdint>
#include <vector>

namespace DarkRecomp::Native {
class Memory;

constexpr uint32_t kAudioDriverTokenBase = 0x44400000u;
constexpr uint32_t kAudioDriverTokenGenMask = 0xFFFFFu;
constexpr uint32_t kAudioChannels = 6;
constexpr uint32_t kAudioRateHz = 48000;
constexpr uint32_t kAudioFramesPerSubmit = 256;
constexpr uint32_t kAudioSubmitBytes = kAudioChannels * kAudioFramesPerSubmit * 4;
// 42.7 ms of PCM before starting the continuous render voice.
constexpr uint32_t kAudioPrefillBuffers = 8;
constexpr uint32_t kAudioMaxQueuedBuffers = 16;
constexpr uint32_t kAudioPrefillDeadlineMs = 30;

struct AudioDriverCounters {
    uint64_t callbacks = 0;
    uint64_t framesSubmitted = 0;
    uint64_t samplesQueued = 0;
    uint64_t buffersCompleted = 0;
    uint64_t deviceErrors = 0;
    // Source shortages and engine deadline misses are not HRESULT errors.
    uint64_t starvationPasses = 0;
    uint64_t starvationBytes = 0;
    uint64_t maxCallbackMicros = 0;
    uint64_t callbacksWithoutSubmit = 0;
    uint64_t queuedFrames = 0;
    uint32_t queuedBuffers = 0;
    uint32_t engineGlitches = 0;
    int32_t lastHresult = 0;
    bool workerRunning = false;
    bool deviceReady = false;
    bool playbackStarted = false;
    bool workerMmcss = false;
};

void audioConvertPlanarBEFloat(const uint8_t* planarBE, float* interleavedOut);
bool audioDriverPeekLastSubmitted(std::vector<float>& out);
uint32_t audioDriverInjectVoiceError(int32_t hr);
void audioDriverInjectStartupError(int32_t hr);
uint32_t audioDriverVoiceCategoryVolumeChangeMask(uint32_t token, uint32_t outMask);

class AudioRenderDriver {
public:
    struct Impl;
    static AudioRenderDriver& instance();
    uint32_t registerClient(uint32_t guestCallback, uint32_t rawArgument, uint32_t driverOut);
    uint32_t submitFrame(uint32_t token, uint32_t samplesGuest);
    uint32_t unregisterClient(uint32_t token);
    void shutdown();
    void shutdownForMemory(Memory* owner);
    AudioDriverCounters counters();
    // Host output only; decoding, callbacks and completion timing continue.
    bool setMuted(bool muted);

private:
    AudioRenderDriver() = default;
    Impl& impl();
};

void audioDriverShutdownForMemory(Memory* owner);
}
