#pragma once

static PPC_FUNC(fileCompletionProbe) {
    const uint32_t result = ctx.r3.u32;
    memory->write32(result, memory->read32(result) + 1);
    memory->write32(result + 4, memory->read32(ctx.r4.u32));
    memory->write32(result + 8, memory->read32(ctx.r4.u32 + 4));
}

static void testFileCompletion(PPCContext& ctx, uint32_t file, uint32_t fixture) {
    auto* base = memory->base();
    const uint32_t ios = fixture + 64, offset = fixture + 80;
    const uint32_t callback = fixture + 112, timeout = fixture + 128;
    const uint32_t data = fixture + 4096;
    ctx.r3.u64 = fixture + 100; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
    __imp__NtCreateEvent(ctx, base);
    check(ctx.r3.u32 == 0, "File completion event creation failed");
    const uint32_t event = memory->read32(fixture + 100);
    auto* original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = fileCompletionProbe;
    auto cleanup = [&] {
        SleepEx(0, TRUE);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
        ctx.r3.u64 = event; __imp__NtClose(ctx, base);
    };
    try {
        const auto size = std::filesystem::file_size(memory->gameDirectory() / "basefile.exe");
        const uint64_t eof = (size + 4095) & ~uint64_t(4095);
        struct ReadCase { uint64_t offset; uint32_t length, status, bytes; bool completes; };
        // Native NtReadFile completes EOF requests (including an APC), but
        // rejects unbuffered alignment errors before any I/O is submitted.
        const ReadCase cases[]{{0, 512, 0, 512, true}, {eof, 512, 0xc0000011, 0, true},
                               {0, 1, 0xc000000d, 0, false}, {1, 512, 0xc000000d, 0, false}};
        for (const auto& test : cases) {
            ctx.r3.u64 = event; __imp__NtClearEvent(ctx, base);
            check(ctx.r3.u32 == 0, "File completion event reset failed");
            memset(base + callback, 0, 12); memset(base + data, 0xA5, 512);
            PPC_STORE_U64(timeout, 0); PPC_STORE_U64(offset, test.offset);
            ctx.r3.u64 = file; ctx.r4.u64 = event; ctx.r5.u64 = PPC_CODE_BASE | 1;
            ctx.r6.u64 = callback; ctx.r7.u64 = ios; ctx.r8.u64 = data;
            ctx.r9.u64 = test.length; ctx.r10.u64 = offset;
            __imp__NtReadFile(ctx, base);
            check(ctx.r3.u32 == test.status && memory->read32(ios) == test.status &&
                      memory->read32(ios + 4) == test.bytes,
                  "File completion status or byte count differs from native read");
            check(memory->read32(callback) == 0, "File APC ran outside an alertable wait");
            ctx.r3.u64 = event; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = timeout;
            __imp__NtWaitForSingleObjectEx(ctx, base);
            const bool signaled = ctx.r3.u32 == 0;
            check(signaled || ctx.r3.u32 == 0x102, "File completion event wait failed");
            SleepEx(0, TRUE);
            const uint32_t calls = memory->read32(callback);
            std::printf("FileCompletion[offset=%llu length=%u] event=%u APCs=%u expected=%u\n",
                        test.offset, test.length, unsigned(signaled), calls, unsigned(test.completes));
            check(signaled == test.completes && calls == unsigned(test.completes),
                  "Rejected native read fabricated an event/APC completion");
            if (test.completes)
                check(memory->read32(callback + 4) == test.status &&
                          memory->read32(callback + 8) == test.bytes,
                      "File APC lost the completed read status");
            if (test.bytes == 0)
                check(std::all_of(base + data, base + data + 512, [](uint8_t b) { return b == 0xA5; }),
                      "Rejected or EOF read changed the destination");
        }
    } catch (...) { cleanup(); throw; }
    cleanup();
    puts("File completion preserves successful/EOF callbacks and rejects unsubmitted read notifications.");
}

static void testFileEventTyping(PPCContext& ctx, uint32_t file, uint32_t fixture) {
    // `file` is the unbuffered read-only basefile handle opened by testFileBuffers.
    // 512-byte offset-0 reads succeed on it, so any rejection below is the event
    // guard rather than alignment. Real-event and no-event success stay covered by
    // testFileCompletion and testFileBuffers' read() helper; this test proves the
    // impostor rejections, their lack of side effects, and that a duplicated event
    // remains fully functional.
    auto* base = memory->base();
    const uint32_t ios = fixture + 64, offset = fixture + 80;
    const uint32_t callback = fixture + 112, timeout = fixture + 128;
    const uint32_t data = fixture + 4096;
    const uint32_t semOut = fixture + 100, eventOut = fixture + 96, dupOut = fixture + 92;
    const uint32_t outCell = fixture + 88, posCell = fixture + 200;
    const uint32_t pattern = fixture + 160, patternBytes = fixture + 512;
    const std::string folder = "file-event-typing-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64());
    const auto root = memory->gameDirectory().parent_path() / "build_native/run/cache" / folder;
    uint32_t event = 0, dupEvent = 0, semaphore = 0, writeFile = 0, dirFile = 0;
    auto* original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = fileCompletionProbe;
    auto cleanup = [&] {
        SleepEx(0, TRUE);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
        if (writeFile) { ctx.r3.u64 = writeFile; __imp__NtClose(ctx, base); writeFile = 0; }
        if (dirFile) { ctx.r3.u64 = dirFile; __imp__NtClose(ctx, base); dirFile = 0; }
        if (semaphore) { ctx.r3.u64 = semaphore; __imp__NtClose(ctx, base); semaphore = 0; }
        if (dupEvent) { ctx.r3.u64 = dupEvent; __imp__NtClose(ctx, base); dupEvent = 0; }
        if (event) { ctx.r3.u64 = event; __imp__NtClose(ctx, base); event = 0; }
        std::error_code ec;
        std::filesystem::remove(root / "writable.bin", ec);
        std::filesystem::remove(root / "entry-a", ec);
        std::filesystem::remove(root / "entry-b", ec);
        std::filesystem::remove(root, ec);
    };
    auto openGuestFile = [&](const std::string& guestPath, uint32_t access, uint32_t& out) {
        std::memcpy(base + fixture + 256, guestPath.c_str(), guestPath.size() + 1);
        memory->write32(fixture, 0xfffffffd);
        memory->write32(fixture + 4, fixture + 16);
        memory->write32(fixture + 8, 0x40);
        memory->write32(fixture + 16, uint32_t(guestPath.size() << 16) | uint32_t(guestPath.size() + 1));
        memory->write32(fixture + 20, fixture + 256);
        ctx.r3.u64 = outCell; ctx.r4.u64 = access; ctx.r5.u64 = fixture;
        ctx.r6.u64 = ios; ctx.r7.u64 = 0; ctx.r8.u64 = 0;
        ctx.r9.u64 = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE; ctx.r10.u64 = 1;
        memory->write32(ctx.r1.u32 + 84, 0x60);
        __imp__NtCreateFile(ctx, base);
        if (ctx.r3.u32 != 0) return false;
        out = memory->read32(outCell);
        return true;
    };
    auto queryPosition = [&](uint32_t handle, uint64_t& position) {
        ctx.r3.u64 = handle; ctx.r4.u64 = ios; ctx.r5.u64 = posCell; ctx.r6.u64 = 8; ctx.r7.u64 = 14;
        __imp__NtQueryInformationFile(ctx, base);
        if (ctx.r3.u32 != 0) return false;
        position = PPC_LOAD_U64(posCell);
        return true;
    };
    auto readHost = [&](const char* name, char* bytes, uint32_t count) {
        std::ifstream stream(root / name, std::ios::binary);
        if (!stream) return false;
        stream.read(bytes, count);
        return stream.gcount() == int(count);
    };
    try {
        ctx.r3.u64 = eventOut; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        __imp__NtCreateEvent(ctx, base);
        check(ctx.r3.u32 == 0, "File event-typing event creation failed");
        event = memory->read32(eventOut);
        ctx.r3.u64 = event; ctx.r4.u64 = dupOut; ctx.r5.u64 = 0;
        __imp__NtDuplicateObject(ctx, base);
        check(ctx.r3.u32 == 0, "File event-typing event duplication failed");
        dupEvent = memory->read32(dupOut);
        ctx.r3.u64 = semOut; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 8;
        __imp__NtCreateSemaphore(ctx, base);
        check(ctx.r3.u32 == 0, "File event-typing semaphore creation failed");
        semaphore = memory->read32(semOut);
        check(std::filesystem::create_directories(root), "File event-typing fixture already exists");
        {
            std::ofstream stream(root / "writable.bin", std::ios::binary);
            const std::string filler(512, 0x43);
            stream.write(filler.data(), 512);
            check(bool(stream), "File event-typing writable entry creation failed");
            for (const auto* name : {"entry-a", "entry-b"}) {
                std::ofstream entry(root / name, std::ios::binary);
                entry << name;
                check(bool(entry), "File event-typing directory entry creation failed");
            }
        }
        check(openGuestFile("cache:\\" + folder + "\\writable.bin",
                            GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, writeFile),
              "File event-typing writable open failed");
        {
            const std::string dirPath = "cache:\\" + folder;
            std::memcpy(base + fixture + 256, dirPath.c_str(), dirPath.size() + 1);
            memory->write32(fixture, 0xfffffffd);
            memory->write32(fixture + 4, fixture + 16);
            memory->write32(fixture + 8, 0x40);
            memory->write32(fixture + 16, uint32_t(dirPath.size() << 16) | uint32_t(dirPath.size() + 1));
            memory->write32(fixture + 20, fixture + 256);
            ctx.r3.u64 = outCell; ctx.r4.u64 = GENERIC_READ | SYNCHRONIZE; ctx.r5.u64 = fixture;
            ctx.r6.u64 = ios; ctx.r7.u64 = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            ctx.r8.u64 = 0x21;
            __imp__NtOpenFile(ctx, base);
            check(ctx.r3.u32 == 0, "File event-typing directory open failed");
            dirFile = memory->read32(outCell);
        }
        // Contrast: the duplicated event performs a full 512-byte offset-0 read
        // with APC delivery, proving the size/offset below would succeed absent
        // the guard and that duplication preserves event behavior.
        ctx.r3.u64 = dupEvent; __imp__NtClearEvent(ctx, base);
        check(ctx.r3.u32 == 0, "File event-typing event reset failed");
        memset(base + callback, 0, 12); memset(base + data, 0xA5, 512);
        PPC_STORE_U64(offset, 0); PPC_STORE_U64(timeout, 0);
        ctx.r3.u64 = file; ctx.r4.u64 = dupEvent; ctx.r5.u64 = PPC_CODE_BASE | 1;
        ctx.r6.u64 = callback; ctx.r7.u64 = ios; ctx.r8.u64 = data;
        ctx.r9.u64 = 512; ctx.r10.u64 = offset;
        __imp__NtReadFile(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(ios) == 0 && memory->read32(ios + 4) == 512,
              "Duplicated-event 512-byte read failed");
        check(memory->read32(callback) == 0, "File APC ran outside an alertable wait");
        ctx.r3.u64 = dupEvent; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = timeout;
        __imp__NtWaitForSingleObjectEx(ctx, base);
        check(ctx.r3.u32 == 0, "Duplicated event was not signaled by the completed read");
        SleepEx(0, TRUE);
        check(memory->read32(callback) == 1 && memory->read32(callback + 4) == 0 &&
                  memory->read32(callback + 8) == 512,
              "Duplicated event lost the completed read APC");
        // Impostor reads: same size/offset, probe APC queued, alertable delivery
        // must show no callback alongside the invalid-handle rejection.
        const uint32_t readImpostors[] = {semaphore, file};
        for (const uint32_t impostor : readImpostors) {
            memset(base + callback, 0, 12); memset(base + data, 0xA5, 512);
            memory->write32(ios, 0xa5a5a5a5); memory->write32(ios + 4, 0xa5a5a5a5);
            PPC_STORE_U64(offset, 0);
            ctx.r3.u64 = file; ctx.r4.u64 = impostor; ctx.r5.u64 = PPC_CODE_BASE | 1;
            ctx.r6.u64 = callback; ctx.r7.u64 = ios; ctx.r8.u64 = data;
            ctx.r9.u64 = 512; ctx.r10.u64 = offset;
            __imp__NtReadFile(ctx, base);
            check(ctx.r3.u32 == 0xc0000008 && memory->read32(ios) == 0xc0000008 &&
                      memory->read32(ios + 4) == 0,
                  "Read with a non-event completion object was not rejected before I/O");
            check(std::all_of(base + data, base + data + 512, [](uint8_t b) { return b == 0xA5; }),
                  "Rejected read with a non-event completion object changed the destination");
            check(memory->read32(callback) == 0, "Rejected read queued an APC before delivery");
            SleepEx(0, TRUE);
            check(memory->read32(callback) == 0, "Rejected read delivered an APC after the wait");
        }
        // Impostor writes against a genuinely writable file: position and bytes
        // must be unchanged, then a valid no-event write proves the handle could
        // have been clobbered absent the guard.
        uint64_t posBefore = 0;
        check(queryPosition(writeFile, posBefore) && posBefore == 0,
              "Writable fixture did not start at position 0");
        memset(base + data, 0x57, 512);
        const uint32_t writeImpostors[] = {semaphore, dirFile};
        for (const uint32_t impostor : writeImpostors) {
            memset(base + callback, 0, 12);
            memory->write32(ios, 0xa5a5a5a5); memory->write32(ios + 4, 0xa5a5a5a5);
            PPC_STORE_U64(offset, 0);
            ctx.r3.u64 = writeFile; ctx.r4.u64 = impostor; ctx.r5.u64 = PPC_CODE_BASE | 1;
            ctx.r6.u64 = callback; ctx.r7.u64 = ios; ctx.r8.u64 = data;
            ctx.r9.u64 = 512; ctx.r10.u64 = offset;
            __imp__NtWriteFile(ctx, base);
            check(ctx.r3.u32 == 0xc0000008 && memory->read32(ios) == 0xc0000008 &&
                      memory->read32(ios + 4) == 0,
                  "Write with a non-event completion object was not rejected before I/O");
            check(memory->read32(callback) == 0, "Rejected write queued an APC before delivery");
            SleepEx(0, TRUE);
            check(memory->read32(callback) == 0, "Rejected write delivered an APC after the wait");
        }
        {
            uint64_t posAfter = 0;
            check(queryPosition(writeFile, posAfter) && posAfter == posBefore,
                  "Rejected write moved the writable file position");
            char hostBytes[512];
            check(readHost("writable.bin", hostBytes, sizeof(hostBytes)) &&
                      std::all_of(hostBytes, hostBytes + sizeof(hostBytes),
                                  [](char b) { return b == 0x43; }),
                  "Rejected write changed the writable file bytes");
        }
        memory->write32(ios, 0xa5a5a5a5); memory->write32(ios + 4, 0xa5a5a5a5);
        PPC_STORE_U64(offset, 0);
        ctx.r3.u64 = writeFile; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        ctx.r7.u64 = ios; ctx.r8.u64 = data; ctx.r9.u64 = 512; ctx.r10.u64 = offset;
        __imp__NtWriteFile(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(ios) == 0 && memory->read32(ios + 4) == 512,
              "Valid no-event write to the writable fixture failed");
        {
            char hostBytes[512];
            check(readHost("writable.bin", hostBytes, sizeof(hostBytes)) &&
                      std::all_of(hostBytes, hostBytes + sizeof(hostBytes),
                                  [](char b) { return b == 0x57; }),
                  "Valid write did not reach the writable fixture (writability unproven)");
        }
        // QueryDirectoryFile: a rejected restart-0 request must not consume the
        // first entry. Valid restart-1 yields entry-a; after the rejection, a
        // valid restart-0 must yield entry-b rather than EOF.
        auto dirQuery = [&](uint32_t completion, bool useApc, bool restart, uint32_t length) {
            memory->write32(pattern, (7u << 16) | 8u);
            memory->write32(pattern + 4, patternBytes);
            std::strcpy(reinterpret_cast<char*>(base + patternBytes), "entry-*");
            std::memset(base + data, 0xa5, 256);
            memory->write32(ios, 0xcccccccc); memory->write32(ios + 4, 0xcccccccc);
            memset(base + callback, 0, 12);
            ctx.r3.u64 = dirFile; ctx.r4.u64 = completion;
            ctx.r5.u64 = useApc ? PPC_CODE_BASE | 1 : 0; ctx.r6.u64 = callback;
            ctx.r7.u64 = ios; ctx.r8.u64 = data; ctx.r9.u64 = length; ctx.r10.u64 = pattern;
            memory->write32(ctx.r1.u32 + 84, restart ? 1 : 0);
            __imp__NtQueryDirectoryFile(ctx, base);
        };
        dirQuery(0, false, true, 256);
        check(ctx.r3.u32 == 0 && memory->read32(data + 60) == 7 &&
                  std::memcmp(base + data + 64, "entry-a", 7) == 0,
              "Directory restart did not yield the first entry");
        dirQuery(semaphore, true, false, 256);
        check(ctx.r3.u32 == 0xc0000008 && memory->read32(ios) == 0xc0000008 &&
                  memory->read32(ios + 4) == 0,
              "Directory query with a non-event object was not rejected before I/O");
        check(std::all_of(base + data, base + data + 256, [](uint8_t b) { return b == 0xA5; }),
              "Rejected directory query changed the output buffer");
        check(memory->read32(callback) == 0, "Rejected directory query queued an APC before delivery");
        SleepEx(0, TRUE);
        check(memory->read32(callback) == 0, "Rejected directory query delivered an APC after the wait");
        dirQuery(0, false, false, 256);
        check(ctx.r3.u32 == 0 && memory->read32(data + 60) == 7 &&
                  std::memcmp(base + data + 64, "entry-b", 7) == 0,
              "Rejected directory query consumed the first entry");
    } catch (...) { cleanup(); throw; }
    cleanup();
    std::printf("FileEventTypingReads=2 FileEventTypingWrites=2 FileEventTypingDirQueries=1 non-event completions rejected without I/O\n");
    puts("File completion objects reject non-event handles before native I/O.");
}
