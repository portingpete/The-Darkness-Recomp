#pragma once
#include "runtime/native/audio_pcm_convert.h"
#include "runtime/native/audio_pcm_storage.h"
#include <algorithm>
#include <cstring>
#include <deque>
#ifdef _WIN32
#include <windows.h>
#endif

namespace DarkRecomp::Native {
template<class Require, class Convert>
void verifyAudioPcmConversion(Require require, Convert convert) {
    std::array<uint8_t, kAudioSubmitBytes + 32> input{};
    std::array<float, size_t(kAudioFramesPerSubmit) * kAudioChannels + 8> output{};
    uint32_t random = 0x9e3779b9u;
    constexpr uint32_t special[]{0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u,
        0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u,
        0xffc12345u, 0xff812345u};
    for (uint32_t trial = 0; trial < 512; ++trial) {
        input.fill(0xa5);
        memset(output.data(), 0x5a, sizeof(output));
        auto* source = input.data() + trial % 16;
        auto* destination = output.data() + trial % 4;
        for (uint32_t ch = 0; ch < kAudioChannels; ++ch) {
            for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; ++frame) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                // Visit every exponent with both signs and random mantissas,
                // plus explicit signed zero, subnormal, infinity and NaN cases.
                const uint32_t bits = frame < std::size(special) ? special[frame] :
                    (random & 0x807fffffu) | ((trial & 255u) << 23);
                uint8_t* at = source + (size_t(ch) * kAudioFramesPerSubmit + frame) * 4;
                at[0] = uint8_t(bits >> 24); at[1] = uint8_t(bits >> 16);
                at[2] = uint8_t(bits >> 8); at[3] = uint8_t(bits);
            }
        }
        const auto original = input;
        convert(source, destination);
        require(input == original, "PCM conversion modified input/canary bytes");
        for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; ++frame) {
            for (uint32_t ch = 0; ch < kAudioChannels; ++ch) {
                const auto* at = source + (size_t(ch) * kAudioFramesPerSubmit + frame) * 4;
                const uint32_t expected = (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) |
                                          (uint32_t(at[2]) << 8) | at[3];
                uint32_t actual;
                memcpy(&actual, destination + size_t(frame) * kAudioChannels + ch, 4);
                require(actual == expected, "PCM conversion changed sample bits/channel/frame order");
            }
        }
        for (size_t i = 0; i < output.size(); ++i) {
            if (i >= trial % 4 && i < trial % 4 + size_t(kAudioFramesPerSubmit) * kAudioChannels) continue;
            uint32_t canary;
            memcpy(&canary, output.data() + i, 4);
            require(canary == 0x5a5a5a5au, "PCM conversion overwrote output canaries");
        }
    }
#ifdef _WIN32
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t accessibleBytes = (kAudioSubmitBytes + system.dwPageSize - 1) /
                                  system.dwPageSize * system.dwPageSize;
    struct ProbeMemory {
        uint8_t* allocation;
        ~ProbeMemory() { if (allocation) VirtualFree(allocation, 0, MEM_RELEASE); }
    } guarded{static_cast<uint8_t*>(VirtualAlloc(nullptr, accessibleBytes + system.dwPageSize,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
    require(guarded.allocation != nullptr, "PCM guarded-page fixture allocation failed");
    DWORD previous;
    require(VirtualProtect(guarded.allocation + accessibleBytes, system.dwPageSize, PAGE_NOACCESS,
                           &previous) != 0, "PCM guarded-page fixture protection failed");
    // End exactly at an inaccessible page. The final SIMD read/store must stop
    // at the PCM quantum's last sample rather than fetch a speculative tail.
    auto* boundedInput = guarded.allocation + accessibleBytes - kAudioSubmitBytes;
    memcpy(boundedInput, input.data(), kAudioSubmitBytes);
    convert(boundedInput, output.data());
    auto* boundedOutput = reinterpret_cast<float*>(boundedInput);
    convert(input.data(), boundedOutput);
    require(memcmp(output.data(), boundedOutput, kAudioSubmitBytes) == 0,
            "PCM page-bounded conversion changed sample bits");
#endif
}

template<class Require>
void verifyAudioPcmQueue(Require require) {
    AudioPcmQueue queue;
    std::deque<std::pair<const AudioPcmFrame*, uint32_t>> device;
    uint32_t next = 1;
    auto verifyPending = [&] {
        require(queue.size() == device.size(), "PCM ring/device ownership count differs");
        for (const auto& [samples, id] : device)
            for (float sample : *samples)
                require(sample == float(id), "PCM ring reused a frame before device completion");
    };
    auto submit = [&](bool accepted) {
        require(queue.size() < kAudioMaxQueuedBuffers, "PCM ring fixture overfilled");
        auto& frame = queue.next();
        std::fill(frame.begin(), frame.end(), float(next));
        queue.push();
        if (accepted) device.emplace_back(&frame, next++);
        else queue.pop_back(); // Rejected host submits never publish completion.
        verifyPending();
    };
    for (uint32_t cycle = 0; cycle < 128; ++cycle) {
        while (queue.size() < kAudioMaxQueuedBuffers) {
            if (next % 3 == 0) submit(false);
            submit(true);
        }
        require(queue.size() == kAudioMaxQueuedBuffers, "PCM ring failed to retain every full-queue frame");
        verifyPending();
        // Coalesced completion notifications may reclaim multiple frames.
        for (uint32_t i = 0; i < cycle % kAudioMaxQueuedBuffers + 1; ++i) {
            queue.pop_front(); device.pop_front();
        }
        verifyPending();
    }
    while (!queue.empty()) { queue.pop_front(); device.pop_front(); }
    require(device.empty(), "PCM ring failed to drain");
    queue.clear();
    submit(false); submit(true);
    queue.clear(); device.clear();
    require(queue.empty(), "PCM ring clear retained queued frames");
}
}
