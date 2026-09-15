#pragma once

enum class FilePathOperation { Open, Create, Attributes };

static bool filePathWithoutHostFault(PPCContext& ctx, uint8_t* base, FilePathOperation operation) {
    __try {
        switch (operation) {
            case FilePathOperation::Open: __imp__NtOpenFile(ctx, base); break;
            case FilePathOperation::Create: __imp__NtCreateFile(ctx, base); break;
            case FilePathOperation::Attributes: __imp__NtQueryFullAttributesFile(ctx, base); break;
        }
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

static void testFilePathBuffers(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t scratch = memory->allocate(4 * 4096);
    check(scratch != 0, "File path fixture allocation failed");
    const uint32_t ios = scratch + 64, allocation = scratch + 80, handleOut = scratch + 96;
    const uint32_t frame = scratch + 512, data = scratch + 4096, guard = scratch + 8192;
    constexpr char path[] = "game:\\basefile.exe";
    constexpr uint32_t pathLength = sizeof(path) - 1, sentinel = 0xa5a5a5a5;
    enum Field { Attributes, String, Path, Output, IoStatus, Allocation, Stack };
    const char* fields[]{"attributes", "string", "path", "output", "ios", "allocation", "stack"};
    const char* modes[]{"noaccess", "split", "wrap", "guard", "readonly"};
    const char* operations[]{"open", "create", "query"};
    unsigned failures = 0, rejectedCases = 0;
    PPCContext call{};
    auto protect = [&](DWORD protection) {
        DWORD previous;
        check(VirtualProtect(base + guard, 4096, protection, &previous) != 0,
              "File path protection setup failed");
    };
    auto prepare = [&](FilePathOperation operation) {
        protect(PAGE_READWRITE);
        std::memset(base + scratch, 0xa5, 4 * 4096);
        memory->write32(scratch, 0xfffffffd);
        memory->write32(scratch + 4, scratch + 16);
        memory->write32(scratch + 8, 0x40);
        memory->write32(scratch + 16, (pathLength << 16) | (pathLength + 1));
        memory->write32(scratch + 20, scratch + 256);
        std::memcpy(base + scratch + 256, path, sizeof(path));
        PPC_STORE_U64(allocation, 0);
        memory->write32(frame + 84, 0x60);
        call = ctx;
        call.r1.u64 = frame;
        if (operation == FilePathOperation::Attributes) {
            call.r3.u64 = scratch; call.r4.u64 = data;
        } else {
            call.r3.u64 = handleOut; call.r4.u64 = GENERIC_READ | SYNCHRONIZE;
            call.r5.u64 = scratch; call.r6.u64 = ios;
            if (operation == FilePathOperation::Open) {
                call.r7.u64 = FILE_SHARE_READ; call.r8.u64 = 0x60;
            } else {
                call.r7.u64 = allocation; call.r8.u64 = 0;
                call.r9.u64 = FILE_SHARE_READ; call.r10.u64 = 1;
            }
        }
    };
    auto fieldSize = [&](FilePathOperation operation, Field field) -> uint32_t {
        switch (field) {
            case Attributes: return 12;
            case String: case IoStatus: case Allocation: return 8;
            case Path: return pathLength;
            case Output: return operation == FilePathOperation::Attributes ? 56 : 4;
            case Stack: return 4;
        }
        return 0;
    };
    auto relocate = [&](FilePathOperation operation, Field field, uint32_t target, bool copy) {
        const uint32_t sources[]{scratch, scratch + 16, scratch + 256,
            operation == FilePathOperation::Attributes ? data : handleOut, ios, allocation, frame + 84};
        if (copy) std::memcpy(base + target, base + sources[field], fieldSize(operation, field));
        switch (field) {
            case Attributes:
                if (operation == FilePathOperation::Attributes) call.r3.u64 = target;
                else call.r5.u64 = target;
                break;
            case String: memory->write32(scratch + 4, target); break;
            case Path: memory->write32(scratch + 20, target); break;
            case Output:
                if (operation == FilePathOperation::Attributes) call.r4.u64 = target;
                else call.r3.u64 = target;
                break;
            case IoStatus: call.r6.u64 = target; break;
            case Allocation: call.r7.u64 = target; break;
            case Stack: call.r1.u64 = target - 84; break;
        }
    };
    auto close = [&](uint32_t handle) {
        PPCContext cleanup = ctx; cleanup.r3.u64 = handle;
        __imp__NtClose(cleanup, base);
        check(cleanup.r3.u32 == 0, "File path fixture close failed");
    };
    try {
        for (auto operation : {FilePathOperation::Open, FilePathOperation::Create, FilePathOperation::Attributes}) {
            for (int f = Attributes; f <= Stack; ++f) {
                const auto field = static_cast<Field>(f);
                if (operation == FilePathOperation::Attributes && field > Output) continue;
                if (operation == FilePathOperation::Open && field > IoStatus) continue;
                const uint32_t size = fieldSize(operation, field);
                const bool output = field == Output || field == IoStatus;
                for (unsigned mode = 0; mode < (output ? 5u : 4u); ++mode) {
                    prepare(operation);
                    const uint32_t target = mode == 1 ? guard - size / 2 :
                                            mode == 2 ? UINT32_MAX - size / 2 + 1 : guard;
                    relocate(operation, field, target, mode != 2);
                    if (field == Stack && mode == 2) call.r1.u64 = 0xfffffffcu; // r1 + 84 wraps.
                    const std::vector<uint8_t> original(base + data, base + data + 8192);
                    protect(mode == 3 ? PAGE_READWRITE | PAGE_GUARD : mode == 4 ? PAGE_READONLY : PAGE_NOACCESS);
                    DWORD handlesBefore = 0, handlesAfter = 0;
                    check(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != 0, "Handle count query failed");
                    const bool returned = filePathWithoutHostFault(call, base, operation);
                    protect(PAGE_READWRITE);
                    check(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != 0, "Handle count query failed");
                    const bool preserved = std::memcmp(base + data, original.data(), original.size()) == 0 &&
                                           memory->read32(handleOut) == sentinel;
                    const bool completion = operation == FilePathOperation::Attributes ||
                        (field == IoStatus ? memory->read32(ios) == sentinel && memory->read32(ios + 4) == sentinel :
                         memory->read32(ios) == 0xc0000005 && memory->read32(ios + 4) == 0);
                    const bool passed = returned && call.r3.u32 == 0xc0000005 && preserved && completion &&
                                        handlesBefore == handlesAfter;
                    std::printf("FilePath[%s %s %s] %s (host-fault=%d, handles=%ld)\n",
                        operations[static_cast<unsigned>(operation)], fields[f], modes[mode], passed ? "passed" : "FAILED",
                        !returned, long(handlesAfter) - long(handlesBefore));
                    if (!passed) ++failures;
                    ++rejectedCases;
                    // A broken implementation can publish a handle before faulting on the IOS.
                    if (operation != FilePathOperation::Attributes && memory->read32(handleOut) != sentinel)
                        close(memory->read32(handleOut));
                }
                // Legal spans can cross VirtualQuery regions. Counted paths need
                // only Length readable bytes, even when MaximumLength is larger.
                prepare(operation);
                const uint32_t target = field == Path ? guard - pathLength : guard - size / 2;
                relocate(operation, field, target, true);
                protect(field == Path ? PAGE_NOACCESS : output ? PAGE_EXECUTE_READWRITE : PAGE_READONLY);
                const bool returned = filePathWithoutHostFault(call, base, operation);
                protect(PAGE_READWRITE);
                check(returned && call.r3.u32 == 0, "Valid split file path buffer was rejected");
                if (operation == FilePathOperation::Attributes) {
                    const uint32_t record = field == Output ? target : data;
                    check(PPC_LOAD_U64(record + 40) == std::filesystem::file_size(memory->gameDirectory() / "basefile.exe"),
                          "File attributes lost the original file size");
                } else {
                    check(memory->read32(field == IoStatus ? target : ios) == 0, "Valid file open lost completion status");
                    close(memory->read32(field == Output ? target : handleOut));
                }
            }
        }
        check(failures == 0, "File path buffers caused host faults, partial writes, or leaked handles");
        std::printf("File path buffer validation passed: %u rejected spans and 16 valid boundary cases.\n", rejectedCases);
    } catch (...) {
        protect(PAGE_READWRITE);
        memory->release(scratch);
        throw;
    }
    check(memory->release(scratch), "File path fixture cleanup failed");
}
