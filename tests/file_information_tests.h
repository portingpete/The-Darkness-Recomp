#pragma once
#include <cstdlib>

static bool fileInformationWithoutHostFault(PPCContext& ctx, uint8_t* base, bool set) {
    __try {
        if (set) __imp__NtSetInformationFile(ctx, base);
        else __imp__NtQueryInformationFile(ctx, base);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

static void testFileInformation(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t scratch = memory->allocate(4 * 4096);
    check(scratch != 0, "File information fixture allocation failed");
    const uint32_t ios = scratch + 64, value = scratch + 80;
    const uint32_t data = scratch + 4096, guard = scratch + 8192;
    const char* path = "game:\\basefile.exe";
    std::strcpy(reinterpret_cast<char*>(base + scratch + 256), path);
    memory->write32(scratch, 0xfffffffd); memory->write32(scratch + 4, scratch + 16);
    memory->write32(scratch + 8, 0x40);
    memory->write32(scratch + 16, uint32_t(strlen(path) << 16) | uint32_t(strlen(path) + 1));
    memory->write32(scratch + 20, scratch + 256);
    PPCContext call = ctx;
    call.r3.u64 = scratch + 96; call.r4.u64 = GENERIC_READ | SYNCHRONIZE;
    call.r5.u64 = scratch; call.r6.u64 = ios; call.r7.u64 = FILE_SHARE_READ;
    call.r8.u64 = 0x60; // Buffered synchronous non-directory file: byte-granular seek.
    __imp__NtOpenFile(call, base);
    check(call.r3.u32 == 0, "File information fixture open failed");
    const uint32_t file = memory->read32(scratch + 96);
    auto cleanup = [&] {
        PPCContext close = ctx; close.r3.u64 = file; __imp__NtClose(close, base);
        memory->release(scratch);
    };
    unsigned failures = 0;
    auto report = [&](const char* op, const char* label, bool valid) {
        std::printf("FileInformation[%s %s] %s\n", op, label, valid ? "passed" : "FAILED");
        if (!valid) ++failures;
    };
    auto prepare = [&](uint32_t output = 0, uint32_t statusBlock = 0, uint32_t kind = 14, uint32_t length = 8) {
        memory->write32(ios, 0xcccccccc); memory->write32(ios + 4, 0xcccccccc);
        call = ctx;
        call.r3.u64 = file; call.r4.u64 = statusBlock ? statusBlock : ios;
        call.r5.u64 = output ? output : value; call.r6.u64 = length; call.r7.u64 = kind;
    };
    auto seek = [&](uint64_t position) {
        PPC_STORE_U64(value, position);
        prepare(); __imp__NtSetInformationFile(call, base);
        check(call.r3.u32 == 0, "Valid file position update failed");
    };
    auto position = [&]() {
        prepare(); __imp__NtQueryInformationFile(call, base);
        check(call.r3.u32 == 0 && memory->read32(ios + 4) == 8, "Valid file position query failed");
        return PPC_LOAD_U64(value);
    };
    auto protect = [&](DWORD protection) {
        DWORD previous;
        check(VirtualProtect(base + guard, 4096, protection, &previous), "File information protection setup failed");
    };
    try {
        seek(16);
        // Queries use a host buffer. A rejected guest output must not receive a
        // partial converted record, even when the I/O status block is invalid.
        const char* queryLabels[]{"output-noaccess", "output-cross-page", "output-readonly", "ios-noaccess",
                                  "ios-cross-page", "output-wrap", "oversized-output-cross-page",
                                  "ios-readonly", "ios-wrap", "output-guard"};
        for (unsigned test = 0; test < std::size(queryLabels); ++test) {
            protect(PAGE_READWRITE);
            std::memset(base + data, 0xa5, 8192);
            protect(test == 2 || test == 7 ? PAGE_READONLY : test == 9 ? PAGE_READWRITE | PAGE_GUARD : PAGE_NOACCESS);
            prepare(data);
            switch (test) {
                case 0: case 2: call.r5.u64 = guard; break;
                case 1: call.r5.u64 = guard - 4; break;
                case 3: call.r4.u64 = guard; break;
                case 4: call.r4.u64 = guard - 4; break;
                case 5: call.r5.u64 = 0xfffffffcu; break;
                case 6: call.r6.u64 = 4104; break;
                case 7: call.r4.u64 = guard; break;
                case 8: call.r4.u64 = 0xfffffffcu; break;
                case 9: call.r5.u64 = guard; break;
            }
            const bool returned = fileInformationWithoutHostFault(call, base, false);
            const bool rejected = returned && call.r3.u32 == 0xc0000005;
            const bool completion = test == 3 || test == 4 || test == 7 || test == 8 ||
                (memory->read32(ios) == 0xc0000005 && memory->read32(ios + 4) == 0);
            bool preserved = true;
            for (uint32_t i = 0; i < 4096; ++i) preserved = preserved && base[data + i] == 0xa5;
            report("query", queryLabels[test], rejected && completion && preserved && position() == 16);
        }
        const char* setLabels[]{"ios-noaccess", "ios-cross-page", "ios-readonly", "input-noaccess",
                                "input-cross-page", "input-wrap", "ios-wrap"};
        for (unsigned test = 0; test < std::size(setLabels); ++test) {
            protect(PAGE_READWRITE);
            std::memset(base + data, 0xa5, 8192);
            seek(16);
            PPC_STORE_U64(value, 80);
            protect(test == 2 ? PAGE_READONLY : PAGE_NOACCESS);
            prepare();
            switch (test) {
                case 0: case 2: call.r4.u64 = guard; break;
                case 1: call.r4.u64 = guard - 4; break;
                case 3: call.r5.u64 = guard; break;
                case 4: call.r5.u64 = guard - 4; break;
                case 5: call.r5.u64 = 0xfffffffcu; break;
                case 6: call.r4.u64 = 0xfffffffcu; break;
            }
            const bool returned = fileInformationWithoutHostFault(call, base, true);
            const bool rejected = returned && call.r3.u32 == 0xc0000005;
            const bool completion = test < 3 || test == 6 ||
                (memory->read32(ios) == 0xc0000005 && memory->read32(ios + 4) == 0);
            bool preserved = true;
            for (uint32_t i = 0; i < 4096; ++i) preserved = preserved && base[data + i] == 0xa5;
            const uint64_t after = position(); // Query does not take the seek/read mutex.
            report("set", setLabels[test], rejected && completion && preserved && after == 16);
            if (!returned) {
                // /EHsc does not unwind the seek's lock_guard on SEH. Stop this
                // failing test process rather than destroy/reenter a held mutex.
                std::fprintf(stderr, "FileInformation: host fault during seek; position=%llu expected=16\n",
                             static_cast<unsigned long long>(after));
                std::fflush(nullptr);
                std::_Exit(1);
            }
            seek(24); // A rejected request must leave the shared file lock usable.
            check(position() == 24, "Rejected seek prevented a later valid seek");
        }
        // Different readable/writable VirtualQuery regions are legal. Validate
        // read-only input, all translated query structures, and their byte counts.
        protect(PAGE_READWRITE);
        PPC_STORE_U64(guard, 37);
        protect(PAGE_READONLY);
        prepare(guard); __imp__NtSetInformationFile(call, base);
        report("set", "readonly-input", call.r3.u32 == 0 && position() == 37);
        protect(PAGE_READWRITE);
        PPC_STORE_U64(guard - 4, 53);
        protect(PAGE_READONLY);
        prepare(guard - 4); __imp__NtSetInformationFile(call, base);
        report("set", "split-readable-input", call.r3.u32 == 0 && position() == 53);
        protect(PAGE_EXECUTE_READWRITE);
        const uint32_t classes[]{4, 5, 6, 14, 16, 17, 34};
        const uint32_t sizes[]{40, 24, 8, 8, 4, 4, 56};
        for (unsigned i = 0; i < std::size(classes); ++i) {
            const uint32_t out = guard - 2;
            prepare(out, 0, classes[i], sizes[i]); __imp__NtQueryInformationFile(call, base);
            check(call.r3.u32 == 0 && memory->read32(ios + 4) == sizes[i], "Valid split file information query failed");
            if (classes[i] == 14) check(PPC_LOAD_U64(out) == 53, "File position endian conversion changed");
            if (classes[i] == 5) check(PPC_LOAD_U64(out + 8) == std::filesystem::file_size(memory->gameDirectory() / "basefile.exe"),
                                       "File standard information length changed");
        }
        report("query", "all-classes-split-writable", true);
        std::memset(base + data, 0xa5, 8192);
        prepare(data, guard - 4, 14, 4104); __imp__NtQueryInformationFile(call, base);
        check(call.r3.u32 == 0 && memory->read32(guard - 4) == 0 && memory->read32(guard) == 8 &&
              PPC_LOAD_U64(data) == 53 && base[data + 8] == 0xa5 && base[data + 4104] == 0xa5,
              "Valid oversized query or split-writable IOS changed trailing bytes");
        report("query", "oversized-and-split-ios", true);
        prepare(data); call.r4.u64 = 0; __imp__NtQueryInformationFile(call, base);
        check(call.r3.u32 == 0 && PPC_LOAD_U64(data) == 53, "Null-IOS query compatibility changed");
        PPC_STORE_U64(value, 53);
        prepare(); call.r4.u64 = 0; __imp__NtSetInformationFile(call, base);
        report("set", "null-ios-compatibility", call.r3.u32 == 0 && position() == 53);
        // A valid implicit-offset read must observe the surviving seek state.
        call = ctx; call.r3.u64 = file; call.r4.u64 = 0; call.r5.u64 = 0; call.r6.u64 = 0;
        call.r7.u64 = ios; call.r8.u64 = data; call.r9.u64 = 16; call.r10.u64 = 0;
        __imp__NtReadFile(call, base);
        std::ifstream source(memory->gameDirectory() / "basefile.exe", std::ios::binary);
        char expected[16]; source.seekg(53); source.read(expected, sizeof(expected));
        report("read", "implicit-offset-after-seek", call.r3.u32 == 0 && std::memcmp(base + data, expected, 16) == 0 && position() == 69);
    } catch (...) { cleanup(); throw; }
    cleanup();
    check(failures == 0, "File information span/position contract failed");
}
