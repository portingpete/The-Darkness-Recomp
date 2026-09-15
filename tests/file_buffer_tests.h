#pragma once
#include "file_completion_tests.h"
#include "directory_retry_tests.h"
#include "directory_validation_tests.h"
#include "file_information_tests.h"
#include "file_path_buffer_tests.h"

static bool readFileWithoutHostFault(PPCContext& ctx, uint8_t* base) {
    __try { __imp__NtReadFile(ctx, base); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
static void testFileBuffers(PPCContext& ctx) {
    testFilePathBuffers(ctx);
    testFileInformation(ctx);
    auto* base = memory->base();
    const uint32_t fixture = memory->allocate(5 * 4096);
    check(fixture != 0, "File buffer fixture allocation failed");
    const uint32_t ios = fixture + 64, offset = fixture + 80;
    const uint32_t data = fixture + 4096, guarded = data + 4096;
    const char* path = "game:\\basefile.exe";
    strcpy_s(reinterpret_cast<char*>(base + fixture + 256), 128, path);
    memory->write32(fixture, 0xfffffffd);
    memory->write32(fixture + 4, fixture + 16);
    memory->write32(fixture + 8, 0x40);
    memory->write32(fixture + 16, uint32_t(strlen(path) << 16) | uint32_t(strlen(path) + 1));
    memory->write32(fixture + 20, fixture + 256);
    ctx.r3.u64 = fixture + 96; ctx.r4.u64 = GENERIC_READ | SYNCHRONIZE;
    ctx.r5.u64 = fixture; ctx.r6.u64 = ios; ctx.r7.u64 = FILE_SHARE_READ;
    ctx.r8.u64 = 0x68; // Non-directory, synchronous, no intermediate buffering.
    __imp__NtOpenFile(ctx, base);
    check(ctx.r3.u32 == 0, "Unbuffered file fixture open failed");
    const uint32_t file = memory->read32(fixture + 96);
    testFileCompletion(ctx, file, fixture);
    testFileEventTyping(ctx, file, fixture);
    DWORD oldProtection;
    check(VirtualProtect(base + guarded, 4096, PAGE_NOACCESS, &oldProtection) != 0,
          "File buffer guard setup failed");
    auto read = [&](uint32_t destination, uint32_t statusBlock = 0, uint32_t position = 0) {
        memory->write32(ios, 0xa5a5a5a5); memory->write32(ios + 4, 0xa5a5a5a5);
        PPC_STORE_U64(offset, 0);
        ctx.r3.u64 = file; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        ctx.r7.u64 = statusBlock ? statusBlock : ios; ctx.r8.u64 = destination;
        ctx.r9.u64 = 512; ctx.r10.u64 = position ? position : offset;
        return readFileWithoutHostFault(ctx, base);
    };
    check(read(guarded) && ctx.r3.u32 == 0xc0000005 &&
          memory->read32(ios) == 0xc0000005 && memory->read32(ios + 4) == 0,
          "Unbuffered read into inaccessible guest memory caused a host fault");
    memset(base + guarded - 256, 0xa5, 256);
    check(read(guarded - 256) && ctx.r3.u32 == 0xc0000005 &&
          base[guarded - 256] == 0xa5 && base[guarded - 1] == 0xa5,
          "Unbuffered read partially wrote a buffer spanning an inaccessible page");
    check(read(data, guarded) && ctx.r3.u32 == 0xc0000005,
          "Read dereferenced an inaccessible guest I/O status block");
    check(read(data, ios, guarded) && ctx.r3.u32 == 0xc0000005 && memory->read32(ios + 4) == 0,
          "Read dereferenced an inaccessible guest file offset");
    check(read(0xfffffff0u) && ctx.r3.u32 == 0xc0000005,
          "Read accepted a destination wrapping the guest address space");
    check(VirtualProtect(base + guarded, 4096, PAGE_READONLY, &oldProtection) != 0,
          "Read-only file buffer setup failed");
    check(read(guarded) && ctx.r3.u32 == 0xc0000005,
          "Unbuffered read wrote into a read-only guest buffer");
    check(VirtualProtect(base + guarded, 4096, PAGE_EXECUTE_READWRITE, &oldProtection) != 0,
          "Split writable file buffer setup failed");
    check(read(guarded - 256) && ctx.r3.u32 == 0 && memory->read32(ios + 4) == 512,
          "Valid unaligned buffer spanning writable regions was rejected");
    std::ifstream source(memory->gameDirectory() / "basefile.exe", std::ios::binary);
    char expected[512]; source.read(expected, sizeof(expected));
    check(memcmp(base + guarded - 256, expected, sizeof(expected)) == 0,
          "Validated bounce-buffer read changed original file bytes");
    ctx.r3.u64 = file; __imp__NtClose(ctx, base);
    check(memory->release(fixture), "File buffer fixture cleanup failed");
    puts("Guest file buffers reject inaccessible spans before native I/O and copy.");
    testDirectoryRetry(ctx);
    testDirectoryValidation(ctx);
}
