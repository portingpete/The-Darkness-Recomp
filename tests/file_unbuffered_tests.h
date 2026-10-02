#pragma once
#include <malloc.h>

static void testUnbufferedRequestValidation(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t fixture = memory->allocate(8192);
    check(fixture != 0, "Unbuffered validation fixture allocation failed");
    const uint32_t ios = fixture + 64, offset = fixture + 80, out = fixture + 96;
    const uint32_t callback = fixture + 112, timeout = fixture + 128, position = fixture + 160;
    const uint32_t data = fixture + 4096;
    const std::string folder = "file-unbuffered-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64());
    const auto root = memory->gameDirectory().parent_path() / "build_native/run/cache" / folder;
    const auto path = root / "writable.bin";
    const auto oraclePath = root / "native.bin";
    uint32_t file = 0, event = 0;
    HANDLE oracle = INVALID_HANDLE_VALUE, oracleEvent = nullptr;
    std::unique_ptr<void, decltype(&_aligned_free)> oracleData(nullptr, _aligned_free);
    auto* original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = fileCompletionProbe;
    auto cleanup = [&] {
        SleepEx(0, TRUE);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
        if (file) { ctx.r3.u64 = file; __imp__NtClose(ctx, base); file = 0; }
        if (event) { ctx.r3.u64 = event; __imp__NtClose(ctx, base); event = 0; }
        if (oracle != INVALID_HANDLE_VALUE) { CloseHandle(oracle); oracle = INVALID_HANDLE_VALUE; }
        if (oracleEvent) { CloseHandle(oracleEvent); oracleEvent = nullptr; }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(oraclePath, ec);
        std::filesystem::remove(root, ec);
        memory->release(fixture);
    };
    auto open = [&](uint32_t options) {
        const std::string guest = "cache:\\" + folder + "\\writable.bin";
        std::memcpy(base + fixture + 256, guest.c_str(), guest.size() + 1);
        memory->write32(fixture, 0xfffffffd);
        memory->write32(fixture + 4, fixture + 16);
        memory->write32(fixture + 8, 0x40);
        memory->write32(fixture + 16, uint32_t(guest.size() << 16) | uint32_t(guest.size() + 1));
        memory->write32(fixture + 20, fixture + 256);
        ctx.r3.u64 = out; ctx.r4.u64 = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
        ctx.r5.u64 = fixture; ctx.r6.u64 = ios;
        ctx.r7.u64 = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE; ctx.r8.u64 = options;
        __imp__NtOpenFile(ctx, base);
        check(ctx.r3.u32 == 0, "Unbuffered validation writable open failed");
        file = memory->read32(out);
    };
    auto seek = [&](uint64_t value) {
        PPC_STORE_U64(position, value);
        ctx.r3.u64 = file; ctx.r4.u64 = ios; ctx.r5.u64 = position; ctx.r6.u64 = 8; ctx.r7.u64 = 14;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0, "Unbuffered validation seek failed");
    };
    auto tell = [&] {
        ctx.r3.u64 = file; ctx.r4.u64 = ios; ctx.r5.u64 = position; ctx.r6.u64 = 8; ctx.r7.u64 = 14;
        __imp__NtQueryInformationFile(ctx, base);
        check(ctx.r3.u32 == 0, "Unbuffered validation position query failed");
        return PPC_LOAD_U64(position);
    };
    auto request = [&](bool write, uint64_t value, uint32_t length, bool implicit = false) {
        PPC_STORE_U64(offset, value);
        ctx.r3.u64 = file; ctx.r4.u64 = event; ctx.r5.u64 = PPC_CODE_BASE | 1;
        ctx.r6.u64 = callback; ctx.r7.u64 = ios; ctx.r8.u64 = data;
        ctx.r9.u64 = length; ctx.r10.u64 = implicit ? 0 : offset;
        if (write) __imp__NtWriteFile(ctx, base); else __imp__NtReadFile(ctx, base);
    };
    try {
        check(std::filesystem::create_directories(root), "Unbuffered validation fixture already exists");
        const std::string expected(1024, 0x43);
        {
            std::ofstream stream(path, std::ios::binary);
            stream.write(expected.data(), expected.size());
            check(bool(stream), "Unbuffered validation file creation failed");
        }
        ctx.r3.u64 = out; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        __imp__NtCreateEvent(ctx, base);
        check(ctx.r3.u32 == 0, "Unbuffered validation event creation failed");
        event = memory->read32(out);
        open(0x68);
        for (bool write : {false, true}) {
            for (const auto& test : {std::pair<uint64_t, uint32_t>{0, 1}, {1, 512}}) {
                seek(512);
                ctx.r3.u64 = event; __imp__NtClearEvent(ctx, base);
                check(ctx.r3.u32 == 0, "Unbuffered validation event reset failed");
                std::memset(base + callback, 0, 12); std::memset(base + data, 0xa5, 512);
                request(write, test.first, test.second);
                check(ctx.r3.u32 == 0xc000000d && memory->read32(ios) == 0xc000000d &&
                          memory->read32(ios + 4) == 0,
                      "Misaligned unbuffered request was submitted to the host");
                check(tell() == 512, "Rejected unbuffered request changed the file position");
                check(std::all_of(base + data, base + data + 512, [](uint8_t b) { return b == 0xa5; }),
                      "Rejected unbuffered request changed guest data");
                PPC_STORE_U64(timeout, 0);
                ctx.r3.u64 = event; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = timeout;
                __imp__NtWaitForSingleObjectEx(ctx, base);
                check(ctx.r3.u32 == 0x102, "Rejected unbuffered request signaled an event");
                SleepEx(0, TRUE);
                check(memory->read32(callback) == 0, "Rejected unbuffered request queued an APC");
                std::ifstream stream(path, std::ios::binary);
                const std::string actual((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                check(actual == expected, "Rejected unbuffered request changed the file");
            }
        }
        // Native implementations differ on unbuffered sentinels. Compare with
        // direct aligned requests rather than asserting that they succeed.
        // Zero-length requests also retain native offset/completion handling.
        {
            std::ofstream stream(oraclePath, std::ios::binary);
            stream.write(expected.data(), expected.size());
            check(bool(stream), "Unbuffered native oracle creation failed");
        }
        oracle = CreateFileW(oraclePath.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, nullptr);
        oracleEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        oracleData.reset(_aligned_malloc(512, 4096));
        check(oracle != INVALID_HANDLE_VALUE && oracleEvent && oracleData,
              "Unbuffered native oracle setup failed");
        using NativeIo = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
            PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
        auto nativeRead = reinterpret_cast<NativeIo>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtReadFile"));
        auto nativeWrite = reinterpret_cast<NativeIo>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWriteFile"));
        check(nativeRead && nativeWrite, "Unbuffered native oracle entry points missing");
        struct NativeCase { bool write, implicit; uint64_t offset; uint32_t length; };
        const NativeCase nativeCases[]{{false, false, UINT64_MAX - 1, 512}, {false, true, 0, 512},
            {false, false, 1, 0}, {true, false, UINT64_MAX - 1, 512},
            {true, false, UINT64_MAX, 512}, {true, false, 1, 0}};
        for (const auto& test : nativeCases) {
            seek(0);
            LARGE_INTEGER zero{}, nativeOffset{};
            nativeOffset.QuadPart = test.offset;
            check(SetFilePointerEx(oracle, zero, nullptr, FILE_BEGIN) && ResetEvent(oracleEvent),
                  "Unbuffered native oracle reset failed");
            ctx.r3.u64 = event; __imp__NtClearEvent(ctx, base);
            check(ctx.r3.u32 == 0, "Unbuffered native comparison event reset failed");
            std::memset(base + callback, 0, 12);
            std::memset(base + data, test.write ? 0x43 : 0xa5, 512);
            std::memset(oracleData.get(), test.write ? 0x43 : 0xa5, 512);
            IO_STATUS_BLOCK nativeStatus{};
            uint32_t status = uint32_t((test.write ? nativeWrite : nativeRead)(oracle, oracleEvent,
                nullptr, nullptr, &nativeStatus, oracleData.get(), test.length,
                test.implicit ? nullptr : &nativeOffset, nullptr));
            if (status == 0x103) {
                check(WaitForSingleObject(oracleEvent, INFINITE) == WAIT_OBJECT_0,
                      "Unbuffered native oracle pending request did not complete");
                status = uint32_t(nativeStatus.Status);
            }
            const bool completed = WaitForSingleObject(oracleEvent, 0) == WAIT_OBJECT_0;
            request(test.write, test.offset, test.length, test.implicit);
            check(ctx.r3.u32 == status && memory->read32(ios) == status &&
                      memory->read32(ios + 4) == nativeStatus.Information,
                  "Unbuffered sentinel/zero-length request changed native status or byte count");
            check(std::memcmp(base + data, oracleData.get(), 512) == 0,
                  "Unbuffered sentinel/zero-length request changed native buffer handling");
            LARGE_INTEGER nativePosition{};
            check(SetFilePointerEx(oracle, zero, &nativePosition, FILE_CURRENT) &&
                      tell() == uint64_t(nativePosition.QuadPart),
                  "Unbuffered sentinel/zero-length request changed native file position");
            PPC_STORE_U64(timeout, 0);
            ctx.r3.u64 = event; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = timeout;
            __imp__NtWaitForSingleObjectEx(ctx, base);
            check(ctx.r3.u32 == (completed ? 0 : 0x102),
                  "Unbuffered sentinel/zero-length request changed native event completion");
            SleepEx(0, TRUE);
            check(memory->read32(callback) == unsigned(completed) &&
                      std::filesystem::file_size(path) == std::filesystem::file_size(oraclePath),
                  "Unbuffered sentinel/zero-length request changed native APC or file size");
        }
        ctx.r3.u64 = file; __imp__NtClose(ctx, base); file = 0;
        open(0x60);
        // Identical lengths/offsets remain legal with buffering enabled.
        for (bool write : {false, true}) {
            request(write, 1, 1);
            check(ctx.r3.u32 == 0 && memory->read32(ios + 4) == 1,
                  "Unbuffered validation leaked into buffered I/O");
            SleepEx(0, TRUE);
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
    puts("Unbuffered length/offset rejection preserves file, cursor, guest data and completion state.");
}
