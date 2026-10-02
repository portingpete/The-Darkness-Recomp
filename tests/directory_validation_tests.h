#pragma once
#include <winternl.h>

static bool directoryQueryWithoutHostFault(PPCContext& ctx, uint8_t* base) {
    __try { __imp__NtQueryDirectoryFile(ctx, base); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

static void testDirectoryValidation(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t scratch = memory->allocate(4 * 4096);
    check(scratch != 0, "Directory validation allocation failed");
    const uint32_t ios = scratch + 64, callback = scratch + 128, pattern = scratch + 160;
    const uint32_t data = scratch + 4096, guard = scratch + 8192;
    const std::string folder = "directory-validation-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const auto root = memory->gameDirectory().parent_path() / "build_native/run/cache" / folder;
    check(std::filesystem::create_directories(root), "Directory validation fixture already exists");
    for (const auto* name : {"entry-a", "entry-b"}) {
        std::ofstream stream(root / name); stream << name;
        check(bool(stream), "Directory validation entry creation failed");
    }
    uint32_t file = 0, event = 0;
    auto* original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = fileCompletionProbe;
    auto cleanup = [&] {
        SleepEx(0, TRUE);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
        PPCContext close{};
        if (file) { close.r3.u64 = file; __imp__NtClose(close, base); }
        if (event) { close.r3.u64 = event; __imp__NtClose(close, base); }
        for (const auto* name : {"entry-a", "entry-b"}) std::filesystem::remove(root / name);
        std::filesystem::remove(root);
        memory->release(scratch);
    };
    unsigned failures = 0;
    auto report = [&](const char* label, bool retained, bool valid) {
        std::printf("DirectoryValidation[%s retained=%u] %s\n", label, unsigned(retained), valid ? "passed" : "FAILED");
        if (!valid) ++failures;
    };
    try {
        PPCContext call = ctx;
        call.r3.u64 = scratch + 96; call.r4.u64 = 0; call.r5.u64 = 0; call.r6.u64 = 0;
        __imp__NtCreateEvent(call, base);
        check(call.r3.u32 == 0, "Directory validation event creation failed");
        event = memory->read32(scratch + 96);
        auto open = [&](uint32_t access, bool regular = false) {
            const std::string path = "cache:\\" + folder + (regular ? "\\entry-a" : "");
            std::memcpy(base + scratch + 256, path.c_str(), path.size() + 1);
            memory->write32(scratch, 0xfffffffd); memory->write32(scratch + 4, scratch + 16);
            memory->write32(scratch + 8, 0x40);
            memory->write32(scratch + 16, uint32_t(path.size() << 16) | uint32_t(path.size() + 1));
            memory->write32(scratch + 20, scratch + 256);
            call = ctx;
            call.r3.u64 = scratch + 80; call.r4.u64 = access; call.r5.u64 = scratch;
            call.r6.u64 = ios; call.r7.u64 = 7; call.r8.u64 = regular ? 0x40 : 1;
            __imp__NtOpenFile(call, base);
            check(call.r3.u32 == 0, "Directory validation open failed");
            file = memory->read32(scratch + 80);
        };
        auto close = [&] { call.r3.u64 = file; __imp__NtClose(call, base); file = 0; };
        auto prepare = [&](bool notify, bool restart = false, uint32_t length = 256) {
            call = ctx;
            call.r3.u64 = event; __imp__NtClearEvent(call, base);
            std::memset(base + callback, 0, 12);
            std::memset(base + data, 0xa5, 4096);
            memory->write32(ios, 0xcccccccc); memory->write32(ios + 4, 0xcccccccc);
            memory->write32(pattern, (7u << 16) | 8u); memory->write32(pattern + 4, scratch + 512);
            std::strcpy(reinterpret_cast<char*>(base + scratch + 512), "entry-*");
            call = ctx;
            call.r3.u64 = file; call.r4.u64 = notify ? event : 0; call.r5.u64 = notify ? PPC_CODE_BASE | 1 : 0;
            call.r6.u64 = callback; call.r7.u64 = ios; call.r8.u64 = data; call.r9.u64 = length; call.r10.u64 = pattern;
            memory->write32(ctx.r1.u32 + 84, restart);
        };
        auto notification = [&] {
            PPCContext poll = ctx;
            const uint32_t timeout = scratch + 192;
            PPC_STORE_U64(timeout, 0);
            poll.r3.u64 = event; poll.r4.u64 = 0; poll.r5.u64 = 0; poll.r6.u64 = timeout;
            __imp__NtWaitForSingleObjectEx(poll, base);
            check(poll.r3.u32 == 0 || poll.r3.u32 == 0x102, "Directory completion poll failed");
            const bool premature = memory->read32(callback) != 0;
            SleepEx(0, TRUE);
            if (memory->read32(callback) == 1 &&
                (memory->read32(callback + 4) != memory->read32(ios) ||
                 memory->read32(callback + 8) != memory->read32(ios + 4))) return -1;
            return !premature && (poll.r3.u32 == 0) == (memory->read32(callback) == 1)
                ? int(memory->read32(callback)) : -1;
        };
        const char* labels[]{"output-noaccess", "output-cross-page", "output-readonly", "ios-noaccess",
                             "ios-cross-page", "pattern-descriptor", "pattern-bytes", "stack-restart", "output-wrap"};
        for (bool retained : {false, true}) for (unsigned kind = 0; kind < std::size(labels); ++kind) {
            DWORD previous;
            check(VirtualProtect(base + guard, 4096, PAGE_NOACCESS, &previous), "Directory guard setup failed");
            open(GENERIC_READ);
            if (retained) {
                prepare(false, false, 65);
                __imp__NtQueryDirectoryFile(call, base);
                check(call.r3.u32 == 0x80000005, "Directory retained-entry setup failed");
            }
            prepare(true, retained);
            switch (kind) {
                case 0: call.r8.u64 = guard; break;
                case 1: call.r8.u64 = guard - 68; break;
                case 2:
                    check(VirtualProtect(base + guard, 4096, PAGE_READONLY, &previous), "Directory read-only setup failed");
                    call.r8.u64 = guard; break;
                case 3: call.r7.u64 = guard; break;
                case 4: call.r7.u64 = guard - 4; break;
                case 5: call.r10.u64 = guard - 4; memory->write32(guard - 4, (7u << 16) | 8u); break;
                case 6: memory->write32(pattern + 4, guard - 4); std::memcpy(base + guard - 4, "entr", 4); break;
                case 7: call.r1.u64 = guard - 84; break;
                case 8: call.r8.u64 = 0xfffffff0u; break;
            }
            uint8_t before[4096]; std::memcpy(before, base + data, sizeof(before));
            const bool returned = directoryQueryWithoutHostFault(call, base);
            if (!returned) {
                report(labels[kind], retained, false);
                check(false, "Directory query raised a host access violation");
            }
            bool valid = returned && call.r3.u32 == 0xc0000005 && notification() == 0;
            // Drain even after an unexpected fault so the next case is independent.
            SleepEx(0, TRUE);
            valid = valid && std::memcmp(before, base + data, sizeof(before)) == 0;
            if (kind != 3 && kind != 4)
                valid = valid && memory->read32(ios) == 0xc0000005 && memory->read32(ios + 4) == 0;
            prepare(false);
            __imp__NtQueryDirectoryFile(call, base);
            valid = valid && call.r3.u32 == 0 && memory->read32(data + 60) == 7 &&
                std::memcmp(base + data + 64, "entry-a", 7) == 0;
            report(labels[kind], retained, valid);
            close();
        }
        struct NativeErrorCompletion { uint32_t status; bool completed; };
        auto nativeErrorCompletion = [&](uint32_t access, bool regular) {
            struct Handles {
                HANDLE file = INVALID_HANDLE_VALUE, event = nullptr;
                ~Handles() {
                    if (event) CloseHandle(event);
                    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                }
            } handles;
            const auto path = regular ? root / "entry-a" : root;
            handles.file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | (regular ? 0 : FILE_FLAG_BACKUP_SEMANTICS), nullptr);
            check(handles.file != INVALID_HANDLE_VALUE, "Native directory error oracle open failed");
            handles.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            check(handles.event != nullptr, "Native directory error oracle event allocation failed");
            using Query = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID,
                ULONG, FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
            auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile"));
            check(query != nullptr, "Native directory error oracle query unavailable");
            constexpr uint32_t untouchedStatus = 0xcccccccc;
            constexpr ULONG_PTR untouchedInformation = ~ULONG_PTR(0);
            IO_STATUS_BLOCK io{};
            auto resetIo = [&] { io.Status = NTSTATUS(untouchedStatus); io.Information = untouchedInformation; };
            resetIo();
            alignas(8) uint8_t bytes[1024]; std::memset(bytes, 0xa5, sizeof(bytes));
            wchar_t mask[] = L"entry-*";
            UNICODE_STRING filter{14, 14, mask};
            auto submit = [&](HANDLE completion) {
                return uint32_t(query(handles.file, completion, nullptr, nullptr, &io, bytes, sizeof(bytes),
                    FILE_INFORMATION_CLASS(1), TRUE, &filter, FALSE));
            };
            uint32_t status = submit(handles.event);
            const bool eventless = status == 0xc0000002;
            if (eventless) {
                resetIo();
                std::memset(bytes, 0xa5, sizeof(bytes));
                status = submit(nullptr);
            }
            if (status == 0x103) {
                check(WaitForSingleObject(eventless ? handles.file : handles.event, INFINITE) == WAIT_OBJECT_0,
                      "Native directory error oracle completion did not signal");
                status = uint32_t(io.Status);
                check(status != 0x103, "Native directory error oracle remained pending");
            }
            bool completed;
            if (eventless) {
                // For these two errors, an untouched IOSB proves rejection
                // before submission. A written terminal IOSB proves native
                // completion when the host does not support directory events.
                const bool untouched = uint32_t(io.Status) == untouchedStatus && io.Information == untouchedInformation;
                const bool recorded = uint32_t(io.Status) == status && io.Information == 0;
                check(untouched || recorded, "Native directory error oracle left an ambiguous completion status");
                completed = recorded;
            } else completed = WaitForSingleObject(handles.event, 0) == WAIT_OBJECT_0;
            if (completed)
                check(uint32_t(io.Status) == status && io.Information == 0,
                      "Native directory error oracle completion did not record its error");
            for (const auto byte : bytes) check(byte == 0xa5, "Native directory error oracle changed its output");
            std::printf("DirectoryNativeError[%s] status=%08X eventless=%u completed=%u\n",
                regular ? "regular-file" : "access-denial", status, unsigned(eventless), unsigned(completed));
            return NativeErrorCompletion{status, completed};
        };
        // Compare errors with an independent host submission oracle. Hosts can
        // reject a regular-file query before submission or complete its error;
        // neither the returned status nor NT_SUCCESS alone proves completion.
        for (bool regular : {false, true}) {
            const uint32_t access = regular ? GENERIC_READ : FILE_READ_ATTRIBUTES;
            open(access, regular);
            const auto native = nativeErrorCompletion(access, regular);
            prepare(true);
            __imp__NtQueryDirectoryFile(call, base);
            const uint32_t expected = regular ? 0xc000000d : 0xc0000022;
            check(native.status == expected, "Native directory error oracle returned an unexpected error");
            const int notifications = notification();
            const bool valid = call.r3.u32 == expected && memory->read32(ios) == expected &&
                memory->read32(ios + 4) == 0 && notifications == int(native.completed) &&
                std::all_of(base + data, base + data + 4096, [](uint8_t byte) { return byte == 0xa5; });
            report(regular ? "regular-file-error" : "unsubmitted-access-denial", false, valid);
            close();
        }
        // Accept a writable span across protection regions and retain normal
        // notifications for both native results and cached overflow retries.
        DWORD previous;
        check(VirtualProtect(base + guard, 4096, PAGE_EXECUTE_READWRITE, &previous), "Directory split buffer setup failed");
        open(GENERIC_READ);
        for (unsigned step = 0; step < 4; ++step) {
            prepare(true, false, step == 0 ? 65 : 256);
            call.r8.u64 = guard - 68;
            __imp__NtQueryDirectoryFile(call, base);
            const uint32_t expected = step == 0 ? 0x80000005 : step == 3 ? 0x80000006 : 0;
            bool valid = call.r3.u32 == expected && notification() == 1;
            if (step < 3) valid = valid && memory->read32(guard - 8) == 7 &&
                std::memcmp(base + guard - 4, step == 2 ? "entry-b" : "entry-a", step == 0 ? 1 : 7) == 0;
            report(step == 0 ? "overflow-completion" : step == 1 ? "cached-completion" : step == 2 ? "split-writable" : "end-completion", true, valid);
        }
        close();
    } catch (...) { cleanup(); throw; }
    cleanup();
    check(failures == 0, "Directory validation faulted, consumed entries, or fabricated completion notifications");
}
