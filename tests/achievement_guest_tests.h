#pragma once
#include "runtime/native/achievements.h"
#include "runtime/native/objects.h"
#include "runtime/native/storage.h"
#include <atomic>
#include <charconv>
#include <fstream>
#include <iterator>

static std::atomic<uint32_t> achievementApcCalls{0};
static std::atomic<uint32_t> achievementApcError{0}, achievementApcLength{0}, achievementApcOverlap{0};
static PPC_FUNC(achievementApcProbe) {
    achievementApcError = ctx.r3.u32;
    achievementApcLength = ctx.r4.u32;
    achievementApcOverlap = ctx.r5.u32;
    ++achievementApcCalls;
    ctx.r3.u64 = 0;
}

static void testAchievementRaceWorker(PPCContext& ctx, const char* root, const char* id,
                                      const char* marker) {
    namespace Ach = DarkRecomp::Native::Achievements;
    namespace Save = DarkRecomp::Native::Storage;
    uint32_t achievement = 0;
    const auto parsed = std::from_chars(id, id + strlen(id), achievement);
    check(parsed.ec == std::errc{} && parsed.ptr == id + strlen(id), "invalid achievement race-worker ID");
    const std::string markerName(marker);
    check(!markerName.empty() && markerName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos,
          "invalid achievement race-worker marker");
    Save::SetSaveRootOverride(root);
    Ach::reset();
    const auto cached = Ach::snapshot();
    check(cached.entries.size() == 50 && cached.error.empty(), "race worker could not cache the retail catalog");
    { std::ofstream ready(std::filesystem::path(root) / (markerName + ".ready")); ready << "ready\n"; check(bool(ready), "race-worker ready marker failed"); }
    const uint64_t deadline = GetTickCount64() + 5000;
    std::error_code ec;
    while (!std::filesystem::exists(std::filesystem::path(root) / "go", ec)) {
        check(!ec && GetTickCount64() < deadline, "achievement race-worker synchronization timed out");
        Sleep(5);
    }
    const uint32_t pair = memory->allocate(8);
    check(pair != 0, "achievement race-worker pair allocation failed");
    memory->write32(pair, 0); memory->write32(pair + 4, achievement);
    ctx.r3.u64 = 1; ctx.r4.u64 = pair; ctx.r5.u64 = 0;
    sub_828A7528(ctx, memory->base());
    memory->release(pair);
    check(ctx.r3.u32 == 0, "concurrent guest achievement award failed");
    Save::ClearSaveRootOverride(); Ach::reset();
}

static void testAchievementGuest(PPCContext& ctx) {
    namespace Ach = DarkRecomp::Native::Achievements;
    namespace Save = DarkRecomp::Native::Storage;
    auto* base = memory->base();
    currentContext = &ctx;
    const auto temporary = std::filesystem::temp_directory_path();
    std::filesystem::path root;
    for (uint32_t n = 0; n < 1000; ++n) {
        auto candidate = temporary / ("darkrecomp-achievements-" + std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(n));
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) { root = candidate; break; }
    }
    check(!root.empty(), "unique achievement test directory creation failed");
    Save::SetSaveRootOverride(root);
    Ach::reset();
    const uint32_t scratch = memory->allocate(4096), buffer = memory->allocate(65536);
    check(scratch && buffer, "achievement fixture allocation failed");
    auto* originalProbe = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = achievementApcProbe;
    std::vector<uint32_t> handles;
    std::vector<HANDLE> workerProcesses;
    auto close = [&](uint32_t handle) {
        ctx.r3.u64 = handle;
        __imp__NtClose(ctx, base);
        check(ctx.r3.u32 == 0, "achievement fixture handle close failed");
        handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
    };
    auto cleanup = [&] {
        for (HANDLE worker : workerProcesses) {
            if (WaitForSingleObject(worker, 0) == WAIT_TIMEOUT) {
                TerminateProcess(worker, 1); WaitForSingleObject(worker, 1000);
            }
            CloseHandle(worker);
        }
        for (uint32_t handle : handles) { ctx.r3.u64 = handle; __imp__NtClose(ctx, base); }
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = originalProbe;
        memory->release(scratch);
        memory->release(buffer);
        Save::ClearSaveRootOverride();
        Ach::reset();
        // Only flat files and directories created by this fixture are removed.
        // No recursive cleanup or traversal of links is involved.
        std::error_code ec;
        std::filesystem::remove(root / "achievements.dat", ec);
        std::filesystem::remove(root / "corrupt" / "achievements.dat", ec);
        std::filesystem::remove(root / "corrupt", ec);
        std::filesystem::remove(root / "blocked", ec);
        std::filesystem::remove(root / "race" / "achievements.dat", ec);
        std::filesystem::remove(root / "race" / "zero.ready", ec);
        std::filesystem::remove(root / "race" / "melee.ready", ec);
        std::filesystem::remove(root / "race" / "go", ec);
        std::filesystem::remove(root / "race", ec);
        std::filesystem::remove(root / "external" / "achievements.dat", ec);
        std::filesystem::remove(root / "external", ec);
        std::filesystem::remove(root, ec);
    };
    const uint32_t sizeOut = scratch, handleOut = scratch + 4, countOut = scratch + 8;
    const uint32_t pair = scratch + 16, ov = scratch + 64, eventOut = scratch + 96;
    auto makeOverlap = [&](uint32_t event, uint32_t callback) {
        memset(base + ov, 0, 28);
        memory->write32(ov, 0xDEADu);
        memory->write32(ov + 4, 0xDEADu);
        memory->write32(ov + 12, event);
        memory->write32(ov + 16, callback);
        memory->write32(ov + 20, 0xA55Au);
        memory->write32(ov + 24, 0xDEADu);
    };
    auto create = [&](uint32_t user, uint32_t flags, uint32_t offset, uint32_t fetch,
                      uint32_t title = 0, uint64_t xuid = 0, bool wrapper = false) {
        memory->write32(sizeOut, 0xDEADu);
        memory->write32(handleOut, 0xDEADu);
        ctx.r3.u64 = title; ctx.r4.u64 = user; ctx.r5.u64 = xuid; ctx.r6.u64 = flags;
        ctx.r7.u64 = offset; ctx.r8.u64 = fetch; ctx.r9.u64 = sizeOut; ctx.r10.u64 = handleOut;
        if (wrapper) sub_828A7958(ctx, base);
        else __imp__XamUserCreateAchievementEnumerator(ctx, base);
        if (!ctx.r3.u32) handles.push_back(memory->read32(handleOut));
        return ctx.r3.u32;
    };
    auto enumerate = [&](uint32_t handle, uint32_t bytes, uint32_t overlapped,
                         bool wrapper = false, uint32_t data = 0) {
        if (!data) data = buffer;
        if (wrapper) {
            ctx.r3.u64 = handle; ctx.r4.u64 = data; ctx.r5.u64 = bytes;
            ctx.r6.u64 = countOut; ctx.r7.u64 = overlapped;
            sub_828A7C88(ctx, base);
        } else {
            ctx.r3.u64 = handle; ctx.r4.u64 = 0; ctx.r5.u64 = data;
            ctx.r6.u64 = bytes; ctx.r7.u64 = countOut; ctx.r8.u64 = overlapped;
            __imp__XamEnumerate(ctx, base);
        }
        return ctx.r3.u32;
    };
    auto entry = [](const Ach::Snapshot& state, uint32_t id) -> const Ach::Entry& {
        const auto found = std::find_if(state.entries.begin(), state.entries.end(),
            [id](const auto& value) { return value.id == id; });
        check(found != state.entries.end(), "achievement ID missing from loaded catalog");
        return *found;
    };
    auto fileBytes = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    auto award = [&](uint32_t id, uint32_t user = 0, uint32_t overlap = 0) {
        memory->write32(pair, user); memory->write32(pair + 4, id);
        ctx.r3.u64 = 1; ctx.r4.u64 = pair; ctx.r5.u64 = overlap;
        sub_828A7528(ctx, base); // Original game's XUserWriteAchievements wrapper.
        return ctx.r3.u32;
    };
    auto drainApc = [&](uint32_t expected) {
        const uint64_t deadline = GetTickCount64() + 2000;
        while (achievementApcCalls.load() < expected && GetTickCount64() < deadline)
            SleepEx(10, TRUE);
        check(achievementApcCalls.load() == expected, "achievement completion APC missing or duplicated");
    };
    try {
        const auto catalog = Ach::snapshot();
        check(catalog.entries.size() == 50 && catalog.error.empty(), "retail achievement catalog unavailable");
        uint32_t score = 0;
        for (const auto& value : catalog.entries) { score += value.gamerscore; check(!value.unlockedAt, "fixture loaded live progress"); }
        check(score == 1000 && entry(catalog, 0).gamerscore == 5 && entry(catalog, 39).gamerscore == 5,
              "achievement catalog IDs or gamerscore disagree with retail XDBF");

        // The main-menu callback tails through this exact guest wrapper. Its
        // r4=0 assignment must reach the supported desktop viewer request.
        ctx.r3.u64 = 0; ctx.r4.u64 = 0xFFFFFFFFu;
        sub_828A7A48(ctx, base);
        check(ctx.r3.u32 == 0 && Ach::consumeShowRequest() && !Ach::consumeShowRequest(),
              "original achievements menu wrapper failed to open the viewer");
        ctx.r3.u64 = 1;
        sub_828A7A48(ctx, base);
        check(ctx.r3.u32 == 0x525 && !Ach::consumeShowRequest(), "viewer accepted an unsigned-in profile");
        ctx.r3.u64 = 0; ctx.r4.u64 = 0x12345678;
        __imp__XamShowAchievementsUI(ctx, base);
        check(ctx.r3.u32 == 0x57 && !Ach::consumeShowRequest(), "viewer accepted an unrelated title");
        ctx.r3.u64 = 1; ctx.r4.u64 = 0;
        __imp__XamNotifyCreateListener(ctx, base);
        const uint32_t listener = ctx.r3.u32;
        check(listener != 0, "system-UI notification fixture failed"); handles.push_back(listener);
        auto nextUi = [&](uint32_t expected) {
            ctx.r3.u64 = listener; ctx.r4.u64 = 9; ctx.r5.u64 = scratch + 112; ctx.r6.u64 = scratch + 116;
            __imp__XNotifyGetNext(ctx, base);
            check(ctx.r3.u32 == 1 && memory->read32(scratch + 112) == 9 && memory->read32(scratch + 116) == expected,
                  "achievement viewer system-UI notification wrong");
        };
        Ach::setViewerOpen(true); Ach::setViewerOpen(true); nextUi(1);
        ctx.r3.u64 = listener; ctx.r4.u64 = 0; ctx.r5.u64 = scratch + 112; ctx.r6.u64 = scratch + 116;
        __imp__XNotifyGetNext(ctx, base);
        check(ctx.r3.u32 == 0, "duplicate viewer state emitted duplicate notification");
        Ach::setViewerOpen(false); nextUi(0);
        close(listener);

        ctx.r3.u64 = 0; ctx.r4.u64 = 7; ctx.r5.u64 = scratch + 128;
        __imp__XamUserGetXUID(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(scratch + 128) == 0xE0000000u && memory->read32(scratch + 132) == 1,
              "local profile XUID ABI is wrong");
        ctx.r3.u64 = 1; ctx.r4.u64 = 7; ctx.r5.u64 = scratch + 128;
        __imp__XamUserGetXUID(ctx, base);
        check(ctx.r3.u32 == 0x80070525u && !memory->read32(scratch + 128) && !memory->read32(scratch + 132),
              "unsigned-in profile reported a local XUID");
        ctx.r3.u64 = 0; ctx.r4.u64 = 7; ctx.r5.u64 = 0xFFFFFFFCu;
        __imp__XamUserGetXUID(ctx, base);
        check(ctx.r3.u32 == 0x80070057u, "XUID accepted a wrapping output");

        check(create(0, 39, 0, 50, 0, 0, true) == 0, "original achievement enumerator wrapper failed");
        const uint32_t menuEnum = memory->read32(handleOut), menuBytes = memory->read32(sizeOut);
        check(menuBytes >= 50 * 500 && menuBytes <= 65536, "achievement size query omitted metadata or string reserve");
        ctx.r3.u64 = menuEnum; ctx.r4.u64 = 0; ctx.r5.u64 = buffer;
        ctx.r6.u64 = menuBytes; ctx.r7.u64 = buffer; ctx.r8.u64 = 0;
        __imp__XamEnumerate(ctx, base);
        check(ctx.r3.u32 == 0x57, "achievement output aliased its count cell");
        ctx.r3.u64 = menuEnum; ctx.r4.u64 = 0; ctx.r5.u64 = buffer;
        ctx.r6.u64 = menuBytes; ctx.r7.u64 = countOut; ctx.r8.u64 = buffer;
        memset(base + buffer, 0, 28);
        __imp__XamEnumerate(ctx, base);
        check(ctx.r3.u32 == 0x57, "achievement output aliased its completion record");
        ctx.r3.u64 = eventOut; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 0;
        __imp__NtCreateEvent(ctx, base);
        check(ctx.r3.u32 == 0, "achievement overlap event fixture failed");
        const uint32_t event = memory->read32(eventOut); handles.push_back(event);
        achievementApcCalls = 0;
        makeOverlap(event, PPC_CODE_BASE);
        check(enumerate(menuEnum, menuBytes, ov, true) == 997, "guest achievement enumeration did not pend");
        check(memory->read32(ov) == 0 && memory->read32(ov + 4) == 50 && memory->read32(ov + 24) == 0 &&
              memory->read32(countOut) == 50 && WaitForSingleObject(object(event)->handle, 0) == WAIT_OBJECT_0,
              "achievement enumeration did not complete its count and event");
        drainApc(1);
        check(achievementApcError == 0 && achievementApcLength == 50 && achievementApcOverlap == ov,
              "achievement enumeration callback ABI is wrong");
        for (uint32_t i = 0; i < 50; ++i) {
            const uint32_t at = buffer + i * 36;
            const auto& original = catalog.entries[i];
            check(memory->read32(at) == original.id && memory->read32(at + 16) == original.imageId &&
                  memory->read32(at + 20) == original.gamerscore && !memory->read32(at + 24) &&
                  !memory->read32(at + 28), "guest achievement record metadata or locked timestamp is wrong");
            const std::string* texts[] = {&original.name, &original.description, &original.lockedDescription};
            for (uint32_t s = 0; s < 3; ++s) {
                const uint32_t pointer = memory->read32(at + 4 + s * 4);
                check(pointer >= buffer + 50 * 36 && pointer < buffer + menuBytes,
                      "achievement string pointer outside returned buffer");
                const int length = MultiByteToWideChar(CP_UTF8, 0, texts[s]->data(), int(texts[s]->size()), nullptr, 0);
                std::wstring expected(length, L'\0');
                if (length) MultiByteToWideChar(CP_UTF8, 0, texts[s]->data(), int(texts[s]->size()), expected.data(), length);
                check(uint64_t(pointer) + (length + 1) * 2 <= uint64_t(buffer) + menuBytes,
                      "achievement UTF-16 string exceeds returned buffer");
                for (int c = 0; c <= length; ++c) {
                    const uint16_t value = (uint16_t(base[pointer + c * 2]) << 8) | base[pointer + c * 2 + 1];
                    check(value == (c < length ? expected[c] : 0), "achievement text is not terminated big-endian UTF-16");
                }
            }
        }
        check(enumerate(menuEnum, menuBytes, 0) == 18 && !memory->read32(countOut), "achievement cursor did not reach clean end");
        close(menuEnum);
        check(enumerate(menuEnum, menuBytes, 0) == 0x57, "closed achievement enumerator stayed usable");

        check(create(0, 0, 49, 2, 0x545407EEu, 0xE000000000000001ull) == 0,
              "achievement offset or local XUID query failed");
        const uint32_t pageEnum = memory->read32(handleOut);
        check(memory->read32(sizeOut) == 72, "metadata-only achievement query size wrong");
        check(enumerate(pageEnum, 71, 0) == 122, "undersized achievement buffer accepted");
        check(enumerate(pageEnum, 72, 0) == 0 && memory->read32(countOut) == 1 &&
              memory->read32(buffer) == 49 && !memory->read32(buffer + 4) &&
              !memory->read32(buffer + 8) && !memory->read32(buffer + 12),
              "short final achievement page or metadata-only flags wrong");
        check(enumerate(pageEnum, 72, 0) == 18 && !memory->read32(countOut), "offset achievement cursor did not stop");
        close(pageEnum);
        check(create(1, 7, 0, 1) == 0x525 && memory->read32(sizeOut) == 0xDEADu && memory->read32(handleOut) == 0xDEADu,
              "unsigned-in achievement query published outputs");
        check(create(0, 7, 0, 1, 0, 99) == 0x525, "achievement query accepted another XUID");
        check(create(0, 7, 0, 0) == 0x57 && create(0, 0x80000000u, 0, 1) == 0x57 &&
              create(0, 7, 0, 1, 0x12345678u) == 0x57, "invalid achievement query arguments accepted");
        ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 7;
        ctx.r7.u64 = 0; ctx.r8.u64 = 1; ctx.r9.u64 = 0xFFFFFFFEu; ctx.r10.u64 = handleOut;
        __imp__XamUserCreateAchievementEnumerator(ctx, base);
        check(ctx.r3.u32 == 0x57, "achievement query accepted a wrapping size output");
        ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.r6.u64 = 7;
        ctx.r7.u64 = 0; ctx.r8.u64 = 1; ctx.r9.u64 = sizeOut; ctx.r10.u64 = sizeOut + 2;
        __imp__XamUserCreateAchievementEnumerator(ctx, base);
        check(ctx.r3.u32 == 0x57, "achievement query accepted overlapping size and handle cells");

        // A malformed overlap, live non-event handle, or non-code callback
        // must be rejected before the award can create a progress file.
        check(award(0, 0, 0xFFFFFFF0u) == 1627 && !std::filesystem::exists(root / "achievements.dat"),
              "invalid achievement overlap mutated progress");
        makeOverlap(menuEnum, 0); // Closed enumerator, never a live event.
        check(award(0, 0, ov) == 1627 && !std::filesystem::exists(root / "achievements.dat"),
              "invalid achievement event mutated progress");
        makeOverlap(0, pair);
        check(award(0, 0, ov) == 1627 && !std::filesystem::exists(root / "achievements.dat"),
              "heap callback accepted for achievement completion");
        makeOverlap(0, PPC_CODE_BASE + 2);
        check(award(0, 0, ov) == 1627, "unaligned achievement callback accepted");
        check(award(0, 1) == 1627 && !entry(Ach::snapshot(), 0).unlockedAt,
              "achievement award accepted an unsigned-in user");

        const uint32_t thread = memory->read32(ctx.r13.u32 + 256);
        memory->write32(thread + 352, 0xDEADu);
        check(award(0) == 0 && memory->read32(thread + 352) == 0, "synchronous guest award failed or left stale last error");
        auto progress = Ach::snapshot();
        const uint64_t zeroTime = entry(progress, 0).unlockedAt;
        check(zeroTime != 0 && std::filesystem::exists(root / "achievements.dat"), "zero-ID achievement was not durably awarded");
        const std::string saved = fileBytes(root / "achievements.dat");
        const auto modified = std::filesystem::last_write_time(root / "achievements.dat");
        check(award(0) == 0 && entry(Ach::snapshot(), 0).unlockedAt == zeroTime &&
              fileBytes(root / "achievements.dat") == saved && std::filesystem::last_write_time(root / "achievements.dat") == modified,
              "duplicate achievement changed timestamp or rewrote progress");
        memory->write32(pair, 0); memory->write32(pair + 4, 42);
        memory->write32(pair + 8, 0); memory->write32(pair + 12, 9999);
        ctx.r3.u64 = 2; ctx.r4.u64 = pair; ctx.r5.u64 = 0;
        sub_828A7528(ctx, base);
        check(ctx.r3.u32 == 1627 && !entry(Ach::snapshot(), 42).unlockedAt && fileBytes(root / "achievements.dat") == saved,
              "invalid achievement batch partially published or persisted");
        ResetEvent(object(event)->handle);
        makeOverlap(event, PPC_CODE_BASE);
        check(award(39, 0, ov) == 997 && memory->read32(ov) == 0 && memory->read32(ov + 4) == 0 &&
              WaitForSingleObject(object(event)->handle, 0) == WAIT_OBJECT_0, "asynchronous guest award did not notify completion");
        drainApc(2);
        check(achievementApcError == 0 && achievementApcLength == 0 && achievementApcOverlap == ov,
              "achievement award callback ABI is wrong");
        const uint64_t meleeTime = entry(Ach::snapshot(), 39).unlockedAt;
        check(meleeTime != 0, "gameplay achievement not published");
        Ach::reset(); progress = Ach::snapshot();
        check(entry(progress, 0).unlockedAt == zeroTime && entry(progress, 39).unlockedAt == meleeTime,
              "achievement reset failed to reload persisted timestamps");
        check(create(0, 0, 39, 1) == 0, "unlocked achievement query failed");
        const uint32_t unlockedEnum = memory->read32(handleOut);
        check(enumerate(unlockedEnum, 36, 0) == 0 && memory->read32(buffer) == 39 &&
              ((uint64_t(memory->read32(buffer + 24)) << 32) | memory->read32(buffer + 28)) == meleeTime &&
              (memory->read32(buffer + 32) & 0x20000u), "guest achievement record omitted earned flag or FILETIME");
        close(unlockedEnum);

        // Corrupt progress is retained, and a write failure never publishes a
        // successful unlock. Both cases use independent disposable roots.
        const auto corrupt = root / "corrupt";
        std::filesystem::create_directory(corrupt);
        { std::ofstream out(corrupt / "achievements.dat", std::ios::binary); out << "corrupt fixture\n"; }
        Save::SetSaveRootOverride(corrupt); Ach::reset();
        const std::string corruptBytes = fileBytes(corrupt / "achievements.dat");
        const uint32_t id = 0;
        check(Ach::award(std::span(&id, 1)) == ERROR_READ_FAULT && !entry(Ach::snapshot(), 0).unlockedAt &&
              fileBytes(corrupt / "achievements.dat") == corruptBytes, "corrupt achievement progress was overwritten");
        const auto blocked = root / "blocked";
        std::filesystem::create_directory(blocked);
        Save::SetSaveRootOverride(blocked); Ach::reset();
        check(!entry(Ach::snapshot(), 0).unlockedAt, "write-failure fixture loaded progress");
        std::filesystem::remove(blocked);
        { std::ofstream out(blocked, std::ios::binary); out << "keep"; }
        check(Ach::award(std::span(&id, 1)) != 0 && !entry(Ach::snapshot(), 0).unlockedAt && fileBytes(blocked) == "keep",
              "failed achievement persistence published an unlock or changed the blocking file");
        Save::SetSaveRootOverride(root); Ach::reset();
        check(entry(Ach::snapshot(), 0).unlockedAt == zeroTime, "failure fixture damaged valid progress");

        // A second writer can update disk after our process caches progress.
        // Its timestamp must be preserved when this process merges an award.
        const auto external = root / "external";
        std::filesystem::create_directory(external);
        Save::SetSaveRootOverride(external); Ach::reset();
        check(!entry(Ach::snapshot(), 0).unlockedAt, "external-writer fixture loaded progress");
        { std::ofstream out(external / "achievements.dat", std::ios::binary);
          out << "DarkRecomp achievements v1\n0 " << zeroTime << "\n"; check(bool(out), "external achievement progress fixture failed"); }
        const uint32_t otherId = 39;
        check(Ach::award(std::span(&otherId, 1)) == 0 && entry(Ach::snapshot(), 0).unlockedAt == zeroTime &&
              entry(Ach::snapshot(), 39).unlockedAt != 0,
              "cached achievement award overwrote an external writer's progress");

        // Both child processes cache an empty root before the same go marker.
        // Awarding distinct IDs concurrently must produce their union on disk.
        const auto race = root / "race";
        std::filesystem::create_directory(race);
        wchar_t executable[32768]{};
        check(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))) != 0,
              "achievement race-worker executable lookup failed");
        const auto quote = [](std::wstring_view value) {
            std::wstring result(1, L'\"');
            size_t slashes = 0;
            for (wchar_t c : value) {
                if (c == L'\\') { ++slashes; continue; }
                if (c == L'\"') result.append(slashes * 2 + 1, L'\\');
                else result.append(slashes, L'\\');
                slashes = 0; result.push_back(c);
            }
            result.append(slashes * 2, L'\\'); result.push_back(L'\"');
            return result;
        };
        const auto launch = [&](const wchar_t* id, const wchar_t* marker) {
            std::wstring command = quote(executable) + L" " + quote(memory->gameDirectory().wstring()) +
                L" --achievement-worker " + quote(race.wstring()) + L" " + id + L" " + marker;
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                 nullptr, nullptr, &startup, &process) != FALSE, "achievement race-worker launch failed");
            CloseHandle(process.hThread); workerProcesses.push_back(process.hProcess);
        };
        launch(L"0", L"zero"); launch(L"39", L"melee");
        const uint64_t readyDeadline = GetTickCount64() + 5000;
        while (!std::filesystem::exists(race / "zero.ready") || !std::filesystem::exists(race / "melee.ready")) {
            check(GetTickCount64() < readyDeadline, "achievement workers did not cache progress in time");
            for (HANDLE worker : workerProcesses)
                check(WaitForSingleObject(worker, 0) == WAIT_TIMEOUT, "achievement race-worker exited before synchronization");
            Sleep(5);
        }
        { std::ofstream go(race / "go"); go << "go\n"; check(bool(go), "achievement race go marker failed"); }
        for (HANDLE worker : workerProcesses) {
            check(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0, "concurrent achievement worker timed out");
            DWORD status = 1;
            check(GetExitCodeProcess(worker, &status) && status == 0, "concurrent achievement worker failed");
        }
        Save::SetSaveRootOverride(race); Ach::reset();
        const auto merged = Ach::snapshot();
        check(merged.error.empty() && entry(merged, 0).unlockedAt && entry(merged, 39).unlockedAt,
              "concurrent processes lost an achievement unlock");
        Save::SetSaveRootOverride(root); Ach::reset();
        close(event);
        cleanup();
    } catch (...) { cleanup(); throw; }
    std::puts("Achievements: original guest menu, catalog, ABI, UTF-16, cursor, notifications, durable awards, invalid buffers and persistence failures passed.");
}
