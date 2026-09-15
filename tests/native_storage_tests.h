#pragma once
#include "runtime/native/storage.h"
#include "runtime/native/objects.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <system_error>

static std::atomic<uint32_t> gXamApcError{0xFFFFFFFFu};
static std::atomic<uint32_t> gXamApcLength{0xFFFFFFFFu};
static std::atomic<uint32_t> gXamApcOverlap{0};
static std::atomic<uint32_t> gXamApcCalls{0};
static PPC_FUNC(xamApcProbe) {
    gXamApcError.store(ctx.r3.u32, std::memory_order_relaxed);
    gXamApcLength.store(ctx.r4.u32, std::memory_order_relaxed);
    gXamApcOverlap.store(ctx.r5.u32, std::memory_order_relaxed);
    gXamApcCalls.fetch_add(1, std::memory_order_relaxed);
    ctx.r3.u64 = 0;
}

static void writeGuestContentRecord(uint8_t* base, uint32_t ptr, uint32_t device, uint32_t type,
                                    const char* display, const char* file) {
    memset(base + ptr, 0, 308);
    memory->write32(ptr, device);
    memory->write32(ptr + 4, type);
    size_t dlen = strlen(display);
    if (dlen > 127) dlen = 127;
    for (size_t i = 0; i < dlen; ++i) {
        base[ptr + 8 + i * 2] = 0;
        base[ptr + 8 + i * 2 + 1] = static_cast<uint8_t>(display[i]);
    }
    size_t flen = strlen(file);
    memcpy(base + ptr + 264, file, flen);
}

static uint32_t makeGuestEvent(PPCContext& ctx, uint8_t* base, uint32_t scratch) {
    ctx.r3.u64 = scratch;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = 1;
    ctx.r6.u64 = 0;
    __imp__NtCreateEvent(ctx, base);
    if (ctx.r3.u32) throw std::runtime_error("save event creation failed");
    return memory->read32(scratch);
}

static void writeGuestOverlapped(uint32_t ov, uint32_t event, uint32_t routine) {
    memory->write32(ov, 0xDEAD);
    memory->write32(ov + 4, 0xDEAD);
    memory->write32(ov + 8, 0xDEAD);
    memory->write32(ov + 12, event);
    memory->write32(ov + 16, routine);
    memory->write32(ov + 20, 0xA5A50001u);
    memory->write32(ov + 24, 0xDEAD);
}

static uint32_t storageCreateEx(PPCContext& ctx, uint8_t* base, uint32_t user, const char* root,
                                uint32_t data, uint32_t flags, uint32_t dispOut, uint32_t licenseOut,
                                uint32_t overlapped, bool writeStack = true) {
    uint32_t rootPtr = memory->allocate(64);
    if (!rootPtr) throw std::runtime_error("save root fixture allocation failed");
    memcpy(base + rootPtr, root, strlen(root) + 1);
    ctx.r3.u64 = user;
    ctx.r4.u64 = rootPtr;
    ctx.r5.u64 = data;
    ctx.r6.u64 = flags;
    ctx.r7.u64 = dispOut;
    ctx.r8.u64 = licenseOut;
    ctx.r9.u64 = 0;
    ctx.r10.u64 = 0;
    // Malformed-stack cases must reach production validation without any
    // fixture-side write at the wrapped address: memory->write32 performs a
    // raw host store, so writing via r1+84 after r1 is wrapped would fault in
    // the fixture instead of exercising the import's own slot check.
    if (writeStack) {
        memory->write32(ctx.r1.u32 + 84, overlapped);
        memory->write32(ctx.r1.u32 + 88, 0xC0DEC0DEu);
    }
    __imp__XamContentCreateEx(ctx, base);
    uint32_t status = ctx.r3.u32;
    memory->release(rootPtr);
    return status;
}

static uint32_t storageReadProfile(PPCContext& ctx, uint8_t* base, uint32_t title, uint32_t user,
                                   uint32_t xuidCount, uint32_t xuids, uint32_t count, uint32_t ids,
                                   uint32_t sizePtr, uint32_t buffer, uint32_t overlapped,
                                   bool writeStack = true) {
    ctx.r3.u64 = title;
    ctx.r4.u64 = user;
    ctx.r5.u64 = xuidCount;
    ctx.r6.u64 = xuids;
    ctx.r7.u64 = count;
    ctx.r8.u64 = ids;
    ctx.r9.u64 = sizePtr;
    ctx.r10.u64 = buffer;
    if (writeStack) {
        memory->write32(ctx.r1.u32 + 84, overlapped);
        memory->write32(ctx.r1.u32 + 88, 0xC0DEC0DEu);
    }
    __imp__XamUserReadProfileSettings(ctx, base);
    return ctx.r3.u32;
}

static uint32_t storageOpenFile(PPCContext& ctx, uint8_t* base, const char* name, uint32_t access,
                                uint32_t disposition, uint32_t* handleOut) {
    uint32_t scratch = memory->allocate(4096);
    if (!scratch) throw std::runtime_error("save file fixture allocation failed");
    size_t length = strlen(name);
    memcpy(base + scratch + 1024, name, length + 1);
    memory->write32(scratch, 0xfffffffd);
    memory->write32(scratch + 4, scratch + 16);
    memory->write32(scratch + 8, 0x40);
    memory->write32(scratch + 16, uint32_t(length << 16) | uint32_t(length + 1));
    memory->write32(scratch + 20, scratch + 1024);
    ctx.r3.u64 = scratch + 80;
    // Fixed sync mode 0x60 requires SYNCHRONIZE per NtCreateFile docs; the
    // caller-supplied read/write rights are preserved.
    ctx.r4.u64 = access | SYNCHRONIZE;
    ctx.r5.u64 = scratch;
    ctx.r6.u64 = scratch + 64;
    ctx.r7.u64 = 0;
    ctx.r8.u64 = 0;
    ctx.r9.u64 = 3;
    ctx.r10.u64 = disposition;
    memory->write32(ctx.r1.u32 + 84, 0x60);
    __imp__NtCreateFile(ctx, base);
    uint32_t status = ctx.r3.u32;
    if (handleOut) *handleOut = memory->read32(scratch + 80);
    memory->release(scratch);
    return status;
}

static void testNativeStorage(PPCContext& ctx) {
    auto* base = memory->base();
    currentContext = &ctx;
    auto* originalProbe = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = xamApcProbe;
    // Unique per-run root (pid + tick + attempt, fail-if-exists): never reuse
    // or remove a directory this run did not create. Independent of cwd, and
    // the default saves tree is never touched (override is always active).
    std::error_code ec;
    std::filesystem::path testBase = std::filesystem::temp_directory_path();
    uint32_t testPid = GetCurrentProcessId();
    uint64_t testTick = GetTickCount64();
    std::filesystem::path testRoot;
    bool testRootCreated = false;
    for (uint64_t n = 0; n < 1000 && !testRootCreated; ++n) {
        char name[128]{};
        snprintf(name, sizeof(name), "darkrecomp-save-tests-%lu-%llu-%llu", static_cast<unsigned long>(testPid),
                 static_cast<unsigned long long>(testTick), static_cast<unsigned long long>(n));
        std::filesystem::path candidate = testBase / name;
        std::error_code dirEc;
        if (std::filesystem::create_directory(candidate, dirEc) && !dirEc) {
            testRoot = candidate / "run";
            std::filesystem::create_directories(testRoot, dirEc);
            testRootCreated = !dirEc;
        }
    }
    check(testRootCreated, "unique save test root creation failed");
    std::filesystem::path createdCanon = std::filesystem::weakly_canonical(testRoot, ec);
    check(!ec, "save test root canonicalization failed");
    // Unrelated sentinel outside the unique root: must survive the whole run.
    char sentinelName[128]{};
    snprintf(sentinelName, sizeof(sentinelName), "darkrecomp-save-sentinel-%lu-%llu.bin",
             static_cast<unsigned long>(testPid), static_cast<unsigned long long>(testTick));
    std::filesystem::path sentinelPath = testBase / sentinelName;
    {
        std::ofstream sentinel(sentinelPath, std::ios::binary | std::ios::trunc);
        check(bool(sentinel), "sentinel fixture creation failed");
        sentinel.write("SENTINEL", 8);
        sentinel.flush();
        check(bool(sentinel), "sentinel fixture write failed");
    }
    auto readSentinel = [&]() {
        std::ifstream sentinel(sentinelPath, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(sentinel)), std::istreambuf_iterator<char>());
        return contents;
    };
    DarkRecomp::Native::Storage::SetSaveRootOverride(testRoot);
    DarkRecomp::Native::Storage::ClearMounts();
    auto cleanup = [&]() {
        DarkRecomp::Native::Storage::ClearMounts();
        DarkRecomp::Native::Storage::ClearSaveRootOverride();
        // Remove only the exact directory this run created: re-canonicalize
        // and compare before any removal.
        std::error_code ignore;
        std::filesystem::path nowCanon = std::filesystem::weakly_canonical(testRoot, ignore);
        if (!ignore && nowCanon == createdCanon) {
            std::filesystem::path parent = nowCanon.parent_path();
            std::filesystem::path baseCanon = std::filesystem::weakly_canonical(testBase, ignore);
            if (!ignore && parent.string().starts_with(baseCanon.string())) std::filesystem::remove_all(parent, ignore);
        }
        std::filesystem::remove(sentinelPath, ignore);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = originalProbe;
    };
    auto waitApc = [&](uint32_t calls, uint32_t timeoutMs) {
        uint64_t deadline = GetTickCount64() + timeoutMs;
        while (gXamApcCalls.load(std::memory_order_acquire) < calls) {
            if (SleepEx(10, TRUE) == WAIT_IO_COMPLETION) break;
            if (GetTickCount64() >= deadline) break;
            SleepEx(10, TRUE);
        }
        return gXamApcCalls.load(std::memory_order_acquire) >= calls;
    };
    try {
        uint32_t scratch = memory->allocate(8192);
        check(scratch != 0, "save scratch allocation failed");
        uint32_t data = scratch + 4096;
        uint32_t dispCell = scratch + 4608;
        uint32_t licenseCell = scratch + 4612;
        uint32_t deviceCell = scratch + 4616;
        uint32_t enumCell = scratch + 4620;
        uint32_t ovCell = scratch + 4700;
        uint32_t evCell = scratch + 4740;

        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = 0;
        ctx.r9.u64 = 0xC0DEC0DEu;
        ctx.r10.u64 = 0xC0DEC0DEu;
        memory->write32(deviceCell, 0xDEAD);
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(deviceCell) == 1, "device selector exact ABI failed");
        check(ctx.r9.u32 == 0xC0DEC0DEu, "selector clobbered unrelated r9");
        ctx.r3.u64 = 1;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = 0;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x525, "selector exposed saves to unsupported user");
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 2;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = 0;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x57, "selector accepted unsupported type");

        uint32_t event = makeGuestEvent(ctx, base, evCell);
        writeGuestOverlapped(ovCell, event, (PPC_CODE_BASE | 1));
        gXamApcCalls.store(0, std::memory_order_relaxed);
        memory->write32(deviceCell, 0xDEAD);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = ovCell;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x3E5, "overlapped selector did not pend");
        check(memory->read32(deviceCell) == 1, "overlapped selector skipped device write");
        check(memory->read32(ovCell) == 0 && memory->read32(ovCell + 4) == 4 &&
                  memory->read32(ovCell + 8) == 0xFFFFFFFE,
              "overlapped success fields wrong");
        check(WaitForSingleObject(object(event)->handle, 0) == WAIT_OBJECT_0, "overlapped event not signaled");
        check(waitApc(1, 3000), "XAM completion APC never delivered");
        check(gXamApcError.load() == 0 && gXamApcLength.load() == 4 && gXamApcOverlap.load() == ovCell,
              "XAM APC registers are not (error,length,overlap)");
        ctx.r3.u64 = event;
        __imp__NtClose(ctx, base);

        event = makeGuestEvent(ctx, base, evCell);
        writeGuestOverlapped(ovCell, event, 0);
        ctx.r3.u64 = 9;
        ctx.r4.u64 = ovCell;
        __imp__XamContentGetDeviceState(ctx, base);
        check(ctx.r3.u32 == 0x3E5 && memory->read32(ovCell) == 0x48F, "failure completion not reported in overlap");
        check(WaitForSingleObject(object(event)->handle, 0) == WAIT_OBJECT_0, "failure event not signaled");
        ctx.r3.u64 = event;
        __imp__NtClose(ctx, base);

        writeGuestOverlapped(ovCell, 0x12345678u, 0);
        memory->write32(deviceCell, 0xDEAD);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = ovCell;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x57 && memory->read32(deviceCell) == 0xDEAD,
              "invalid event silently dropped with success");

        // Inaccessible 28-byte span (wraps past the 4GB guest limit): must be
        // rejected without touching the device output. (evCell itself is a
        // valid writable span, so it cannot serve as the invalid-span case;
        // the bad-event target above covers that distinct path.)
        memory->write32(deviceCell, 0xDEAD);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = 0xFFFFFFF0u;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x57 && memory->read32(deviceCell) == 0xDEAD,
              "inaccessible overlap span accepted");

        // A semaphore is a live handle-table object but not an event: it must
        // be rejected before any storage effects, not pended without signal.
        ctx.r3.u64 = scratch + 4750;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 1;
        ctx.r6.u64 = 1;
        __imp__NtCreateSemaphore(ctx, base);
        check(ctx.r3.u32 == 0, "semaphore fixture failed");
        uint32_t semHandle = memory->read32(scratch + 4750);
        writeGuestOverlapped(ovCell, semHandle, 0);
        memory->write32(deviceCell, 0xDEAD);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = ovCell;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x57 && memory->read32(deviceCell) == 0xDEAD,
              "semaphore accepted as overlap event");
        ctx.r3.u64 = semHandle;
        __imp__NtClose(ctx, base);

        // Completion callbacks must be real AOT call targets: a readable heap
        // buffer and an unaligned code address are both rejected with no host
        // effects and no APC. The mapped PPC_CODE_BASE probe stays valid.
        uint32_t apcBefore = gXamApcCalls.load(std::memory_order_acquire);
        writeGuestOverlapped(ovCell, 0, data + 64);
        memory->write32(deviceCell, 0xDEAD);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = ovCell;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x57 && memory->read32(deviceCell) == 0xDEAD &&
                  gXamApcCalls.load(std::memory_order_acquire) == apcBefore,
              "readable heap buffer accepted as completion routine");
        writeGuestOverlapped(ovCell, 0, PPC_CODE_BASE + 2);
        memory->write32(deviceCell, 0xDEAD);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = deviceCell;
        ctx.r8.u64 = ovCell;
        __imp__XamShowDeviceSelectorUI(ctx, base);
        check(ctx.r3.u32 == 0x57 && memory->read32(deviceCell) == 0xDEAD &&
                  gXamApcCalls.load(std::memory_order_acquire) == apcBefore,
              "unaligned code address accepted as completion routine");

        ctx.r3.u64 = 1;
        ctx.r4.u64 = 0;
        __imp__XamContentGetDeviceState(ctx, base);
        check(ctx.r3.u32 == 0, "save device state not ready");
        ctx.r3.u64 = 9;
        ctx.r4.u64 = 0;
        __imp__XamContentGetDeviceState(ctx, base);
        check(ctx.r3.u32 == 0x48F, "unknown device reported ready");

        // XamGetExecutionId: real digest-verified XEX execution-info header.
        uint32_t execCell = scratch + 4820;
        uint32_t expectedField = memory->headerField(0x40006);
        check(expectedField != 0, "XEX execution-info header missing");
        memory->write32(execCell, 0xDEAD);
        ctx.r3.u64 = execCell;
        ctx.r4.u64 = 0xC0DEC0DEu;
        ctx.r5.u64 = 0xC0DEC0DEu;
        ctx.r6.u64 = 0xC0DEC0DEu;
        ctx.r7.u64 = 0xC0DEC0DEu;
        ctx.r8.u64 = 0xC0DEC0DEu;
        ctx.r9.u64 = 0xC0DEC0DEu;
        ctx.r10.u64 = 0xC0DEC0DEu;
        __imp__XamGetExecutionId(ctx, base);
        check(ctx.r3.u32 == 0, "execution-id query failed");
        check(memory->read32(execCell) == expectedField, "execution-id pointer is not the loaded header");
        check(ctx.r4.u32 == 0xC0DEC0DEu && ctx.r5.u32 == 0xC0DEC0DEu && ctx.r6.u32 == 0xC0DEC0DEu &&
                  ctx.r7.u32 == 0xC0DEC0DEu && ctx.r8.u32 == 0xC0DEC0DEu && ctx.r9.u32 == 0xC0DEC0DEu &&
                  ctx.r10.u32 == 0xC0DEC0DEu,
              "execution-id clobbered unrelated registers");
        check(memcmp(base + expectedField, base + memory->read32(execCell), 24) == 0,
              "execution-id header bytes differ from loaded XEX");
        check(memory->read32(expectedField + 12) != 0, "execution-info title word is not real data");
        uint32_t execTitle = memory->read32(expectedField + 12);
        memory->write32(execCell, 0);
        ctx.r3.u64 = execCell;
        __imp__XamGetExecutionId(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(execCell) == expectedField,
              "repeated execution-id call unstable");
        ctx.r3.u64 = 0;
        __imp__XamGetExecutionId(ctx, base);
        check(ctx.r3.u32 == 0xC000000D, "execution-id accepted null output");
        // 0xFFFFFFFE + 4 bytes wraps past the 4GB guest limit, so the span is
        // invalid on every mapping; a numerically unusual address alone (e.g.
        // 0x00FFFFFF, committed in this test image) proves nothing.
        ctx.r3.u64 = 0xFFFFFFFEu;
        __imp__XamGetExecutionId(ctx, base);
        check(ctx.r3.u32 == 0xC000000D, "execution-id accepted invalid output");
        // Real save-path caller sub_828AAE40 uses rlwinm r10,r31,16,16,31:
        // rotate left 16 then keep bits 16..31, i.e. HIGH16 of the caller
        // title ID, compared against the BE u16 at header+12. Pass the full
        // title word for success; flip a HIGH16 bit (low16 preserved) for the
        // publisher-mismatch path.
        ctx.r3.u64 = execTitle;
        sub_828AAE40(ctx, base);
        check(ctx.r3.u32 == 0, "original execution-id wrapper rejected real header");
        ctx.r3.u64 = execTitle ^ 0x80000000u;
        sub_828AAE40(ctx, base);
        check(ctx.r3.u32 == 1627, "original wrapper accepted mismatched header");
        ctx.r3.u64 = 0;
        sub_828AAE40(ctx, base);
        check(ctx.r3.u32 == 0, "original wrapper null path changed");

        writeGuestContentRecord(base, data, 1, 1, "T", "STOR_EMPTY00");
        ctx.r3.u64 = 1;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 1;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = 8;
        ctx.r8.u64 = ovCell;
        ctx.r9.u64 = enumCell;
        __imp__XamContentCreateEnumerator(ctx, base);
        check(ctx.r3.u32 == 0x525, "enumerator exposed saves to unsupported user");

        writeGuestContentRecord(base, data, 1, 1, "T", "STOR_EMPTY00");
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 1;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = 8;
        ctx.r8.u64 = ovCell;
        ctx.r9.u64 = enumCell;
        __imp__XamContentCreateEnumerator(ctx, base);
        check(ctx.r3.u32 == 0, "empty enumerator creation failed");
        uint32_t enumerator = memory->read32(enumCell);
        ctx.r3.u64 = enumerator;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = scratch + 5000;
        ctx.r6.u64 = 308 * 8;
        ctx.r7.u64 = scratch + 4800;
        ctx.r8.u64 = 0;
        __imp__XamEnumerate(ctx, base);
        check(ctx.r3.u32 == 0x12, "empty enumeration did not report no-more-files");
        ctx.r3.u64 = enumerator;
        __imp__NtClose(ctx, base);

        const char* rootA = "savet0";
        const char* fileA = "STOR_A00";
        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        memory->write32(dispCell, 0xDEAD);
        memory->write32(licenseCell, 0xDEAD);
        check(storageCreateEx(ctx, base, 0, rootA, data, 2, dispCell, licenseCell, 0) == 0,
              "content CREATE_ALWAYS failed");
        check(memory->read32(dispCell) == 1 && memory->read32(licenseCell) == 0,
              "create disposition or license wrong");
        check(memory->read32(ctx.r1.u32 + 88) == 0xC0DEC0DEu, "CreateEx consumed stack88 decoy");
        check(std::filesystem::is_directory(testRoot / (std::string("00000001_") + fileA)),
              "content directory not persisted");

        // Exact game saved-game caller flags (loc_827A75A0: user0/device1,
        // type1, flags0x1000 EXCLUDECOMMON, fetch1, no sizeOut): accepted,
        // the persisted STOR_A00 enumerated by filename, normal 0x12 end
        // with countOut cleared, handle closed. The high bit 0x80000000 is
        // unsupported by this implementation and stays rejected.
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 1;
        ctx.r6.u64 = 0x1000;
        ctx.r7.u64 = 1;
        ctx.r8.u64 = 0;
        ctx.r9.u64 = enumCell;
        __imp__XamContentCreateEnumerator(ctx, base);
        check(ctx.r3.u32 == 0, "EXCLUDECOMMON enumerator creation failed");
        uint32_t flagEnumerator = memory->read32(enumCell);
        ctx.r3.u64 = flagEnumerator;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = scratch + 5000;
        ctx.r6.u64 = 308;
        ctx.r7.u64 = scratch + 4800;
        ctx.r8.u64 = 0;
        __imp__XamEnumerate(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(scratch + 4800) == 1,
              "EXCLUDECOMMON enumeration missed the persisted save");
        check(memcmp(base + scratch + 5000 + 264, fileA, 9) == 0,
              "enumerated filename is not the persisted save");
        ctx.r3.u64 = flagEnumerator;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = scratch + 5000;
        ctx.r6.u64 = 308;
        ctx.r7.u64 = scratch + 4800;
        ctx.r8.u64 = 0;
        __imp__XamEnumerate(ctx, base);
        check(ctx.r3.u32 == 0x12 && memory->read32(scratch + 4800) == 0,
              "EXCLUDECOMMON enumeration missed clean end-of-enum");
        ctx.r3.u64 = flagEnumerator;
        __imp__NtClose(ctx, base);
        check(ctx.r3.u32 == 0, "enumerator close failed");
        ctx.r3.u64 = flagEnumerator;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = scratch + 5000;
        ctx.r6.u64 = 308;
        ctx.r7.u64 = scratch + 4800;
        ctx.r8.u64 = 0;
        __imp__XamEnumerate(ctx, base);
        check(ctx.r3.u32 == 0x57, "closed enumerator handle still usable");
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 1;
        ctx.r5.u64 = 1;
        ctx.r6.u64 = 0x80000000u;
        ctx.r7.u64 = 1;
        ctx.r8.u64 = 0;
        ctx.r9.u64 = enumCell;
        __imp__XamContentCreateEnumerator(ctx, base);
        check(ctx.r3.u32 == 0x57, "unsupported enumerator flag accepted");

        uint32_t savedR1 = ctx.r1.u32;
        ctx.r1.u32 = 0xFFFFFFFCu;
        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        memory->write32(dispCell, 0xDEAD);
        check(storageCreateEx(ctx, base, 0, rootA, data, 2, dispCell, licenseCell, 0, false) == 0x57,
              "wrapping stack accepted");
        ctx.r1.u32 = savedR1;
        check(memory->read32(dispCell) == 0xDEAD, "wrapping stack overwrote output");

        uint32_t badOv = scratch + 4900;
        writeGuestOverlapped(badOv, 0, 0);
        memory->write32(dispCell, 0xDEAD);
        // 0xFFFFFFF0 + 28 bytes wraps past the 4GB guest limit: guaranteed
        // invalid on every mapping (0x12345678 is committed zero RAM here, a
        // valid empty overlap, so it cannot serve as the malformed case).
        check(storageCreateEx(ctx, base, 0, "savetbad", data, 2, dispCell, licenseCell, 0xFFFFFFF0u) == 0x57,
              "invalid overlap at stack84 accepted");
        check(memory->read32(dispCell) == 0xDEAD, "bad overlap overwrote output");
        check(!std::filesystem::exists(testRoot / "00000001_STOR_A00_new"), "bad overlap mutated store");

        uint32_t handle = 0;
        std::string guestPath = std::string(rootA) + ":\\profile.bin";
        check(storageOpenFile(ctx, base, guestPath.c_str(), 0xC0000000, 2, &handle) == 0,
              "save file create failed");
        uint8_t payload[64];
        for (uint32_t i = 0; i < 64; ++i) payload[i] = uint8_t((i * 37 + 11) & 0xFF);
        memcpy(base + data + 64, payload, sizeof(payload));
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(0);
        ctx.r3.u64 = handle;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = scratch + 64;
        ctx.r8.u64 = data + 64;
        ctx.r9.u64 = sizeof(payload);
        ctx.r10.u64 = scratch + 48;
        __imp__NtWriteFile(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(scratch + 68) == sizeof(payload),
              "save bytes did not persist through NtWriteFile");
        ctx.r3.u64 = handle;
        ctx.r4.u64 = scratch + 64;
        __imp__NtFlushBuffersFile(ctx, base);
        check(ctx.r3.u32 == 0, "save flush failed");
        ctx.r3.u64 = scratch + 64;
        ctx.r4.u64 = 0;
        __imp__NtFlushBuffersFile(ctx, base);
        check(ctx.r3.u32 == 0xc000000d, "flush accepted null IOS");
        ctx.r3.u64 = handle;
        __imp__NtClose(ctx, base);
        check(storageOpenFile(ctx, base, "savet0:\\_xcontent.meta", 0xC0000000, 2, &handle) == 0xc0000033,
              "host metadata reachable through guest path");

        uint32_t closeRoot = memory->allocate(64);
        memcpy(base + closeRoot, rootA, strlen(rootA) + 1);
        ctx.r3.u64 = closeRoot;
        ctx.r4.u64 = 0;
        __imp__XamContentClose(ctx, base);
        check(ctx.r3.u32 == 0, "content close failed");
        memory->release(closeRoot);
        DarkRecomp::Native::Storage::ClearMounts();

        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        check(storageCreateEx(ctx, base, 0, "savet1", data, 3, dispCell, licenseCell, 0) == 0,
              "reopen after restart failed");
        check(memory->read32(dispCell) == 2, "reopen did not report existing content");
        check(storageOpenFile(ctx, base, "savet1:\\profile.bin", 0x80000000, 1, &handle) == 0,
              "saved file did not survive restart");
        memset(base + data + 128, 0xCC, sizeof(payload));
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(0);
        ctx.r3.u64 = handle;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = scratch + 64;
        ctx.r8.u64 = data + 128;
        ctx.r9.u64 = sizeof(payload);
        ctx.r10.u64 = scratch + 48;
        __imp__NtReadFile(ctx, base);
        check(ctx.r3.u32 == 0 && memcmp(base + data + 128, payload, sizeof(payload)) == 0,
              "reloaded save bytes differ");
        ctx.r3.u64 = handle;
        __imp__NtClose(ctx, base);

        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        check(storageCreateEx(ctx, base, 0, "savet2", data, 1, dispCell, 0, 0) == 0xB7,
              "CREATE_NEW over existing save succeeded");
        check(storageCreateEx(ctx, base, 0, "savet2", data, 3, dispCell, 0, 0) == 0,
              "OPEN_EXISTING over existing save failed");
        writeGuestContentRecord(base, data, 1, 1, "Missing", "STOR_MISS00");
        check(storageCreateEx(ctx, base, 0, "savetmiss", data, 3, dispCell, 0, 0) == 0x03,
              "OPEN_EXISTING over missing save succeeded");

        writeGuestContentRecord(base, data, 1, 1, "Sentinel", "STOR_SENT0");
        check(storageCreateEx(ctx, base, 0, "sentinel", data, 2, dispCell, 0, 0) == 0,
              "sentinel create failed");
        uint32_t sentinelHandle = 0;
        check(storageOpenFile(ctx, base, "sentinel:\\keep.bin", 0xC0000000, 2, &sentinelHandle) == 0,
              "sentinel file create failed");
        ctx.r3.u64 = sentinelHandle;
        __imp__NtClose(ctx, base);
        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        check(storageOpenFile(ctx, base, "savet1:\\old.bin", 0xC0000000, 2, &handle) == 0,
              "overwrite precondition file create failed");
        ctx.r3.u64 = handle;
        __imp__NtClose(ctx, base);
        check(storageCreateEx(ctx, base, 0, "savet1", data, 2, dispCell, 0, 0) == 0 &&
                  memory->read32(dispCell) == 1,
              "CREATE_ALWAYS overwrite failed");
        check(!std::filesystem::exists(testRoot / (std::string("00000001_") + fileA) / "old.bin"),
              "CREATE_ALWAYS preserved stale files");
        check(std::filesystem::exists(testRoot / "00000001_STOR_SENT0" / "keep.bin"),
              "overwrite touched sentinel container");
        check(storageOpenFile(ctx, base, "savet1:\\profile.bin", 0x80000000, 1, &handle) == 0xc0000034,
              "overwritten save unexpectedly retained files");

        auto contentCount = [&]() {
            size_t n = 0;
            std::error_code ignore;
            for (auto it = std::filesystem::directory_iterator(testRoot, ignore); it != std::filesystem::directory_iterator();
                 it.increment(ignore)) {
                if (ignore) break;
                ++n;
            }
            return n;
        };
        size_t before = contentCount();
        writeGuestContentRecord(base, data, 1, 1, "Bad", fileA);
        // 0xFFFFFFFE + 4 bytes wraps past the 4GB guest limit: guaranteed
        // invalid on every mapping (0x00FFFFFF is committed here). The helper
        // never stores to dispOut, so no fixture-side wrapped write occurs.
        check(storageCreateEx(ctx, base, 0, "savet2", data, 2, 0xFFFFFFFEu, 0, 0) == 0x57,
              "invalid out pointer wrote content");
        check(contentCount() == before, "invalid output created filesystem side effects");

        writeGuestContentRecord(base, data, 1, 1, "Collision", "STOR_COLA0");
        check(storageCreateEx(ctx, base, 0, "colalias", data, 2, dispCell, 0, 0) == 0,
              "collision precondition failed");
        writeGuestContentRecord(base, data, 1, 1, "Collision", "STOR_COLB0");
        check(storageCreateEx(ctx, base, 0, "colalias", data, 2, dispCell, 0, 0) == 0x57,
              "duplicate root alias remounted a different directory");
        check(!std::filesystem::exists(testRoot / "00000001_STOR_COLB0"),
              "alias collision created a second directory");

        check(storageOpenFile(ctx, base, "savet1:\\..\\other.bin", 0xC0000000, 2, &handle) == 0xc0000033,
              "mount escape not rejected");
        check(storageOpenFile(ctx, base, "game:\\basefile.exe", 0xC0000000, 1, &handle) == 0xc00000a2,
              "read-only game asset opened writable");
        check(storageOpenFile(ctx, base, "game:\\basefile.exe", 0x80000000, 1, &handle) == 0,
              "game asset read open failed");
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(0);
        ctx.r3.u64 = handle;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = scratch + 64;
        ctx.r8.u64 = data + 64;
        ctx.r9.u64 = 4;
        ctx.r10.u64 = scratch + 48;
        __imp__NtWriteFile(ctx, base);
        check(ctx.r3.u32 == 0xc00000a2, "write to read-only game asset succeeded");
        ctx.r3.u64 = handle;
        __imp__NtClose(ctx, base);

        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        check(storageCreateEx(ctx, base, 0, "savet1", data, 4, dispCell, 0, 0) == 0,
              "delete precondition open failed");
        {
            std::ofstream corrupt(testRoot / (std::string("00000001_") + fileA) / "_xcontent.meta",
                                  std::ios::binary | std::ios::trunc);
            corrupt.write("GARBAGE", 7);
        }
        std::vector<DarkRecomp::Native::Storage::ContentInfo> listed;
        check(DarkRecomp::Native::Storage::EnumerateChecked(1, listed) == 0,
              "enumeration after corruption failed");
        bool foundCorrupt = false;
        for (const auto& item : listed) {
            if (item.fileName == fileA) foundCorrupt = true;
        }
        check(!foundCorrupt, "corrupt metadata exposed as a save");
        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        check(storageCreateEx(ctx, base, 0, "savet1", data, 3, dispCell, 0, 0) == 0x03,
              "OPEN_EXISTING mounted corrupt metadata");
        check(storageCreateEx(ctx, base, 0, "savet1", data, 4, dispCell, 0, 0) == 0x03,
              "OPEN_ALWAYS mounted corrupt metadata");
        ctx.r3.u64 = 0;
        ctx.r4.u64 = data;
        ctx.r5.u64 = 0;
        __imp__XamContentDelete(ctx, base);
        check(ctx.r3.u32 == 0, "content delete failed");
        check(!std::filesystem::exists(testRoot / (std::string("00000001_") + fileA)),
              "deleted content remained on disk");
        check(storageOpenFile(ctx, base, "savet1:\\profile.bin", 0x80000000, 1, &handle) == 0xc000000e,
              "stale mount survived delete");
        // CREATE_NEW on the truly absent directory is the expected create
        // case (ENOENT), not an access failure.
        writeGuestContentRecord(base, data, 1, 1, "Save A", fileA);
        memory->write32(dispCell, 0xDEAD);
        check(storageCreateEx(ctx, base, 0, "savet1", data, 1, dispCell, 0, 0) == 0 &&
                  memory->read32(dispCell) == 1,
              "CREATE_NEW on absent directory failed");
        check(std::filesystem::is_directory(testRoot / (std::string("00000001_") + fileA)),
              "first real content creation left no directory");

        // File-size finalization (classes 20/19) on a separate fixture file.
        auto setInfo = [&](uint32_t h, uint32_t kind, uint64_t value) {
            *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(value);
            ctx.r3.u64 = h;
            ctx.r4.u64 = scratch + 64;
            ctx.r5.u64 = scratch + 48;
            ctx.r6.u64 = 8;
            ctx.r7.u64 = kind;
            __imp__NtSetInformationFile(ctx, base);
            return ctx.r3.u32;
        };
        auto queryEof = [&](uint32_t h) {
            ctx.r3.u64 = h;
            ctx.r4.u64 = scratch + 64;
            ctx.r5.u64 = data + 512;
            ctx.r6.u64 = 24;
            ctx.r7.u64 = 5;
            __imp__NtQueryInformationFile(ctx, base);
            check(ctx.r3.u32 == 0, "eof fixture size query failed");
            return _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + data + 512 + 8));
        };
        auto writeAt = [&](uint32_t h, uint64_t offset, const uint8_t* bytes, uint32_t n) {
            memcpy(base + data + 64, bytes, n);
            *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(offset);
            ctx.r3.u64 = h;
            ctx.r4.u64 = 0;
            ctx.r5.u64 = 0;
            ctx.r6.u64 = 0;
            ctx.r7.u64 = scratch + 64;
            ctx.r8.u64 = data + 64;
            ctx.r9.u64 = n;
            ctx.r10.u64 = scratch + 48;
            __imp__NtWriteFile(ctx, base);
            check(ctx.r3.u32 == 0 && memory->read32(scratch + 68) == n, "eof fixture write failed");
        };
        auto readAt = [&](uint32_t h, uint64_t offset, uint32_t n) {
            *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(offset);
            ctx.r3.u64 = h;
            ctx.r4.u64 = 0;
            ctx.r5.u64 = 0;
            ctx.r6.u64 = 0;
            ctx.r7.u64 = scratch + 64;
            ctx.r8.u64 = data + 128;
            ctx.r9.u64 = n;
            ctx.r10.u64 = scratch + 48;
            __imp__NtReadFile(ctx, base);
            check(ctx.r3.u32 == 0 && memory->read32(scratch + 68) == n, "eof fixture read failed");
        };
        uint8_t eofPayload[64];
        for (uint32_t i = 0; i < 64; ++i) eofPayload[i] = uint8_t((i * 53 + 7) & 0xFF);
        check(storageOpenFile(ctx, base, "savet1:\\eoffix.bin", 0xC0000000, 2, &handle) == 0,
              "eof fixture create failed");
        uint32_t eofHandle = handle;
        writeAt(eofHandle, 0, eofPayload, 64);
        check(setInfo(eofHandle, 20, 16) == 0, "EOF shrink failed");
        check(queryEof(eofHandle) == 16, "EOF shrink size wrong");
        readAt(eofHandle, 0, 16);
        check(memcmp(base + data + 128, eofPayload, 16) == 0, "EOF shrink prefix wrong");
        check(setInfo(eofHandle, 20, 48) == 0, "EOF extend failed");
        check(queryEof(eofHandle) == 48, "EOF extend size wrong");
        readAt(eofHandle, 16, 32);
        for (uint32_t i = 0; i < 32; ++i) check(base[data + 128 + i] == 0, "EOF extension not zero-filled");
        check(setInfo(eofHandle, 19, 4096) == 0, "allocation grow failed");
        check(queryEof(eofHandle) == 48, "allocation grow changed EOF");
        // Real Windows behavior (verified live): shrinking allocation does not
        // truncate EOF; end-of-file stays 48. No cluster-size assertions.
        check(setInfo(eofHandle, 19, 8) == 0, "allocation shrink failed");
        check(queryEof(eofHandle) == 48, "allocation shrink changed EOF");
        ctx.r3.u64 = eofHandle;
        __imp__NtClose(ctx, base);
        check(storageOpenFile(ctx, base, "savet1:\\eoffix.bin", 0x80000000, 1, &handle) == 0,
              "eof fixture reopen failed");
        eofHandle = handle;
        readAt(eofHandle, 0, 8);
        check(memcmp(base + data + 128, eofPayload, 8) == 0, "trimmed save did not persist");
        ctx.r3.u64 = eofHandle;
        __imp__NtClose(ctx, base);
        // Original wrapper: position query, EOF trim, allocation set.
        check(storageOpenFile(ctx, base, "savet1:\\eofwrap.bin", 0xC0000000, 2, &handle) == 0,
              "wrapper fixture create failed");
        eofHandle = handle;
        writeAt(eofHandle, 0, eofPayload, 32);
        check(setInfo(eofHandle, 20, 64) == 0, "wrapper precondition extend failed");
        ctx.r3.u64 = eofHandle;
        sub_828A9810(ctx, base);
        check(ctx.r3.u32 == 1, "original trim wrapper failed");
        check(queryEof(eofHandle) == 32, "wrapper did not trim to position");
        ctx.r3.u64 = eofHandle;
        __imp__NtClose(ctx, base);
        // Invalid inputs: no file mutation (EOF stays 32).
        check(storageOpenFile(ctx, base, "savet1:\\eofwrap.bin", 0xC0000000, 3, &handle) == 0,
              "eof invalid-input open failed");
        eofHandle = handle;
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(32);
        ctx.r3.u64 = eofHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 4;
        ctx.r7.u64 = 20;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc000000d, "short set-info length accepted");
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(uint64_t(-1));
        ctx.r3.u64 = eofHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 20;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc000000d, "negative EOF accepted");
        ctx.r3.u64 = eofHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = 0xFFFFFFFEu;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 20;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc0000005, "wrapping set-info input accepted");
        ctx.r3.u64 = eofHandle;
        ctx.r4.u64 = 0xFFFFFFFEu;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 20;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc0000005, "wrapping set-info IOS accepted");
        check(queryEof(eofHandle) == 32, "invalid set-info mutated file");
        ctx.r3.u64 = eofHandle;
        __imp__NtClose(ctx, base);
        // Read-only game asset: size changes blocked, seeks still work.
        check(storageOpenFile(ctx, base, "game:\\basefile.exe", 0x80000000, 1, &handle) == 0,
              "game asset read open failed");
        uint32_t roHandle = handle;
        uint64_t roSize = queryEof(roHandle);
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(roSize + 16);
        ctx.r3.u64 = roHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 20;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc00000a2, "EOF change on read-only asset succeeded");
        ctx.r3.u64 = roHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 19;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc00000a2, "allocation change on read-only asset succeeded");
        check(queryEof(roHandle) == roSize, "read-only asset mutated");
        *reinterpret_cast<uint64_t*>(base + scratch + 48) = _byteswap_uint64(16);
        ctx.r3.u64 = roHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 14;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0, "read-only position seek regressed");
        ctx.r3.u64 = roHandle;
        __imp__NtClose(ctx, base);

        // Delete-on-close finalization (class 13) on a separate fixture.
        auto setDisposition = [&](uint32_t h, uint8_t mark) {
            base[scratch + 48] = mark;
            ctx.r3.u64 = h;
            ctx.r4.u64 = scratch + 64;
            ctx.r5.u64 = scratch + 48;
            ctx.r6.u64 = 1;
            ctx.r7.u64 = 13;
            __imp__NtSetInformationFile(ctx, base);
            return ctx.r3.u32;
        };
        // Class 13 requires DELETE access: request RW|DELETE for marks.
        check(storageOpenFile(ctx, base, "savet1:\\dispfix.bin", 0xC0010000, 2, &handle) == 0,
              "disposition fixture create failed");
        uint32_t dispHandle = handle;
        writeAt(dispHandle, 0, eofPayload, 16);
        check(setDisposition(dispHandle, 1) == 0, "disposition delete mark failed");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        check(storageOpenFile(ctx, base, "savet1:\\dispfix.bin", 0x80000000, 1, &handle) == 0xc0000034,
              "delete-on-close file survived");
        check(storageOpenFile(ctx, base, "savet1:\\dispfix.bin", 0xC0010000, 2, &handle) == 0,
              "disposition cancel fixture create failed");
        dispHandle = handle;
        writeAt(dispHandle, 0, eofPayload, 8);
        check(setDisposition(dispHandle, 1) == 0, "cancel precondition mark failed");
        check(setDisposition(dispHandle, 0) == 0, "disposition cancel failed");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        check(storageOpenFile(ctx, base, "savet1:\\dispfix.bin", 0x80000000, 1, &handle) == 0,
              "cancelled delete removed file");
        dispHandle = handle;
        readAt(dispHandle, 0, 8);
        check(memcmp(base + data + 128, eofPayload, 8) == 0, "cancelled file bytes wrong");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        // Handle without DELETE: native access denied, file preserved.
        check(storageOpenFile(ctx, base, "savet1:\\dispfix.bin", 0xC0000000, 3, &handle) == 0,
              "no-DELETE fixture open failed");
        dispHandle = handle;
        check(setDisposition(dispHandle, 1) == 0xC0000022, "delete without DELETE access succeeded");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        check(storageOpenFile(ctx, base, "savet1:\\dispfix.bin", 0x80000000, 1, &handle) == 0,
              "denied delete removed file");
        dispHandle = handle;
        readAt(dispHandle, 0, 8);
        check(memcmp(base + data + 128, eofPayload, 8) == 0, "denied-delete file bytes wrong");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        // Real original delete wrapper: opens with DELETE access itself.
        check(storageOpenFile(ctx, base, "savet1:\\wrapdel.bin", 0xC0000000, 2, &handle) == 0,
              "wrapper fixture create failed");
        dispHandle = handle;
        writeAt(dispHandle, 0, eofPayload, 8);
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        uint32_t delPath = memory->allocate(64);
        check(delPath != 0, "wrapper path allocation failed");
        constexpr char delName[] = "savet1:\\wrapdel.bin";
        memcpy(base + delPath, delName, sizeof(delName));
        ctx.r3.u64 = delPath;
        sub_828A9B00(ctx, base);
        check(ctx.r3.u32 == 1, "original delete wrapper failed");
        memory->release(delPath);
        check(storageOpenFile(ctx, base, "savet1:\\wrapdel.bin", 0x80000000, 1, &handle) == 0xc0000034,
              "wrapper delete left file behind");
        // Invalid disposition inputs: no file mutation.
        check(storageOpenFile(ctx, base, "savet1:\\dispinv.bin", 0xC0000000, 2, &handle) == 0,
              "disposition invalid-input fixture failed");
        dispHandle = handle;
        writeAt(dispHandle, 0, eofPayload, 8);
        base[scratch + 48] = 1;
        ctx.r3.u64 = dispHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 8;
        ctx.r7.u64 = 13;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc000000d, "oversize disposition length accepted");
        // A 1-byte range cannot wrap: use a deliberately guarded NOACCESS page
        // (restored afterwards) instead of assuming an address is unmapped.
        uint32_t guardPage = memory->allocate(4096);
        check(guardPage != 0, "guard page allocation failed");
        DWORD guardOld = 0;
        check(VirtualProtect(memory->base() + guardPage, 4096, PAGE_NOACCESS, &guardOld) != 0,
              "guard page protect failed");
        ctx.r3.u64 = dispHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = guardPage;
        ctx.r6.u64 = 1;
        ctx.r7.u64 = 13;
        __imp__NtSetInformationFile(ctx, base);
        uint32_t guardStatus = ctx.r3.u32;
        DWORD guardRestore = 0;
        VirtualProtect(memory->base() + guardPage, 4096, guardOld, &guardRestore);
        memory->release(guardPage);
        check(guardStatus == 0xc0000005, "guarded disposition input accepted");
        ctx.r3.u64 = dispHandle;
        ctx.r4.u64 = 0xFFFFFFFEu;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 1;
        ctx.r7.u64 = 13;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc0000005, "wrapping disposition IOS accepted");
        check(queryEof(dispHandle) == 8, "invalid disposition mutated file");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        // Rejected marks must not linger: close/reopen proves bytes + length.
        check(storageOpenFile(ctx, base, "savet1:\\dispinv.bin", 0x80000000, 1, &handle) == 0,
              "invalid fixture reopen failed");
        dispHandle = handle;
        check(queryEof(dispHandle) == 8, "invalid fixture length changed");
        readAt(dispHandle, 0, 8);
        check(memcmp(base + data + 128, eofPayload, 8) == 0, "invalid fixture bytes changed");
        ctx.r3.u64 = dispHandle;
        __imp__NtClose(ctx, base);
        // Read-only game asset: disposition delete blocked, asset intact.
        check(storageOpenFile(ctx, base, "game:\\basefile.exe", 0x80000000, 1, &handle) == 0,
              "game asset read open failed");
        roHandle = handle;
        roSize = queryEof(roHandle);
        base[scratch + 48] = 1;
        ctx.r3.u64 = roHandle;
        ctx.r4.u64 = scratch + 64;
        ctx.r5.u64 = scratch + 48;
        ctx.r6.u64 = 1;
        ctx.r7.u64 = 13;
        __imp__NtSetInformationFile(ctx, base);
        check(ctx.r3.u32 == 0xc00000a2, "disposition delete on read-only asset succeeded");
        check(queryEof(roHandle) == roSize, "read-only asset mutated by disposition");
        ctx.r3.u64 = roHandle;
        __imp__NtClose(ctx, base);

        std::filesystem::path blocker = testRoot.parent_path() / "save-blocker.tmp";
        {
            std::ofstream flag(blocker, std::ios::binary | std::ios::trunc);
            flag.write("x", 1);
        }
        DarkRecomp::Native::Storage::SetSaveRootOverride(blocker);
        writeGuestContentRecord(base, data, 1, 1, "T", "STOR_EMPTY00");
        ctx.r3.u64 = 0;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 1;
        ctx.r6.u64 = 0;
        ctx.r7.u64 = 8;
        ctx.r8.u64 = ovCell;
        ctx.r9.u64 = enumCell;
        __imp__XamContentCreateEnumerator(ctx, base);
        check(ctx.r3.u32 == 0x48F, "unavailable store reported as empty saves");
        DarkRecomp::Native::Storage::SetSaveRootOverride(testRoot);
        std::error_code ignore;
        std::filesystem::remove(blocker, ignore);

        // Offline profile read: the six gamer preferences the menu requests.
        uint32_t profBlock = memory->allocate(1024);
        check(profBlock != 0, "profile fixture allocation failed");
        uint32_t profIds = profBlock, profSize = profBlock + 256;
        uint32_t profBuf = profBlock + 512;
        uint32_t liveTitle = memory->read32(memory->headerField(0x40006) + 12);
        check(liveTitle != 0, "live title id missing");
        const uint32_t wantIds[6] = {0x10040018u, 0x10040015u, 0x10040022u, 0x10040002u, 0x10040024u, 0x10040003u};
        const uint32_t wantValues[6] = {0, 0, 1, 0, 0, 3};
        for (uint32_t i = 0; i < 6; ++i) memory->write32(profIds + i * 4, wantIds[i]);
        memory->write32(profSize, 0);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, 0, 0) == 122,
              "profile size query failed");
        check(memory->read32(profSize) == 8 + 40 * 6, "profile needed size wrong");
        memory->write32(profSize, 0);
        check(storageReadProfile(ctx, base, 0, 0, 0, 0, 6, profIds, profSize, 0, 0) == 122,
              "title-zero size query failed");
        check(memory->read32(profSize) == 8 + 40 * 6, "title-zero needed size wrong");
        memory->write32(profSize, 8 + 40 * 6);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, 0, 0, 0) == 0x57,
              "nonzero size with null buffer accepted");
        memset(base + profBuf, 0xA5, 256);
        memory->write32(profSize, 8 + 40 * 6);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, 0) == 0,
              "profile full read failed");
        check(memory->read32(profBuf) == 6 && memory->read32(profBuf + 4) == profBuf + 8,
              "profile header count/pointer wrong");
        for (uint32_t i = 0; i < 6; ++i) {
            uint32_t rec = profBuf + 8 + i * 40;
            check(memory->read32(rec) == 1 && memory->read32(rec + 4) == 0 &&
                      memory->read32(rec + 8) == 0 && memory->read32(rec + 16) == wantIds[i] &&
                      memory->read32(rec + 20) == 0 && base[rec + 24] == 1 &&
                      memory->read32(rec + 28) == 0 && memory->read32(rec + 32) == wantValues[i] &&
                      memory->read32(rec + 36) == 0,
                  "profile record bytes/order/values wrong");
        }
        check(base[profBuf + 8 + 40 * 6] == 0xA5, "profile write overran needed span");
        for (uint32_t i = 0; i < 6; ++i)
            check(memory->read32(profIds + i * 4) == wantIds[i], "profile read clobbered input ids");
        // Real wrapper two-call flow with the live title.
        memory->write32(profSize, 0);
        ctx.r3.u64 = liveTitle;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 6;
        ctx.r6.u64 = profIds;
        ctx.r7.u64 = profSize;
        ctx.r8.u64 = 0;
        ctx.r9.u64 = 0;
        sub_82883970(ctx, base);
        check(ctx.r3.u32 == 122 && memory->read32(profSize) == 8 + 40 * 6, "wrapper size query failed");
        memset(base + profBuf, 0, 256);
        ctx.r3.u64 = liveTitle;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 6;
        ctx.r6.u64 = profIds;
        ctx.r7.u64 = profSize;
        ctx.r8.u64 = profBuf;
        ctx.r9.u64 = 0;
        sub_82883970(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(profBuf) == 6 &&
                  memory->read32(profBuf + 8 + 16) == wantIds[0],
              "wrapper full read failed");
        // Short (nonzero) buffer: 122 with the size cell preserved.
        memory->write32(profSize, 8);
        memset(base + profBuf, 0xA5, 256);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, 0) == 122,
              "short profile buffer accepted");
        check(memory->read32(profSize) == 8, "short query overwrote size cell");
        check(base[profBuf] == 0xA5, "short query wrote buffer");
        // Unknown setting with a valid buffer: honest error, no writes.
        memory->write32(profIds, 0x10040099u);
        memory->write32(profSize, 8 + 40 * 6);
        memset(base + profBuf, 0xA5, 256);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, 0) == 0x57,
              "unknown profile setting accepted");
        check(base[profBuf] == 0xA5 && memory->read32(profSize) == 8 + 40 * 6,
              "unknown setting mutated outputs");
        for (uint32_t i = 0; i < 6; ++i) memory->write32(profIds + i * 4, wantIds[i]);
        // User and XUID modes.
        check(storageReadProfile(ctx, base, liveTitle, 1, 0, 0, 6, profIds, profSize, profBuf, 0) == 0x525,
              "profile exposed to unsupported user");
        check(storageReadProfile(ctx, base, liveTitle, 0, 1, 0, 6, profIds, profSize, profBuf, 0) == 0x57,
              "profile XUID count accepted");
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, profIds, 6, profIds, profSize, profBuf, 0) == 0x57,
              "profile XUID pointer accepted");
        check(storageReadProfile(ctx, base, liveTitle ^ 0xFFFFFFFFu, 0, 0, 0, 6, profIds, profSize, profBuf, 0) == 0x57,
              "profile foreign title accepted");
        // Malformed spans: null/unmapped size cell, null ids, empty/huge count.
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, 0, profBuf, 0) == 0x57,
              "profile null size pointer accepted");
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, 0xFFFFFFFEu, profBuf, 0) == 0x57,
              "profile wrapping size pointer accepted");
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, 0, profSize, profBuf, 0) == 0x57,
              "profile null ids accepted");
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 0, profIds, profSize, profBuf, 0) == 0x57,
              "profile empty count accepted");
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 33, profIds, profSize, profBuf, 0) == 0x57,
              "profile oversized count accepted");
        // Wrapping stack slot and stack88 decoy.
        savedR1 = ctx.r1.u32;
        ctx.r1.u32 = 0xFFFFFFFCu;
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, 0, false) == 0x57,
              "profile wrapping stack accepted");
        ctx.r1.u32 = savedR1;
        memory->write32(profSize, 8 + 40 * 6);
        memset(base + profBuf, 0, 256);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, 0) == 0 &&
                  memory->read32(ctx.r1.u32 + 88) == 0xC0DEC0DEu,
              "profile consumed stack88 decoy");
        // Overlapped success and failure completion via existing probes.
        event = makeGuestEvent(ctx, base, evCell);
        writeGuestOverlapped(ovCell, event, (PPC_CODE_BASE | 1));
        gXamApcCalls.store(0, std::memory_order_relaxed);
        memory->write32(profSize, 8 + 40 * 6);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, ovCell) == 0x3E5,
              "profile overlap did not pend");
        check(memory->read32(ovCell) == 0 && memory->read32(ovCell + 8) == 0xFFFFFFFE,
              "profile overlap fields wrong");
        check(WaitForSingleObject(object(event)->handle, 0) == WAIT_OBJECT_0, "profile event not signaled");
        check(waitApc(1, 3000), "profile APC never delivered");
        check(gXamApcError.load() == 0 && gXamApcLength.load() == 0 && gXamApcOverlap.load() == ovCell,
              "profile APC registers wrong");
        ctx.r3.u64 = event;
        __imp__NtClose(ctx, base);
        memory->write32(profIds, 0x10040099u);
        event = makeGuestEvent(ctx, base, evCell);
        writeGuestOverlapped(ovCell, event, 0);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, profBuf, ovCell) == 0x3E5,
              "profile failure overlap did not pend");
        check(memory->read32(ovCell) == 0x57, "profile failure not reported in overlap");
        check(WaitForSingleObject(object(event)->handle, 0) == WAIT_OBJECT_0, "profile failure event lost");
        ctx.r3.u64 = event;
        __imp__NtClose(ctx, base);
        for (uint32_t i = 0; i < 6; ++i) memory->write32(profIds + i * 4, wantIds[i]);
        event = makeGuestEvent(ctx, base, evCell);
        writeGuestOverlapped(ovCell, event, 0);
        gXamApcCalls.store(0, std::memory_order_relaxed);
        memory->write32(profSize, 0);
        check(storageReadProfile(ctx, base, liveTitle, 0, 0, 0, 6, profIds, profSize, 0, ovCell) == 122,
              "122 path touched overlap");
        check(memory->read32(ovCell) == 0xDEAD && memory->read32(profSize) == 8 + 40 * 6,
              "122 path mutated overlap or skipped size");
        check(gXamApcCalls.load(std::memory_order_acquire) == 0, "122 path queued APC");
        check(WaitForSingleObject(object(event)->handle, 0) == WAIT_TIMEOUT, "122 path signaled event");
        ctx.r3.u64 = event;
        __imp__NtClose(ctx, base);
        memory->release(profBlock);

        check(readSentinel() == "SENTINEL", "external sentinel outside test root mutated");
        memory->release(scratch);
        puts("Native save storage: exact ABI, APC, containment, overwrite, validation, read-only guards passed.");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
