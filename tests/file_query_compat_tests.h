#pragma once
#include "runtime/native/file_query_compat.h"

static void testDirectoryEventCompatibility() {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(event != nullptr, "Directory compatibility event allocation failed");
    try {
        for (uint32_t terminal : {0u, 0x80000005u, 0x80000006u, 0xc000000fu, 0xc000000du}) {
            IO_STATUS_BLOCK io{};
            unsigned calls = 0, advances = 0;
            const auto result = queryDirectoryWithCompletion([&](HANDLE supplied) {
                ++calls;
                if (supplied) {
                    check(supplied == event, "Directory query lost its original completion event");
                    io.Status = NTSTATUS(0xc0000002); io.Information = 999;
                    return 0xc0000002u;
                }
                check(io.Status == 0 && io.Information == 0, "Directory retry reused a rejected status block");
                if (terminal == 0 || terminal == 0x80000005) ++advances;
                io.Status = NTSTATUS(terminal); io.Information = advances ? 64 : 0;
                return terminal;
            }, event, event, io);
            check(calls == 2 && result.status == terminal && advances == unsigned(terminal == 0 || terminal == 0x80000005),
                  "Eventless directory retry skipped or consumed an extra entry");
            check(result.completed == (terminal != 0xc000000d),
                  "Eventless directory query lost terminal completion or fabricated a rejected completion");
        }
        for (uint32_t rejection : {0xc000000du, 0xc0000022u}) {
            IO_STATUS_BLOCK io{};
            unsigned calls = 0;
            const auto result = queryDirectoryWithCompletion([&](HANDLE) { ++calls; return rejection; }, event, event, io);
            check(calls == 1 && result.status == rejection && !result.completed,
                  "Directory fallback retried a real native error or fabricated completion");
        }
        IO_STATUS_BLOCK io{};
        unsigned calls = 0;
        const auto pending = queryDirectoryWithCompletion([&](HANDLE supplied) {
            ++calls; check(supplied == event, "Pending directory query lost its event");
            io.Status = 0; io.Information = 64; SetEvent(event);
            return 0x103u;
        }, event, event, io);
        check(calls == 1 && pending.status == 0 && pending.completed,
              "Ordinary Windows directory pending completion changed");
    } catch (...) { CloseHandle(event); throw; }
    CloseHandle(event);
    puts("Directory queries preserve Windows events and retry unsupported events without advancing twice.");
}

static void testFileOpenModeCompatibility(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t scratch = memory->allocate(4096);
    check(scratch != 0, "File mode compatibility allocation failed");
    const uint32_t ios = scratch + 64, output = scratch + 128;
    uint32_t active = 0;
    auto cleanup = [&] {
        if (active) { PPCContext close = ctx; close.r3.u64 = active; __imp__NtClose(close, base); }
        memory->release(scratch);
    };
    try {
        const char* path = "game:\\default.xex";
        std::strcpy(reinterpret_cast<char*>(base + scratch + 256), path);
        memory->write32(scratch, 0xfffffffd); memory->write32(scratch + 4, scratch + 16);
        memory->write32(scratch + 8, 0x40);
        memory->write32(scratch + 16, uint32_t(strlen(path) << 16) | uint32_t(strlen(path) + 1));
        memory->write32(scratch + 20, scratch + 256);
        for (uint32_t options : {0x40u, 0x60u, 0x68u, 0x64u}) {
            PPCContext call = ctx;
            call.r3.u64 = scratch + 80; call.r4.u64 = GENERIC_READ | SYNCHRONIZE;
            call.r5.u64 = scratch; call.r6.u64 = ios; call.r7.u64 = FILE_SHARE_READ; call.r8.u64 = options;
            __imp__NtOpenFile(call, base);
            check(call.r3.u32 == 0, "File mode compatibility open failed");
            active = memory->read32(scratch + 80);
            auto query = [&](uint32_t kind) {
                call = ctx; call.r3.u64 = active; call.r4.u64 = ios;
                call.r5.u64 = output; call.r6.u64 = 4; call.r7.u64 = kind;
                __imp__NtQueryInformationFile(call, base);
                check(call.r3.u32 == 0 && memory->read32(ios) == 0 && memory->read32(ios + 4) == 4,
                      "File mode/alignment query did not complete four bytes");
                return memory->read32(output);
            };
            check(query(16) == (options & 0x103e), "File mode query changed the successful open options");
            const uint32_t alignment = query(17);
            check((alignment & (alignment + 1)) == 0 && alignment != UINT32_MAX,
                  "File alignment query returned an unsupported buffer requirement");
            call = ctx; call.r3.u64 = active; call.r4.u64 = scratch + 80; call.r5.u64 = 1;
            __imp__NtDuplicateObject(call, base);
            check(call.r3.u32 == 0, "File mode alias creation failed");
            active = memory->read32(scratch + 80);
            check(query(16) == (options & 0x103e), "Duplicated file lost its open-mode metadata");
            call.r3.u64 = active; __imp__NtClose(call, base); active = 0;
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
    puts("File queries retain asynchronous/synchronous, unbuffered, sequential and alias mode semantics.");
}
