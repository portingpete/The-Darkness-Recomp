#pragma once
#include "runtime/native/audio_resampler_trace.h"
#include <filesystem>

static void testAudioResamplerTrace() {
    using DarkRecomp::Native::AudioResamplerTrace;
    wchar_t temporary[MAX_PATH]{}, unique[MAX_PATH]{};
    check(GetTempPathW(MAX_PATH, temporary) && GetTempFileNameW(temporary, L"drt", 0, unique),
          "Resampler trace temporary path failed");
    // GetTempFileName creates the placeholder; our capture requires CREATE_NEW.
    check(DeleteFileW(unique), "Resampler trace placeholder removal failed");
    struct Cleanup {
        const wchar_t* path;
        ~Cleanup() { DeleteFileW(path); }
    } cleanup{unique};
    AudioTapTestEnvironment environment(L"DARKRECOMP_RESAMPLER_TRACE", unique);
    AudioTapTestEnvironment startWindow(L"DARKRECOMP_RESAMPLER_TRACE_START_SECONDS", L"20");
    AudioTapTestEnvironment mixMode(L"DARKRECOMP_AUDIO_MIX_TRACE", L"0");
    AudioTapTestEnvironment mixSources(L"DARKRECOMP_AUDIO_MIX_SOURCES", L"0");
    AudioTapTestEnvironment sparseMode(L"DARKRECOMP_AUDIO_TRACE_SPARSE", L"0");
    AudioTapTestEnvironment refillMode(L"DARKRECOMP_AUDIO_REFILL_TRACE", L"0");
    AudioResamplerTrace trace;
    trace.initialize();
    trace.submitted(3750); // Exactly 20 seconds; no real device or game wait.
    check(!trace.engineWindow(), "Engine trace started before its bounded window");
    auto size = [&] {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        check(GetFileAttributesExW(unique, GetFileExInfoStandard, &data),
              "Resampler trace output was not created");
        return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    };
    {
        AudioResamplerTrace::Call call;
        trace.begin(call);
        check(call.record != nullptr, "Resampler trace did not enter capture window");
        call.record->descriptor = 0x12345678;
        call.record->before[13] = 2;
        call.record->after[13] = 2;
        call.record->first[0] = 0.125f;
        trace.submitted(8437);
        check(!trace.engineWindow(), "Engine trace started before 45 seconds");
        trace.submitted(8438);
        check(trace.engineWindow() && trace.sourceWindow(), "Engine trace missed its start boundary");
        trace.submitted(10312);
        check(trace.engineWindow(), "Engine trace ended before 55 seconds");
        trace.submitted(10313);
        check(!trace.engineWindow(), "Engine trace exceeded its ten-second window");
        trace.submitted(15000); // Completion must wake an independent writer.
        Sleep(20);
        check(size() == 0, "Resampler trace persisted an in-flight record");
        call.record->last[0] = 0.25f;
    }
    const auto deadline = GetTickCount64() + 5000;
    while (size() != 64 + sizeof(AudioResamplerTrace::Record) && GetTickCount64() < deadline)
        Sleep(1);
    // This assertion is intentionally BEFORE finish: timed game exits bypass it.
    check(size() == 320, "Resampler trace failed automatic persistence before shutdown");
    trace.finish();
    std::ifstream input{std::filesystem::path(unique), std::ios::binary};
    std::array<uint8_t, 320> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    check(input.gcount() == bytes.size() && std::memcmp(bytes.data(), "DRRSMP01", 8) == 0,
          "Resampler trace header/extent invalid");
    uint64_t count = 0;
    std::memcpy(&count, bytes.data() + 16, 8);
    AudioResamplerTrace::Record saved{};
    std::memcpy(&saved, bytes.data() + 64, sizeof(saved));
    check(count == 1 && saved.descriptor == 0x12345678 && saved.nextSubmit == 3751 &&
          saved.before[13] == 2 && saved.after[13] == 2 &&
          saved.first[0] == 0.125f && saved.last[0] == 0.25f,
          "Resampler trace lost record contents or completion writes");
    input.close();
    trace.initialize(); // Existing destination must never be overwritten.
    trace.finish();
    check(size() == 320, "Resampler trace overwrote existing capture");

    wchar_t later[MAX_PATH]{};
    check(GetTempFileNameW(temporary, L"drt", 0, later) && DeleteFileW(later),
          "Later trace temporary path failed");
    Cleanup laterCleanup{later};
    AudioTapTestEnvironment laterEnvironment(L"DARKRECOMP_RESAMPLER_TRACE", later);
    AudioTapTestEnvironment laterStart(L"DARKRECOMP_RESAMPLER_TRACE_START_SECONDS", L"120");
    AudioTapTestEnvironment laterMode(L"DARKRECOMP_AUDIO_MIX_TRACE", L"1");
    trace.initialize();
    trace.submitted(22499);
    { AudioResamplerTrace::Call call; trace.begin(call);
      check(!call.record, "Shifted trace captured menu audio before its start"); }
    trace.submitted(22500);
    { AudioResamplerTrace::Call call; trace.begin(call);
      check(call.record != nullptr, "Shifted trace missed its first block"); }
    trace.submitted(27187);
    check(!trace.mixWindow(), "Shifted mix trace started before 145 seconds");
    trace.submitted(27188);
    check(trace.mixWindow() && !trace.engineWindow() && !trace.sourceWindow(), "Shifted mix trace window/mode incorrect");
    trace.submitted(29062);
    check(trace.mixWindow(), "Shifted mix trace ended before 155 seconds");
    trace.submitted(29063);
    check(!trace.mixWindow(), "Shifted mix trace exceeded its ten-second window");
    trace.submitted(33750);
    trace.finish();
    std::ifstream shifted{std::filesystem::path(later), std::ios::binary};
    shifted.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    uint64_t windowStart = 0, windowEnd = 0;
    std::memcpy(&windowStart, bytes.data() + 32, 8);
    std::memcpy(&windowEnd, bytes.data() + 40, 8);
    check(shifted.gcount() == bytes.size() && windowStart == 5760000 && windowEnd == 8640000,
          "Shifted trace did not record its actual capture window");
    shifted.close();
    check(DeleteFileW(later), "Source mix fixture cleanup failed");
    {
        AudioTapTestEnvironment includeSources(L"DARKRECOMP_AUDIO_MIX_SOURCES", L"1");
        trace.initialize();
        trace.submitted(27187);
        check(!trace.sourceWindow(), "Mix source trace started before the detail window");
        trace.submitted(27188);
        check(trace.sourceWindow() && trace.mixWindow() && !trace.engineWindow(),
              "Mix source identity must not enable nested SRC tracing");
        trace.submitted(29063);
        check(!trace.sourceWindow(), "Mix source trace exceeded the detail window");
        trace.finish();
        check(!trace.sourceWindow(), "Closed mix source trace remained enabled");
    }
    check(DeleteFileW(later), "Sparse trace fixture cleanup failed");
    {
        AudioTapTestEnvironment sparse(L"DARKRECOMP_AUDIO_TRACE_SPARSE", L"1");
        AudioTapTestEnvironment includeSources(L"DARKRECOMP_AUDIO_MIX_SOURCES", L"1");
        trace.initialize();
        uint32_t captured = 0;
        for (uint64_t sequence = 22500; sequence < 22628; ++sequence) {
            trace.submitted(sequence);
            const bool selected = sequence % 64 < 2;
            check(trace.sourceWindow() == selected && trace.mixWindow() == selected &&
                  trace.engineWindow() == selected, "Sparse hooks must skip unselected blocks before memory queries");
            AudioResamplerTrace::Call call;
            trace.begin(call);
            check(bool(call.record) == selected, "Sparse final-stage gate disagrees with source gate");
            captured += bool(call.record);
        }
        check(captured == 4, "Sparse trace must retain exactly two consecutive blocks per 64");
        trace.submitted(33750);
        check(!trace.sourceWindow() && !trace.mixWindow() && !trace.engineWindow(),
              "Sparse trace exceeded its bounded window");
        trace.finish();
    }
    check(DeleteFileW(later), "Refill trace fixture cleanup failed");
    {
        AudioTapTestEnvironment refill(L"DARKRECOMP_AUDIO_REFILL_TRACE", L"1");
        AudioTapTestEnvironment sparse(L"DARKRECOMP_AUDIO_TRACE_SPARSE", L"1");
        trace.initialize();
        trace.submitted(22500);
        check(trace.refillWindow() && !trace.sourceWindow() && !trace.engineWindow() && !trace.mixWindow(),
              "Refill timing must disable all detailed sample hooks");
        { AudioResamplerTrace::Call call; trace.begin(call);
          check(!call.record, "Refill timing unexpectedly captured final samples"); }
        { AudioResamplerTrace::Call call; trace.begin(call, true);
          check(call.record != nullptr, "Refill timing incorrectly used sparse sample selection"); }
        trace.submitted(33750);
        check(!trace.refillWindow(), "Refill timing exceeded capture window");
        trace.finish();
    }
    puts("Resampler trace persistence, no-overwrite, shifted, sparse and refill timing windows passed.");
}
