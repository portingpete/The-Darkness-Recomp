#pragma once
#include "runtime/native/audio_pcm_tap.h"
#include <filesystem>
#include <fstream>
#include <string>

struct AudioTapTestEnvironment {
        const wchar_t* name;
        std::wstring old;
        bool existed;
        AudioTapTestEnvironment(const wchar_t* key, const wchar_t* value) : name(key) {
            const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
            existed = length != 0;
            if (length) {
                old.resize(length);
                GetEnvironmentVariableW(name, old.data(), length);
                old.resize(length - 1);
            }
            SetEnvironmentVariableW(name, value);
        }
        ~AudioTapTestEnvironment() { SetEnvironmentVariableW(name, existed ? old.c_str() : nullptr); }
};

static void testAudioPcmTap() {
    // Synthetic PCM only: this fixture opens no audio device or game save.
    wchar_t temp[MAX_PATH]{}, folder[MAX_PATH]{};
    check(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"dpa", 0, folder),
          "PCM tap temporary path failed");
    check(DeleteFileW(folder) && CreateDirectoryW(folder, nullptr), "PCM tap temporary folder failed");
    const std::wstring wave = std::wstring(folder) + L"\\synthetic.wav";
    const std::wstring csv = wave + L".csv";
    AudioTapTestEnvironment path(L"DARKRECOMP_AUDIO_TAP", wave.c_str());
    AudioTapTestEnvironment delay(L"DARKRECOMP_AUDIO_TAP_START_SECONDS", L"0");
    AudioTapTestEnvironment duration(L"DARKRECOMP_AUDIO_TAP_SECONDS", L"1");
    auto read = [&] {
        std::ifstream file(std::filesystem::path(wave), std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
    };
    auto cleanup = [&] {
        DeleteFileW(csv.c_str());
        DeleteFileW(wave.c_str());
        RemoveDirectoryW(folder);
    };
    auto le32 = [](const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    try {
        std::vector<float> pcm(kAudioFramesPerSubmit * kAudioChannels);
        {
            AudioPcmTap tap;
            tap.initializeFromEnvironment();
            for (uint32_t block = 0; block < 200; ++block) {
                for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; ++frame)
                    for (uint32_t ch = 0; ch < kAudioChannels; ++ch)
                        pcm[frame * kAudioChannels + ch] = float((block * 256 + frame) % 2000) / 2000.f + float(ch);
                tap.submit(pcm.data(), block + 1, 0x12340000u);
            }
            tap.finish();
        }
        const auto bytes = read();
        check(bytes.size() >= 68 && !memcmp(bytes.data(), "RIFF", 4) &&
                  !memcmp(bytes.data() + 8, "WAVE", 4) && le32(bytes.data() + 4) + 8 == bytes.size(),
              "PCM tap did not finalize a valid bounded WAV");
        size_t data = 0;
        uint32_t size = 0;
        bool validFormat = false;
        for (size_t at = 12; at + 8 <= bytes.size();) {
            const uint32_t count = le32(bytes.data() + at + 4);
            check(uint64_t(at) + 8 + count <= bytes.size(), "PCM tap WAV chunk exceeds file");
            if (!memcmp(bytes.data() + at, "fmt ", 4)) {
                const auto* fmt = bytes.data() + at + 8;
                validFormat = count >= 40 && fmt[0] == 0xfe && fmt[1] == 0xff &&
                    fmt[2] == kAudioChannels && le32(fmt + 4) == kAudioRateHz &&
                    fmt[14] == 32 && le32(fmt + 24) == 3;
            }
            if (!memcmp(bytes.data() + at, "data", 4)) { data = at + 8; size = count; }
            at += 8 + size_t(count) + (count & 1);
        }
        check(validFormat && data && size == kAudioRateHz * kAudioChannels * sizeof(float),
              "PCM tap changed format or exceeded its one-second sample cap");
        // Exact output checks catch dropped/duplicated blocks, channel changes,
        // clipping, and improper truncation of the last 128-frame partial block.
        for (uint32_t frame = 0; frame < kAudioRateHz; ++frame) {
            for (uint32_t ch = 0; ch < kAudioChannels; ++ch) {
                float actual;
                memcpy(&actual, bytes.data() + data + (frame * kAudioChannels + ch) * sizeof(float), sizeof(float));
                check(actual == float(frame % 2000) / 2000.f + float(ch), "PCM tap altered submitted samples");
            }
        }
        {
            AudioPcmTap existing;
            existing.initializeFromEnvironment();
            existing.submit(pcm.data(), 1, 0);
            existing.finish();
        }
        check(read() == bytes, "PCM tap overwrote an existing capture");
        check(std::filesystem::file_size(csv) > 100, "PCM tap submission timing metadata missing");
        puts("Game PCM tap preserves exact samples/channel order, truncates at the configured cap, and refuses overwrite.");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
