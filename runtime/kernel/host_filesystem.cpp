#include "host_filesystem.h"
#include <windows.h>
#include <dbghelp.h>
#include <iostream>
#include <unordered_map>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <mutex>
#include <string>

namespace DarkRecomp {

struct HostFile {
    HANDLE hFile;
    uint64_t fileSize;
    std::string path;
};

static std::unordered_map<uint32_t, HostFile> g_openFiles;
static uint32_t g_nextHandle = 0x1000;
static CRITICAL_SECTION g_fsLock;

void InitializeFileSystem() {
    InitializeCriticalSection(&g_fsLock);
    std::cout << "[FS] Initialized native Xbox 360 File System Bridge." << std::endl;
}

static std::string DecodeGuestUnicodeString(uint32_t bufferPtr, uint16_t byteLen, uint8_t* base) {
    // Xbox 360 guest memory is big-endian: UTF-16 code units are stored BE.
    // Convert to UTF-8/ANSI (game paths are ASCII subset).
    if (!bufferPtr || byteLen == 0) return "";
    // Clamp to sane length to avoid OOB on corrupt guest pointers.
    if (byteLen > 4096) byteLen = 4096;
    uint16_t charCount = byteLen / 2;
    std::string out;
    out.reserve(charCount);
    uint8_t* buf = base + bufferPtr;
    for (uint16_t i = 0; i < charCount; i++) {
        uint16_t be = (static_cast<uint16_t>(buf[i * 2]) << 8) | buf[i * 2 + 1];
        // Fast path: ASCII subset
        if (be < 0x80) {
            out.push_back(static_cast<char>(be));
        } else if (be < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (be >> 6)));
            out.push_back(static_cast<char>(0x80 | (be & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (be >> 12)));
            out.push_back(static_cast<char>(0x80 | ((be >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (be & 0x3F)));
        }
    }
    return out;
}

static std::string DecodeGuestAnsiString(uint32_t bufferPtr, uint16_t byteLen, uint8_t* base) {
    if (!bufferPtr || byteLen == 0) return "";
    if (byteLen > 4096) byteLen = 4096;
    const char* s = reinterpret_cast<const char*>(base + bufferPtr);
    size_t n = 0;
    while (n < byteLen && s[n] != '\0') n++;
    return std::string(s, n);
}

static std::string ExtractGuestPath(uint32_t objectAttributesPtr, uint8_t* base) {
    if (!objectAttributesPtr) return "";
    uint8_t* pAttr = base + objectAttributesPtr;
    // Primary: OBJECT_ATTRIBUTES (32-bit guest):
    //   +0x00 ULONG Length
    //   +0x04 HANDLE RootDirectory
    //   +0x08 PUNICODE_STRING ObjectName
    //   +0x0C ULONG Attributes
    uint32_t namePtr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pAttr + 8));
    if (namePtr) {
        uint8_t* pName = base + namePtr;
        // UNICODE_STRING (32-bit guest):
        //   +0x00 USHORT Length (bytes, BE)
        //   +0x02 USHORT MaximumLength (BE)
        //   +0x04 PWSTR Buffer (BE)
        uint16_t length = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pName + 0));
        uint32_t bufferPtr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pName + 4));
        if (bufferPtr && length != 0) {
            std::string uni = DecodeGuestUnicodeString(bufferPtr, length, base);
            if (!uni.empty()) return uni;
        }
    }
    // Fallback: LIBCMT CRT open struct used by sub_828ABF60-family callers
    // (observed: [-3, ANSI_STRING*, 64] at PATTR). The import thunk at
    // 0x829C6054/0x829C60B4 funnels these here. Read ANSI_STRING at +4:
    //   +0x00 USHORT Length (bytes, BE, ANSI chars)
    //   +0x02 USHORT MaximumLength (BE)
    //   +0x04 PCHAR Buffer (BE, guest ANSI bytes)
    __try {
        uint32_t ansiPtr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pAttr + 4));
        if (ansiPtr) {
            uint8_t* pA = base + ansiPtr;
            uint16_t alen = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pA + 0));
            uint32_t abuf = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 4));
            if (abuf && alen != 0) {
                return DecodeGuestAnsiString(abuf, alen, base);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}

static std::string ResolveGuestPath(const std::string& raw) {
    std::string p = raw;
    // Normalize separators first so prefix matching is stable.
    for (char& c : p) {
        if (c == '\\') c = '/';
    }

    const char* prefixes[] = {
        "/Device/Cdrom0/",
        "Device/Cdrom0/",
        "/Device/Dvd0/",
        "Device/Dvd0/",
        "/Device/Harddisk0/Partition1/",
        "Device/Harddisk0/Partition1/",
        "game:/",
        "game:/",
        "d:/",
        "D:/",
        "devkit:/",
        "e:/",
        nullptr
    };

    for (int i = 0; prefixes[i]; i++) {
        size_t pos = p.find(prefixes[i]);
        if (pos != std::string::npos) {
            p = p.substr(pos + strlen(prefixes[i]));
            break;
        }
    }
    // Also strip a bare "game:" / "d:" style prefix without slash.
    if (p.size() > 5 && (p.compare(0, 5, "game:") == 0 || p.compare(0, 2, "d:") == 0)) {
        size_t colon = p.find(':');
        if (colon != std::string::npos) p = p.substr(colon + 1);
    }

    while (!p.empty() && (p[0] == '/' || p[0] == '\\' || p[0] == ':')) {
        p.erase(0, 1);
    }

    // Try candidates in Darkness directory
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::path("Darkness") / p,
        std::filesystem::path("Darkness/Content") / p,
        std::filesystem::path("Darkness/Content_Eng") / p,
        std::filesystem::path(p)
    };

    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) {
            return c.string();
        }
    }

    // Return default target even if not yet created (for write/open)
    return (std::filesystem::path("Darkness") / p).string();
}

void Hook_NtOpenFile(PPCContext& ctx, uint8_t* base) {
    uint32_t pHandle = ctx.r3.u32;
    uint32_t access = ctx.r4.u32;
    uint32_t pAttr = ctx.r5.u32;
    uint32_t pStatus = ctx.r6.u32;

    std::string rawPath = ExtractGuestPath(pAttr, base);
    std::string resolved = ResolveGuestPath(rawPath);

    if (rawPath.empty()) {
        // Diagnostic: dump OBJECT_ATTRIBUTES + UNICODE_STRING fields so we can
        // see why the path is empty (RootDirectory-relative open? bad offset?).
        // Also dump all PPC arg regs to verify Xbox arg order.
        __try {
            uint8_t* pA = base + pAttr;
            uint32_t lenA = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 0));
            uint32_t root = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 4));
            uint32_t nameP = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 8));
            uint32_t attr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + 12));
            std::cout << "[FS] NtOpenFile empty path diag: pAttr=0x" << std::hex << pAttr
                      << " Len=0x" << lenA << " Root=0x" << root << " NamePtr=0x" << nameP
                      << " Attr=0x" << attr
                      << " | regs r3=0x" << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                      << " r5=0x" << ctx.r5.u32 << " r6=0x" << ctx.r6.u32
                      << " r7=0x" << ctx.r7.u32 << " r8=0x" << ctx.r8.u32
                      << " r9=0x" << ctx.r9.u32 << " r10=0x" << ctx.r10.u32
                      << " | mem:";
            for (int i = 0; i < 8; i++) {
                uint32_t w = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pA + i * 4));
                char tmp[16]; sprintf_s(tmp, " [%d]=0x%x", i, w);
                std::cout << tmp;
            }
            if (nameP) {
                uint8_t* pN = base + nameP;
                uint16_t ulen = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pN + 0));
                uint16_t umax = _byteswap_ushort(*reinterpret_cast<uint16_t*>(pN + 2));
                uint32_t ubuf = _byteswap_ulong(*reinterpret_cast<uint32_t*>(pN + 4));
                std::cout << " ULen=" << std::dec << ulen << " UMax=" << umax
                          << " UBuf=0x" << std::hex << ubuf;
                if (ubuf) {
                    std::cout << " UBytes:";
                    for (int i = 0; i < 16; i++) {
                        char tmp[8]; sprintf_s(tmp, " %02X", (base + ubuf)[i]);
                        std::cout << tmp;
                    }
                }
            }
            std::cout << std::dec << std::endl;
            // First empty-path call: dump host stack once to identify the PPC
            // caller (which wrapper builds this struct layout).
            {
                static uint64_t s_fsStackLogged = 0;
                static std::mutex s_fsStackMutex;
                std::lock_guard<std::mutex> lk(s_fsStackMutex);
                if (++s_fsStackLogged == 1) {
                    HANDLE hProcess = GetCurrentProcess();
                    HANDLE hThread = GetCurrentThread();
                    SymInitialize(hProcess, nullptr, TRUE);
                    CONTEXT hc = {};
                    RtlCaptureContext(&hc);
                    STACKFRAME64 sf = {};
                    sf.AddrPC.Offset = hc.Rip;
                    sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = hc.Rbp;
                    sf.AddrFrame.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = hc.Rsp;
                    sf.AddrStack.Mode = AddrModeFlat;
                    std::cout << "[FS] NtOpenFile caller stack:" << std::endl;
                    for (int frame = 0; frame < 10; frame++) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &hc,
                                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                            break;
                        if (sf.AddrPC.Offset == 0) break;
                        char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
                        PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
                        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        pSym->MaxNameLen = 256;
                        DWORD64 disp = 0;
                        std::string nm;
                        if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) nm = pSym->Name;
                        else { char tmp[32]; sprintf_s(tmp, "0x%llX", sf.AddrPC.Offset); nm = tmp; }
                        std::cout << "   [F#" << frame << "] " << nm << "+0x" << std::hex << disp << std::dec << std::endl;
                    }
                    std::cout.flush();
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::cout << "[FS] NtOpenFile empty path diag: exception reading guest structs" << std::endl;
        }
    }

    DWORD desiredAccess = GENERIC_READ;
    if (access & 0x0002) desiredAccess |= GENERIC_WRITE;

    HANDLE h = CreateFileA(
        resolved.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h == INVALID_HANDLE_VALUE) {
        std::cout << "[FS] NtOpenFile FAILED: \"" << rawPath << "\" -> \"" << resolved << "\"" << std::endl;
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000034); // STATUS_OBJECT_NAME_NOT_FOUND
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000034;
        return;
    }

    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);

    EnterCriticalSection(&g_fsLock);
    uint32_t guestHandle = g_nextHandle++;
    g_openFiles[guestHandle] = { h, (uint64_t)sz.QuadPart, resolved };
    LeaveCriticalSection(&g_fsLock);

    std::cout << "[FS] NtOpenFile: \"" << rawPath << "\" -> \"" << resolved << "\" (Handle=0x" 
              << std::hex << guestHandle << ", Size=" << std::dec << sz.QuadPart << " bytes)" << std::endl;

    if (pHandle) {
        *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(guestHandle);
    }
    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0; // STATUS_SUCCESS
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(1); // FILE_OPENED
    }
    ctx.r3.u64 = 0;
}

void Hook_NtCreateFile(PPCContext& ctx, uint8_t* base) {
    uint32_t pHandle = ctx.r3.u32;
    uint32_t access = ctx.r4.u32;
    uint32_t pAttr = ctx.r5.u32;
    uint32_t pStatus = ctx.r6.u32;
    uint32_t disposition = ctx.r10.u32;

    std::string rawPath = ExtractGuestPath(pAttr, base);
    std::string resolved = ResolveGuestPath(rawPath);

    DWORD dwDisposition = OPEN_ALWAYS;
    if (disposition == 1) dwDisposition = OPEN_EXISTING;
    else if (disposition == 2) dwDisposition = CREATE_ALWAYS;

    DWORD desiredAccess = GENERIC_READ;
    if (access & 0x0002) desiredAccess |= GENERIC_WRITE;

    HANDLE h = CreateFileA(
        resolved.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        dwDisposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h == INVALID_HANDLE_VALUE) {
        std::cout << "[FS] NtCreateFile FAILED: \"" << rawPath << "\" -> \"" << resolved << "\"" << std::endl;
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000034);
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000034;
        return;
    }

    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);

    EnterCriticalSection(&g_fsLock);
    uint32_t guestHandle = g_nextHandle++;
    g_openFiles[guestHandle] = { h, (uint64_t)sz.QuadPart, resolved };
    LeaveCriticalSection(&g_fsLock);

    std::cout << "[FS] NtCreateFile: \"" << rawPath << "\" -> \"" << resolved << "\" (Handle=0x"
              << std::hex << guestHandle << ", Size=" << std::dec << sz.QuadPart << " bytes)" << std::endl;

    if (pHandle) {
        *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(guestHandle);
    }
    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(1);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtReadFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r7.u32;
    uint32_t pBuffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t pOffset = ctx.r10.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        if (pStatus) {
            *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000008); // STATUS_INVALID_HANDLE
            *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
        }
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    DWORD bytesRead = 0;
    BOOL ok = FALSE;

    if (pOffset != 0) {
        uint64_t offset = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset));
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ok = ReadFile(file.hFile, base + pBuffer, length, &bytesRead, &ov);
    } else {
        ok = ReadFile(file.hFile, base + pBuffer, length, &bytesRead, nullptr);
    }

    if (!ok && bytesRead == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            if (pStatus) {
                *reinterpret_cast<uint32_t*>(base + pStatus + 0) = _byteswap_ulong(0xC0000011); // STATUS_END_OF_FILE
                *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
            }
            ctx.r3.u64 = 0xC0000011;
            return;
        }
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0; // STATUS_SUCCESS
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(bytesRead);
    }
    {
        static uint64_t s_readLogged = 0;
        static CRITICAL_SECTION s_readLock;
        static bool s_readLockInit = false;
        if (!s_readLockInit) { InitializeCriticalSection(&s_readLock); s_readLockInit = true; }
        EnterCriticalSection(&s_readLock);
        bool log = (++s_readLogged <= 3);
        LeaveCriticalSection(&s_readLock);
        if (log) {
            EnterCriticalSection(&g_fsLock);
            std::string nm = "?";
            auto it2 = g_openFiles.find(handle);
            if (it2 != g_openFiles.end()) nm = it2->second.path;
            LeaveCriticalSection(&g_fsLock);
            std::cout << "[FS] NtReadFile # " << s_readLogged << " handle=0x" << std::hex << handle
                      << " guestBuf=0x" << pBuffer << std::dec << " len=" << length
                      << " got=" << bytesRead << " \"" << nm << "\"" << std::endl;
            std::cout.flush();
        }
    }
    ctx.r3.u64 = 0;
}

void Hook_NtReadFileScatter(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r7.u32;
    uint32_t pSegmentArray = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t pOffset = ctx.r10.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    uint64_t currentOffset = 0;
    if (pOffset) {
        currentOffset = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset));
    }

    uint32_t bytesRemaining = length;
    uint32_t segmentIdx = 0;
    uint32_t totalBytesRead = 0;

    while (bytesRemaining > 0) {
        uint32_t pageAddr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + pSegmentArray + (segmentIdx * 8)));
        if (!pageAddr) break;

        DWORD chunk = (std::min)(bytesRemaining, 4096u);
        DWORD readThisChunk = 0;
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(currentOffset);
        ov.OffsetHigh = static_cast<DWORD>(currentOffset >> 32);

        BOOL ok = ReadFile(file.hFile, base + pageAddr, chunk, &readThisChunk, &ov);
        if (!ok || readThisChunk == 0) break;

        totalBytesRead += readThisChunk;
        currentOffset += readThisChunk;
        bytesRemaining -= readThisChunk;
        segmentIdx++;
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(totalBytesRead);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtWriteFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r7.u32;
    uint32_t pBuffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t pOffset = ctx.r10.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    DWORD bytesWritten = 0;
    BOOL ok = FALSE;
    if (pOffset) {
        uint64_t offset = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pOffset));
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ok = WriteFile(file.hFile, base + pBuffer, length, &bytesWritten, &ov);
    } else {
        ok = WriteFile(file.hFile, base + pBuffer, length, &bytesWritten, nullptr);
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = ok ? 0 : 0xC0000001;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(bytesWritten);
    }
    ctx.r3.u64 = ok ? 0 : 0xC0000001;
}

void Hook_NtClose(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it != g_openFiles.end()) {
        CloseHandle(it->second.hFile);
        g_openFiles.erase(it);
    }
    LeaveCriticalSection(&g_fsLock);
    ctx.r3.u64 = 0;
}

void Hook_NtSetInformationFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r4.u32;
    uint32_t pInfo = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    // Class 4: FilePositionInformation
    if (infoClass == 4 && length >= 8 && pInfo) {
        uint64_t newPos = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pInfo));
        LARGE_INTEGER li;
        li.QuadPart = newPos;
        SetFilePointerEx(file.hFile, li, nullptr, FILE_BEGIN);
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = 0;
    }
    ctx.r3.u64 = 0;
}

void Hook_NtQueryInformationFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    uint32_t pStatus = ctx.r4.u32;
    uint32_t pInfo = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it == g_openFiles.end()) {
        LeaveCriticalSection(&g_fsLock);
        ctx.r3.u64 = 0xC0000008;
        return;
    }
    HostFile file = it->second;
    LeaveCriticalSection(&g_fsLock);

    uint32_t written = 0;

    // Class 4: FilePositionInformation
    if (infoClass == 4 && pInfo && length >= 8) {
        LARGE_INTEGER zero = {}, cur = {};
        SetFilePointerEx(file.hFile, zero, &cur, FILE_CURRENT);
        *reinterpret_cast<uint64_t*>(base + pInfo) = _byteswap_uint64(cur.QuadPart);
        written = 8;
    }
    // Class 5: FileStandardInformation
    else if (infoClass == 5 && pInfo && length >= 24) {
        LARGE_INTEGER sz;
        GetFileSizeEx(file.hFile, &sz);
        uint8_t* out = base + pInfo;
        memset(out, 0, 24);
        *reinterpret_cast<uint64_t*>(out + 0) = _byteswap_uint64(sz.QuadPart); // AllocationSize
        *reinterpret_cast<uint64_t*>(out + 8) = _byteswap_uint64(sz.QuadPart); // EndOfFile
        *reinterpret_cast<uint32_t*>(out + 16) = _byteswap_ulong(1); // NumberOfLinks
        out[20] = 0; // DeletePending
        out[21] = 0; // Directory
        written = 24;
    }

    if (pStatus) {
        *reinterpret_cast<uint32_t*>(base + pStatus + 0) = 0;
        *reinterpret_cast<uint32_t*>(base + pStatus + 4) = _byteswap_ulong(written);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtQueryFullAttributesFile(PPCContext& ctx, uint8_t* base) {
    uint32_t pAttr = ctx.r3.u32;
    uint32_t pInfo = ctx.r4.u32;

    std::string rawPath = ExtractGuestPath(pAttr, base);
    std::string resolved = ResolveGuestPath(rawPath);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(resolved.c_str(), GetFileExInfoStandard, &fad)) {
        ctx.r3.u64 = 0xC0000034; // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }

    if (pInfo) {
        uint8_t* out = base + pInfo;
        memset(out, 0, 0x34);
        uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        *reinterpret_cast<uint64_t*>(out + 0x20) = _byteswap_uint64(sz); // AllocationSize
        *reinterpret_cast<uint64_t*>(out + 0x28) = _byteswap_uint64(sz); // EndOfFile
        *reinterpret_cast<uint32_t*>(out + 0x30) = _byteswap_ulong(fad.dwFileAttributes);
    }
    ctx.r3.u64 = 0;
}

void Hook_NtFlushBuffersFile(PPCContext& ctx, uint8_t* base) {
    uint32_t handle = ctx.r3.u32;
    EnterCriticalSection(&g_fsLock);
    auto it = g_openFiles.find(handle);
    if (it != g_openFiles.end()) {
        FlushFileBuffers(it->second.hFile);
    }
    LeaveCriticalSection(&g_fsLock);
    ctx.r3.u64 = 0;
}

} // namespace DarkRecomp
