#include "runtime/native/runtime.h"
#include "runtime/native/audio_driver.h"
#include "runtime/native/display_mode.h"
#include "runtime/native/thread_topology.h"
#include "renderer/engine/simple_mesh.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <memory>
#include <future>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <algorithm>
using namespace DarkRecomp::Native;
static void check(bool success, const char* message) { if (!success) throw std::runtime_error(message); }
#include "dispatcher_tests.h"
#include "xma_bridge_tests.h"
#include "input_tests.h"
#include "mouse_look_tests.h"
#include "native_delay_tests.h"
#include "native_timed_wait_tests.h"
#include "native_format_tests.h"
#include "file_buffer_tests.h"
#include "audio_buffer_tests.h"
#include "audio_output_headroom_tests.h"
#include "audio_resampler_tests.h"
#include "audio_final_sdk_tests.h"
#include "audio_voice_src_tests.h"
#include "audio_voice_input_tests.h"
#include "audio_biquad_tests.h"
#include "audio_volume_tests.h"
#include "audio_volume_matrix_tests.h"
#include "audio_resampler_trace_tests.h"
#include "tls_reuse_tests.h"
#include "notification_alias_tests.h"
#include "critical_section_tests.h"
#include "c_view_tests.h"
#include "object_output_tests.h"
#include "texture_mip_layout_tests.h"
#include "texture_upload_tests.h"
#include "texture_residency_tests.h"
#include "native_storage_tests.h"
#include "fov_camera_tests.h"
#include "video_settings_tests.h"
static PPC_FUNC(threadProbe) {
    uint32_t argument = ctx.r3.u32;
    memory->write32(argument + 24, base[ctx.r13.u32 + 0x10c]);
    memory->write32(argument, ctx.r13.u32);
    memory->write32(argument + 4, memory->read32(memory->read32(ctx.r13.u32 + 0x100) + 0x14c));
    ctx.r3.u64 = 47;
    __imp__KeTlsGetValue(ctx, base);
    memory->write32(argument + 8, ctx.r3.u32);
    ctx.r3.u64 = 0xfffffffe;
    ctx.r4.u64 = 0x80000000;
    ctx.r5.u64 = argument + 12;
    __imp__ObReferenceObjectByHandle(ctx, base);
    memory->write32(argument + 16, ctx.r3.u32);
    ctx.r3.u64 = memory->read32(argument + 12);
    __imp__ObDereferenceObject(ctx, base);
    ctx.r3.u64 = 47;
    ctx.r4.u64 = 0xfeedabcd;
    __imp__KeTlsSetValue(ctx, base);
    ctx.r3.u64 = 23;
    __imp__ExTerminateThread(ctx, base);
    memory->write32(argument + 20, 1); // Must be unreachable.
}
static PPC_FUNC(fileApcProbe) {
    uint32_t out = ctx.r3.u32;
    memory->write32(out, GetCurrentThreadId());
    memory->write32(out + 4, memory->read32(ctx.r4.u32));
    memory->write32(out + 8, memory->read32(ctx.r4.u32 + 4));
    ctx.r31.u64 = 0xbadc0ffe; // APC register changes must not leak to the interrupted call.
}
static PPC_FUNC(launcherFaultEntry) { RaiseException(0xC0000005, 0, 0, nullptr); ctx.r3.u64 = 0; }
static PPC_FUNC(countProbe) {
    uint32_t argument = ctx.r3.u32;
    memory->write32(argument, 1);
    while (!memory->read32(argument + 4)) Sleep(1);
    ctx.r3.u64 = 0;
    __imp__ExTerminateThread(ctx, base);
}
static PPC_FUNC(selfProbe) {
    uint32_t argument = ctx.r3.u32;
    memory->write32(argument, 1);
    while (!memory->read32(argument + 4)) Sleep(1);
    ctx.r3.u64 = memory->read32(argument + 8);
    ctx.r4.u64 = argument + 12;
    __imp__NtSuspendThread(ctx, base);
    if (ctx.r3.u32) {
        memory->write32(argument, 0xDEADu);
        ctx.r3.u64 = 0xDEADu;
        __imp__ExTerminateThread(ctx, base);
    }
    memory->write32(argument, 2);
    memory->write32(argument + 4, 0);
    while (!memory->read32(argument + 16)) Sleep(1);
    ctx.r3.u64 = 0;
    __imp__ExTerminateThread(ctx, base);
}
static void testSuspendResume(PPCContext& ctx) {
    auto* base = memory->base();
    uint32_t timeoutCell = memory->allocate(16);
    check(timeoutCell, "Suspend wait fixture allocation failed");
    auto waitThread = [&](uint32_t handle) {
        *reinterpret_cast<uint64_t*>(base + timeoutCell) = _byteswap_uint64(uint64_t(-50000000ll));
        ctx.r3.u64 = handle; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = timeoutCell;
        __imp__NtWaitForSingleObjectEx(ctx, base);
        return ctx.r3.u32;
    };
    auto pollEqual = [&](uint32_t address, uint32_t value) {
        uint64_t deadline = GetTickCount64() + 5000;
        for (;;) {
            if (memory->read32(address) == value) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(1);
        }
    };
    auto suspend = [&](uint32_t handle, uint32_t out) {
        ctx.r3.u64 = handle; ctx.r4.u64 = out;
        __imp__NtSuspendThread(ctx, base);
        return ctx.r3.u32;
    };
    auto resume = [&](uint32_t handle, uint32_t out) {
        ctx.r3.u64 = handle; ctx.r4.u64 = out;
        __imp__NtResumeThread(ctx, base);
        return ctx.r3.u32;
    };
    uint32_t scratch = memory->allocate(512);
    check(scratch, "Suspend scratch allocation failed");
    auto createWorker = [&](uint32_t entry, uint32_t argument) {
        ctx.r3.u64 = scratch; ctx.r4.u64 = 0x20000; ctx.r5.u64 = scratch + 4;
        ctx.r6.u64 = 0; ctx.r7.u64 = entry; ctx.r8.u64 = argument; ctx.r9.u64 = 1;
        __imp__ExCreateThread(ctx, base);
        check(ctx.r3.u32 == 0, "Suspend worker creation failed");
        return memory->read32(scratch);
    };
    constexpr uint32_t probeA = PPC_CODE_BASE, probeB = PPC_CODE_BASE + 4;
    auto originalA = PPC_LOOKUP_FUNC(base, probeA);
    auto originalB = PPC_LOOKUP_FUNC(base, probeB);
    PPC_LOOKUP_FUNC(base, probeA) = countProbe;
    PPC_LOOKUP_FUNC(base, probeB) = selfProbe;
    uint32_t blockA = memory->allocate(64);
    uint32_t blockB = memory->allocate(64);
    uint32_t prevOut = memory->allocate(64);
    check(blockA && blockB && prevOut, "Suspend worker fixture allocation failed");
    memset(base + blockA, 0, 64);
    memset(base + blockB, 0, 64);
    uint32_t workerA = createWorker(probeA, blockA);
    ctx.r3.u64 = workerA; ctx.r4.u64 = 0;
    sub_828A7F10(ctx, base);
    check(ctx.r3.u32 == 1, "Original suspend wrapper count wrong");
    check(suspend(workerA, prevOut) == 0 && memory->read32(prevOut) == 2, "Second suspend count wrong");
    Sleep(100);
    check(memory->read32(blockA) == 0, "Suspended worker executed early");
    check(resume(workerA, prevOut) == 0 && memory->read32(prevOut) == 3, "First resume count wrong");
    check(resume(workerA, prevOut) == 0 && memory->read32(prevOut) == 2, "Second resume count wrong");
    Sleep(50);
    check(memory->read32(blockA) == 0, "Worker ran before final resume");
    check(resume(workerA, prevOut) == 0 && memory->read32(prevOut) == 1, "Final resume count wrong");
    check(pollEqual(blockA, 1), "Worker did not run after final resume");
    memory->write32(blockA + 4, 1);
    check(waitThread(workerA) == 0, "Count worker did not exit");
    ctx.r3.u64 = workerA; __imp__NtClose(ctx, base);
    uint32_t workerB = createWorker(probeB, blockB);
    memory->write32(blockB + 8, workerB);
    check(resume(workerB, 0) == 0, "Self-suspend worker resume failed");
    check(pollEqual(blockB, 1), "Self-suspend worker did not start");
    memory->write32(blockB + 4, 1);
    check(memory->read32(blockB) != 0xDEADu, "Self-suspend failed inside worker");
    {
        uint64_t deadline = GetTickCount64() + 5000;
        uint32_t previous = 0;
        bool suspended = false;
        for (;;) {
            ctx.r3.u64 = workerB; ctx.r4.u64 = prevOut;
            __imp__NtResumeThread(ctx, base);
            previous = memory->read32(prevOut);
            if (previous == 1) {
                suspended = true;
                break;
            }
            if (GetTickCount64() >= deadline) break;
            Sleep(10);
        }
        check(suspended, "Worker never observed suspended");
        check(memory->read32(blockB + 12) == 0, "Self-suspend previous count wrong");
    }
    check(pollEqual(blockB, 2), "Worker did not continue after resume");
    memory->write32(blockB + 16, 1);
    check(waitThread(workerB) == 0, "Self-suspend worker did not exit");
    ctx.r3.u64 = workerB; __imp__NtClose(ctx, base);
    check(suspend(workerB, prevOut) == 0xc0000008, "Closed handle suspend succeeded");
    check(suspend(0x12345678, prevOut) == 0xc0000008, "Invalid handle suspend succeeded");
    ctx.r3.u64 = scratch + 16; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 0;
    __imp__NtCreateEvent(ctx, base);
    check(ctx.r3.u32 == 0, "Suspend test event creation failed");
    uint32_t eventHandle = memory->read32(scratch + 16);
    check(suspend(eventHandle, prevOut) == 0xc0000008, "Non-thread suspend succeeded");
    ctx.r3.u64 = eventHandle; __imp__NtClose(ctx, base);
    ctx.r3.u64 = 0x12345678; ctx.r4.u64 = 0;
    sub_828A7F10(ctx, base);
    check(ctx.r3.u32 == 0xffffffff, "Original wrapper did not report invalid handle");
    memory->release(blockA); memory->release(blockB); memory->release(prevOut);
    memory->release(timeoutCell); memory->release(scratch);
    PPC_LOOKUP_FUNC(base, probeA) = originalA;
    PPC_LOOKUP_FUNC(base, probeB) = originalB;
    puts("Suspend counts, self-suspension and handle validation passed.");
}
static PPC_FUNC(launcherReturnEntry) { ctx.r3.u64 = 0; }
static PPC_FUNC(inflateAllocate) {
    uint64_t bytes = uint64_t(ctx.r4.u32) * ctx.r5.u32;
    ctx.r3.u64 = bytes <= UINT32_MAX ? memory->allocate(uint32_t(bytes)) : 0;
}
static PPC_FUNC(inflateFree) {
    if (ctx.r4.u32) check(memory->release(ctx.r4.u32), "Inflate released an unknown allocation");
}
static void testInflate(PPCContext& ctx, const char* fixturePath) {
    auto* base = memory->base();
    auto allocOriginal = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    auto freeOriginal = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE + 4);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = inflateAllocate;
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE + 4) = inflateFree;
    std::ifstream fixtures(fixturePath, std::ios::binary);
    check(bool(fixtures), "Cannot open compression fixtures");
    for (int sample = 0; sample < 3; ++sample) {
        uint32_t sizes[2]; fixtures.read(reinterpret_cast<char*>(sizes), sizeof(sizes));
        check(bool(fixtures) && sizes[0] < 1000000 && sizes[1] < 1000000, "Invalid compression fixture header");
        std::vector<uint8_t> expected(sizes[0]), compressed(sizes[1]);
        fixtures.read(reinterpret_cast<char*>(expected.data()), expected.size());
        fixtures.read(reinterpret_cast<char*>(compressed.data()), compressed.size());
        check(bool(fixtures), "Truncated compression fixture");
        uint32_t stream = memory->allocate(4096), input = memory->allocate(sizes[1]), output = memory->allocate(sizes[0] + 64);
        check(stream && input && output, "Compression fixture allocation failed");
        memcpy(base + input, compressed.data(), compressed.size());
        memory->write32(stream + 32, PPC_CODE_BASE);
        memory->write32(stream + 36, PPC_CODE_BASE + 4);
        ctx.r3.u64 = stream;
        sub_8222B248(ctx, base);
        check(ctx.r3.s32 == 0, "Original decompressor initialization failed");
        memory->write32(stream, input);
        memory->write32(stream + 12, output);
        uint32_t supplied = 0, offered = 0;
        int status = 0;
        for (int iteration = 0; iteration < 100000 && status != 1; ++iteration) {
            if (!memory->read32(stream + 4) && supplied < sizes[1]) {
                uint32_t count = (std::min)(17u, sizes[1] - supplied);
                memory->write32(stream + 4, count); supplied += count;
            }
            if (!memory->read32(stream + 16) && offered < sizes[0] + 64) {
                uint32_t count = (std::min)(313u, sizes[0] + 64 - offered);
                memory->write32(stream + 16, count); offered += count;
            }
            ctx.r3.u64 = stream; ctx.r4.u64 = 0;
            sub_8222B468(ctx, base);
            status = ctx.r3.s32;
            if (status != 0 && status != 1) fprintf(stderr, "inflate fixture=%d status=%d in=%u out=%u\n",
                sample, status, memory->read32(stream + 8), memory->read32(stream + 20));
            check(status == 0 || status == 1, "Original decompressor rejected an independently generated stream");
        }
        check(status == 1 && memory->read32(stream + 20) == sizes[0], "Original decompressor did not finish at the expected size");
        check(memcmp(base + output, expected.data(), expected.size()) == 0, "Native game decompression differs from Python zlib");
        ctx.r3.u64 = stream; sub_8222CBB0(ctx, base);
        check(ctx.r3.s32 == 0, "Original decompressor cleanup failed");
        memory->release(stream); memory->release(input); memory->release(output);
    }
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = allocOriginal;
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE + 4) = freeOriginal;
    puts("Original game decompressor matches independent stored, fixed and dynamic DEFLATE fixtures.");
}
static void testSynchronization(PPCContext& ctx, uint32_t scratch) {
    {
        std::vector<HostProcessor> topology;
        for(unsigned cpu=0;cpu<16;++cpu)topology.push_back({0,BYTE(cpu),BYTE(cpu/2),8});
        for(unsigned cpu=16;cpu<32;++cpu)topology.push_back({0,BYTE(cpu),BYTE(cpu-8),0});
        topology.push_back({1,0,0,15}); // Another group must not win the ranking.
        const auto map=mapGuestProcessors(topology,0,0xFFFFFFFFull);
        check(map==std::array<unsigned,6>{4,6,8,10,12,14},"Guest workers share SMT siblings or crowd engine/display cores");
        check(guestProcessorMask(0x21,map)==0x4010,"Guest mask did not retain both mapped processor choices");
        check(!guestProcessorMask(0,map) && !guestProcessorMask(64,map),"Invalid guest affinity accepted");
        const auto restricted=mapGuestProcessors(topology,0,(1ull<<9)|(1ull<<15));
        check(restricted==std::array<unsigned,6>{9,15,9,15,9,15},"Restricted affinity escaped the allowed host processors");
        check(mapGuestProcessors({{0,0,0,0},{0,1,0,0},{0,2,1,0},{0,3,1,0}},0,15)==
              std::array<unsigned,6>{0,2,0,2,0,2},"Small host did not prefer different physical cores");
        check(guestProcessorMask(63,mapGuestProcessors({},0,0))==0,"Empty host topology produced a processor mask");
    }
    auto* base = memory->base();
    auto wait = [&](uint32_t handle) {
        ctx.r3.u64 = handle; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = scratch + 64;
        __imp__NtWaitForSingleObjectEx(ctx, base);
        return ctx.r3.u32;
    };
    ctx.r3.u64 = scratch; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 0;
    __imp__NtCreateEvent(ctx, base);
    check(ctx.r3.u32 == 0, "Native event creation failed");
    uint32_t event = memory->read32(scratch);
    check(wait(event) == 0x102, "Unsignaled event did not time out");
    ctx.r3.u64 = event; ctx.r4.u64 = scratch + 4;
    __imp__NtSetEvent(ctx, base);
    check(ctx.r3.u32 == 0 && memory->read32(scratch + 4) == 0, "Event previous state is wrong");
    check(wait(event) == 0 && wait(event) == 0x102, "Auto-reset event did not consume exactly one signal");
    ctx.r3.u64 = event; __imp__NtClose(ctx, base);
    check(wait(event) == 0xc0000008, "Closed event handle is still valid");
    constexpr uint32_t probeAddress = PPC_CODE_BASE;
    auto original = PPC_LOOKUP_FUNC(base, probeAddress);
    PPC_LOOKUP_FUNC(base, probeAddress) = threadProbe;
    uint32_t allocatedBefore = memory->allocatedBytes();
    ctx.r3.u64 = scratch; ctx.r4.u64 = 0x20000; ctx.r5.u64 = scratch + 4;
    ctx.r6.u64 = 0; ctx.r7.u64 = probeAddress; ctx.r8.u64 = scratch + 96; ctx.r9.u64 = 0x10000001;
    __imp__ExCreateThread(ctx, base);
    check(ctx.r3.u32 == 0, "Native suspended thread creation failed");
    uint32_t thread = memory->read32(scratch);
    check(wait(thread) == 0x102 && memory->read32(scratch + 96) == 0, "Suspended thread executed early");
    ctx.r3.u64=thread;ctx.r4.u64=0x80000000;ctx.r5.u64=scratch+28;
    __imp__ObReferenceObjectByHandle(ctx,base);
    check(ctx.r3.u32==0,"Cannot reference suspended affinity-test worker");
    const auto threadObject=memory->read32(scratch+28);
    const auto processorByte=threadObject-Memory::threadObjectOffset+0x10c;
    ctx.r3.u64=threadObject;ctx.r4.u64=6;ctx.r5.u64=scratch+32;
    __imp__KeSetAffinityThread(ctx,base);
    check(ctx.r3.u32==0 && memory->read32(scratch+32)==16 && base[processorByte]==1,
          "Native affinity mapping changed guest processor identity or previous mask");
    memory->write32(scratch+32,0xDEADBEEF);
    ctx.r3.u64=threadObject;ctx.r4.u64=64;ctx.r5.u64=scratch+32;
    __imp__KeSetAffinityThread(ctx,base);
    check(ctx.r3.u32==0xC000000D && memory->read32(scratch+32)==0xDEADBEEF && base[processorByte]==1,
          "Rejected affinity request changed guest state");
    ctx.r3.u64=threadObject;ctx.r4.u64=16;ctx.r5.u64=scratch+32;
    __imp__KeSetAffinityThread(ctx,base);
    check(ctx.r3.u32==0 && memory->read32(scratch+32)==6 && base[processorByte]==4,
          "Restoring affinity did not restore guest PCR identity");
    ctx.r3.u64=threadObject;__imp__ObDereferenceObject(ctx,base);
    auto exitQuery = [&](uint32_t handle, uint32_t out) {
        memory->write32(0x820007DC, 0x80000000);
        ctx.r3.u64 = handle; ctx.r4.u64 = out;
        sub_828A7F50(ctx, base);
        return ctx.r3.u32;
    };
    check(exitQuery(thread, scratch + 24) == 1 && memory->read32(scratch + 24) == 259, "Live thread did not report STILL_ACTIVE");
    ctx.r3.u64 = thread; ctx.r4.u64 = scratch + 8;
    __imp__NtResumeThread(ctx, base);
    check(ctx.r3.u32 == 0 && memory->read32(scratch + 8) == 1, "Resume count is wrong");
    *reinterpret_cast<uint64_t*>(base + scratch + 64) = _byteswap_uint64(uint64_t(-50000000ll));
    check(wait(thread) == 0, "Native thread did not exit within five seconds");
    check(exitQuery(thread, scratch + 24) == 1 && memory->read32(scratch + 24) == 23, "Terminated thread did not report its exit code");
    check(memory->read32(scratch + 96) != ctx.r13.u32, "Thread reused the main PCR");
    check(memory->read32(scratch + 100) == memory->read32(scratch + 4), "Thread ID does not match its TEB");
    check(memory->read32(scratch + 104) == 0, "Thread inherited unrelated TLS contents");
    check(memory->read32(scratch + 112) == 0 && memory->read32(scratch + 108) != 0, "Current-thread pseudo handle is invalid");
    check(memory->read32(scratch + 116) == 0, "Thread termination returned to guest code");
    check(memory->read32(scratch + 120) == 4, "Creation affinity was not reflected in PCR.Number");
    {
        uint32_t model = memory->allocate(512);
        check(model != 0, "Mixer barrier model allocation failed");
        memory->write32(model + 304, 1);
        memory->write32(model + 324, 1); // Only processor 4 participates.
        PPCContext barrier = ctx;
        barrier.r13.u64 = memory->read32(scratch + 96);
        barrier.r3.u64 = model; barrier.r4.u64 = model + 356;
        sub_828B3DC8(barrier, base);
        check(memory->read32(model + 356) == 0 && memory->read32(model + 360) == 0,
              "Original mixer barrier did not consume its processor completion marker");
        check(base[ctx.r13.u32 + 0x10c] == 0, "Worker processor identity changed the main PCR");
        memory->release(model);
    }
    ctx.r3.u64 = 47; __imp__KeTlsGetValue(ctx, base);
    check(ctx.r3.u32 == 0x12345678, "Worker changed the main thread's TLS");
    ctx.r3.u64 = thread; __imp__NtClose(ctx, base);
    check(exitQuery(thread, scratch + 24) == 0, "Closed thread handle still resolves");
    check(memory->allocatedBytes() == allocatedBefore, "Closed and terminated thread leaked its guest allocation");
    PPC_LOOKUP_FUNC(base, probeAddress) = original;
}
static void testVsnprintf(PPCContext& ctx) {
    auto* base = memory->base();
    uint32_t destination = memory->allocate(128);
    uint32_t format = memory->allocate(64);
    uint32_t value = memory->allocate(32);
    uint32_t arguments = memory->allocate(32);
    check(destination && format && value && arguments, "Formatter test allocation failed");
    memcpy(base + format, "%s %03d %x", 11);
    memcpy(base + value, "ready", 6);
    // The original std instructions spill full big-endian 64-bit GPR slots.
    PPC_STORE_U64(arguments + 0, value);
    PPC_STORE_U64(arguments + 8, 7);
    PPC_STORE_U64(arguments + 16, 0xabcd);
    ctx.r3.u64 = destination; ctx.r4.u64 = 128; ctx.r5.u64 = format; ctx.r6.u64 = arguments;
    __imp___vsnprintf(ctx, base);
    check(ctx.r3.s32 == 14 && strcmp(reinterpret_cast<const char*>(base + destination), "ready 007 abcd") == 0,
          "Guest PPC vsnprintf formatting contract failed");
    memory->release(destination); memory->release(format); memory->release(value);
    memory->release(arguments);
}
static void testInput(PPCContext& ctx) {
    auto* base = memory->base();
    uint32_t state = memory->allocate(32);
    check(state != 0, "Input test allocation failed");
    memset(base + state, 0xa5, 16);
    ctx.r3.u64 = 4;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = state;
    __imp__XamInputGetState(ctx, base);
    check(ctx.r3.u32 == ERROR_INVALID_PARAMETER &&
          std::all_of(base + state, base + state + 16, [](uint8_t value) { return value == 0xa5; }),
          "XamInputGetState accepted an invalid player or altered its output");
    ctx.r3.u64 = 0;
    ctx.r5.u64 = 0;
    __imp__XamInputGetState(ctx, base);
    check(ctx.r3.u32 == ERROR_INVALID_PARAMETER, "XamInputGetState accepted a null state pointer");
    memory->release(state);
}
static std::atomic<uint32_t> gAudioProbeCell{0};
static std::atomic<uint32_t> gAudioProbeRaw{0};
static std::atomic<uint32_t> gAudioProbeCalls{0};
static std::atomic<uint32_t> gAudioProbeR13{0};
static std::atomic<uint32_t> gAudioProbeSubmitOk{0};
static std::atomic<uint32_t> gAudioProbePlanar{0};
static std::atomic<uint32_t> gAudioProbeDoSubmit{0};
static std::atomic<uint32_t> gAudioProbeDriverOut{0};
static std::atomic<uint32_t> gAudioProbeSelfUnreg{0};
static std::atomic<uint32_t> gAudioProbeSelfUnregStatus{0xFFFFFFFFu};
static std::atomic<uint32_t> gAudioProbeWaitEvent{0};
static std::atomic<uint32_t> gAudioProbeWaitEntered{0};
static HANDLE gAudioProbeHoldEntered = nullptr;
static HANDLE gAudioProbeHoldRelease = nullptr;
static PPC_FUNC(audioProbeCallback) {
    if (gAudioProbeHoldRelease) {
        SetEvent(gAudioProbeHoldEntered);
        WaitForSingleObject(gAudioProbeHoldRelease, 5000);
    }
    uint32_t cell = ctx.r3.u32;
    gAudioProbeCell.store(cell, std::memory_order_relaxed);
    gAudioProbeRaw.store(memory->read32(cell), std::memory_order_relaxed);
    gAudioProbeR13.store(ctx.r13.u32, std::memory_order_relaxed);
    if (uint32_t event = gAudioProbeWaitEvent.load(std::memory_order_acquire)) {
        gAudioProbeWaitEntered.store(1, std::memory_order_release);
        ctx.r3.u64 = event; ctx.r6.u64 = 0; ctx.r7.u64 = 0;
        __imp__KeWaitForSingleObject(ctx, base);
    }
    if (gAudioProbeSelfUnreg.load(std::memory_order_acquire) == 1) {
        uint32_t tok = memory->read32(gAudioProbeDriverOut.load(std::memory_order_acquire));
        ctx.r3.u64 = tok;
        __imp__XAudioUnregisterRenderDriverClient(ctx, base);
        gAudioProbeSelfUnregStatus.store(ctx.r3.u32, std::memory_order_relaxed);
        gAudioProbeSelfUnreg.store(2, std::memory_order_release);
    }
    if (gAudioProbeDoSubmit.load(std::memory_order_acquire) != 0) {
        uint32_t planar = gAudioProbePlanar.load(std::memory_order_acquire);
        uint32_t outSlot = gAudioProbeDriverOut.load(std::memory_order_acquire);
        if (planar && outSlot) {
            ctx.r3.u64 = memory->read32(outSlot);
            ctx.r4.u64 = planar;
            __imp__XAudioSubmitRenderDriverFrame(ctx, base);
            if (ctx.r3.u32 == 0) gAudioProbeSubmitOk.store(1, std::memory_order_release);
        }
    }
    gAudioProbeCalls.fetch_add(1, std::memory_order_release);
    ctx.r3.u64 = 0;
}
static void testAudioDriver(PPCContext& ctx, const char* gameDir) {
    testAudioOutputHeadroom();
    AudioTapTestEnvironment disableExternalResamplerTrace(L"DARKRECOMP_RESAMPLER_TRACE", nullptr);
    testAudioPcmTap();
    // A launcher's capture path must not be populated with synthetic test audio.
    AudioTapTestEnvironment disableExternalTap(L"DARKRECOMP_AUDIO_TAP", nullptr);
    testAudioResampler(ctx);
    testAudioFinalSdk(ctx);
    testAudioBiquad(ctx);
    testAudioVolume(ctx);
    testAudioVolumeMatrix(ctx);
    testAudioVoiceInput(ctx);
    testAudioVoiceSrc(ctx);
    testAudioResamplerTrace();
    testAudioContinuity();
    auto* base = memory->base();
    Memory* mainMem = memory;
    {
        uint8_t planarBE[6 * 256 * 4];
        for (uint32_t ch = 0; ch < 6; ++ch)
            for (uint32_t fr = 0; fr < 256; ++fr) {
                float v = 0.0f;
                if (ch == 0 && fr == 0) v = 1.0f;
                else if (ch == 5 && fr == 255) v = -1.0f;
                else if (ch == 2 && fr == 7) v = 0.5f;
                uint32_t bits;
                memcpy(&bits, &v, 4);
                bits = _byteswap_ulong(bits);
                memcpy(planarBE + (size_t(ch) * 256 + fr) * 4, &bits, 4);
            }
        float inter[6 * 256];
        audioConvertPlanarBEFloat(planarBE, inter);
        check(inter[0 * 6 + 0] == 1.0f, "Conversion misplaced channel-0 frame-0");
        check(inter[255 * 6 + 5] == -1.0f, "Conversion misplaced channel-5 frame-255");
        check(inter[7 * 6 + 2] == 0.5f, "Conversion misplaced channel-2 frame-7");
        check(inter[3 * 6 + 4] == 0.0f, "Conversion leaked into untouched cell");
        puts("Audio planar-BE to interleaved conversion math verified (structural data).");
    }
    auto* original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = audioProbeCallback;
    uint32_t descriptor = memory->allocate(8);
    uint32_t outSlot = memory->allocate(4);
    uint32_t planar = memory->allocate(6 * 256 * 4);
    check(descriptor && outSlot && planar, "Audio driver fixture allocation failed");
    for (uint32_t ch = 0; ch < 6; ++ch)
        for (uint32_t fr = 0; fr < 256; ++fr) {
            float v = float(int((ch * 256 + fr) % 200 - 100)) * 0.005f;
            uint32_t bits;
            memcpy(&bits, &v, 4);
            memory->write32(planar + (ch * 256 + fr) * 4, bits);
        }
    auto streamSample = [&](uint32_t ch, uint32_t fr) {
        return float(int((ch * 256 + fr) % 200 - 100)) * 0.005f;
    };
    memory->write32(descriptor, PPC_CODE_BASE);
    memory->write32(descriptor + 4, 0xA5A50001u);
    memory->write32(outSlot, 0xDEADBEEFu);
    gAudioProbePlanar.store(planar, std::memory_order_relaxed);
    gAudioProbeDriverOut.store(outSlot, std::memory_order_relaxed);
    gAudioProbeDoSubmit.store(1, std::memory_order_relaxed);
    gAudioProbeCalls.store(0, std::memory_order_relaxed);
    gAudioProbeSubmitOk.store(0, std::memory_order_relaxed);
    gAudioProbeSelfUnreg.store(0, std::memory_order_relaxed);
    auto waitCalls = [&](uint32_t target, uint32_t timeoutMs) {
        uint64_t deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            if (gAudioProbeCalls.load(std::memory_order_acquire) >= target) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }
    };
    auto waitCounters = [&](uint64_t frames, uint64_t done, uint32_t timeoutMs) {
        uint64_t deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            AudioDriverCounters c = AudioRenderDriver::instance().counters();
            if (c.framesSubmitted >= frames && c.buffersCompleted >= done) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }
    };
    auto waitDrained = [&](uint32_t timeoutMs) {
        uint64_t deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            if (AudioRenderDriver::instance().counters().queuedBuffers == 0) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(5);
        }
    };
    bool registered = false;
    uint32_t token = 0;
    uint32_t seenTokens[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int genCount = 0;
    auto noteToken = [&](uint32_t t) {
        check(t != 0 && (t & 0xFFF00000u) == 0x44400000u, "Driver generation token malformed");
        for (int i = 0; i < genCount; ++i) check(t != seenTokens[i], "Driver token reused");
        seenTokens[genCount++] = t;
    };
    auto registerOne = [&]() {
        ctx.r3.u64 = descriptor;
        ctx.r4.u64 = outSlot;
        __imp__XAudioRegisterRenderDriverClient(ctx, base);
        check(ctx.r3.u32 == 0, "Audio registration failed");
        uint32_t t = memory->read32(outSlot);
        noteToken(t);
        token = t;
        registered = true;
        return t;
    };
    auto unregisterOne = [&]() {
        ctx.r3.u64 = token;
        __imp__XAudioUnregisterRenderDriverClient(ctx, base);
        check(ctx.r3.u32 == 0, "Unregister failed");
        registered = false;
    };
    auto waitNewCallbacks = [&](uint32_t timeoutMs) {
        uint32_t before = gAudioProbeCalls.load(std::memory_order_acquire);
        uint64_t deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            if (gAudioProbeCalls.load(std::memory_order_acquire) > before) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }
    };
    try {
        ctx.r3.u64 = descriptor;
        ctx.r4.u64 = outSlot;
        __imp__XAudioRegisterRenderDriverClient(ctx, base);
        if (ctx.r3.u32 != 0) {
            check(!AudioRenderDriver::instance().counters().workerRunning,
                  "Failed registration left a worker running");
            check(AudioRenderDriver::instance().counters().deviceErrors > 0,
                  "Failed registration reported no device error");
            check(memory->read32(outSlot) == 0xDEADBEEFu, "Failed registration wrote a token");
            printf("Audio device unavailable (status=0x%08X); explicit error path verified.\n",
                   ctx.r3.u32);
            PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
            memory->release(descriptor);
            memory->release(outSlot);
            memory->release(planar);
            return;
        }
        {
            uint32_t t = memory->read32(outSlot);
            noteToken(t);
            token = t;
            registered = true;
        }
        check(waitCalls(1, 5000), "Guest audio callback never invoked");
        check(gAudioProbeRaw.load(std::memory_order_acquire) == 0xA5A50001u,
              "Callback did not receive owned arg cell");
        check(gAudioProbeR13.load(std::memory_order_acquire) != ctx.r13.u32 &&
                  gAudioProbeR13.load(std::memory_order_acquire) != 0,
              "Callback worker shares the main PCR");
        printf("Audio callback on worker PCR 0x%08X with owned arg cell.\n",
               gAudioProbeR13.load(std::memory_order_acquire));
        {
            ctx.r3.u64 = descriptor;
            ctx.r4.u64 = outSlot;
            __imp__XAudioRegisterRenderDriverClient(ctx, base);
            check(ctx.r3.u32 == 0x800700AAu, "Duplicate active client not rejected");
        }
        {
            uint32_t maskCell = memory->allocate(4);
            check(maskCell, "Volume-mask fixture allocation failed");
            memory->write32(maskCell, 0xDEADBEEFu);
            ctx.r3.u64 = token;
            ctx.r4.u64 = maskCell;
            __imp__XAudioGetVoiceCategoryVolumeChangeMask(ctx, base);
            check(ctx.r3.u32 == 0, "Volume change mask query failed");
            check(memory->read32(maskCell) == 0, "Volume change mask not honestly empty");
            ctx.r3.u64 = 0x12345678;
            ctx.r4.u64 = maskCell;
            __imp__XAudioGetVoiceCategoryVolumeChangeMask(ctx, base);
            check(ctx.r3.u32 != 0, "Volume mask accepted foreign token");
            memory->release(maskCell);
            puts("Volume change mask validated against generation token.");
        }
        check(waitCounters(16, 12, 20000),
              "Callback-originated stream stalled (need >=16 frames, >=12 completions)");
        {
            AudioDriverCounters a = AudioRenderDriver::instance().counters();
            Sleep(400);
            AudioDriverCounters b = AudioRenderDriver::instance().counters();
            check(b.framesSubmitted > a.framesSubmitted && b.buffersCompleted > a.buffersCompleted,
                  "Stream did not progress across separated samples");
            printf("Stream progress (not continuity proof): %llu frames/%llu done -> %llu frames/%llu done.\n",
                   a.framesSubmitted, a.buffersCompleted, b.framesSubmitted, b.buffersCompleted);
        }
        {
            uint64_t deadline = GetTickCount64() + 5000;
            while (!gAudioProbeSubmitOk.load(std::memory_order_acquire) &&
                   GetTickCount64() < deadline)
                Sleep(10);
            check(gAudioProbeSubmitOk.load(std::memory_order_acquire) == 1,
                  "No in-callback submit accepted");
        }
        {
            std::vector<float> peek;
            check(audioDriverPeekLastSubmitted(peek) && peek.size() == 6 * 256,
                  "Submitted buffer peek failed");
            check(peek[0 * 6 + 0] == streamSample(0, 0) &&
                      peek[1 * 6 + 1] == streamSample(1, 1) &&
                      peek[255 * 6 + 5] == streamSample(5, 255) &&
                      peek[7 * 6 + 3] == streamSample(3, 7),
                  "Submitted interleaved content wrong");
        }
        {
            ctx.r3.u64 = token + 1;
            ctx.r4.u64 = planar;
            __imp__XAudioSubmitRenderDriverFrame(ctx, base);
            check(ctx.r3.u32 == 0xC000000Du, "Fabricated token accepted while client active");
            ctx.r3.u64 = token + 1;
            __imp__XAudioUnregisterRenderDriverClient(ctx, base);
            check(ctx.r3.u32 == 0xC000000Du, "Fabricated unregister accepted while active");
        }
        gAudioProbeDoSubmit.store(0, std::memory_order_release);
        check(waitDrained(8000), "Idle source never drained");
        check(AudioRenderDriver::instance().counters().workerRunning,
              "Idle worker reported dead");
        {
            const auto before = AudioRenderDriver::instance().counters();
            const uint32_t calls = gAudioProbeCalls.load();
            const uint64_t start = GetTickCount64();
            Sleep(400);
            const uint64_t elapsed = GetTickCount64() - start;
            const uint32_t retries = gAudioProbeCalls.load() - calls;
            const auto after = AudioRenderDriver::instance().counters();
            check(retries > kAudioPrefillBuffers && retries <= elapsed / 5 + 4,
                  "Empty callbacks either lost all credits or retried without pacing");
            check(after.framesSubmitted == before.framesSubmitted &&
                      after.callbacksWithoutSubmit > before.callbacksWithoutSubmit,
                  "Empty retries fabricated audio or were not counted");
            gAudioProbeDoSubmit.store(1, std::memory_order_release);
            check(waitCounters(after.framesSubmitted + 16, after.buffersCompleted + 12, 5000),
                  "Audio failed to recover after more empty callbacks than its credit budget");
            gAudioProbeDoSubmit.store(0, std::memory_order_release);
            check(waitDrained(5000), "Recovered audio failed to drain");
        }
        puts("Empty callbacks retry at a bounded rate and recover without an external submit.");
        testAudioBufferRegions(token, planar);
        {
            AudioDriverCounters before = AudioRenderDriver::instance().counters();
            ctx.r3.u64 = token;
            ctx.r4.u64 = planar;
            __imp__XAudioSubmitRenderDriverFrame(ctx, base);
            check(ctx.r3.u32 == 0, "Deferred submit rejected");
            check(waitCounters(before.framesSubmitted + 1, before.buffersCompleted + 1, 5000),
                  "Deferred frame never completed");
            AudioDriverCounters after = AudioRenderDriver::instance().counters();
            check(after.buffersCompleted > before.buffersCompleted,
                  "Deferred frame never completed");
            printf("Deferred submit reached real device completion.\n");
        }
        gAudioProbeDoSubmit.store(1, std::memory_order_release);
        {
            uint64_t completedBefore = AudioRenderDriver::instance().counters().buffersCompleted;
            bool busySeen = false;
            for (uint32_t i = 0; i < kAudioMaxQueuedBuffers + 4; ++i) {
                ctx.r3.u64 = token;
                ctx.r4.u64 = planar;
                __imp__XAudioSubmitRenderDriverFrame(ctx, base);
                if (ctx.r3.u32 == 0x800700AAu) {
                    busySeen = true;
                    break;
                }
                check(ctx.r3.u32 == 0, "Direct submit rejected");
            }
            bool drained =
                AudioRenderDriver::instance().counters().buffersCompleted > completedBefore;
            check(busySeen || drained, "Neither queue backpressure nor device drain observed");
            printf("Capacity check: busy=%d drained=%d.\n", int(busySeen), int(drained));
        }
        check(audioDriverInjectVoiceError(int32_t(0x80004005u)) == 0,
              "Injected device-error seam missing");
        check(AudioRenderDriver::instance().counters().deviceErrors > 0 &&
                  AudioRenderDriver::instance().counters().lastHresult == int32_t(0x80004005u),
              "Injected device error not recorded");
        {
            ctx.r3.u64 = token;
            ctx.r4.u64 = planar;
            __imp__XAudioSubmitRenderDriverFrame(ctx, base);
            check(ctx.r3.u32 == 0x80004005u, "Submit after device error did not report it");
        }
        {
            Sleep(300);
            uint32_t s1 = gAudioProbeCalls.load(std::memory_order_acquire);
            Sleep(300);
            uint32_t s2 = gAudioProbeCalls.load(std::memory_order_acquire);
            check(s1 == s2, "Worker pretended healthy after device error");
        }
        puts("Injected device error recorded, reported, worker parked (real sink otherwise).");
        unregisterOne();
        {
            ctx.r3.u64 = token;
            ctx.r4.u64 = planar;
            __imp__XAudioSubmitRenderDriverFrame(ctx, base);
            check(ctx.r3.u32 == 0xC000000Du, "Stale generation token accepted");
        }
        uint32_t oldToken = token;
        // A failed worker handshake must preserve the output slot and permit retry.
        uint32_t beforeFailedStart = memory->allocatedBytes();
        audioDriverInjectStartupError(int32_t(0x80004005u));
        ctx.r3.u64 = descriptor; ctx.r4.u64 = outSlot;
        __imp__XAudioRegisterRenderDriverClient(ctx, base);
        check(ctx.r3.u32 == 0x80004005u && memory->read32(outSlot) == oldToken,
              "Failed startup lost HRESULT or published a token");
        check(!AudioRenderDriver::instance().counters().workerRunning &&
              !AudioRenderDriver::instance().counters().deviceReady &&
              memory->allocatedBytes() == beforeFailedStart,
              "Failed startup retained driver/engine/guest allocations");
        registerOne();
        check(waitNewCallbacks(5000), "Second-generation worker lost");
        ctx.r3.u64 = oldToken; ctx.r4.u64 = planar;
        __imp__XAudioSubmitRenderDriverFrame(ctx, base);
        check(ctx.r3.u32 == 0xc000000d, "Old token accepted while a new client was active");
        gAudioProbeSelfUnreg.store(1, std::memory_order_release);
        {
            uint64_t deadline = GetTickCount64() + 5000;
            while (gAudioProbeSelfUnreg.load(std::memory_order_acquire) != 2 &&
                   GetTickCount64() < deadline)
                Sleep(10);
            check(gAudioProbeSelfUnreg.load(std::memory_order_acquire) == 2,
                  "Self-unregister call never ran");
            check(gAudioProbeSelfUnregStatus.load(std::memory_order_acquire) == 0x800700AAu,
                  "Self-unregister did not return busy");
        }
        check(waitNewCallbacks(3000), "Worker died after self-unregister busy");
        puts("Self-unregister returns busy without self-join; worker survives.");
        unregisterOne();
        registerOne();
        check(waitNewCallbacks(5000), "Third-generation worker lost");
        unregisterOne();
        printf("Repeated registration cycles clean; %d unique tokens.\n", genCount);
        registerOne();
        check(waitNewCallbacks(5000), "Fourth-generation worker lost");
        {
            uint32_t callsBefore = gAudioProbeCalls.load(std::memory_order_acquire);
            {
                Memory other;
                (void)other;
            }
            uint64_t deadline = GetTickCount64() + 5000;
            while (gAudioProbeCalls.load(std::memory_order_acquire) == callsBefore &&
                   GetTickCount64() < deadline)
                Sleep(10);
            check(gAudioProbeCalls.load(std::memory_order_acquire) > callsBefore,
                  "Secondary Memory teardown stopped owner audio");
        }
        puts("Secondary Memory destruction left owner audio running.");
        unregisterOne();
        {
            auto altOwner = std::make_unique<Memory>();
            Memory* alt = altOwner.get();
            Memory* saved = memory;
            try {
                alt->load(gameDir);
                memory = alt;
                PPC_LOOKUP_FUNC(alt->base(), PPC_CODE_BASE) = audioProbeCallback;
                gAudioProbeDoSubmit.store(0, std::memory_order_release);
                gAudioProbeSelfUnreg.store(0, std::memory_order_release);
                gAudioProbeDriverOut.store(0, std::memory_order_relaxed);
                uint32_t aDesc = alt->allocate(8);
                uint32_t aOut = alt->allocate(4);
                check(aDesc && aOut, "Alt-arena fixture allocation failed");
                alt->write32(aDesc, PPC_CODE_BASE);
                alt->write32(aDesc + 4, 0xBEEF0001u);
                alt->write32(aOut, 0xDEADBEEFu);
                gAudioProbeDriverOut.store(aOut, std::memory_order_release);
                gAudioProbeHoldEntered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
                gAudioProbeHoldRelease = CreateEvent(nullptr, TRUE, FALSE, nullptr);
                check(gAudioProbeHoldEntered && gAudioProbeHoldRelease, "Callback gate allocation failed");
                uint32_t altCallsBefore = gAudioProbeCalls.load(std::memory_order_acquire);
                ctx.r3.u64 = aDesc;
                ctx.r4.u64 = aOut;
                __imp__XAudioRegisterRenderDriverClient(ctx, alt->base());
                check(ctx.r3.u32 == 0, "Alt-arena registration failed");
                uint32_t altToken = alt->read32(aOut);
                check(altToken != 0 && altToken != token, "Alt-arena token wrong");
                check(WaitForSingleObject(gAudioProbeHoldEntered, 2000) == WAIT_OBJECT_0,
                      "Alt-arena worker never entered callback gate");
                auto unregistering = std::async(std::launch::async, [altToken] {
                    return AudioRenderDriver::instance().unregisterClient(altToken);
                });
                uint64_t deadline = GetTickCount64() + 2000;
                while (AudioRenderDriver::instance().counters().workerRunning && GetTickCount64() < deadline)
                    Sleep(1);
                check(!AudioRenderDriver::instance().counters().workerRunning,
                      "Concurrent unregister did not enter closing state");
                // Destruction on an STA thread must wait for that in-flight unregister.
                auto destroying = std::async(std::launch::async, [&] {
                    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    altOwner.reset();
                    if (SUCCEEDED(hr)) CoUninitialize();
                    return hr;
                });
                check(destroying.wait_for(std::chrono::milliseconds(60)) == std::future_status::timeout,
                      "Owner destruction freed the arena during its callback");
                SetEvent(gAudioProbeHoldRelease);
                check(unregistering.get() == 0 && SUCCEEDED(destroying.get()),
                      "Concurrent unregister/STA owner teardown failed");
                check(gAudioProbeCalls.load(std::memory_order_acquire) > altCallsBefore,
                      "Alt-arena callback did not finish before teardown");
                check(gAudioProbeRaw.load(std::memory_order_acquire) == 0xBEEF0001u,
                      "Alt-arena arg cell wrong");
                memory = saved;
                gAudioProbeDriverOut.store(outSlot, std::memory_order_relaxed);
                CloseHandle(gAudioProbeHoldEntered); CloseHandle(gAudioProbeHoldRelease);
                gAudioProbeHoldEntered = gAudioProbeHoldRelease = nullptr;
                printf("Owning Memory destruction joined worker before freeing arena.\n");
            } catch (...) {
                if (gAudioProbeHoldRelease) SetEvent(gAudioProbeHoldRelease);
                altOwner.reset();
                memory = saved;
                gAudioProbeDriverOut.store(outSlot, std::memory_order_relaxed);
                if (gAudioProbeHoldEntered) CloseHandle(gAudioProbeHoldEntered);
                if (gAudioProbeHoldRelease) CloseHandle(gAudioProbeHoldRelease);
                gAudioProbeHoldEntered = gAudioProbeHoldRelease = nullptr;
                throw;
            }
        }
        gAudioProbeDoSubmit.store(1, std::memory_order_release);
        registerOne();
        check(waitNewCallbacks(5000), "Post-teardown registration failed");
        unregisterOne();
        for (bool ownerShutdown : {false, true}) {
            uint32_t blockedEvent = memory->allocate(16);
            check(blockedEvent != 0, "Audio wait fixture allocation failed");
            memset(base + blockedEvent, 0, 16);
            base[blockedEvent] = 1;
            gAudioProbeWaitEvent.store(blockedEvent, std::memory_order_release);
            gAudioProbeWaitEntered.store(0, std::memory_order_release);
            gAudioProbeDoSubmit.store(0, std::memory_order_release);
            registerOne();
            uint64_t deadline = GetTickCount64() + 2000;
            while (!gAudioProbeWaitEntered.load(std::memory_order_acquire) && GetTickCount64() < deadline)
                Sleep(1);
            check(gAudioProbeWaitEntered.load(std::memory_order_acquire) == 1,
                  "Audio callback did not enter guest dispatcher wait");
            if (ownerShutdown) {
                auto teardown = std::async(std::launch::async, [mainMem] {
                    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    AudioRenderDriver::instance().shutdownForMemory(mainMem);
                    if (SUCCEEDED(hr)) CoUninitialize();
                    return hr;
                });
                check(SUCCEEDED(teardown.get()), "STA owner shutdown fixture failed to initialize COM");
                registered = false;
                check(!AudioRenderDriver::instance().counters().workerRunning &&
                      !AudioRenderDriver::instance().counters().deviceReady,
                      "STA owner shutdown left its callback/engine alive");
            } else {
                unregisterOne();
            }
            check(memory->read32(blockedEvent + 4) == 0,
                  "Audio teardown fabricated a guest event signal");
            gAudioProbeWaitEvent.store(0, std::memory_order_release);
            memory->release(blockedEvent);
        }
        {
            // S_FALSE must be balanced: after releasing our explicit MTA init and
            // the engine cookie, this same thread must be able to initialize STA.
            auto balancedCom = std::async(std::launch::async, [&] {
                HRESULT first = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(first)) return false;
                uint32_t status = AudioRenderDriver::instance().registerClient(PPC_CODE_BASE, 0xA5A50001u, outSlot);
                uint32_t stop = status ? status : AudioRenderDriver::instance().unregisterClient(memory->read32(outSlot));
                AudioRenderDriver::instance().shutdown();
                CoUninitialize();
                HRESULT sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(sta)) CoUninitialize();
                return status == 0 && stop == 0 && SUCCEEDED(sta);
            });
            check(balancedCom.get(), "Audio COM scopes or MTA cookie leaked across teardown");
        }
        {
            uint32_t callsAfter = gAudioProbeCalls.load(std::memory_order_acquire);
            Sleep(400);
            check(gAudioProbeCalls.load(std::memory_order_acquire) == callsAfter,
                  "Callback fired after unregister");
            ctx.r3.u64 = token;
            ctx.r4.u64 = planar;
            __imp__XAudioSubmitRenderDriverFrame(ctx, base);
            check(ctx.r3.u32 != 0, "Submit after unregister accepted");
            ctx.r3.u64 = token;
            __imp__XAudioUnregisterRenderDriverClient(ctx, base);
            check(ctx.r3.u32 != 0, "Double unregister accepted");
            ctx.r3.u64 = 0x12345678;
            __imp__XAudioUnregisterRenderDriverClient(ctx, base);
            check(ctx.r3.u32 != 0, "Foreign unregister accepted");
        }
        puts("Audio driver holds: callback, stream, completion, errors, tokens, teardown.");
    } catch (...) {
        gAudioProbeDoSubmit.store(0, std::memory_order_relaxed);
        gAudioProbeSelfUnreg.store(0, std::memory_order_relaxed);
        memory = mainMem;
        if (registered) {
            ctx.r3.u64 = token;
            __imp__XAudioUnregisterRenderDriverClient(ctx, base);
        }
        throw;
    }
    if (registered) {
        ctx.r3.u64 = token;
        __imp__XAudioUnregisterRenderDriverClient(ctx, base);
    }
    gAudioProbeDoSubmit.store(0, std::memory_order_relaxed);
    gAudioProbeSelfUnreg.store(0, std::memory_order_relaxed);
    gAudioProbePlanar.store(0, std::memory_order_relaxed);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
    memory->release(descriptor);
    memory->release(outSlot);
    memory->release(planar);
}
extern "C" PPC_FUNC(__imp__sub_828AFB00);
extern "C" PPC_FUNC(__imp__sub_828AE188);
extern "C" PPC_FUNC(__imp__sub_82793C48);
extern "C" PPC_FUNC(__imp__sub_8225AC90);
extern "C" PPC_FUNC(__imp__sub_828689A0);
extern "C" PPC_FUNC(__imp__sub_828685D0);
extern "C" PPC_FUNC(__imp__sub_82871600);
static void testOriginalPositionFetchOracle(PPCContext& ctx) {
    std::ifstream stream(memory->gameDirectory()/"System/Xenon/ProgramCache.xpc",std::ios::binary);
    const std::vector<uint8_t> cache{std::istreambuf_iterator<char>(stream),{}};
    auto big=[&](size_t p) {check(p+4<=cache.size(),"Original shader cache extent invalid");return uint32_t(cache[p])<<24|uint32_t(cache[p+1])<<16|uint32_t(cache[p+2])<<8|cache[p+3];};
    check(big(0x60)==442,"Unexpected original shader cache");
    size_t rec=0x64;for(unsigned i=0;i<13;++i)rec+=41+big(rec+37);
    check(rec+41+big(rec+37)<=cache.size(),"Oracle environment shader record missing");
    size_t blob=rec+41;
    uint32_t offset=big(blob+4);
    uint32_t size=big(blob+8);
    check(offset<0x1000 && size<0x1000 && offset+size==big(rec+37),"Oracle shader fixture too large");
    check(size>=120 && size%12==0,"Oracle shader window unexpected");
    const size_t count=size/12;
    struct Case { const char* name; uint32_t word; };
    const Case cases[]={{"19",0x002A2290},{"14",0x002A2190},{"15",0x002A2090},{"FLOAT3",0x002A23B9}};
    struct SlotWords { uint32_t w0{}; uint32_t w1{}; uint32_t w2{}; };
    std::vector<SlotWords> got[4]={std::vector<SlotWords>(count),std::vector<SlotWords>(count),
        std::vector<SlotWords>(count),std::vector<SlotWords>(count)};
    for(unsigned c=0;c<4;++c) {
        uint32_t storage=memory->allocate(0x8000);check(storage!=0,"Oracle fixture allocation failed");
        try {
            uint32_t shader=storage,code=storage+0x2000,declaration=storage+0x3000,elements=storage+0x4000;
            uint32_t formats=storage+0x5000,mask=storage+0x5100,strides=storage+0x5200;
            auto* base=memory->base();memset(base+storage,0,0x8000);
            memcpy(base+shader+872,cache.data()+blob,offset);
            memcpy(base+code,cache.data()+blob+offset,size);memcpy(base+formats,cache.data()+rec,17);
            ctx.r3.u64=shader;ctx.r4.u64=code;__imp__sub_828685D0(ctx,base);
            ctx.r3.u64=elements;ctx.r4.u64=formats;ctx.r5.u64=mask;__imp__sub_8225AC90(ctx,base);
            ctx.r3.u64=elements;ctx.r4.u64=declaration;__imp__sub_828689A0(ctx,base);
            check(base[formats+1]!=0,"Oracle fixture has no position input");
            check(memory->read32(declaration+24)>0 && memory->read32(declaration+24)<=16,"Oracle declaration count unexpected");
            const uint32_t declW0=memory->read32(declaration+52);
            const uint32_t declW1=memory->read32(declaration+52+4);
            const uint32_t declW2=memory->read32(declaration+52+8);
            check(declW0==0,"Oracle declaration entry0 is not offset0 POSITION");
            check((declW2&0xFFFFFF00)==0,"Oracle declaration entry0 is not usage0/index0 POSITION");
            check(memory->read32(elements+4)==declW1,"Oracle declaration copy mismatch at entry0");
            fprintf(stderr,"Oracle case %s decl0 {%08X %08X %08X} patched to %08X\n",
                cases[c].name,declW0,declW1,declW2,cases[c].word);
            memory->write32(declaration+52+4,cases[c].word);
            base[strides]=20;
            ctx.r3.u64=shader;ctx.r4.u64=code;ctx.r5.u64=declaration;ctx.r6.u64=strides;ctx.r7.u64=0;
            __imp__sub_82871600(ctx,base);
            for(size_t i=0;i<count;++i) {
                got[c][i].w0=memory->read32(uint32_t(code+i*12));
                got[c][i].w1=memory->read32(uint32_t(code+i*12+4));
                got[c][i].w2=memory->read32(uint32_t(code+i*12+8));
            }
            check(memory->release(storage),"Oracle fixture cleanup failed");
        } catch (...) { memory->release(storage); throw; }
    }
    auto decodeFields=[](uint32_t w0,uint32_t w1,unsigned& op,unsigned& dst,unsigned& fmt,
        unsigned& sign,unsigned& num,int& exp,unsigned& swiz) {
        op=w0&31;dst=(w0>>12)&63;fmt=(w1>>16)&63;sign=(w1>>12)&1;num=(w1>>13)&1;
        exp=int((w1>>24)&63);if(exp&32)exp-=64;swiz=w1&0xFFF;
    };
    unsigned op,dst,fmt,sign,num,swiz;int exp;
    for(size_t i=0;i<count;++i) {
        bool any=false;
        for(unsigned c=1;c<4 && !any;++c)
            any=got[0][i].w0!=got[c][i].w0||got[0][i].w1!=got[c][i].w1||got[0][i].w2!=got[c][i].w2;
        if(!any) continue;
        for(unsigned c=0;c<4;++c) {
            decodeFields(got[c][i].w0,got[c][i].w1,op,dst,fmt,sign,num,exp,swiz);
            fprintf(stderr,"Oracle varying slot %zu case %s: %08X %08X %08X op=%u dst=%u fmt=%u sign=%u num=%u exp=%d swiz=%03X\n",
                i,cases[c].name,got[c][i].w0,got[c][i].w1,got[c][i].w2,op,dst,fmt,sign,num,exp,swiz);
        }
    }
    unsigned w1varyingCount=0;
    for(size_t i=0;i<count;++i)
        if(got[0][i].w1!=got[1][i].w1||got[0][i].w1!=got[2][i].w1||got[0][i].w1!=got[3][i].w1)
            ++w1varyingCount;
    fprintf(stderr,"Oracle w1-varying slot count=%u\n",w1varyingCount);
    // POSITION is dst3 by shader semantics: rec013 FETCH R3 feeds the oPos
    // export chain and the originally linked baseline slot5 is dst3 fmt16 swizA88.
    // The linker reorders by declaration size (packed cases keep dst3 at slot
    // 5; 12-byte FLOAT3 moves it), so track destination, not slot number.
    size_t pos[4]={count,count,count,count};
    for(unsigned c=0;c<4;++c) {
        unsigned found=0;
        for(size_t i=0;i<count;++i)
            if(((got[c][i].w0>>12)&63)==3&&(got[c][i].w0&31)==0) { pos[c]=i;++found; }
        if(found!=1) {
            fprintf(stderr,"Oracle case %s dst3 position count=%u\n",cases[c].name,found);
            check(false,"Original POSITION destination linkage not unique");
        }
    }
    fprintf(stderr,"Oracle position slots 19/14/15/FLOAT3=%zu/%zu/%zu/%zu\n",pos[0],pos[1],pos[2],pos[3]);
    const unsigned dst0=3;
    for(unsigned c=0;c<4;++c) {
        decodeFields(got[c][pos[c]].w0,got[c][pos[c]].w1,op,dst,fmt,sign,num,exp,swiz);
        fprintf(stderr,"Oracle %s slot %zu: %08X %08X %08X dst=%u fmt=%u sign=%u num=%u exp=%d swiz=%03X\n",
            cases[c].name,pos[c],got[c][pos[c]].w0,got[c][pos[c]].w1,got[c][pos[c]].w2,dst0,fmt,sign,num,exp,swiz);
    }
    decodeFields(got[0][pos[0]].w0,got[0][pos[0]].w1,op,dst,fmt,sign,num,exp,swiz);
    if(fmt!=16||sign!=0||num!=1||exp!=0||swiz!=0xA88) {
        fprintf(stderr,"Oracle 19 actual fmt=%u sign=%u num=%u exp=%d swiz=%03X dst=%u\n",fmt,sign,num,exp,swiz,dst0);
        check(false,"Original 19 position fetch contradicts format16/unsigned/raw/exp0/xyz1");
    }
    decodeFields(got[1][pos[1]].w0,got[1][pos[1]].w1,op,dst,fmt,sign,num,exp,swiz);
    if(fmt!=16||sign!=1||num!=0||exp!=0||swiz!=0xA88) {
        fprintf(stderr,"Oracle 14 actual fmt=%u sign=%u num=%u exp=%d swiz=%03X\n",fmt,sign,num,exp,swiz);
        check(false,"Original 14 control contradicts format16/signed/normalized/exp0/xyz1");
    }
    decodeFields(got[2][pos[2]].w0,got[2][pos[2]].w1,op,dst,fmt,sign,num,exp,swiz);
    if(fmt!=16||sign!=0||num!=0||exp!=0||swiz!=0xA88) {
        fprintf(stderr,"Oracle 15 actual fmt=%u sign=%u num=%u exp=%d swiz=%03X\n",fmt,sign,num,exp,swiz);
        check(false,"Original 15 control contradicts format16/unsigned/normalized/exp0/xyz1");
    }
    decodeFields(got[3][pos[3]].w0,got[3][pos[3]].w1,op,dst,fmt,sign,num,exp,swiz);
    if(fmt!=57||exp!=0||swiz!=0xA88) {
        fprintf(stderr,"Oracle FLOAT3 actual fmt=%u sign=%u num=%u exp=%d swiz=%03X dst=%u\n",fmt,sign,num,exp,swiz,dst0);
        check(false,"Original FLOAT3 control contradicts fmt57/exp0/xyz1");
    }
    puts("Original 19 position fetch is format16 unsigned raw exp0 xyz1 with shared destination linkage.");
}
static void testOriginalMissingVertexInput(PPCContext& ctx) {
    std::ifstream stream(memory->gameDirectory()/"System/Xenon/ProgramCache.xpc",std::ios::binary);
    const std::vector<uint8_t> cache{std::istreambuf_iterator<char>(stream),{}};
    auto big=[&](size_t p) {check(p+4<=cache.size(),"Original shader cache extent invalid");return uint32_t(cache[p])<<24|uint32_t(cache[p+1])<<16|uint32_t(cache[p+2])<<8|cache[p+3];};
    check(big(0x60)==442,"Unexpected original shader cache");
    size_t p=0x64;for(unsigned i=0;i<13;++i)p+=41+big(p+37);
    check(p+41+big(p+37)<=cache.size(),"Missing original environment shader record");
    const auto blob=p+41;const auto offset=big(blob+4),size=big(blob+8);
    check(offset<0x1000 && size<0x1000 && offset+size==big(p+37),"Original shader fixture too large");
    const auto storage=memory->allocate(0x8000);check(storage!=0,"Vertex input fixture allocation failed");
    const auto shader=storage,code=storage+0x2000,declaration=storage+0x3000,elements=storage+0x4000;
    const auto formats=storage+0x5000,mask=storage+0x5100,strides=storage+0x5200;
    auto* base=memory->base();memcpy(base+shader+872,cache.data()+blob,offset);
    memcpy(base+code,cache.data()+blob+offset,size);memcpy(base+formats,cache.data()+p,17);
    ctx.r3.u64=shader;ctx.r4.u64=code;__imp__sub_828685D0(ctx,base);
    ctx.r3.u64=elements;ctx.r4.u64=formats;ctx.r5.u64=mask;__imp__sub_8225AC90(ctx,base);
    ctx.r3.u64=elements;ctx.r4.u64=declaration;__imp__sub_828689A0(ctx,base);
    base[strides]=20;
    ctx.r3.u64=shader;ctx.r4.u64=code;ctx.r5.u64=declaration;ctx.r6.u64=strides;ctx.r7.u64=0;
    __imp__sub_82871600(ctx,base);
    for(unsigned i=5;i<10;++i)fprintf(stderr,"Original patched vertex fetch %u: %08X %08X %08X\n",i,
        memory->read32(code+i*12),memory->read32(code+i*12+4),memory->read32(code+i*12+8));
    unsigned missingCount=0;
    // Original82871600 may reorder fetches. Track destination r1, the
    // original TEXCOORD3 input, rather than assuming its instruction index.
    for(unsigned i=5;i<10;++i)if(((memory->read32(code+i*12)>>12)&63)==1) {
        const auto missing=memory->read32(code+i*12+4);
        check((missing&0xfff)==0xf24,"Original missing tangent does not supply zero XYZ with unused W");
        ++missingCount;
    }
    check(missingCount==1,"Original missing tangent fetch was lost or duplicated");
    check(memory->release(storage),"Vertex input fixture cleanup failed");
}
static void testOriginalStickConversion(PPCContext& ctx) {
    const auto output=memory->allocate(4096);check(output!=0,"Analog fixture allocation failed");
    for(const auto& input:std::initializer_list<std::array<uint32_t,2>>{
            {32768,32768},{0,32768},{65535,32768},{32768,0},{32768,65535}}) {
        ctx.r3.u64=output;ctx.r4.u64=0;ctx.r5.u64=input[0];ctx.r6.u64=input[1];
        __imp__sub_82793C48(ctx,memory->base());
        const int32_t x=int32_t(memory->read32(output)),y=int32_t(memory->read32(output+4));
        fprintf(stderr,"Original analog input=%u,%u output=%d,%d\n",input[0],input[1],x,y);
        if(input[0]==32768 && input[1]==32768)check(x==0 && y==0,"Original neutral stick did not remain neutral");
        else {
            if(input[0]!=32768)check(input[0]?x>128:x< -128,"Original full horizontal stick did not generate an axis event");
            if(input[1]!=32768)check(input[1]?y>128:y< -128,"Original full vertical stick did not generate an axis event");
        }
    }
    check(memory->release(output),"Analog fixture cleanup failed");
}
static void testTextureTileLayout(PPCContext& ctx) {
    testOriginalStickConversion(ctx);
    testOriginalMissingVertexInput(ctx);
    testOriginalPositionFetchOracle(ctx);
    auto* base=memory->base();const auto source=memory->allocate(0x100000),target=memory->allocate(0x200000);
    check(source && target,"Tile oracle fixture allocation failed");
    for(unsigned bytes:{1,2,4,8,16})for(unsigned width:{32,64,128})for(unsigned height:{1,8,32,64}) {
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned lane=0;lane<bytes;++lane)
            base[source+(y*width+x)*bytes+lane]=uint8_t(x*47+y*29+lane*13);
        memset(base+target,0xcd,0x200000);
        ctx.r3.u64=target;ctx.r4.u64=width;ctx.r5.u64=height;ctx.r6.u64=0;
        ctx.r7.u64=source;ctx.r8.u64=width*bytes;ctx.r9.u64=0;ctx.r10.u64=bytes;
        __imp__sub_828AFB00(ctx,base);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto offset=worldTextureTiledOffset(x,y,width,bytes);
            check(offset+bytes<=0x100000,"Native tiled offset escaped fixture");
            if(memcmp(base+source+(y*width+x)*bytes,base+target+offset,bytes)) {
                fprintf(stderr,"Tile mismatch x=%u y=%u width=%u height=%u bpp=%u offset=%u\n",x,y,width,height,bytes,offset);
                check(false,"Native texture addressing disagrees with original CPU tile routine");
            }
        }
    }
    const auto header=source+0x80000;
    for(const auto size:std::initializer_list<std::array<unsigned,2>>{{4,4},{8,16},{16,8},{16,16},{64,32},{324,18},{18,324}})
    for(unsigned format:{2,6,18,20,49}) {
        const unsigned width=size[0],height=size[1],block=format<=6?1:4,bytes=format==2?1:format==6?4:format==18?8:16;
        if(block==4 && ((width|height)&3))continue;
        const unsigned bw=width/block,bh=height/block,endian=format==2?0:format==6?2:1;
        const unsigned pitch=(std::max)(block==4?128u:32u,(width+31)&~31u);
        memset(base+source,0,0x40000);memset(base+target,0,0x100000);
        for(unsigned y=0;y<bh;++y)for(unsigned x=0;x<bw;++x) {
            uint8_t value[16]{};
            if(format==2)value[0]=uint8_t(x*3+y);
            else if(format==6){value[0]=uint8_t(x);value[1]=uint8_t(y);value[2]=uint8_t(255-x);value[3]=127;}
            else if(format==18 || format==20) {
                auto* color=value+(format==20?8:0);
                const auto rgb=uint16_t((x<<11)|(y<<5)|x);color[0]=color[2]=uint8_t(rgb);color[1]=color[3]=uint8_t(rgb>>8);
                if(format==20)value[0]=value[1]=255;
            }
            else {value[0]=value[1]=uint8_t(y*20);value[8]=value[9]=128;}
            for(unsigned lane=0;lane<bytes;++lane)base[source+(y*bw+x)*bytes+(lane^(endian==2?3:endian==1?1:0))]=value[lane];
        }
        const auto rectangle=header+128;
        memory->write32(rectangle,0);memory->write32(rectangle+4,0);
        memory->write32(rectangle+8,bw);memory->write32(rectangle+12,bh);
        ctx.r3.u64=target;ctx.r4.u64=pitch/block;ctx.r5.u64=bh;ctx.r6.u64=0;
        ctx.r7.u64=source;ctx.r8.u64=bw*bytes;ctx.r9.u64=rectangle;ctx.r10.u64=bytes;
        __imp__sub_828AFB00(ctx,base);
        memset(base+header,0,64);
        memory->write32(header+28,0x80000002u|((pitch/32)<<22));
        memory->write32(header+32,target|format|(endian<<6));memory->write32(header+36,(width-1)|((height-1)<<13));
        memory->write32(header+40,format==2?0x1400:format==6?0x1414:format==49?0x400:0xd10);memory->write32(header+48,0x200);
        ctx.r3.u64=header;ctx.r4.u64=0;ctx.r5.u64=0;__imp__sub_828AE188(ctx,base);
        check(ctx.r3.u32==0,"Original non-packed small base level has a nonzero offset");
        DarkRecomp::ColorImage decoded;
        check(!decodeWorldTextureImage(base,header,decoded),"Original-tiled texture failed native decoding");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto* pixel=decoded.pixels.data()+(y*width+x)*4;
            if(format==2)check(pixel[0]==uint8_t(x*3+y) && pixel[1]==pixel[0] && pixel[2]==pixel[0] && pixel[3]==255,"L8 resource component selectors differ");
            else if(format==6)check(pixel[0]==uint8_t(255-x) && pixel[1]==uint8_t(y) && pixel[2]==uint8_t(x) && pixel[3]==255,"RGBA texture byte-order/swizzle differs");
            else if(format==18 || format==20) {
                const auto red=((x/4)<<3)|((x/4)>>2),green=((y/4)<<2)|((y/4)>>4);
                check(pixel[0]==red && pixel[1]==green && pixel[2]==red && pixel[3]==255,"BC1 texture block/channel order differs");
            } else check(pixel[0]==(y/4)*20 && pixel[1]==pixel[0] && pixel[2]==pixel[0] && pixel[3]==128,"DXN resource L,L,L,A selectors differ");
        }
    }
    // Actual 64x64 cube descriptor from the first level. The original layout
    // query validates face offsets independently of native resource decoding.
    const uint32_t cube[]{0x00200003,1,0,0,0,0xffff0000,0xffff0000,0x80800002,0xc1af4086,0x1407e03f,0x1414,0x180,0xc1b0ce00};
    for(unsigned i=0;i<std::size(cube);++i)memory->write32(header+i*4,cube[i]);
    // This fixture initializes only the base faces, not the captured mip allocation.
    memory->write32(header+44,0);
    for(unsigned face=0;face<6;++face) {
        for(unsigned i=0;i<64*64;++i) {
            base[source+i*4]=255;base[source+i*4+1]=uint8_t(face*30+5);
            base[source+i*4+2]=17;base[source+i*4+3]=34;
        }
        ctx.r3.u64=target+face*0x4000;ctx.r4.u64=64;ctx.r5.u64=64;ctx.r6.u64=0;
        ctx.r7.u64=source;ctx.r8.u64=256;ctx.r9.u64=0;ctx.r10.u64=4;__imp__sub_828AFB00(ctx,base);
    }
    memory->write32(header+32,target|0x86);
    DarkRecomp::ColorImage cubeImage;
    check(!decodeWorldTextureImage(base,header,cubeImage) && cubeImage.faces==6 && cubeImage.pixels.size()==6*64*64*4,
          "Original-tiled cube failed native decoding");
    for(unsigned face=0;face<6;++face)for(unsigned i=0;i<64*64;++i) {
        const auto* pixel=cubeImage.pixels.data()+(face*64*64+i)*4;
        check(pixel[0]==face*30+5 && pixel[1]==17 && pixel[2]==34 && pixel[3]==255,"Cube decoding lost/reordered a face");
    }
    memory->write32(header+32,cube[8]);
    for(unsigned face=0;face<6;++face) {
        ctx.r3.u64=header;ctx.r4.u64=face;ctx.r5.u64=0;__imp__sub_828AE188(ctx,base);
        check(ctx.r3.u32==face*0x4000,"Cube face stride disagrees with original resource layout");
    }
    for(unsigned dimension:{4,8,16,32,64,128,256})for(unsigned format:{6,18,20,49}) {
        const unsigned pitch=format==6?(std::max)(32u,dimension):(std::max)(128u,dimension);
        memory->write32(header+48,cube[12]&~0x800u);
        memory->write32(header+28,0x80000002u|((pitch/32)<<22));
        memory->write32(header+32,0xc1af4000|format|(format==6?0x80:0x40));
        memory->write32(header+36,0x14000000|(dimension-1)|((dimension-1)<<13));
        const unsigned blocks=format==6?dimension:dimension/4,bytes=format==6?4:format==18?8:16;
        unsigned extent=0;
        for(unsigned y=0;y<blocks;++y)for(unsigned x=0;x<blocks;++x)
            extent=(std::max)(extent,worldTextureTiledOffset(x,y,format==6?pitch:pitch/4,bytes)+bytes);
        const unsigned pitchBlocks=format==6?pitch:pitch/4;
        const unsigned stride=(pitchBlocks*((blocks+31)&~31u)*bytes+4095)&~4095u;
        check(extent<=stride,"Cube tiled addresses escape padded face allocation");
        memset(base+target,0xcd,0x100000);
        for(unsigned face=0;face<6;++face) {
            ctx.r3.u64=header;ctx.r4.u64=face;ctx.r5.u64=0;__imp__sub_828AE188(ctx,base);
            const auto originalOffset=ctx.r3.u32;
            check(originalOffset==face*stride,"Native cube stride disagrees with original layout query");
            check(uint64_t(originalOffset)+stride<=0x200000,"Cube oracle escaped fixture allocation");
            for(unsigned y=0;y<blocks;++y)for(unsigned x=0;x<blocks;++x) {
                uint8_t value[16]{};
                if(format==6){value[0]=34;value[1]=17;value[2]=uint8_t(face*30+5);value[3]=255;}
                else if(format==49){value[0]=value[1]=uint8_t(face*30+5);value[8]=value[9]=uint8_t(x+y);}
                else {
                    auto* color=value+(format==20?8:0);
                    const auto rgb=uint16_t(((face*5)<<11)|((y&63)<<5)|(x&31));
                    color[0]=color[2]=uint8_t(rgb);color[1]=color[3]=uint8_t(rgb>>8);
                    if(format==20)value[0]=value[1]=255;
                }
                for(unsigned lane=0;lane<bytes;++lane)
                    base[source+(y*blocks+x)*bytes+(lane^(format==6?3:1))]=value[lane];
            }
            ctx.r3.u64=target+originalOffset;ctx.r4.u64=pitchBlocks;ctx.r5.u64=blocks;ctx.r6.u64=0;
            ctx.r7.u64=source;ctx.r8.u64=blocks*bytes;ctx.r9.u64=0;ctx.r10.u64=bytes;
            // A source rectangle keeps the padded destination pitch from
            // making the original tiler read beyond the logical source row.
            const auto rectangle=header+128;
            memory->write32(rectangle,0);memory->write32(rectangle+4,0);
            memory->write32(rectangle+8,blocks);memory->write32(rectangle+12,blocks);
            ctx.r9.u64=rectangle;__imp__sub_828AFB00(ctx,base);
        }
        memory->write32(header+32,target|format|(format==6?0x80:0x40));
        memory->write32(header+40,format==6?0x1414:format==49?0x400:0xd10);
        DarkRecomp::ColorImage decoded;
        check(!decodeWorldTextureImage(base,header,decoded) && decoded.faces==6 && decoded.pixels.size()==size_t(dimension)*dimension*24,
              "Original tiled compressed/RGBA cube failed decoding");
        for(unsigned face=0;face<6;++face)for(unsigned y=0;y<dimension;++y)for(unsigned x=0;x<dimension;++x) {
            const auto* pixel=decoded.pixels.data()+((size_t(face)*dimension+y)*dimension+x)*4;
            if(format==6)check(pixel[0]==face*30+5 && pixel[1]==17 && pixel[2]==34 && pixel[3]==255,"RGBA cube face pixel differs");
            else if(format==49)check(pixel[0]==face*30+5 && pixel[1]==pixel[0] && pixel[2]==pixel[0] && pixel[3]==x/4+y/4,"DXN cube face pixel differs");
            else {
                const unsigned r=face*5,g=(y/4)&63,b=(x/4)&31;
                check(pixel[0]==((r<<3)|(r>>2)) && pixel[1]==((g<<2)|(g>>4)) && pixel[2]==((b<<3)|(b>>2)) && pixel[3]==255,
                      "BC1/BC3 cube face/block pixel differs");
            }
        }
    }
    check(memory->release(source) && memory->release(target),"Tile oracle fixture cleanup failed");
    puts("Native tiled addressing agrees with original828AFB00 for 60 dimension/format combinations.");
}
static void testPhysicalAliases(PPCContext& ctx) {
    auto* base=memory->base();
    uint32_t a=memory->allocate(0x10000,0x10000,0xa0000000,0xc0000000);
    check(a && (a&0xffff)==0,"64 KiB physical allocation failed");
    uint32_t offset=Memory::physicalAddress(a),c=0xc0000000+offset;
    memory->write32(a+0x1234,0x91827364);
    check(memory->read32(c+0x1234)==0x91827364,"A/C physical aliases do not share writes");
    const uint32_t e=0xe0000000+offset+0x234;
    check(memory->read32(e)==0x91827364,"E alias did not apply the original 4 KiB bias");
    memory->write32(e,0xabcdef01);
    check(memory->read32(a+0x1234)==0xabcdef01,"E writes were copied instead of shared");
    ctx.r3.u64=e;__imp__MmGetPhysicalAddress(ctx,base);
    check(ctx.r3.u32==offset+0x1234,"Physical-address import disagrees with original alias arithmetic");
    uint32_t second=memory->allocate(4096,4096,0xe0000000,0xffd00000);
    check(second && Memory::physicalAddress(second)>=offset+0x10000,"Different physical aliases overlap allocations");
    memory->write32(second,0x44556677);
    check(memory->release(a),"Physical allocation release failed");
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(base+c+0x1234,&info,sizeof(info));
    check((info.Protect&0xff)==PAGE_NOACCESS,"Released C alias remains accessible");
    VirtualQuery(base+e,&info,sizeof(info));
    check((info.Protect&0xff)==PAGE_NOACCESS,"Released E alias remains accessible");
    check(memory->read32(second)==0x44556677,"Releasing one allocation damaged another alias allocation");
    uint32_t reused=memory->allocate(0x10000,0x10000,0xa0000000,0xc0000000);
    check(reused==a && memory->read32(reused+0x1234)==0,"Physical reuse leaked stale backing bytes");
    check(memory->release(reused) && memory->release(second),"Physical fixture cleanup failed");
    for(const auto& [protection,region]:std::initializer_list<std::pair<uint32_t,uint32_t>>{
            {0x80000004,0xc0000000},{0x20000004,0xa0000000},{4,0xe0000000}}) {
        ctx.r3.u64=0;ctx.r4.u64=4096;ctx.r5.u64=protection;ctx.r6.u64=0;ctx.r7.u64=0xffffffff;ctx.r8.u64=0;
        __imp__MmAllocatePhysicalMemoryEx(ctx,base);
        const auto address=ctx.r3.u32;
        check(address && (address&0xe0000000)==region,"Physical page size selected the wrong CPU alias");
        ctx.r4.u64=address;__imp__MmFreePhysicalMemory(ctx,base);
    }
    puts("Native shared physical aliases, allocation isolation, protection and reuse passed.");
}
static void testHighRamMmio() {
    auto* base = memory->base();
    uint32_t page = memory->allocate(4096, 4096, 0xF037C000, 0xF0380000);
    check(page == 0xF037C000, "High-RAM fixture did not land on the observed 0xF037Cxxx page");
    uint32_t obj = page + 0x870;
    check(obj == 0xF037C870, "High-RAM object offset mismatch");
    uint32_t vtable = page + 0xA00;
    check(!PPCIsDynamicMmio(obj), "High-RAM object misclassified as MMIO");
    check(!PPCIsDynamicMmio(vtable + 4), "High-RAM vtable slot misclassified as MMIO");
    PPCStoreU32(base, obj, 0x82095798);
    check(*reinterpret_cast<volatile uint32_t*>(base + obj) == _byteswap_ulong(0x82095798), "High-RAM u32 store did not reach host bytes");
    check(PPCLoadU32(base, obj) == 0x82095798, "High-RAM u32 load did not round-trip");
    *reinterpret_cast<volatile uint32_t*>(base + obj + 4) = _byteswap_ulong(0x20008000);
    check(PPCLoadU32(base, obj + 4) == 0x20008000, "High-RAM u32 raw-seeded load failed");
    check(PPC_LOAD_U32(obj + 4) == 0x20008000, "High-RAM u32 macro load failed");
    PPC_STORE_U8(obj + 8, 0xAB);
    check(*(volatile uint8_t*)(base + obj + 8) == 0xAB, "High-RAM u8 store did not reach host bytes");
    check(PPCLoadU8(base, obj + 8) == 0xAB, "High-RAM u8 load did not round-trip");
    check(PPC_LOAD_U8(obj + 8) == 0xAB, "High-RAM u8 macro load failed");
    *(volatile uint8_t*)(base + obj + 9) = 0x7E;
    check(PPCLoadU8(base, obj + 9) == 0x7E, "High-RAM u8 raw-seeded load failed");
    PPC_STORE_U16(obj + 12, 0xC0DE);
    check(*(volatile uint16_t*)(base + obj + 12) == _byteswap_ushort(0xC0DE), "High-RAM u16 store did not reach host bytes");
    check(PPCLoadU16(base, obj + 12) == 0xC0DE, "High-RAM u16 load did not round-trip");
    check(PPC_LOAD_U16(obj + 12) == 0xC0DE, "High-RAM u16 macro load failed");
    *(volatile uint16_t*)(base + obj + 14) = _byteswap_ushort(0x1234);
    check(PPCLoadU16(base, obj + 14) == 0x1234, "High-RAM u16 raw-seeded load failed");
    PPCStoreU64(base, obj + 16, 0x1122334455667788ull);
    check(*reinterpret_cast<volatile uint64_t*>(base + obj + 16) == _byteswap_uint64(0x1122334455667788ull), "High-RAM u64 store did not reach host bytes");
    check(PPCLoadU64(base, obj + 16) == 0x1122334455667788ull, "High-RAM u64 load did not round-trip");
    check(PPC_LOAD_U64(obj + 16) == 0x1122334455667788ull, "High-RAM u64 macro load failed");
    *reinterpret_cast<volatile uint64_t*>(base + obj + 24) = _byteswap_uint64(0xA5A55A5A0F0FF00Full);
    check(PPCLoadU64(base, obj + 24) == 0xA5A55A5A0F0FF00Full, "High-RAM u64 raw-seeded load failed");
    PPCStoreU32(base, obj, vtable);
    PPCStoreU32(base, vtable + 4, 0x82215978);
    check(*reinterpret_cast<volatile uint32_t*>(base + obj) == _byteswap_ulong(vtable), "High-RAM vtable pointer did not reach host bytes");
    check(*reinterpret_cast<volatile uint32_t*>(base + vtable + 4) == _byteswap_ulong(0x82215978), "High-RAM vtable slot did not reach host bytes");
    check(PPCLoadU32(base, obj) == vtable, "High-RAM vtable pointer load failed");
    check(PPCLoadU32(base, PPCLoadU32(base, obj) + 4) == 0x82215978, "High-RAM vtable indirect slot chain failed");
    check(PPCIsDynamicMmio(0x7FC01000), "Small MMIO window 7FC lost");
    check(PPCIsDynamicMmio(0x7FE01000), "Small MMIO window 7FE lost");
    check(PPCLoadU32(base, 0x7FC01000) == 0, "Small MMIO window 7FC load changed");
    check(PPCLoadU32(base, 0x7FE01000) == 0, "Small MMIO window 7FE load changed");
    PPCStoreU32(base, 0x7FC01000, 0xDEADBEEF);
    PPCStoreU64(base, 0x7FE01000, 0xDEADBEEFCAFEBABEull);
    check(PPCLoadU32(base, 0x7FC01000) == 0, "Small MMIO window 7FC store not discarded");
    check(PPCLoadU64(base, 0x7FE01000) == 0, "Small MMIO window 7FE store not discarded");
    check(memory->release(page), "High-RAM fixture release failed");
}
static void testFiles(PPCContext& ctx, uint32_t scratch) {
    auto* base = memory->base();
    memset(base + scratch, 0, 4096);
    auto open = [&](const char* name) {
        size_t length = strlen(name);
        memcpy(base + scratch + 256, name, length + 1);
        memory->write32(scratch, 0xfffffffd);
        memory->write32(scratch + 4, scratch + 16);
        memory->write32(scratch + 8, 0x40);
        memory->write32(scratch + 16, uint32_t(length << 16) | uint32_t(length + 1));
        memory->write32(scratch + 20, scratch + 256);
        ctx.r3.u64 = scratch + 80; ctx.r4.u64 = GENERIC_READ | SYNCHRONIZE;
        ctx.r5.u64 = scratch; ctx.r6.u64 = scratch + 64; ctx.r7.u64 = 0;
        ctx.r8.u64 = 0; ctx.r9.u64 = FILE_SHARE_READ; ctx.r10.u64 = 1;
        memory->write32(ctx.r1.u32 + 84, 0x60);
        __imp__NtCreateFile(ctx, base);
        return ctx.r3.u32;
    };
    check(open("game:\\..\\CMakeLists.txt") == 0xc0000033, "File path escaped its guest mount");
    check(open("game:\\__native_test_missing_file__") == 0xc0000034, "Missing file did not return its native status");
    check(open("game:\\basefile.exe") == 0, "Original image could not be opened through the guest file ABI");
    uint32_t file = memory->read32(scratch + 80);
    // This title's SDK passes ShareAccess in r7 and OpenOptions in r8.
    // Original callers at 0x828A8B88 and 0x828A9C0C set both registers.
    auto openExisting = [&](uint32_t sharing, uint32_t options) {
        ctx.r3.u64 = scratch + 84; ctx.r4.u64 = GENERIC_READ | SYNCHRONIZE;
        ctx.r5.u64 = scratch; ctx.r6.u64 = scratch + 64;
        ctx.r7.u64 = sharing; ctx.r8.u64 = options;
        __imp__NtOpenFile(ctx, base);
        return ctx.r3.u32;
    };
    check(openExisting(FILE_SHARE_READ, 0x60) == 0,
          "NtOpenFile confused sharing flags with non-directory open options");
    uint32_t reopened = memory->read32(scratch + 84);
    check(memory->read32(scratch + 64) == 0 && memory->read32(scratch + 68) == 1,
          "NtOpenFile did not report FILE_OPENED in the guest I/O status");
    ctx.r3.u64 = reopened; ctx.r4.u64 = scratch + 64; ctx.r5.u64 = scratch + 128;
    ctx.r6.u64 = 4; ctx.r7.u64 = 16;
    __imp__NtQueryInformationFile(ctx, base);
    check(ctx.r3.u32 == 0 && (memory->read32(scratch + 128) & 0x30) == 0x20,
          "NtOpenFile discarded the requested synchronous mode");
    ctx.r3.u64 = reopened; __imp__NtClose(ctx, base);
    check(openExisting(0, 0x60) == 0xc0000043,
          "NtOpenFile ignored an incompatible exclusive sharing request");
    check(openExisting(FILE_SHARE_READ, 0x21) == 0xc0000103,
          "NtOpenFile ignored the directory-only option for a regular file");
    ctx.r3.u64 = file; ctx.r4.u64 = scratch + 64; ctx.r5.u64 = scratch + 128;
    ctx.r6.u64 = 24; ctx.r7.u64 = 5;
    __imp__NtQueryInformationFile(ctx, base);
    check(ctx.r3.u32 == 0, "File size query failed");
    uint64_t size = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + scratch + 136));
    check(size == std::filesystem::file_size(memory->gameDirectory() / "basefile.exe"), "File size endian translation failed");
    auto read = [&](uint64_t offset) {
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(offset);
        ctx.r3.u64 = file; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        ctx.r7.u64 = scratch + 64; ctx.r8.u64 = scratch + 512; ctx.r9.u64 = 32; ctx.r10.u64 = scratch + 48;
        __imp__NtReadFile(ctx, base);
        return ctx.r3.u32;
    };
    check(read(0) == 0 && memory->read32(scratch + 68) == 32, "Native file read failed");
    char expected[32];
    std::ifstream actual(memory->gameDirectory() / "basefile.exe", std::ios::binary);
    actual.read(expected, sizeof(expected));
    check(memcmp(expected, base + scratch + 512, sizeof(expected)) == 0, "File read changed the original bytes");
    auto original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = fileApcProbe;
    *reinterpret_cast<uint64_t*>(base + scratch + 48) = 0;
    ctx.r3.u64 = file; ctx.r4.u64 = 0; ctx.r5.u64 = PPC_CODE_BASE | 1; ctx.r6.u64 = scratch + 768;
    ctx.r7.u64 = scratch + 64; ctx.r8.u64 = scratch + 512; ctx.r9.u64 = 32; ctx.r10.u64 = scratch + 48;
    __imp__NtReadFile(ctx, base);
    check(ctx.r3.u32 == 0 && memory->read32(scratch + 768) == 0, "File APC executed before an alertable wait");
    ctx.r31.u64 = 0x12344321;
    check(SleepEx(1000, TRUE) == WAIT_IO_COMPLETION, "Windows did not deliver the file APC");
    check(memory->read32(scratch + 768) == GetCurrentThreadId(), "File APC ran on the wrong thread");
    check(memory->read32(scratch + 772) == 0 && memory->read32(scratch + 776) == 32, "File APC received incorrect completion data");
    check(ctx.r31.u64 == 0x12344321 && currentContext == &ctx, "File APC corrupted the interrupted context");
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
    ctx.r3.u64 = file; ctx.r4.u64 = scratch + 64; ctx.r5.u64 = scratch + 1024;
    ctx.r6.u64 = 56; ctx.r7.u64 = 34;
    __imp__NtQueryInformationFile(ctx, base);
    check(ctx.r3.u32 == 0, "File class-34 size query failed");
    uint64_t allSize = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + scratch + 1064));
    check(allSize == size, "File class-34 EndOfFile mismatch");
    uint32_t wrapper = memory->allocate(8);
    check(wrapper != 0, "Length wrapper allocation failed");
    memory->write32(wrapper, file);
    memory->write32(wrapper + 4, 0);
    ctx.r3.u64 = wrapper;
    sub_82107040(ctx, base);
    check(ctx.r3.u64 == size, "Original file-length helper did not return the file size");
    check(memory->release(wrapper), "Length wrapper release failed");
    const char* archiveName = "game:\\Content\\Xdf\\GameContext_Create.XDF";
    size_t archiveLength = strlen(archiveName);
    memcpy(base + scratch + 256, archiveName, archiveLength + 1);
    memory->write32(scratch, 0xfffffffd);
    memory->write32(scratch + 4, scratch + 16);
    memory->write32(scratch + 8, 0x40);
    memory->write32(scratch + 16, uint32_t(archiveLength << 16) | uint32_t(archiveLength + 1));
    memory->write32(scratch + 20, scratch + 256);
    ctx.r3.u64 = scratch + 80; ctx.r4.u64 = 0x80100080;
    ctx.r5.u64 = scratch; ctx.r6.u64 = scratch + 64; ctx.r7.u64 = 0;
    ctx.r8.u64 = 0; ctx.r9.u64 = FILE_SHARE_READ; ctx.r10.u64 = 1;
    memory->write32(ctx.r1.u32 + 84, 0x48);
    __imp__NtCreateFile(ctx, base);
    check(ctx.r3.u32 == 0, "Archive could not be opened with game access/options");
    uint32_t archive = memory->read32(scratch + 80);
    // The native menu replaces this archive beside the executable. Compare
    // against that host fixture, not the unmodified console asset's size.
    wchar_t testExecutable[32768]{};
    const DWORD testExecutableLength = GetModuleFileNameW(nullptr, testExecutable, DWORD(std::size(testExecutable)));
    check(testExecutableLength && testExecutableLength < std::size(testExecutable),
          "Could not locate the native archive fixture");
    uint64_t archiveExpected = uint64_t(std::filesystem::file_size(
        std::filesystem::path(testExecutable).parent_path() / "GameContext_Create.pc.xdf"));
    check(archiveExpected != 0, "Archive fixture is empty");
    uint32_t archiveWrapper = memory->allocate(8);
    check(archiveWrapper != 0, "Archive wrapper allocation failed");
    memory->write32(archiveWrapper, archive);
    memory->write32(archiveWrapper + 4, 0);
    ctx.r3.u64 = archiveWrapper;
    sub_82107040(ctx, base);
    check(ctx.r3.u64 == archiveExpected, "Original file-length helper did not return the archive size");
    check(memory->release(archiveWrapper), "Archive wrapper release failed");
    ctx.r3.u64 = archive; __imp__NtClose(ctx, base);
    check(read(size) == 0xc0000011 && memory->read32(scratch + 68) == 0, "End-of-file read was reported as successful data");
    ctx.r3.u64 = file; __imp__NtClose(ctx, base);
    check(read(0) == 0xc0000008, "Closed file handle remained usable");
}
int main(int argc, char** argv) {
    try {
        check(argc >= 2, "Game directory required");
        Memory addressSpace;
        memory = &addressSpace;
        addressSpace.load(argv[1]);
        PPCContext ctx{};
        addressSpace.initThread(ctx);
        if (argc == 3 && strcmp(argv[2], "--video-settings") == 0) {
            testVideoSettings(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--texture-upload") == 0) {
            testTextureUpload();
            puts("Texture upload transactions: initial/streamed mips, cubes, immutable publication, skipped metadata, copy witnesses and rejected incomplete updates passed.");
            testTextureResidency(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--texture-mip-layout") == 0) {
            testTextureMipLayout(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--critical-sections") == 0) {
            testCriticalSections(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--object-outputs") == 0) {
            testObjectOutputs(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--format-abi") == 0) {
            testNativeFormatAbi(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--format-width") == 0) {
            testNativeFormatWidth(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--file-buffers") == 0) {
            testFileBuffers(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--save-storage") == 0) {
            testNativeStorage(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--native-delay") == 0) {
            testNativeDelay(ctx);
            testNativeTimedWait(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--input") == 0) {
            testInputContract(ctx);
            testMouseLookContract(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--dispatcher") == 0) {
            testDispatcher();
            return 0;
        }
        if (argc == 4 && strcmp(argv[2], "--inflate") == 0) {
            testInflate(ctx, argv[3]);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--launcher-status") == 0) {
            currentContext = &ctx;
            check(runGuestWithEntry(ctx, memory->base(), launcherFaultEntry) == 3,
                  "Caught guest fault must report status 3");
            check(runGuestWithEntry(ctx, memory->base(), launcherReturnEntry) == 4,
                  "Premature guest return must report status 4");
            puts("Launcher propagates guest fault (3) and premature return (4).");
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--null-record") == 0) {
            uint8_t* nullBase = memory->base();
            uint32_t obj = memory->allocate(64);
            uint32_t buckets = memory->allocate(512);
            check(obj && buckets, "Null-record fixture allocation failed");
            memset(nullBase + obj, 0, 64);
            memset(nullBase + buckets, 0xFF, 512);
            memory->write32(obj + 36, buckets);
            memory->write32(obj + 12, 0);
            ctx.r3.u64 = obj;
            ctx.r4.u64 = 7;
            sub_8238A140(ctx, nullBase);
            fprintf(stderr, "NULL RECORD SWALLOWED r3=0x%08X\n", ctx.r3.u32);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--resolve-boundary") == 0) {
            uint8_t* resolveBase = memory->base();
            uint32_t scratch = memory->allocate(4096);
            check(scratch, "Resolve-boundary fixture allocation failed");
            auto query = [&](const char* name, uint32_t root) {
                size_t length = strlen(name);
                memcpy(resolveBase + scratch + 512, name, length + 1);
                memory->write32(scratch, root);
                memory->write32(scratch + 4, scratch + 16);
                memory->write32(scratch + 8, 0x40);
                memory->write32(scratch + 16, uint32_t(length << 16) | uint32_t(length + 1));
                memory->write32(scratch + 20, scratch + 512);
                ctx.r3.u64 = scratch;
                ctx.r4.u64 = scratch + 256;
                __imp__NtQueryFullAttributesFile(ctx, resolveBase);
                return ctx.r3.u32;
            };
            check(query("Content", 0xfffffffd) == 0xc000003b,
                  "Bare-relative query acquired implicit volume");
            check(query("Content", 0) == 0xc000003b,
                  "Bare-relative null-root query acquired implicit volume");
            check(query("D:\\Content", 0xfffffffd) == 0,
                  "Explicit mounted Content query failed");
            check(query("\\Device\\CdRom0\\basefile.exe", 0xfffffffd) == 0 &&
                  query("/dEvIcE/cDrOm0/Content", 0xfffffffd) == 0,
                  "CD-ROM mount lost case-insensitive or slash-normalized lookup");
            check(query("\\Device\\CdRom0basefile.exe", 0xfffffffd) == 0xc000000e &&
                  query("\\Device\\CdRom00\\basefile.exe", 0xfffffffd) == 0xc000000e,
                  "Device name prefix was incorrectly accepted as the CD-ROM mount");
            size_t dirLength = strlen("D:\\Content");
            memcpy(resolveBase + scratch + 1024, "D:\\Content", dirLength + 1);
            memory->write32(scratch + 128, 0xfffffffd);
            memory->write32(scratch + 132, scratch + 144);
            memory->write32(scratch + 136, 0x40);
            memory->write32(scratch + 144, uint32_t(dirLength << 16) | uint32_t(dirLength + 1));
            memory->write32(scratch + 148, scratch + 1024);
            ctx.r3.u64 = scratch + 160;
            ctx.r4.u64 = 0x00100001;
            ctx.r5.u64 = scratch + 128;
            ctx.r6.u64 = scratch + 192;
            ctx.r7.u64 = 0;
            ctx.r8.u64 = 0;
            ctx.r9.u64 = FILE_SHARE_READ;
            ctx.r10.u64 = 1;
            memory->write32(ctx.r1.u32 + 84, 0x3);
            __imp__NtCreateFile(ctx, resolveBase);
            check(ctx.r3.u32 == 0, "Explicit Content directory open failed");
            uint32_t dirHandle = memory->read32(scratch + 160);
            check(query("XDF", dirHandle) == 0, "Handle-relative child query failed");
            ctx.r3.u64 = dirHandle;
            __imp__NtClose(ctx, resolveBase);
            check(query("XDF", dirHandle) == 0xc0000008, "Closed handle remained usable");
            check(query("XDF", 0x12345678) == 0xc0000008, "Invalid handle accepted");
            puts("Resolve boundary holds: bare-relative rejected, mounts and handles work.");
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--suspend-resume") == 0) {
            currentContext = &ctx;
            testSuspendResume(ctx);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--system-version") == 0) {
            currentContext = &ctx;
            DarkRecomp::Native::initializeKernel();
            uint32_t guard = memory->allocate(16);
            check(guard, "Version fixture allocation failed");
            memory->write32(guard, 0xA5A5A5A5);
            ctx.r3.u64 = 0x11111111; ctx.r4.u64 = 0x22222222;
            ctx.r5.u64 = 0x33333333; ctx.r6.u64 = 0x44444444; ctx.r7.u64 = 0x55555555;
            __imp__XamGetSystemVersion(ctx, memory->base());
            check(ctx.r3.u32 == 0x20160000, "Packed system version wrong");
            check(ctx.r4.u32 == 0x22222222 && ctx.r5.u32 == 0x33333333 &&
                  ctx.r6.u32 == 0x44444444 && ctx.r7.u32 == 0x55555555,
                  "Version query clobbered argument registers");
            check(memory->read32(guard) == 0xA5A5A5A5, "Version query wrote guest memory");
            check((ctx.r3.u32 >> 28) == 2 && ((ctx.r3.u32 >> 24) & 0xF) == 0 &&
                  ((ctx.r3.u32 >> 8) & 0xFFFF) == 5632 && (ctx.r3.u32 & 0xFF) == 0,
                  "Packed version components wrong");
            uint32_t krnl = memory->read32(0x82000898);
            const uint8_t* bytes = memory->base() + krnl;
            uint32_t rebuilt = ((uint32_t(bytes[1]) & 0xF) << 28) | ((uint32_t(bytes[3]) & 0xF) << 24) |
                ((uint32_t(bytes[4]) << 8) | uint32_t(bytes[5])) << 8 | ((uint32_t(bytes[6]) << 8) | uint32_t(bytes[7]));
            check(rebuilt == ctx.r3.u32, "System version disagrees with XboxKrnlVersion data");
            memory->release(guard);
            puts("System version matches title SDK compatibility profile.");
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--xmp-control") == 0) {
            currentContext = &ctx;
            auto* base = memory->base();
            uint32_t fixture = memory->allocate(256);
            uint32_t timeoutCell = memory->allocate(16);
            check(fixture && timeoutCell, "XMP fixture allocation failed");
            ctx.r3.u64 = 0x20;
            sub_828A7520(ctx, base);
            uint32_t listener = ctx.r3.u32;
            check(listener, "Wrapper listener creation failed");
            ctx.r3.u64 = (uint64_t(1) << 40) | 0x20; ctx.r4.u64 = 2;
            __imp__XamNotifyCreateListener(ctx, base);
            uint32_t wide = ctx.r3.u32;
            check(wide, "Wide-mask listener creation failed");
            ctx.r3.u64 = uint64_t(1) << 40; ctx.r4.u64 = 2;
            __imp__XamNotifyCreateListener(ctx, base);
            uint32_t narrow = ctx.r3.u32;
            check(narrow, "Narrow-mask listener creation failed");
            auto request = [&](uint32_t app, uint32_t message, uint32_t overlapped,
                               uint32_t buffer, uint32_t length) {
                ctx.r3.u64 = app; ctx.r4.u64 = message; ctx.r5.u64 = overlapped;
                ctx.r6.u64 = buffer; ctx.r7.u64 = length; ctx.r8.u64 = 0;
                __imp__XMsgStartIORequestEx(ctx, base);
                return ctx.r3.u32;
            };
            auto getNext = [&](uint32_t handle, uint32_t filter, uint32_t* id, uint32_t* data) {
                ctx.r3.u64 = handle; ctx.r4.u64 = filter;
                ctx.r5.u64 = fixture; ctx.r6.u64 = fixture + 4;
                __imp__XNotifyGetNext(ctx, base);
                if (id) *id = memory->read32(fixture);
                if (data) *data = memory->read32(fixture + 4);
                return ctx.r3.u32;
            };
            auto waitHandle = [&](uint32_t handle, int64_t timeout100ns) {
                *reinterpret_cast<uint64_t*>(base + timeoutCell) = _byteswap_uint64(uint64_t(timeout100ns));
                ctx.r3.u64 = handle; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = timeoutCell;
                __imp__NtWaitForSingleObjectEx(ctx, base);
                return ctx.r3.u32;
            };
            uint32_t id = 0, data = 0;
            uint32_t errCell = memory->allocate(1024);
            check(errCell, "Error cell allocation failed");
            memset(base + errCell, 0, 1024);
            memory->write32(ctx.r13.u32 + 256, errCell);
            ctx.r3.u64 = 0x1234ABCDu;
            sub_828AAEA8(ctx, base);
            ctx.r3.u64 = 0;
            sub_828AAEF8(ctx, base);
            check(ctx.r3.u32 == 0x1234ABCDu, "Seeded thread error unreadable");
            memory->write32(fixture + 16, 2); memory->write32(fixture + 20, 0);
            memory->write32(fixture + 24, 1);
            check(request(0xFA, 0x7001A, 0, fixture + 16, 12) == 0, "Playback claim failed");
            ctx.r3.u64 = 0;
            sub_828AAEF8(ctx, base);
            check(ctx.r3.u32 == 0, "Successful claim did not clear thread error");
            check(waitHandle(wide, -50000000ll) == 0, "Notification did not signal");
            check(getNext(wide, 0x1234, &id, &data) == 0, "Filter miss consumed notification");
            check(waitHandle(wide, -100000ll) == 0, "Filter miss lost signal");
            check(getNext(wide, 0, &id, &data) == 1 && id == 0x0A000003 && data == 0,
                  "Claim notification wrong");
            check(waitHandle(wide, -100000ll) == 0x102, "Drained listener still signaled");
            check(getNext(listener, 0, &id, &data) == 1 && id == 0x0A000003 && data == 0,
                  "Wrapper listener missed claim");
            check(getNext(narrow, 0, &id, &data) == 0, "Unmasked listener got notification");
            memory->write32(fixture + 24, 0);
            check(request(0xFA, 0x7001A, 0, fixture + 16, 12) == 0, "Playback restore failed");
            check(getNext(wide, 0, &id, &data) == 1 && id == 0x0A000003 && data == 1,
                  "Restore notification wrong");
            check(getNext(wide, 0, &id, &data) == 0, "Drain left notification");
            check(request(0xFB, 0x7001A, 0, fixture + 16, 12) == 0x80070057, "Wrong app code wrong");
            check(request(0xFA, 0x1234, 0, fixture + 16, 12) == 0x80004001, "Unknown message code wrong");
            check(request(0xFA, 0x7001A, 0, fixture + 16, 8) == 0x80070057, "Short buffer code wrong");
            check(request(0xFA, 0x7001A, 0, 0, 12) == 0x80070057, "Null buffer code wrong");
            memory->write32(fixture + 16, 9); memory->write32(fixture + 20, 9);
            memory->write32(fixture + 24, 9);
            check(request(0xFA, 0x7001A, 0, fixture + 16, 12) == 0x80070057, "Illegal values code wrong");
            memory->write32(fixture + 16, 2); memory->write32(fixture + 20, 0);
            memory->write32(fixture + 24, 1);
            check(request(0xFA, 0x7001A, 1, fixture + 16, 12) == 0x80070032, "Overlapped code wrong");
            check(getNext(wide, 0, &id, &data) == 0, "Malformed request produced notification");
            for (uint32_t handle : {listener, wide, narrow}) {
                ctx.r3.u64 = handle; __imp__NtClose(ctx, base);
            }
            check(getNext(listener, 0, &id, &data) == 0, "Closed listener resolved");
            memory->write32(ctx.r13.u32 + 256, 0);
            memory->release(errCell);
            memory->release(fixture); memory->release(timeoutCell);
            testNotificationAliases(ctx);
            puts("XMP playback control, notification and malformed rejection passed.");
            return 0;
        }
        if (argc == 4 && strcmp(argv[2], "--xma-bridge") == 0) {
            currentContext = &ctx;
            testXmaBridge(ctx, argv[3]);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--xma-lifecycle") == 0) {
            currentContext = &ctx;
            auto* base = memory->base();
            auto readBE16 = [&](uint32_t address) {
                return uint16_t(uint16_t(base[address]) << 8 | uint16_t(base[address + 1]));
            };
            ctx.r3.u64 = 0x11111111; ctx.r4.u64 = 0x22222222; ctx.r5.u64 = 0x33333333;
            ctx.r6.u64 = 0x44444444; ctx.r7.u64 = 0x55555555; ctx.r8.u64 = 0x66666666;
            sub_828B0B10(ctx, base);
            check(ctx.r3.u32 == 0x11111111 && ctx.r4.u32 == 0x22222222 && ctx.r5.u32 == 0x33333333 &&
                  ctx.r6.u32 == 0x44444444 && ctx.r7.u32 == 0x55555555 && ctx.r8.u32 == 0x66666666,
                  "Discovery clobbered input registers");
            uint32_t poolBase = memory->read32(0x82A49AD0u);
            check(poolBase && !(poolBase & 255), "Pool discovery returned bad base");
            // Verify the actual Windows region extent, not a timing threshold:
            // every decoder context must have a bounded permission query even
            // after ordinary heap allocations grow alongside it.
            auto checkPoolRegion = [&] {
                MEMORY_BASIC_INFORMATION region{}, left{}, right{};
                check(VirtualQuery(base + poolBase, &region, sizeof(region)) &&
                      region.State == MEM_COMMIT && region.Protect == PAGE_READWRITE &&
                      region.BaseAddress == base + poolBase && region.RegionSize == 320 * 64,
                      "XMA context checks would scan beyond their dedicated pool");
                check(VirtualQuery(base + poolBase - 1, &left, sizeof(left)) &&
                      VirtualQuery(base + poolBase + 320 * 64, &right, sizeof(right)) &&
                      left.State == MEM_RESERVE && right.State == MEM_RESERVE,
                      "XMA pool must remain separated from neighboring committed memory");
            };
            checkPoolRegion();
            uint32_t records = memory->allocate(192);
            check(records, "Lifecycle records allocation failed");
            memset(base + records, 0, 192);
            uint32_t group = memory->allocate(16);
            check(group, "Lifecycle group allocation failed");
            checkPoolRegion();
            memory->write32(group, 2);
            memory->write32(group + 4, 0);
            memory->write32(group + 8, records);
            ctx.r3.u64 = group;
            sub_828B1840(ctx, base);
            check(ctx.r3.u32 == 0, "Original create-group failed");
            check(memory->read32(group + 4) & 0x40000, "Create-group flag not set");
            uint32_t first = memory->read32(records + 64);
            uint32_t second = memory->read32(records + 160);
            check(first && second && first != second, "Group contexts not unique");
            check(!(first & 63) && !(second & 63), "Group contexts misaligned");
            check(readBE16(records + 80) == (first - poolBase) / 64, "Record index disagrees with pool origin");
            check(readBE16(records + 176) == (second - poolBase) / 64, "Second index disagrees with pool origin");
            std::vector<uint32_t> live;
            auto directCreate = [&]() {
                uint32_t out = memory->allocate(4);
                check(out, "Lifecycle out-slot allocation failed");
                ctx.r3.u64 = out;
                __imp__XMACreateContext(ctx, base);
                uint32_t status = ctx.r3.u32;
                uint32_t pointer = memory->read32(out);
                memory->release(out);
                return std::pair<uint32_t, uint32_t>(status, pointer);
            };
            auto directRelease = [&](uint32_t pointer) {
                ctx.r3.u64 = pointer;
                __imp__XMAReleaseContext(ctx, base);
                return ctx.r3.u32;
            };
            {
                auto [status, pointer] = directCreate();
                check(status == 0 && pointer && !(pointer & 63), "Direct create failed");
                for (uint32_t i = 0; i < 16; ++i)
                    check(memory->read32(pointer + i * 4) == 0, "Fresh context not zero");
                check(directRelease(pointer) == 0, "Direct release failed");
                auto [restatus, repointer] = directCreate();
                check(restatus == 0 && repointer == pointer, "Released slot not reused zeroed");
                check(directRelease(repointer) == 0, "Reuse release failed");
            }
            check(directRelease(first + 32) == 0xc000000d, "Misaligned release accepted");
            check(directRelease(poolBase + 320 * 64 + 64) == 0xc000000d, "Foreign release accepted");
            {
                auto [status, pointer] = directCreate();
                check(status == 0, "Intact-pool create failed after invalid releases");
                check(directRelease(pointer) == 0, "First release failed");
                check(directRelease(pointer) == 0xc000000d, "Double release accepted");
            }
            {
                ctx.r3.u64 = 0;
                __imp__XMACreateContext(ctx, base);
                check(ctx.r3.u32 == 0xc000000d, "Null out-slot accepted");
                ctx.r3.u64 = 0x60000000;
                __imp__XMACreateContext(ctx, base);
                check(ctx.r3.u32 == 0xc000000d, "Unmapped out-slot accepted");
            }
            {
                uint32_t roSlot = memory->allocate(4096);
                check(roSlot, "Readonly fixture allocation failed");
                DWORD roOld = 0;
                check(VirtualProtect(base + roSlot, 4096, PAGE_READONLY, &roOld),
                      "Readonly fixture protect failed");
                ctx.r3.u64 = roSlot;
                __imp__XMACreateContext(ctx, base);
                check(ctx.r3.u32 == 0xc000000d, "Readonly out-slot accepted");
                check(VirtualProtect(base + roSlot, 4096, PAGE_READWRITE, &roOld),
                      "Readonly fixture restore failed");
                DWORD guardOld = 0;
                check(VirtualProtect(base + roSlot, 4096, PAGE_READWRITE | PAGE_GUARD, &guardOld),
                      "Guard fixture protect failed");
                ctx.r3.u64 = roSlot;
                __imp__XMACreateContext(ctx, base);
                check(ctx.r3.u32 == 0xc000000d, "Guard-page out-slot accepted");
                check(VirtualProtect(base + roSlot, 4096, PAGE_READWRITE, &guardOld),
                      "Guard fixture restore failed");
                check(memory->release(roSlot), "Readonly fixture release failed");
            }
            ctx.r3.u64 = group;
            sub_828B0DE0(ctx, base);
            check(ctx.r3.u32 == 0, "Original release-group failed");
            check(memory->read32(records + 64) == 0 && memory->read32(records + 160) == 0,
                  "Release-group did not clear slots");
            check(!(memory->read32(group + 4) & 0x40000), "Release-group flag not cleared");
            uint32_t made = 0;
            for (uint32_t i = 0; i < 320 + 5; ++i) {
                auto [status, pointer] = directCreate();
                if (status) {
                    check(status == 0xc0000017, "Exhaustion status wrong");
                    break;
                }
                live.push_back(pointer);
                ++made;
            }
            check(made == 320, "Exhaustion capacity wrong");
            {
                uint32_t failOut = memory->allocate(4);
                check(failOut, "Exhaustion out-slot allocation failed");
                memory->write32(failOut, 0xDEADBEEFu);
                ctx.r3.u64 = failOut;
                __imp__XMACreateContext(ctx, base);
                check(ctx.r3.u32 == 0xc0000017, "Exhaustion status wrong");
                check(memory->read32(failOut) == 0, "Exhausted create left stale pointer");
                memory->release(failOut);
            }
            for (uint32_t pointer : live) check(directRelease(pointer) == 0, "Cleanup release failed");
            live.clear();
            {
                auto [status, pointer] = directCreate();
                check(status == 0 && pointer, "Pool not reusable after groups");
                check(directRelease(pointer) == 0, "Final release failed");
            }
            {
                Memory probe;
                uint32_t a = probe.xmaCreate();
                check(a, "Reset instance create failed");
                uint32_t b = probe.xmaCreate();
                check(b && b != a, "Reset instance slots not unique");
                check(probe.xmaFree(a), "Reset instance free failed");
                check(!probe.xmaFree(a), "Reset instance double free accepted");
                check(!probe.xmaFree(0x60000000), "Reset instance foreign free accepted");
                check(probe.xmaPoolBase() != 0, "Reset instance pool homeless");
            }
            {
                Memory probe;
                check(probe.xmaCreate(), "Fresh instance create failed");
            }
            {
                uint32_t idleRecords = memory->allocate(96);
                uint32_t idleGroup = memory->allocate(16);
                check(idleRecords && idleGroup, "Idle submit fixture allocation failed");
                memset(base + idleRecords, 0, 96);
                memory->write32(idleGroup, 0);
                memory->write32(idleGroup + 4, 0x20000);
                memory->write32(idleGroup + 8, idleRecords);
                ctx.r3.u64 = idleGroup;
                sub_828B1BA8(ctx, base);
                check(ctx.r3.u32 == 0, "Empty submit failed");
                auto [idleStatus, idleContext] = directCreate();
                check(idleStatus == 0 && idleContext, "Idle context allocation failed");
                memory->write32(idleRecords + 64, idleContext);
                memory->write32(idleGroup, 1);
                memory->write32(idleGroup + 4, 0x20000);
                ctx.r3.u64 = idleGroup;
                sub_828B1BA8(ctx, base);
                check(ctx.r3.u32 == 0, "Idle snapshot submit failed");
                memory->write32(idleGroup, 1);
                memory->write32(idleGroup + 4, 0x20000);
                ctx.r3.u64 = idleGroup;
                sub_828B1BA8(ctx, base);
                check(ctx.r3.u32 == 0, "Idle snapshot submit failed");
                memory->write32(idleGroup + 4, 0);
                ctx.r3.u64 = idleGroup;
                sub_828B1BA8(ctx, base);
                check(ctx.r3.u32 == 0, "Unflagged submit failed");
                check(directRelease(idleContext) == 0, "Idle context release failed");
                memory->release(idleRecords); memory->release(idleGroup);
            }
            memory->release(records); memory->release(group);
            puts("XMA context lifecycle holds: discovery, groups, reuse, exhaustion.");
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--xma-submit-live") == 0) {
            currentContext = &ctx;
            auto* base = memory->base();
            uint32_t records = memory->allocate(96);
            uint32_t group = memory->allocate(16);
            uint32_t input = memory->allocate(2048);
            uint32_t output = memory->allocate(4096);
            check(records && group && input && output, "Live submit fixture allocation failed");
            memset(base + records, 0, 96);
            uint32_t liveOut = memory->allocate(4);
            check(liveOut, "Live context out-slot allocation failed");
            ctx.r3.u64 = liveOut;
            __imp__XMACreateContext(ctx, base);
            check(ctx.r3.u32 == 0, "Live submit context allocation failed");
            uint32_t live = memory->read32(liveOut);
            memory->write32(live, 0x00100001);
            memory->write32(live + 20, input);
            memory->write32(live + 28, output);
            memory->write32(records + 64, live);
            memory->write32(group, 1);
            memory->write32(group + 4, 0);
            memory->write32(group + 8, records);
            ctx.r3.u64 = group;
            sub_828B1BA8(ctx, base);
            fprintf(stderr, "ACTIVE SUBMIT SWALLOWED r3=0x%08X\n", ctx.r3.u32);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--xma-submit-staged") == 0) {
            currentContext = &ctx;
            auto* base = memory->base();
            uint32_t records = memory->allocate(96);
            uint32_t group = memory->allocate(16);
            uint32_t input = memory->allocate(2048);
            uint32_t output = memory->allocate(4096);
            check(records && group && input && output, "Staged submit fixture allocation failed");
            memset(base + records, 0, 96);
            uint32_t stagedOut = memory->allocate(4);
            check(stagedOut, "Staged context out-slot allocation failed");
            ctx.r3.u64 = stagedOut;
            __imp__XMACreateContext(ctx, base);
            check(ctx.r3.u32 == 0, "Staged submit context allocation failed");
            uint32_t staged = memory->read32(stagedOut);
            memory->write32(records, 0x00100001);
            memory->write32(records + 20, input);
            memory->write32(records + 28, output);
            memory->write32(records + 64, staged);
            memory->write32(group, 1);
            memory->write32(group + 4, 0x20000);
            memory->write32(group + 8, records);
            ctx.r3.u64 = group;
            sub_828B1BA8(ctx, base);
            fprintf(stderr, "ACTIVE SUBMIT SWALLOWED r3=0x%08X\n", ctx.r3.u32);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--audio-driver") == 0) {
            currentContext = &ctx;
            testAudioDriver(ctx, argv[1]);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--timestamp-bundle") == 0) {
            currentContext = &ctx;
            DarkRecomp::Native::initializeKernel();
            uint32_t bundle = memory->read32(0x8200089C);
            MEMORY_BASIC_INFORMATION bundleInfo{};
            bool backed = VirtualQuery(memory->base() + bundle + 16, &bundleInfo, sizeof(bundleInfo)) &&
                bundleInfo.State == MEM_COMMIT;
            if (!backed) {
                fprintf(stderr, "TIMESTAMP BUNDLE UNBACKED ptr=0x%08X\n", bundle);
                return 1;
            }
            auto readBundle = [&]() { ctx.r3.u64 = 0; sub_828AB258(ctx, memory->base()); return ctx.r3.u32; };
            check(memory->read32(bundle) == 0 && memory->read32(bundle + 4) == 0 &&
                  memory->read32(bundle + 8) == 0 && memory->read32(bundle + 12) == 0,
                  "Timestamp bundle high words not zero");
            check(memory->read32(bundle + 20) == 0, "Timestamp bundle trailer not zero");
            uint32_t before = readBundle();
            uint32_t raw = *reinterpret_cast<volatile uint32_t*>(memory->base() + bundle + 16);
            uint32_t after = readBundle();
            uint32_t decoded = _byteswap_ulong(raw);
            check(decoded >= before && decoded <= after, "Timestamp bundle endian race unsafe");
            LARGE_INTEGER hostFrequency{}, hostStart{}, hostEnd{};
            QueryPerformanceFrequency(&hostFrequency);
            QueryPerformanceCounter(&hostStart);
            uint32_t first = readBundle();
            Sleep(120);
            uint32_t second = readBundle();
            QueryPerformanceCounter(&hostEnd);
            double hostMs = double(hostEnd.QuadPart - hostStart.QuadPart) * 1000.0 / double(hostFrequency.QuadPart);
            check(hostMs >= 100 && hostMs <= 10000, "Host clock interval insane");
            uint32_t guestMs = second - first;
            check(double(guestMs) + 100.0 >= hostMs && double(guestMs) <= hostMs + 100.0,
                  "Timestamp bundle unit mismatch");
            memory->stopTimestamp();
            uint32_t stoppedA = readBundle();
            Sleep(40);
            uint32_t stoppedB = readBundle();
            check(stoppedA == stoppedB, "Stopped updater moved");
            DarkRecomp::Native::initializeKernel();
            uint32_t revived = readBundle();
            check(revived >= stoppedB, "Reinit moved uptime backward");
            Sleep(60);
            uint32_t resumed = readBundle();
            check(resumed > revived && resumed - revived < 10000, "Timestamp bundle did not advance after reinit");
            puts("Timestamp bundle advances in milliseconds with original reader.");
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "--invalid-dispatch") == 0) {
            ctx.r3.u64 = 0x11223344;
            PPCSafeIndirect(ctx, memory->base(), 0);
            return 10;
        }
        ctx.r3.u64 = 0xfeeefeee;
        __imp__KeQueryPerformanceFrequency(ctx, memory->base());
        check(ctx.r3.u64 == 49875000, "Timebase frequency ABI mismatch");
        uint64_t before = PPCQueryTimebase();
        Sleep(10);
        uint64_t elapsed = PPCQueryTimebase() - before;
        check(elapsed > 100000 && elapsed < 49875000, "Timebase is not calibrated to its reported frequency");
        uint32_t staticTls = memory->read32(ctx.r13.u32);
        uint8_t staticBefore[20];
        memcpy(staticBefore, memory->base() + staticTls, sizeof(staticBefore));
        for (uint32_t i = 0; i < 64; ++i) {
            __imp__KeTlsAlloc(ctx, memory->base());
            check(ctx.r3.u32 == i, "TLS allocation lost an index above 31");
        }
        __imp__KeTlsAlloc(ctx, memory->base());
        check(ctx.r3.u32 == 0xffffffff, "TLS exhaustion not reported");
        ctx.r3.u64 = 47;
        __imp__KeTlsFree(ctx, memory->base());
        __imp__KeTlsAlloc(ctx, memory->base());
        check(ctx.r3.u32 == 47, "TLS slot not reusable");
        ctx.r4.u64 = 0x12345678;
        __imp__KeTlsSetValue(ctx, memory->base());
        ctx.r3.u64 = 47;
        __imp__KeTlsGetValue(ctx, memory->base());
        check(ctx.r3.u32 == 0x12345678, "TLS endian round trip failed");
        for (uint32_t i = 0; i < 5; ++i) {
            ctx.r3.u64 = i; ctx.r4.u64 = 0xabcdef00 + i;
            __imp__KeTlsSetValue(ctx, memory->base());
        }
        check(memcmp(staticBefore, memory->base() + staticTls, sizeof(staticBefore)) == 0,
              "Kernel TLS slots overwrote compiler-generated static thread variables");
        uint32_t allocation = memory->allocate(4097, 65536);
        check(allocation && !(allocation & 65535), "Guest allocation alignment failed");
        check(memory->allocationSize(allocation) == 8192, "Guest allocation rounding failed");
        check(memory->allocate(0xffffffff) == 0, "Guest allocation overflow accepted");
        check(memory->release(allocation), "Guest allocation release failed");
        check(!memory->release(allocation), "Double free accepted");
        // This original leaf recognizes a control byte and dispatches a 58-way
        // character table. Its case bodies were previously replaced by return.
        // Expected results are independently read from the original PPC case
        // instructions (li r3, constant; blr), not from generated C++.
        uint32_t input = memory->allocate(4096);
        ctx.r3.u64 = input;
        __imp__XGetVideoMode(ctx, memory->base());
        check(ctx.r3.u64 == 0 && memory->read32(input + 0) == 1280 &&
              memory->read32(input + 4) == 720 && memory->read32(input + 20) == 0x42700000,
              "XGetVideoMode ABI mismatch");
        ctx.r3.u64 = input;
        __imp__VdQueryVideoMode(ctx, memory->base());
        check(ctx.r3.u64 == 0 && memory->read32(input + 12) == 1 &&
              memory->read32(input + 32) == 1,
              "VdQueryVideoMode ABI mismatch");
        for (uint32_t width : {1720u, 2560u}) {
            check(setNativeVideoMode(width, 720), "Ultrawide video mode rejected");
            ctx.r3.u64 = input; __imp__XGetVideoMode(ctx, memory->base());
            check(memory->read32(input) == width && memory->read32(input + 4) == 720 &&
                  memory->read32(input + 20) == 0x42700000, "Ultrawide XGetVideoMode ABI mismatch");
            ctx.r3.u64 = input; __imp__VdQueryVideoMode(ctx, memory->base());
            check(memory->read32(input) == width && memory->read32(input + 4) == 720,
                  "Ultrawide VdQueryVideoMode ABI mismatch");
        }
        // The original selected-mode indirection, with an untouched neighbor
        // and metadata canary. Logical mode identifiers must remain unchanged.
        const uint32_t modeFixture = memory->allocate(4096);
        check(modeFixture != 0, "Cannot allocate video mode fixture");
        const uint32_t modeDisplay = modeFixture, modeCollection = modeFixture + 400;
        const uint32_t modeEntries = modeFixture + 440, selectedMode = modeFixture + 500, otherMode = modeFixture + 600;
        memory->write32(modeDisplay + 24, modeCollection); memory->write32(modeDisplay + 304, 1);
        memory->write32(modeCollection + 24, modeEntries);
        memory->write32(modeEntries, otherMode); memory->write32(modeEntries + 4, selectedMode);
        memory->write32(otherMode + 64, 1280); memory->write32(selectedMode + 72, 0x12345678);
        memory->write32(selectedMode + 24, 1280); memory->write32(selectedMode + 28, 720);
        check(configureGuestRenderMode(modeDisplay), "Selected native video mode setup failed");
        check(memory->read32(selectedMode + 24) == 1280 && memory->read32(selectedMode + 64) == 2560 &&
              memory->read32(selectedMode + 28) == 720 && memory->read32(selectedMode + 68) == 720 &&
              memory->read32(selectedMode + 72) == 0x12345678 && memory->read32(otherMode + 64) == 1280,
              "Selected video mode update changed unrelated fields or disagreed with allocation");
        memory->write32(modeDisplay + 304, 0xFFFFFFFF);
        check(!configureGuestRenderMode(modeDisplay), "Invalid selected video mode accepted");
        const uint32_t menuContext = modeFixture + 1024;
        memory->write32(menuContext + 676, 2560); memory->write32(menuContext + 680, 720);
        auto putFloat = [&](uint32_t offset, float value) { memory->write32(menuContext + offset, std::bit_cast<uint32_t>(value)); };
        auto getFloat = [&](uint32_t offset) { return std::bit_cast<float>(memory->read32(menuContext + offset)); };
        putFloat(336, 4); putFloat(340, 1.5f);
        putFloat(272, 2.0f/640); putFloat(320, -1); putFloat(292, -.01f);
        check(fitLegacyMenuMatrix(menuContext), "Legacy menu canvas not recognized");
        check(std::abs((getFloat(320)+1)*1280 - 640) < .001f &&
              std::abs((640*getFloat(272)+getFloat(320)+1)*1280 - 1920) < .001f &&
              getFloat(336) == 4 && getFloat(340) == 1.5f && getFloat(292) == -.01f,
              "Legacy menu was stretched, misplaced, or changed logical/vertical scale");
        putFloat(336, 1.5f);
        check(!fitLegacyMenuMatrix(menuContext), "Height-based HUD was fitted a second time");
        const float leftLabel = fitMenuLabelX(120, 30, false, 1.5f, {2560,720});
        const float rightLabel = fitMenuLabelX(120, 30, true, 1.5f, {2560,720});
        check(std::abs(leftLabel*1.5f - 730) < .001f &&
              std::abs((rightLabel+30)*1.5f - 752.5f) < .001f &&
              fitMenuLabelX(120,30,true,1.5f,{1280,720}) == 120,
              "Menu label lost its anchor, alignment, or 16:9 behavior");
        check(memory->release(modeFixture), "Cannot release video mode fixture");
        check(!setNativeVideoMode(0, 720) && !setNativeVideoMode(5000, 720), "Invalid video mode accepted");
        check(setNativeVideoMode(1280, 720), "Default video mode restore failed");
        uint32_t event = input + 512;
        memory->write32(event, 0x01000000);
        memory->write32(event + 4, 0);
        ctx.r3.u64 = event;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 0;
        __imp__KeSetEvent(ctx, memory->base());
        check(ctx.r3.u32 == 0, "KeSetEvent previous state mismatch");
        ctx.r3.u64 = event;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = 0;
        __imp__KeWaitForSingleObject(ctx, memory->base());
        check(ctx.r3.u32 == 0 && memory->read32(event + 4) == 0,
              "KeWaitForSingleObject did not consume auto-reset event");
        *reinterpret_cast<uint64_t*>(memory->base() + input + 600) = _byteswap_uint64(0);
        ctx.r3.u64 = event;
        ctx.r7.u64 = input + 600;
        __imp__KeWaitForSingleObject(ctx, memory->base());
        check(ctx.r3.u32 == 0x102, "KeWaitForSingleObject ignored zero timeout");
        testVsnprintf(ctx);
        testFovCamera(ctx);
        testInput(ctx);
        testPhysicalAliases(ctx);
        testCViewBoundaries(ctx);
        testTextureTileLayout(ctx);
        testHighRamMmio();
        testSynchronization(ctx, input);
        testTlsReuse(ctx);
        testFiles(ctx, input);
        for (uint32_t ch = 0; ch < 256; ++ch) {
            memory->base()[input] = 167;
            memory->base()[input + 1] = uint8_t(ch);
            uint32_t expected = 1;
            if (ch >= 65 && ch <= 122) {
                uint32_t target = memory->read32(0x821fe804 + (ch - 65) * 4);
                uint32_t instruction = memory->read32(target);
                if (instruction != 0x4e800020) {
                    check((instruction & 0xffff0000) == 0x38600000, "Unexpected original switch case shape");
                    check(memory->read32(target + 4) == 0x4e800020, "Original case must return");
                    expected = instruction & 0xffff;
                }
            }
            ctx.r3.u64 = input;
            ctx.r4.u64 = 0;
            ctx.r31.u64 = 0x76543210;
            sub_821FE7C0(ctx, memory->base());
            check(ctx.r3.u32 == expected, "Recovered switch disagrees with original PPC case");
            check(ctx.r31.u64 == 0x76543210, "Leaf corrupted a nonvolatile register");
        }
        memory->base()[input] = 0;
        ctx.r3.u64 = input;
        ctx.r4.u64 = 0;
        sub_821FE7C0(ctx, memory->base());
        check(ctx.r3.u32 == 0, "Switch leaf rejected-prefix path failed");
        puts("All 256 switch inputs agree with the original game instructions.");
        // Shotgun-dispatch regression for the live crash at guest 824CCB30
        // (selector low word 8 with a nonzero high word produced by 64-bit
        // add-immediate modeling of 32-bit -1 wraparound). sub_82278D48
        // computes its recovered-table index as r4 - 1 under a low-word guard
        // admitting 0..6: the crash shape with a directly drivable selector.
        // Case bodies are virtual calls through r3, so each slot points at a
        // distinct pure `li r3, constant; blr` generated leaf; the
        // out-of-range path calls sub_82278DF8 and returns 0. This executes
        // the actual generated switch, not a copy of the emitter.
        {
            const uint32_t object = memory->allocate(4096);
            const uint32_t vtable = memory->allocate(4096);
            check(object != 0 && vtable != 0, "Switch dispatch fixture allocation failed");
            const uint32_t leaves[7] = {0x8211400C, 0x8237A734, 0x8237A73C, 0x8237A744, 0x8211402C, 0x82114024, 0x82114034};
            const uint32_t results[7] = {1, 2, 3, 4, 5, 6, 8};
            memory->write32(object, vtable);
            for (uint32_t i = 0; i < 7; ++i) memory->write32(vtable + 132 + i * 4, leaves[i]);
            auto dispatch = [&](uint64_t r4, uint32_t expected, const char* what) {
                ctx.r3.u64 = object;
                ctx.r4.u64 = r4;
                sub_82278D48(ctx, memory->base());
                check(ctx.r3.u32 == expected, what);
            };
            for (uint32_t v = 0; v < 7; ++v) {
                const uint64_t index = uint64_t(v) + 1;
                dispatch(index, results[v], "Recovered switch disagrees on zero-high-word index");
                // Nonzero high words from 64-bit wrap arithmetic must still
                // reach the legitimate low-word case: positive carry,
                // all-ones negative, and arbitrary nonzero high words.
                dispatch(0x100000000ull + index, results[v], "Recovered switch rejected a carried high word");
                dispatch(0xFFFFFFFF00000000ull + index, results[v], "Recovered switch rejected a negative high word");
                dispatch(0xDEADBEEF00000000ull + index, results[v], "Recovered switch rejected a nonzero high word");
            }
            // Out-of-range low words still take the reject path with any high word.
            dispatch(0, 0, "Switch dispatch accepted low-word wraparound below range");
            dispatch(8, 0, "Switch dispatch accepted low word above range");
            dispatch(0xFFFFFFFFull, 0, "Switch dispatch accepted negative low word");
            dispatch(0x100000008ull, 0, "Switch dispatch rescued out-of-range low word by high word");
            puts("Recovered switch dispatches low-word cases with nonzero high words and still rejects out-of-range low words.");
        }
        puts("Native memory, timers and all 64 TLS slots passed.");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "TEST FAILED: %s\n", e.what()); return 1; }
}
