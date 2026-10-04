#include "audio_pcm_storage_tests.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace DarkRecomp::Native;
static void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
static void scalar(const uint8_t* source, float* out) {
    for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; ++frame)
        for (uint32_t ch = 0; ch < kAudioChannels; ++ch) {
            const uint8_t* at = source + (size_t(ch) * kAudioFramesPerSubmit + frame) * 4;
            const uint32_t bits = (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) |
                                  (uint32_t(at[2]) << 8) | at[3];
            memcpy(out + size_t(frame) * kAudioChannels + ch, &bits, 4);
        }
}
static volatile uint32_t checksum = 0;
template<class Work>
static double measure(Work work) {
    constexpr uint32_t iterations = 200000;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iterations; ++i) work(i);
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
}
int main(int argc, char** argv) {
    try {
        verifyAudioPcmConversion(require, audioConvertPlanarBEFloatSamples);
        verifyAudioPcmQueue(require);
        puts("PCM conversion IEEE bit classes/exponents, alignment/canaries and ring lifecycle passed.");
        if (argc < 2 || std::string_view(argv[1]) != "--benchmark") return 0;
        std::array<uint8_t, kAudioSubmitBytes> input{};
        for (size_t i = 0; i < input.size(); ++i) input[i] = uint8_t(i * 17);
        AudioPcmFrame output;
        // Function pointers prevent the compiler combining/dropping repeated conversions.
        void (*volatile before)(const uint8_t*, float*) = scalar;
        void (*volatile after)(const uint8_t*, float*) = audioConvertPlanarBEFloatSamples;
        for (uint32_t repeat = 0; repeat < 5; ++repeat) {
            const auto oldConvert = measure([&](uint32_t) { before(input.data(), output.data()); });
            const auto newConvert = measure([&](uint32_t) { after(input.data(), output.data()); });
            std::deque<std::vector<float>> oldQueue;
            std::vector<float> oldSnapshot;
            const auto oldSubmit = measure([&](uint32_t i) {
                if (oldQueue.size() == kAudioPrefillBuffers) oldQueue.pop_front();
                std::vector<float> frame(output.size());
                before(input.data(), frame.data());
                auto snapshot = frame;
                oldQueue.push_back(std::move(frame));
                oldSnapshot = std::move(snapshot);
                uint32_t bits; memcpy(&bits, oldSnapshot.data() + i % oldSnapshot.size(), 4);
                checksum = bits;
            });
            AudioPcmQueue queue;
            AudioPcmFrame snapshot;
            const auto newSubmit = measure([&](uint32_t i) {
                if (queue.size() == kAudioPrefillBuffers) queue.pop_front();
                auto& frame = queue.next();
                after(input.data(), frame.data());
                queue.push();
                memcpy(snapshot.data(), frame.data(), kAudioSubmitBytes);
                uint32_t bits; memcpy(&bits, snapshot.data() + i % snapshot.size(), 4);
                checksum = bits;
            });
            printf("repeat=%u scalarNs=%.1f optimizedNs=%.1f allocatingSubmitNs=%.1f pooledSubmitNs=%.1f\n",
                   repeat, oldConvert, newConvert, oldSubmit, newSubmit);
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "PCM benchmark failed: %s\n", error.what());
        return 1;
    }
}
