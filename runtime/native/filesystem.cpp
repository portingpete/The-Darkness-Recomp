#include "objects.h"
#include "storage.h"
#include "renderer/engine/engine_performance.h"
#include <winternl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <malloc.h>

using namespace DarkRecomp::Native;
namespace {
bool FileTraceEnabled() {
    // Verbose per-syscall file logs cost an unbuffered stderr write each.
    // Keep failures always visible; gate success spam behind opt-in.
    static const bool enabled = std::getenv("DARK_FILE_TRACE") != nullptr &&
        std::getenv("DARK_FILE_TRACE")[0] == '1';
    return enabled;
}
constexpr uint32_t invalidParameter = 0xc000000d, invalidHandle = 0xc0000008, pathSyntax = 0xc000003b;
constexpr uint32_t nameInvalid = 0xc0000033, noDevice = 0xc000000e, writeProtected = 0xc00000a2;
constexpr uint32_t pending = 0x103;
constexpr uint32_t accessViolation = 0xc0000005;
bool fileGuestSpan(uint32_t address, uint32_t bytes, bool writable) {
    if (!address || uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    uint64_t cursor = address, end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!engineProfileVirtualQuery(EnginePhase::queryFile, memory->base() + cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = info.Protect & 0xff;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                              protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (writable ? !canWrite : !(canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ))
            return false;
        const uint64_t next = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - memory->base();
        if (next <= cursor) return false;
        cursor = (std::min)(next, end);
    }
    return true;
}
template<class T> T nt(PPCContext& ctx, const char* name) {
    auto fn = reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
    if (!fn) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), name);
    return fn;
}
uint64_t read64(uint32_t address) { return _byteswap_uint64(*reinterpret_cast<uint64_t*>(memory->base() + address)); }
void write64(uint32_t address, uint64_t value) { *reinterpret_cast<uint64_t*>(memory->base() + address) = _byteswap_uint64(value); }
void ioStatus(uint32_t pointer, uint32_t status, uint32_t bytes) {
    if (pointer) { memory->write32(pointer, status); memory->write32(pointer + 4, bytes); }
}
struct PathResult { std::filesystem::path path; bool writable = false; uint32_t status = 0; };
PathResult resolve(uint32_t attributes) {
    PathResult result;
    if (!attributes) { result.status = invalidParameter; return result; }
    if (!fileGuestSpan(attributes, 12, false)) { result.status = accessViolation; return result; }
    uint32_t root = memory->read32(attributes), string = memory->read32(attributes + 4);
    if (!string) { result.status = nameInvalid; return result; }
    if (!fileGuestSpan(string, 8, false)) { result.status = accessViolation; return result; }
    uint32_t lengths = memory->read32(string), pointer = memory->read32(string + 4);
    uint32_t length = lengths >> 16;
    if (!pointer || length > (lengths & 0xffff) || length > 4096) { result.status = nameInvalid; return result; }
    // The guest supplies a counted string; MaximumLength and a terminator are
    // not part of the bytes consumed by path resolution.
    if (!fileGuestSpan(pointer, length, false)) { result.status = accessViolation; return result; }
    std::string name(reinterpret_cast<char*>(memory->base() + pointer), length);
    if (name.find('\0') != std::string::npos || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c >= 128; })) {
        result.status = nameInvalid; return result;
    }
    std::replace(name.begin(), name.end(), '/', '\\');
    std::string lower = name;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (lower.starts_with("\\??\\")) { name.erase(0, 4); lower.erase(0, 4); }
    std::filesystem::path mount;
    size_t prefix = 0;
    if (lower.starts_with("game:") || lower.starts_with("dvd:") || lower.starts_with("d:")) {
        mount = memory->gameDirectory(); prefix = lower.find(':') + 1;
    } else if (lower == "\\device\\cdrom0" || lower.starts_with("\\device\\cdrom0\\")) {
        mount = memory->gameDirectory(); prefix = 14;
    } else if (lower.starts_with("cache:") || lower.starts_with("cache0:")) {
        mount = memory->gameDirectory().parent_path() / "build_native/run/cache";
        prefix = lower.find(':') + 1; result.writable = true;
        std::filesystem::create_directories(mount);
    } else if (auto colon = lower.find(':'); colon != std::string::npos) {
        auto mounted = Storage::LookupMount(lower.substr(0, colon));
        if (mounted) {
            mount = *mounted; prefix = colon + 1; result.writable = true;
        } else if (root && root != 0xfffffffd) {
            auto parent = object(root);
            if (!parent || !parent->isFile) { result.status = invalidHandle; return result; }
            mount = parent->path; result.writable = parent->writable;
        } else {
            fprintf(stderr, "[File] No native mount for '%s'\n", name.c_str());
            result.status = noDevice; return result;
        }
    } else if (root && root != 0xfffffffd) {
        auto parent = object(root);
        if (!parent || !parent->isFile) { result.status = invalidHandle; return result; }
        mount = parent->path; result.writable = parent->writable;
    } else if (!name.empty() && name.front() != '\\' && name.find(':') == std::string::npos) {
        // A bare relative name with no valid root directory has no volume;
        // it must not silently acquire the game directory. That made
        // directory-existence probes succeed and broke archive root setup
        // (relative content\ root vs absolute D:/Content asset paths).
        result.status = pathSyntax; return result;
    } else {
        fprintf(stderr, "[File] No native mount for '%s'\n", name.c_str());
        result.status = noDevice; return result;
    }
    while (prefix < name.size() && name[prefix] == '\\') ++prefix;
    std::string relative = name.substr(prefix);
    if (relative.find(':') != std::string::npos) { result.status = nameInvalid; return result; }
    for (const auto& component : std::filesystem::path(relative)) {
        if (component == "..") { result.status = nameInvalid; return result; }
        // Reserve the whole host metadata namespace, case-insensitively, on
        // both alias-mounted and root-handle-relative paths: the fixed file
        // plus the entire unique `_xcontent.meta.tmp.*` temp namespace.
        std::string piece = component.string();
        for (char& c : piece) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (piece == "_xcontent.meta" || piece.starts_with("_xcontent.meta.tmp")) {
            result.status = nameInvalid;
            return result;
        }
    }
    result.path = std::filesystem::weakly_canonical(mount / relative);
    auto rootString = std::filesystem::weakly_canonical(mount).wstring();
    auto pathString = result.path.wstring();
    if (pathString.size() < rootString.size() || _wcsnicmp(pathString.c_str(), rootString.c_str(), rootString.size()) ||
        (pathString.size() > rootString.size() && pathString[rootString.size()] != L'\\')) result.status = nameInvalid;
    // Startup serves CubeWnd from the XDF read cache before opening loose
    // files. Both paths must carry the same generated menu. Shipped (often
    // hardlinked) assets remain untouched.
    const wchar_t* replacement = nullptr;
    if (!result.status && !result.writable) {
        const auto relativePath = result.path.lexically_relative(memory->gameDirectory()).make_preferred();
        if (_wcsicmp(relativePath.c_str(), L"Content\\Gui\\CubeWnd.xcr") == 0)
            replacement = L"CubeWnd.pc.xcr";
        else if (_wcsicmp(relativePath.c_str(), L"Content\\Xdf\\GameContext_Create.XDF") == 0)
            replacement = L"GameContext_Create.pc.xdf";
    }
    if (replacement) {
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable)));
        if (length && length < std::size(executable))
            result.path = std::filesystem::path(executable).parent_path() / replacement;
    }
    return result;
}
uint32_t create(PPCContext& ctx, uint32_t out, uint32_t access, uint32_t attributes, uint32_t ios,
                uint32_t allocationPointer, uint32_t fileAttributes, uint32_t sharing, uint32_t disposition, uint32_t options) {
    if (!out || !ios) return invalidParameter;
    // Validate before resolving writable mounts or opening a host file. A bad
    // output must not create/truncate a file or leave a registered handle behind.
    if (!fileGuestSpan(ios, 8, true)) return accessViolation;
    if (!fileGuestSpan(out, 4, true) ||
        (allocationPointer && !fileGuestSpan(allocationPointer, 8, false))) {
        ioStatus(ios, accessViolation, 0); return accessViolation;
    }
    PathResult resolved = resolve(attributes);
    uint32_t status = resolved.status;
    constexpr uint32_t writeAccess = GENERIC_WRITE | GENERIC_ALL | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    if (!status && !resolved.writable && ((access & writeAccess) || disposition != 1 || (options & 0x1000))) status = writeProtected;
    HANDLE handle = nullptr;
    IO_STATUS_BLOCK nativeIo{};
    if (!status) {
        std::wstring nativePath = L"\\??\\" + resolved.path.wstring();
        UNICODE_STRING name{USHORT(nativePath.size() * 2), USHORT(nativePath.size() * 2), nativePath.data()};
        OBJECT_ATTRIBUTES attrs{};
        InitializeObjectAttributes(&attrs, &name, memory->read32(attributes + 8) & 0x40, nullptr, nullptr);
        LARGE_INTEGER allocation{};
        if (allocationPointer) allocation.QuadPart = read64(allocationPointer);
        using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        status = uint32_t(nt<Create>(ctx, "NtCreateFile")(&handle, access, &attrs, &nativeIo,
            allocationPointer ? &allocation : nullptr, fileAttributes, sharing, disposition, options, nullptr, 0));
        if (int32_t(status) >= 0) {
            uint32_t id = storeObject(handle);
            auto stored = object(id);
            stored->isFile = true; stored->path = resolved.path; stored->writable = resolved.writable;
            stored->unbuffered = (options & 8) != 0;
            memory->write32(out, id);
        }
    }
    ioStatus(ios, status, int32_t(status) >= 0 ? uint32_t(nativeIo.Information) : 0);
    if (FileTraceEnabled() || int32_t(status) < 0)
        fprintf(stderr, "[File] open '%ls' access=0x%08X options=0x%08X status=0x%08X\n",
                resolved.path.c_str(), access, options, status);
    return status;
}
}
PPC_FUNC(__imp__NtCreateFile) {
    const uint64_t optionsAddress = uint64_t(ctx.r1.u32) + 84;
    if (optionsAddress > UINT32_MAX || !fileGuestSpan(uint32_t(optionsAddress), 4, false)) {
        if (fileGuestSpan(ctx.r6.u32, 8, true)) ioStatus(ctx.r6.u32, accessViolation, 0);
        ctx.r3.u64 = accessViolation; return;
    }
    ctx.r3.u64 = create(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                        ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, memory->read32(uint32_t(optionsAddress)));
}
PPC_FUNC(__imp__NtOpenFile) {
    // The title's original callers pass ShareAccess and OpenOptions separately.
    ctx.r3.u64 = create(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, ctx.r7.u32, 1, ctx.r8.u32);
}
PPC_FUNC(__imp__NtQueryFullAttributesFile) {
    uint32_t attributes = ctx.r3.u32, out = ctx.r4.u32;
    if (!out) { ctx.r3.u64 = invalidParameter; return; }
    if (!fileGuestSpan(out, 56, true)) { ctx.r3.u64 = accessViolation; return; }
    PathResult resolved = resolve(attributes);
    uint32_t status = resolved.status;
    if (!status) {
        std::wstring nativePath = L"\\??\\" + resolved.path.wstring();
        UNICODE_STRING name{USHORT(nativePath.size() * 2), USHORT(nativePath.size() * 2), nativePath.data()};
        OBJECT_ATTRIBUTES attrs{};
        InitializeObjectAttributes(&attrs, &name, memory->read32(attributes + 8) & 0x40, nullptr, nullptr);
        alignas(8) uint8_t result[56]{};
        using Query = NTSTATUS (NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
        status = uint32_t(nt<Query>(ctx, "NtQueryFullAttributesFile")(&attrs, result));
        if (int32_t(status) >= 0) {
            memset(base + out, 0, 56);
            for (uint32_t i = 0; i < 6; ++i) write64(out + i * 8, reinterpret_cast<uint64_t*>(result)[i]);
            memory->write32(out + 48, *reinterpret_cast<uint32_t*>(result + 48));
        }
    }
    if (FileTraceEnabled() || int32_t(status) < 0)
        fprintf(stderr, "[File] attributes '%ls' status=0x%08X\n", resolved.path.c_str(), status);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__NtReadFile) {
    uint32_t ios = ctx.r7.u32;
    if (!ios) { ctx.r3.u64 = invalidParameter; return; }
    if (!fileGuestSpan(ios, 8, true)) { ctx.r3.u64 = accessViolation; return; }
    auto file = object(ctx.r3.u32), event = ctx.r4.u32 ? object(ctx.r4.u32) : nullptr;
    if (!file || !file->isFile || (ctx.r4.u32 && (!event || !event->isEvent))) {
        ioStatus(ios, invalidHandle, 0); ctx.r3.u64 = invalidHandle; return;
    }
    if (!ctx.r8.u32 && ctx.r9.u32) {
        ioStatus(ios, invalidParameter, 0); ctx.r3.u64 = invalidParameter; return;
    }
    // Validate before submitting I/O: unbuffered reads use a host bounce buffer,
    // so ntdll cannot validate the guest destination on our behalf.
    if ((ctx.r9.u32 && !fileGuestSpan(ctx.r8.u32, ctx.r9.u32, true)) ||
        (ctx.r10.u32 && !fileGuestSpan(ctx.r10.u32, 8, false))) {
        ioStatus(ios, accessViolation, 0); ctx.r3.u64 = accessViolation; return;
    }
    uint32_t apc = ctx.r5.u32, argument = ctx.r6.u32;
    std::lock_guard lock(file->ioMutex);
    HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completion) { ioStatus(ios, 0xc0000017, 0); ctx.r3.u64 = 0xc0000017; return; }
    LARGE_INTEGER offset{};
    if (ctx.r10.u32) offset.QuadPart = read64(ctx.r10.u32);
    IO_STATUS_BLOCK result{};
    std::unique_ptr<void, decltype(&_aligned_free)> aligned(nullptr, _aligned_free);
    void* destination = base + ctx.r8.u32;
    if (file->unbuffered && ctx.r9.u32) {
        aligned.reset(_aligned_malloc(ctx.r9.u32, 4096));
        if (!aligned) { CloseHandle(completion); ioStatus(ios, 0xc0000017, 0); ctx.r3.u64 = 0xc0000017; return; }
        destination = aligned.get();
    }
    using Read = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
    uint32_t status = uint32_t(nt<Read>(ctx, "NtReadFile")(file->handle, completion, nullptr, nullptr, &result,
        destination, ctx.r9.u32, ctx.r10.u32 ? &offset : nullptr, nullptr));
    if (status == pending) {
        WaitForSingleObject(completion, INFINITE);
        status = uint32_t(result.Status);
    }
    // A validation failure can return before Windows submits the request.
    // Notify only actual completions, including completed errors such as EOF.
    const bool completed = WaitForSingleObject(completion, 0) == WAIT_OBJECT_0;
    CloseHandle(completion);
    if (aligned && result.Information) memcpy(base + ctx.r8.u32, aligned.get(), size_t(result.Information));
    ioStatus(ios, status, uint32_t(result.Information));
    if (completed) {
        if (apc & ~1u) queueGuestApc(ctx, apc, argument, ios);
        if (event) SetEvent(event->handle);
    }
    if (FileTraceEnabled() || int32_t(status) < 0)
        fprintf(stderr, "[File] read '%ls' offset=0x%llX bytes=%llu status=0x%08X\n",
                file->path.filename().c_str(), uint64_t(offset.QuadPart), uint64_t(result.Information), status);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__NtQueryInformationFile) {
    auto file = object(ctx.r3.u32);
    uint32_t ios = ctx.r4.u32, out = ctx.r5.u32, length = ctx.r6.u32, kind = ctx.r7.u32;
    if (ios && !fileGuestSpan(ios, 8, true)) { ctx.r3.u64 = accessViolation; return; }
    if (!file || !file->isFile) { ioStatus(ios, invalidHandle, 0); ctx.r3.u64 = invalidHandle; return; }
    uint32_t size;
    switch (kind) {
        case 4: size = 40; break; case 5: size = 24; break;
        case 6: case 14: size = 8; break; case 16: case 17: size = 4; break;
        case 34: size = 56; break;
        default: PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported file information class");
    }
    if (!out || length < size) { ioStatus(ios, 0xc0000004, 0); ctx.r3.u64 = 0xc0000004; return; }
    // The host query sees a temporary buffer, so validate the guest destination
    // before translating any fields or writing the completion status.
    if (!fileGuestSpan(out, length, true)) {
        ioStatus(ios, accessViolation, 0); ctx.r3.u64 = accessViolation; return;
    }
    alignas(8) uint8_t bytes[56]{};
    IO_STATUS_BLOCK result{};
    using Query = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    uint32_t status = uint32_t(nt<Query>(ctx, "NtQueryInformationFile")(file->handle, &result, bytes, size, FILE_INFORMATION_CLASS(kind)));
    if (int32_t(status) >= 0) {
        memset(base + out, 0, size);
        uint32_t qwords = kind == 4 ? 4 : kind == 5 ? 2 : kind == 34 ? 6 : (kind == 6 || kind == 14) ? 1 : 0;
        for (uint32_t i = 0; i < qwords; ++i) write64(out + i * 8, reinterpret_cast<uint64_t*>(bytes)[i]);
        if (kind == 4 || kind == 5 || kind == 34) memory->write32(out + qwords * 8, *reinterpret_cast<uint32_t*>(bytes + qwords * 8));
        if (kind == 5) memcpy(base + out + 20, bytes + 20, 2);
        if (kind == 16 || kind == 17) memory->write32(out, *reinterpret_cast<uint32_t*>(bytes));
    }
    ioStatus(ios, status, int32_t(status) >= 0 ? size : 0);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__NtSetInformationFile) {
    auto file = object(ctx.r3.u32);
    uint32_t ios = ctx.r4.u32, in = ctx.r5.u32, length = ctx.r6.u32, kind = ctx.r7.u32;
    if (ios && !fileGuestSpan(ios, 8, true)) { ctx.r3.u64 = accessViolation; return; }
    if (!file || !file->isFile) { ioStatus(ios, invalidHandle, 0); ctx.r3.u64 = invalidHandle; return; }
    // Proven save-finalization wrappers need 14 (position, preexisting), 20
    // (end-of-file) and 19 (allocation) plus 13 (disposition, used by
    // sub_828A9B00 to delete a chapter file before a new game). Kinds 19/20
    // take an 8-byte BE LARGE_INTEGER; kind 13 takes a 1-byte BOOLEAN (nonzero
    // marks delete-on-close honoring DELETE access, zero cancels). Other
    // classes stay guarded: no rename scope.
    if (kind != 13 && kind != 14 && kind != 19 && kind != 20)
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported set-file information class");
    uint32_t wantLength = kind == 13 ? 1 : 8;
    if (!in || length != wantLength) { ioStatus(ios, invalidParameter, 0); ctx.r3.u64 = invalidParameter; return; }
    // Reject inaccessible input/status before acquiring the lock or moving
    // the shared file position. A host SEH fault cannot unwind this lock_guard.
    if (!fileGuestSpan(in, length, false)) {
        ioStatus(ios, accessViolation, 0); ctx.r3.u64 = accessViolation; return;
    }
    // Disposition delete and size changes require a writable mount; plain
    // seeks stay allowed on read-only assets, which must never be routed to a
    // delete or size change. Handles opened without DELETE access are still
    // denied by the host call below.
    if (kind != 14 && !file->writable) {
        ioStatus(ios, writeProtected, 0); ctx.r3.u64 = writeProtected; return;
    }
    alignas(8) uint8_t input[8]{};
    if (kind == 13) {
        input[0] = base[in] ? 1 : 0;
    } else {
        int64_t value = int64_t(read64(in));
        if (value < 0 && kind != 14) {
            ioStatus(ios, invalidParameter, 0); ctx.r3.u64 = invalidParameter; return;
        }
        memcpy(input, &value, 8);
    }
    std::lock_guard lock(file->ioMutex);
    IO_STATUS_BLOCK result{};
    using Set = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    uint32_t status = uint32_t(nt<Set>(ctx, "NtSetInformationFile")(file->handle, &result, input, wantLength, FILE_INFORMATION_CLASS(kind)));
    // The set path carries no event/APC parameters; if the native call pends,
    // block on the file handle with the stack IO_STATUS_BLOCK still alive.
    // Never return success or pending with a stack IOS escaping: completion
    // must be proven before reporting it.
    if (status == pending) {
        DWORD waitResult = WaitForSingleObject(file->handle, INFINITE);
        status = uint32_t(result.Status);
        if (waitResult != WAIT_OBJECT_0 || status == pending)
            PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "native set-file completion never signaled");
    }
    ioStatus(ios, status, uint32_t(result.Information)); ctx.r3.u64 = status;
}
PPC_FUNC(__imp__NtQueryDirectoryFile) {
    uint32_t ios = ctx.r7.u32, out = ctx.r8.u32, length = ctx.r9.u32;
    if (ios && !fileGuestSpan(ios, 8, true)) { ctx.r3.u64 = accessViolation; return; }
    auto file = object(ctx.r3.u32), event = ctx.r4.u32 ? object(ctx.r4.u32) : nullptr;
    if (!file || !file->isFile || (ctx.r4.u32 && (!event || !event->isEvent))) {
        ioStatus(ios, invalidHandle, 0); ctx.r3.u64 = invalidHandle; return;
    }
    if (!ios || !out || length < 64) { ioStatus(ios, 0xc0000004, 0); ctx.r3.u64 = 0xc0000004; return; }
    // The native query writes into a host buffer and advances its cursor.
    // Validate all guest spans before submitting it or consuming a cached entry.
    const uint64_t restartAddress = uint64_t(ctx.r1.u32) + 84;
    if (!fileGuestSpan(out, length, true) || restartAddress > UINT32_MAX ||
        !fileGuestSpan(uint32_t(restartAddress), 4, false) ||
        (ctx.r10.u32 && !fileGuestSpan(ctx.r10.u32, 8, false))) {
        ioStatus(ios, accessViolation, 0); ctx.r3.u64 = accessViolation; return;
    }
    std::wstring pattern;
    if (ctx.r10.u32) {
        uint32_t descriptor = ctx.r10.u32, count = memory->read32(descriptor) >> 16;
        uint32_t pointer = memory->read32(descriptor + 4);
        if (count && !fileGuestSpan(pointer, count, false)) {
            ioStatus(ios, accessViolation, 0); ctx.r3.u64 = accessViolation; return;
        }
        for (uint32_t i = 0; i < count; ++i) pattern.push_back(base[pointer + i]);
    }
    UNICODE_STRING filter{USHORT(pattern.size() * 2), USHORT(pattern.size() * 2), pattern.data()};
    std::lock_guard lock(file->ioMutex);
    alignas(8) uint8_t native[64 + 65536]{};
    const uint8_t* record = native;
    const bool restart = memory->read32(ctx.r1.u32 + 84) != 0;
    uint32_t status = 0;
    bool completed = true; // A retained entry completes without another host query.
    if (restart || file->pendingDirectoryEntry.empty()) {
        HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completion) { ioStatus(ios, 0xc0000017, 0); ctx.r3.u64 = 0xc0000017; return; }
        IO_STATUS_BLOCK result{};
        using Query = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID,
            ULONG, FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
        status = uint32_t(nt<Query>(ctx, "NtQueryDirectoryFile")(file->handle, completion, nullptr, nullptr,
            &result, native, sizeof(native), FILE_INFORMATION_CLASS(1), TRUE, pattern.empty() ? nullptr : &filter, restart));
        if (status == pending) { WaitForSingleObject(completion, INFINITE); status = uint32_t(result.Status); }
        completed = WaitForSingleObject(completion, 0) == WAIT_OBJECT_0;
        CloseHandle(completion);
        // A restart supersedes the retained entry, including an empty search.
        // Leave it intact when the restart request itself could not execute.
        if (restart && (int32_t(status) >= 0 || status == 0x80000006 || status == 0xc000000f))
            file->pendingDirectoryEntry.clear();
    } else record = file->pendingDirectoryEntry.data();
    uint32_t written = 0;
    if (int32_t(status) >= 0) {
        uint32_t nameBytes = *reinterpret_cast<const uint32_t*>(record + 60);
        if (nameBytes > sizeof(native) - 64 || (nameBytes & 1)) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "invalid native directory record");
        auto* name = reinterpret_cast<const wchar_t*>(record + 64);
        int count = WideCharToMultiByte(CP_UTF8, 0, name, nameBytes / 2, nullptr, 0, nullptr, nullptr);
        std::string utf8(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, name, nameBytes / 2, utf8.data(), count, nullptr, nullptr);
        written = 64 + (std::min)(length - 64, uint32_t(utf8.size()));
        memset(base + out, 0, written);
        memory->write32(out + 4, *reinterpret_cast<const uint32_t*>(record + 4));
        for (uint32_t i = 1; i <= 6; ++i) write64(out + i * 8, reinterpret_cast<const uint64_t*>(record)[i]);
        memory->write32(out + 56, *reinterpret_cast<const uint32_t*>(record + 56));
        memory->write32(out + 60, uint32_t(utf8.size()));
        memcpy(base + out + 64, utf8.data(), written - 64);
        if (written - 64 < utf8.size()) {
            status = 0x80000005;
            if (record == native) file->pendingDirectoryEntry.assign(native, native + 64 + nameBytes);
        } else file->pendingDirectoryEntry.clear();
        if (FileTraceEnabled() || int32_t(status) < 0)
            fprintf(stderr, "[File] directory '%ls': %s\n", file->path.c_str(), utf8.c_str());
    }
    ioStatus(ios, status, written);
    if (completed) {
        if (ctx.r5.u32 & ~1u) queueGuestApc(ctx, ctx.r5.u32, ctx.r6.u32, ios);
        if (event) SetEvent(event->handle);
    }
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__NtWriteFile) {
    uint32_t ios = ctx.r7.u32;
    if (!ios) { ctx.r3.u64 = invalidParameter; return; }
    if (!fileGuestSpan(ios, 8, true)) { ctx.r3.u64 = accessViolation; return; }
    auto file = object(ctx.r3.u32), event = ctx.r4.u32 ? object(ctx.r4.u32) : nullptr;
    if (!file || !file->isFile || (ctx.r4.u32 && (!event || !event->isEvent))) {
        ioStatus(ios, invalidHandle, 0); ctx.r3.u64 = invalidHandle; return;
    }
    if (!file->writable) {
        ioStatus(ios, writeProtected, 0); ctx.r3.u64 = writeProtected; return;
    }
    if (!ctx.r8.u32 && ctx.r9.u32) {
        ioStatus(ios, invalidParameter, 0); ctx.r3.u64 = invalidParameter; return;
    }
    if ((ctx.r9.u32 && !fileGuestSpan(ctx.r8.u32, ctx.r9.u32, false)) ||
        (ctx.r10.u32 && !fileGuestSpan(ctx.r10.u32, 8, false))) {
        ioStatus(ios, accessViolation, 0); ctx.r3.u64 = accessViolation; return;
    }
    uint32_t apc = ctx.r5.u32, argument = ctx.r6.u32;
    std::lock_guard lock(file->ioMutex);
    HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completion) { ioStatus(ios, 0xc0000017, 0); ctx.r3.u64 = 0xc0000017; return; }
    LARGE_INTEGER offset{};
    if (ctx.r10.u32) offset.QuadPart = read64(ctx.r10.u32);
    IO_STATUS_BLOCK result{};
    std::unique_ptr<void, decltype(&_aligned_free)> aligned(nullptr, _aligned_free);
    const void* source = base + ctx.r8.u32;
    if (file->unbuffered && ctx.r9.u32) {
        aligned.reset(_aligned_malloc(ctx.r9.u32 ? ctx.r9.u32 : 1, 4096));
        if (!aligned) { CloseHandle(completion); ioStatus(ios, 0xc0000017, 0); ctx.r3.u64 = 0xc0000017; return; }
        if (ctx.r9.u32) memcpy(aligned.get(), base + ctx.r8.u32, ctx.r9.u32);
        source = aligned.get();
    }
    using Write = NTSTATUS (NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
    uint32_t status = uint32_t(nt<Write>(ctx, "NtWriteFile")(file->handle, completion, nullptr, nullptr, &result,
        const_cast<void*>(source), ctx.r9.u32, ctx.r10.u32 ? &offset : nullptr, nullptr));
    if (status == pending) {
        WaitForSingleObject(completion, INFINITE);
        status = uint32_t(result.Status);
    }
    const bool completed = WaitForSingleObject(completion, 0) == WAIT_OBJECT_0;
    CloseHandle(completion);
    ioStatus(ios, status, uint32_t(result.Information));
    if (completed) {
        if (apc & ~1u) queueGuestApc(ctx, apc, argument, ios);
        if (event) SetEvent(event->handle);
    }
    if (FileTraceEnabled() || int32_t(status) < 0)
        fprintf(stderr, "[File] write '%ls' offset=0x%llX bytes=%llu status=0x%08X\n",
                file->path.filename().c_str(), uint64_t(offset.QuadPart), uint64_t(result.Information), status);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__NtFlushBuffersFile) {
    auto file = object(ctx.r3.u32);
    uint32_t ios = ctx.r4.u32;
    if (!ios) { ctx.r3.u64 = invalidParameter; return; }
    if (!fileGuestSpan(ios, 8, true)) { ctx.r3.u64 = accessViolation; return; }
    if (!file || !file->isFile) { ioStatus(ios, invalidHandle, 0); ctx.r3.u64 = invalidHandle; return; }
    std::lock_guard lock(file->ioMutex);
    IO_STATUS_BLOCK result{};
    using Flush = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK);
    uint32_t status = uint32_t(nt<Flush>(ctx, "NtFlushBuffersFile")(file->handle, &result));
    ioStatus(ios, status, 0);
    if (FileTraceEnabled() || int32_t(status) < 0)
        fprintf(stderr, "[File] flush '%ls' status=0x%08X\n", file->path.filename().c_str(), status);
    ctx.r3.u64 = status;
}
