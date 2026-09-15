#include "kernel_imports.h"
#include "host_filesystem.h"
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <xinput.h>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstring>
#include "renderer/d3d11/display_context_d3d11.h"
#include "ppc/ppc_context.h"

// Forward declare PPC_FUNC signature
#define PPC_FUNC(x) void x(PPCContext& __restrict ctx, uint8_t* base)

// Originals for diagnostic heap hooks (defined in generated PPC lib).
void __imp__sub_821080E8(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82107EA0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821F0B20(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82216B98(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822152C8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820C88B0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8210A738(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8289FEF8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82216270(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82214030(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82216088(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827A72D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821F35F8(PPCContext& __restrict ctx, uint8_t* base);

// Global guest heap pointer for dynamic allocations
// ARENA: [0x10000000, 0x60000000). NEVER grow past 0x60000000: above it live
// guest stacks/PCR (0x700xxxxx), the image (0x82000000), and the dispatch
// table (0x82B10000+). A 15-min soak proved unbounded bump allocs (640k+
// calls from retry loops) march through and zero those regions, silently
// destroying stacks/dispatch. All bump sites must respect the cap.
static uint32_t g_guestHeapOffset = 0x10000000; // 256MB mark in guest RAM
static constexpr uint32_t kGuestHeapCap = 0x60000000;
// Recursive: hooks nest (e.g., terminate diagnostics call back into logging
// paths); a non-recursive mutex deadlocks the bring-up on re-entry. All
// critical sections below are short (counter bumps, handle-map ops) except
// where noted; guest-memory touches stay outside the lock so an SEH fault
// (no C++ unwind under /EHsc) can never leak it held.
static std::recursive_mutex g_kernelMutex;

// --- Import call tracing (throttled to avoid log explosion on spin loops) ---
static std::unordered_map<std::string, uint64_t> g_importCallCounts;
static std::mutex g_traceMutex;
static void TraceImport(const char* name) {
    std::lock_guard<std::mutex> lock(g_traceMutex);
    uint64_t& c = g_importCallCounts[name];
    c++;
    // Log first 3 calls per import + every 10000th to catch spins without flooding.
    if (c <= 3 || (c % 10000) == 0) {
        std::cout << "[KernelTrace] " << name << " call #" << c << std::endl;
        std::cout.flush();
    }
}
#define KTRACE() do { TraceImport(__func__); CheckRdata(base, __func__); } while (0)

// Forward: .rdata integrity probe (defined with AllocGuestHeap below).
static void CheckRdata(uint8_t* base, const char* who);

// 1. Thread Local Storage
static uint32_t g_tlsBitmap = 0;
PPC_FUNC(__imp__KeTlsAlloc) {
    KTRACE();
    CheckRdata(base, "__imp__KeTlsAlloc");
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    for (uint32_t i = 0; i < 64; i++) {
        if (!(g_tlsBitmap & (1ULL << i))) {
            g_tlsBitmap |= (1ULL << i);
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
}

PPC_FUNC(__imp__KeTlsFree) {
    KTRACE();
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    uint32_t index = ctx.r3.u32;
    if (index < 64) {
        g_tlsBitmap &= ~(1ULL << index);
        ctx.r3.u64 = 1; // TRUE
    } else {
        ctx.r3.u64 = 0; // FALSE
    }
}

PPC_FUNC(__imp__KeTlsSetValue) {
    KTRACE();
    uint32_t index = ctx.r3.u32;
    uint32_t value = ctx.r4.u32;
    // PCR is pointed to by r13
    uint32_t tlsPtrGuest = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + ctx.r13.u32));
    if (tlsPtrGuest && index < 64) {
        *reinterpret_cast<uint32_t*>(base + tlsPtrGuest + (index * 4)) = _byteswap_ulong(value);
        ctx.r3.u64 = 1; // TRUE
    } else {
        ctx.r3.u64 = 0; // FALSE
    }
}

PPC_FUNC(__imp__KeTlsGetValue) {
    KTRACE();
    uint32_t index = ctx.r3.u32;
    uint32_t tlsPtrGuest = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + ctx.r13.u32));
    if (tlsPtrGuest && index < 64) {
        ctx.r3.u64 = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + tlsPtrGuest + (index * 4)));
    } else {
        ctx.r3.u64 = 0;
    }
}

// 2. Critical Section Synchronization
// NOTE: CRITICAL_SECTION cannot be copied. Store heap-allocated instances
// keyed by guest address and initialize them in place.
static std::unordered_map<uint32_t, CRITICAL_SECTION*> g_critSecMap;

static CRITICAL_SECTION* GetOrCreateCritSec(uint32_t guestCs, DWORD spinCount, bool useSpin) {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    auto it = g_critSecMap.find(guestCs);
    if (it != g_critSecMap.end()) return it->second;
    CRITICAL_SECTION* cs = new CRITICAL_SECTION();
    if (useSpin) InitializeCriticalSectionAndSpinCount(cs, spinCount);
    else InitializeCriticalSection(cs);
    g_critSecMap[guestCs] = cs;
    return cs;
}

static CRITICAL_SECTION* FindCritSec(uint32_t guestCs) {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    auto it = g_critSecMap.find(guestCs);
    return (it != g_critSecMap.end()) ? it->second : nullptr;
}

PPC_FUNC(__imp__RtlInitializeCriticalSection) {
    KTRACE();
    uint32_t guestCs = ctx.r3.u32;
    GetOrCreateCritSec(guestCs, 0, false);
    ctx.r3.u64 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) {
    KTRACE();
    uint32_t guestCs = ctx.r3.u32;
    uint32_t spinCount = ctx.r4.u32;
    GetOrCreateCritSec(guestCs, spinCount, true);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlEnterCriticalSection) {
    KTRACE();
    uint32_t guestCs = ctx.r3.u32;
    CRITICAL_SECTION* cs = FindCritSec(guestCs);
    if (!cs) {
        // Auto-create on first use: retail code sometimes enters without
        // explicit init on all paths (static initializers).
        cs = GetOrCreateCritSec(guestCs, 0, false);
    }
    EnterCriticalSection(cs);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    KTRACE();
    uint32_t guestCs = ctx.r3.u32;
    CRITICAL_SECTION* cs = FindCritSec(guestCs);
    if (cs) {
        LeaveCriticalSection(cs);
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    KTRACE();
    uint32_t guestCs = ctx.r3.u32;
    CRITICAL_SECTION* cs = FindCritSec(guestCs);
    if (!cs) cs = GetOrCreateCritSec(guestCs, 0, false);
    ctx.r3.u64 = TryEnterCriticalSection(cs);
}

// 2b. RTL string helpers (bring-up: these feed NtOpen/CreateFile paths).
// Without them ANSI_STRING/UNICODE_STRING stay zeroed and every file open
// resolves to "" (observed: sub_828AB450 -> RtlInitAnsiString (was weak
// no-op) -> sub_828ABF60 -> NtOpenFile with empty path).
PPC_FUNC(__imp__RtlInitAnsiString) {
    // RtlInitAnsiString(DestinationString=r3 (ANSI_STRING*), SourceString=r4 (char*))
    uint32_t pDest = ctx.r3.u32;
    uint32_t pSrc = ctx.r4.u32;
    if (pDest) {
        uint16_t len = 0, maxlen = 1;
        uint32_t buf = 0;
        if (pSrc) {
            const char* s = (const char*)(base + pSrc);
            size_t n = 0;
            while (n < 4096 && s[n] != '\0') n++;
            len = (uint16_t)n;
            maxlen = (uint16_t)(n + 1);
            buf = pSrc;
        }
        *reinterpret_cast<uint16_t*>(base + pDest + 0) = _byteswap_ushort(len);
        *reinterpret_cast<uint16_t*>(base + pDest + 2) = _byteswap_ushort(maxlen);
        *reinterpret_cast<uint32_t*>(base + pDest + 4) = _byteswap_ulong(buf);
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlInitUnicodeString) {
    // RtlInitUnicodeString(DestinationString=r3 (UNICODE_STRING*),
    // SourceString=r4 (wchar_t* BE in guest)). Length in bytes.
    uint32_t pDest = ctx.r3.u32;
    uint32_t pSrc = ctx.r4.u32;
    if (pDest) {
        uint16_t len = 0, maxlen = 2;
        uint32_t buf = 0;
        if (pSrc) {
            uint8_t* s = base + pSrc;
            size_t chars = 0;
            while (chars < 2048) {
                uint16_t ch = (uint16_t)((s[chars * 2] << 8) | s[chars * 2 + 1]);
                if (ch == 0) break;
                chars++;
            }
            len = (uint16_t)(chars * 2);
            maxlen = (uint16_t)(chars * 2 + 2);
            buf = pSrc;
        }
        *reinterpret_cast<uint16_t*>(base + pDest + 0) = _byteswap_ushort(len);
        *reinterpret_cast<uint16_t*>(base + pDest + 2) = _byteswap_ushort(maxlen);
        *reinterpret_cast<uint32_t*>(base + pDest + 4) = _byteswap_ulong(buf);
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlNtStatusToDosError) {
    // RtlNtStatusToDosError(Status=r3) -> Win32 error in r3. Minimal map.
    uint32_t status = ctx.r3.u32;
    DWORD dos = ERROR_GEN_FAILURE;
    if (status == 0) dos = ERROR_SUCCESS;
    else if (status == 0xC0000034) dos = ERROR_FILE_NOT_FOUND;
    else if (status == 0xC0000008) dos = ERROR_INVALID_HANDLE;
    else if (status == 0xC0000011) dos = ERROR_HANDLE_EOF;
    ctx.r3.u64 = dos;
}
PPC_FUNC(__imp__RtlLowerChar) {
    uint32_t ch = ctx.r3.u32 & 0xFF;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    ctx.r3.u64 = ch;
}

// 3. Performance Timers
PPC_FUNC(__imp__KeQueryPerformanceCounter) {
    KTRACE();
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    ctx.r3.u64 = li.QuadPart;
}

PPC_FUNC(__imp__KeDelayExecutionThread) {
    // No KTRACE: secondary worker threads yield-spin here millions of times
    // while main initializes; logging (even throttled) floods the log and
    // garbles concurrent lines. Silent except Sleep logic.
    // KeDelayExecutionThread(Alertable=r3?, IntervalLow=r4?...) - guest passes
    // LARGE_INTEGER* in r4 on Xbox (r3=Alertable, r4=Interval*). Be tolerant.
    uint32_t timeoutLow = ctx.r4.u32;
    // Timeout in 100ns negative relative units (tolerant buddy: only low
    // half is consulted; high half/timeouts via APC are bring-up TODO).
    // Bring-up pacing: workers are deferred (nobody signals), so any real
    // Sleep here just burns wall-clock inside retry loops (observed: 100s+
    // stuck in 828AC000 delay loops). Yield only; retry paths (hooked to
    // advance) make progress without wall-time.
    (void)timeoutLow;
    Sleep(0);
    ctx.r3.u64 = 0; // STATUS_SUCCESS
}

// 3b. Kernel dispatcher objects (events / semaphores / waits)
// Handle range 0x50000000+ so we never collide with file handles (0x1000+).
static std::unordered_map<uint32_t, HANDLE> g_kernelObjects;
static uint32_t g_nextKernelHandle = 0x50001000;

static uint32_t AllocKernelHandle(HANDLE h) {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    uint32_t gh = g_nextKernelHandle++;
    g_kernelObjects[gh] = h;
    return gh;
}
static HANDLE FindKernelHandle(uint32_t gh) {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    auto it = g_kernelObjects.find(gh);
    return (it != g_kernelObjects.end()) ? it->second : nullptr;
}

PPC_FUNC(__imp__NtCreateEvent) {
    KTRACE();
    CheckRdata(base, "__imp__NtCreateEvent");
    uint32_t pHandle = ctx.r3.u32;
    // r6 = EventType (0=Notification/manual, 1=Synchronization/auto), r7 = InitialState
    uint32_t eventType = ctx.r6.u32;
    uint32_t initial = ctx.r7.u32;
    BOOL manual = (eventType == 0) ? TRUE : FALSE;
    HANDLE h = CreateEventA(nullptr, manual, initial ? TRUE : FALSE, nullptr);
    uint32_t gh = AllocKernelHandle(h);
    if (pHandle) *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(gh);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtSetEvent) {
    // No KTRACE: signaled per-resource during init (millions); silent.
    uint32_t h = ctx.r3.u32;
    HANDLE host = FindKernelHandle(h);
    if (host) SetEvent(host);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtClearEvent) {
    // No KTRACE: see NtSetEvent.
    uint32_t h = ctx.r3.u32;
    HANDLE host = FindKernelHandle(h);
    if (host) ResetEvent(host);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeSetEvent) {
    // PKEVENT* in guest memory: just mark signaled byte if plausible, else no-op.
    // No KTRACE (high frequency).
    uint32_t ev = ctx.r3.u32;
    if (ev) {
        // KEVENT SignalState is at +0x0C in Xbox DISPATCHER_HEADER (best-effort).
        // Do not fault on wild pointers.
        __try { *(volatile uint8_t*)(base + ev + 0x0C) = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeResetEvent) {
    // No KTRACE (high frequency).
    uint32_t ev = ctx.r3.u32;
    if (ev) {
        __try { *(volatile uint8_t*)(base + ev + 0x0C) = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeInitializeSemaphore) {
    KTRACE();
    // PRKSEMAPHORE Sem=r3, LONG Count=r4, LONG Limit=r5. Store count at +0x10 best-effort.
    uint32_t sem = ctx.r3.u32;
    uint32_t count = ctx.r4.u32;
    if (sem) {
        __try {
            *reinterpret_cast<uint32_t*>(base + sem + 0x10) = _byteswap_ulong(count);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeReleaseSemaphore) {
    KTRACE();
    uint32_t sem = ctx.r3.u32;
    if (sem) {
        __try {
            uint32_t c = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + sem + 0x10));
            *reinterpret_cast<uint32_t*>(base + sem + 0x10) = _byteswap_ulong(c + 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtCreateSemaphore) {
    KTRACE();
    uint32_t pHandle = ctx.r3.u32;
    uint32_t count = ctx.r5.u32;
    uint32_t limit = ctx.r6.u32;
    if (limit == 0) limit = 0x7FFFFFFF;
    HANDLE h = CreateSemaphoreA(nullptr, (LONG)count, (LONG)limit, nullptr);
    uint32_t gh = AllocKernelHandle(h ? h : (HANDLE)1);
    if (pHandle) *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(gh);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtReleaseSemaphore) {
    KTRACE();
    uint32_t h = ctx.r3.u32;
    HANDLE host = FindKernelHandle(h);
    if (host && host != (HANDLE)1) ReleaseSemaphore(host, 1, nullptr);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeWaitForSingleObject) {
    // PVOID Object=r3, ... Timeout*=r7. Bring-up: wait briefly on host handle
    // if known (1ms) so producer threads can run; else return SUCCESS.
    // Pure polling (0ms) causes 40k+/sec busy spins that starve siblings.
    uint32_t obj = ctx.r3.u32;
    HANDLE host = FindKernelHandle(obj);
    if (host && host != (HANDLE)1) {
        DWORD r = WaitForSingleObject(host, 1);
        ctx.r3.u64 = (r == WAIT_OBJECT_0) ? 0 : 0x102; // STATUS_SUCCESS / TIMEOUT
    } else {
        // Unknown dispatcher object (guest struct, not handle): yield.
        Sleep(0);
        ctx.r3.u64 = 0;
    }
}
PPC_FUNC(__imp__KeWaitForMultipleObjects) {
    KTRACE();
    Sleep(1);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
    uint32_t h = ctx.r3.u32;
    HANDLE host = FindKernelHandle(h);
    if (host && host != (HANDLE)1) {
        DWORD r = WaitForSingleObject(host, 1);
        ctx.r3.u64 = (r == WAIT_OBJECT_0) ? 0 : 0x102;
    } else {
        // Unknown object (guest struct / untracked handle): return SUCCESS so
        // waiters proceed instead of livelocking on TIMEOUT. Bring-up trade:
        // may race ahead of producer threads, but those idle-exit anyway and
        // main init is what we need to advance to display/files.
        ctx.r3.u64 = 0;
    }
}
PPC_FUNC(__imp__KeQueryPerformanceFrequency) {
    KTRACE();
    uint32_t pFreq = ctx.r3.u32;
    LARGE_INTEGER li; QueryPerformanceFrequency(&li);
    if (pFreq) *reinterpret_cast<uint64_t*>(base + pFreq) = _byteswap_uint64((uint64_t)li.QuadPart);
    ctx.r3.u64 = 1;
}
PPC_FUNC(__imp__KeQuerySystemTime) {
    KTRACE();
    uint32_t pTime = ctx.r3.u32;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (pTime) *reinterpret_cast<uint64_t*>(base + pTime) = _byteswap_uint64(t);
    ctx.r3.u64 = 0;
}
// KeBugCheck{,Ex} halts the Xbox on fatal errors. During bring-up the game
// calls it when an assert fails (often due to our heap/file/display stubs).
// Log the bugcheck code + args + host stack once, then return so the caller
// can continue (it typically loops). The code identifies the failing subsystem.
static uint64_t g_bugCheckLogged = 0;
static void LogBugCheck(PPCContext& ctx, uint8_t* base, const char* name) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_bugCheckLogged;
    }
    if (n <= 2) {
        std::cout << "[FATAL] " << name
                  << " code=0x" << std::hex << ctx.r3.u32
                  << " p1=0x" << ctx.r4.u32 << " p2=0x" << ctx.r5.u32
                  << " p3=0x" << ctx.r6.u32 << " p4=0x" << ctx.r7.u32
                  << std::dec << " (guest r1=0x" << std::hex << ctx.r1.u32 << std::dec << ")"
                  << std::endl;
        // Host stack to find the PPC caller.
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
        for (int frame = 0; frame < 12; frame++) {
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
            std::cout << "   [BC#" << frame << "] " << nm << "+0x" << std::hex << disp << std::dec << std::endl;
            if (nm.find("_xstart") != std::string::npos && frame > 2) break;
        }
        std::cout.flush();
    }
    // Slow the spin so the log stays readable and other threads can run.
    if ((n % 1000) == 0) Sleep(1);
}
PPC_FUNC(__imp__KeBugCheck) {
    LogBugCheck(ctx, base, "__imp__KeBugCheck");
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeBugCheckEx) {
    LogBugCheck(ctx, base, "__imp__KeBugCheckEx");
    ctx.r3.u64 = 0;
}

// 4. Memory Allocations
PPC_FUNC(__imp__MmQueryStatistics) {
    KTRACE();
    // MM_STATISTICS (Xbox): first ULONG Length, then page counts.
    // Fill with plausible 512MB unified-memory values so CRT heap init proceeds.
    // struct (BE in guest): +0 Length, +4 TotalPhysicalPages, +8 AvailablePages,
    // +12 TotalVirtual, +16 AvailableVirtual, ... zero rest.
    uint32_t pStats = ctx.r3.u32;
    if (pStats) {
        __try {
            uint32_t len = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + pStats));
            if (len == 0 || len > 128) len = 64;
            uint8_t* out = base + pStats;
            // Preserve Length, fill counts (all BE).
            const uint32_t totalPages = 131072;   // 512MB / 4KB
            const uint32_t availPages = 100000;   // leave room for image
            if (len >= 8)  *reinterpret_cast<uint32_t*>(out + 4) = _byteswap_ulong(totalPages);
            if (len >= 12) *reinterpret_cast<uint32_t*>(out + 8) = _byteswap_ulong(availPages);
            if (len >= 16) *reinterpret_cast<uint32_t*>(out + 12) = _byteswap_ulong(totalPages * 4096);
            if (len >= 20) *reinterpret_cast<uint32_t*>(out + 16) = _byteswap_ulong(availPages * 4096);
            for (uint32_t i = 20; i < len; i++) out[i] = 0;
            std::cout << "[Kernel] MmQueryStatistics(0x" << std::hex << pStats
                      << std::dec << ", len=" << len << ") -> 512MB plausible" << std::endl;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__MmQueryAllocationSize) {
    KTRACE();
    uint32_t addr = ctx.r3.u32;
    uint32_t pSize = ctx.r4.u32;
    if (pSize) *reinterpret_cast<uint32_t*>(base + pSize) = _byteswap_ulong(4096);
    (void)addr;
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__MmGetPhysicalAddress) {
    KTRACE();
    // Identity-map virtual->physical for bring-up.
    uint32_t vaddr = ctx.r3.u32;
    ctx.r3.u64 = vaddr;
    ctx.r4.u64 = 0;
}
PPC_FUNC(__imp__MmAllocatePhysicalAddress) {
    KTRACE();
    uint32_t size = ctx.r4.u32;
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    uint32_t allocated = g_guestHeapOffset;
    g_guestHeapOffset = (g_guestHeapOffset + size + 0xFFF) & ~0xFFF; // 4KB page align
    ctx.r3.u64 = allocated;
}

PPC_FUNC(__imp__MmFreePhysicalAddress) {
    KTRACE();
    ctx.r3.u64 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__NtAllocateVirtualMemory) {
    KTRACE();
    uint32_t sizePtr = ctx.r4.u32;
    uint32_t size = sizePtr ? _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + sizePtr)) : 0x10000;
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    uint32_t allocated = g_guestHeapOffset;
    g_guestHeapOffset = (g_guestHeapOffset + size + 0xFFF) & ~0xFFF;
    uint32_t basePtr = ctx.r3.u32;
    if (basePtr) {
        *reinterpret_cast<uint32_t*>(base + basePtr) = _byteswap_ulong(allocated);
    }
    ctx.r3.u64 = 0;
}

// 5. Video Mode & XAM Info
PPC_FUNC(__imp__XGetVideoMode) {
    KTRACE();
    uint32_t videoModePtr = ctx.r3.u32;
    if (videoModePtr) {
        uint8_t* p = base + videoModePtr;
        memset(p, 0, 0x30);
        *reinterpret_cast<uint32_t*>(p + 0x00) = _byteswap_ulong(1280); // DisplayWidth
        *reinterpret_cast<uint32_t*>(p + 0x04) = _byteswap_ulong(720);  // DisplayHeight
        *reinterpret_cast<uint32_t*>(p + 0x08) = _byteswap_ulong(0);    // IsInterlaced = false
        *reinterpret_cast<uint32_t*>(p + 0x0C) = _byteswap_ulong(1);    // IsWidescreen = true
        *reinterpret_cast<uint32_t*>(p + 0x10) = _byteswap_ulong(1);    // IsHighDefinition = true
        *reinterpret_cast<uint32_t*>(p + 0x14) = _byteswap_ulong(0x42700000); // RefreshRate = 60.0f
        *reinterpret_cast<uint32_t*>(p + 0x18) = _byteswap_ulong(1);    // VideoStandard = 1
        *reinterpret_cast<uint32_t*>(p + 0x1C) = _byteswap_ulong(0x4A); // Unknown4A
        *reinterpret_cast<uint32_t*>(p + 0x20) = _byteswap_ulong(0x01); // Unknown01
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdQueryVideoMode) {
    KTRACE();
    __imp__XGetVideoMode(ctx, base);
}

PPC_FUNC(__imp__XGetAVPack) {
    KTRACE();
    ctx.r3.u64 = 6; // XC_AVPACK_HDTV
}

PPC_FUNC(__imp__XGetLanguage) {
    KTRACE();
    ctx.r3.u64 = 1; // XC_LANGUAGE_ENGLISH
}

PPC_FUNC(__imp__XGetGameRegion) {
    KTRACE();
    ctx.r3.u64 = 0xFFFF; // GAME_REGION_ALL
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege) {
    KTRACE();
    ctx.r3.u64 = 1; // TRUE
}

PPC_FUNC(__imp__ExGetXConfigSetting) {
    KTRACE();
    ctx.r3.u64 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    KTRACE();
    uint32_t pDriver = ctx.r4.u32;
    if (pDriver) {
        *reinterpret_cast<uint32_t*>(base + pDriver) = _byteswap_ulong(0x44415544); // 'DAUD'
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    KTRACE();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    KTRACE();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
    KTRACE();
    uint32_t pMask = ctx.r4.u32;
    if (pMask) {
        *reinterpret_cast<uint32_t*>(base + pMask) = 0;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamGetSystemVersion) {
    KTRACE();
    ctx.r3.u64 = 0x20453200; // Dashboard version 2.0.4532.0
}

PPC_FUNC(__imp__XamInputGetCapabilities) {
    KTRACE();
    uint32_t pCaps = ctx.r5.u32;
    if (pCaps) {
        uint8_t* p = base + pCaps;
        memset(p, 0, 20);
        p[0] = 1; // XINPUT_DEVTYPE_GAMEPAD
        p[1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        *reinterpret_cast<uint16_t*>(p + 2) = _byteswap_ushort(0xFFFF);
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamInputGetState) {
    KTRACE();
    uint32_t userIndex = ctx.r3.u32;
    uint32_t flags = ctx.r4.u32;
    uint32_t pState = ctx.r5.u32;

    if (!pState) {
        ctx.r3.u64 = 87; // ERROR_INVALID_PARAMETER
        return;
    }

    uint8_t* p = base + pState;
    memset(p, 0, 16);

    // 1. Try physical XInput gamepad
    XINPUT_STATE xstate = {};
    DWORD res = XInputGetState(userIndex, &xstate);
    if (res == ERROR_SUCCESS) {
        *reinterpret_cast<uint32_t*>(p + 0) = _byteswap_ulong(xstate.dwPacketNumber);
        *reinterpret_cast<uint16_t*>(p + 4) = _byteswap_ushort(xstate.Gamepad.wButtons);
        p[6] = xstate.Gamepad.bLeftTrigger;
        p[7] = xstate.Gamepad.bRightTrigger;
        *reinterpret_cast<int16_t*>(p + 8) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(xstate.Gamepad.sThumbLX)));
        *reinterpret_cast<int16_t*>(p + 10) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(xstate.Gamepad.sThumbLY)));
        *reinterpret_cast<int16_t*>(p + 12) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(xstate.Gamepad.sThumbRX)));
        *reinterpret_cast<int16_t*>(p + 14) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(xstate.Gamepad.sThumbRY)));
        ctx.r3.u64 = 0;
        return;
    }

    // 2. Keyboard & Mouse Fallback for Player 1
    if (userIndex == 0) {
        static uint32_t s_packetNum = 1;
        uint16_t buttons = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        uint8_t lt = 0, rt = 0;

        if (GetAsyncKeyState('W') & 0x8000) ly = 32767;
        if (GetAsyncKeyState('S') & 0x8000) ly = -32768;
        if (GetAsyncKeyState('A') & 0x8000) lx = -32768;
        if (GetAsyncKeyState('D') & 0x8000) lx = 32767;

        if (GetAsyncKeyState(VK_UP) & 0x8000) ry = 32767;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) ry = -32768;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) rx = -32768;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) rx = 32767;

        if (GetAsyncKeyState(VK_SPACE) & 0x8000) buttons |= 0x1000; // A
        if (GetAsyncKeyState('R') & 0x8000) buttons |= 0x2000;      // B
        if (GetAsyncKeyState('E') & 0x8000) buttons |= 0x4000;      // X
        if (GetAsyncKeyState('Q') & 0x8000) buttons |= 0x8000;      // Y
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) buttons |= 0x0010; // START
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) buttons |= 0x0020; // BACK
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) rt = 0xFF;        // Fire
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) lt = 0xFF;        // Aim

        *reinterpret_cast<uint32_t*>(p + 0) = _byteswap_ulong(s_packetNum++);
        *reinterpret_cast<uint16_t*>(p + 4) = _byteswap_ushort(buttons);
        p[6] = lt;
        p[7] = rt;
        *reinterpret_cast<int16_t*>(p + 8) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(lx)));
        *reinterpret_cast<int16_t*>(p + 10) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(ly)));
        *reinterpret_cast<int16_t*>(p + 12) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(rx)));
        *reinterpret_cast<int16_t*>(p + 14) = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(ry)));
        ctx.r3.u64 = 0;
        return;
    }

    ctx.r3.u64 = 1167; // ERROR_DEVICE_NOT_CONNECTED
}

PPC_FUNC(__imp__DbgPrint) {
    KTRACE();
    uint32_t strPtr = ctx.r3.u32;
    if (strPtr) {
        // Bound the read: scan for NUL up to 4KB within guest memory.
        const char* msg = reinterpret_cast<const char*>(base + strPtr);
        size_t len = 0;
        while (len < 4096 && msg[len] != '\0') len++;
        std::string safe(msg, len);
        // Escape non-printables for clean logs.
        std::string esc;
        esc.reserve(len + 16);
        for (char ch : safe) {
            if (ch == '\n') esc += "\\n";
            else if (ch == '\r') esc += "\\r";
            else if (ch == '\t') esc += "\\t";
            else if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) {
                char tmp[8]; sprintf_s(tmp, "\\x%02X", (unsigned char)ch);
                esc += tmp;
            } else esc += ch;
        }
        std::cout << "[Guest Output] guest=0x" << std::hex << strPtr << std::dec
                  << " (" << len << "b) \"" << esc << "\""
                  << " args=[r4=0x" << std::hex << ctx.r4.u32
                  << " (" << std::dec << ctx.r4.s32 << ")"
                  << " r5=0x" << std::hex << ctx.r5.u32
                  << " r6=0x" << ctx.r6.u32 << " r7=0x" << ctx.r7.u32 << std::dec << "]"
                  << std::endl;
        if (len == 0) {
            // Dump raw bytes to distinguish zeroed .rdata from wrong pointer.
            char tmp[96] = {};
            size_t pos = 0;
            for (size_t i = 0; i < 32 && pos < sizeof(tmp) - 4; i++) {
                int n = sprintf_s(tmp + pos, sizeof(tmp) - pos, " %02X", (base + strPtr)[i]);
                if (n < 0) break;
                pos += (size_t)n;
            }
            std::cout << "[Guest Output] raw32 at guest:" << tmp << std::endl;
        }
        std::cout.flush();
    } else {
        std::cout << "[Guest Output] (null)" << std::endl;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__DbgBreakPoint) {
    KTRACE();
    static uint64_t s_bpLogged = 0;
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++s_bpLogged;
    }
    if (n <= 3) {
        std::cout << "[Kernel] DbgBreakPoint called (#" << n << ")." << std::endl;
        std::cout.flush();
    }
    // Sample the spinning assert site once it dominates (content-load phase
    // hits millions of DbgBreakPoints from bypass-induced nulls).
    if (n == 500000) {
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
        std::cout << "[DbgBreakPoint] spin sample stack (call #500000):" << std::endl;
        for (int frame = 0; frame < 12; frame++) {
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
            std::cout << "   [B#" << frame << "] " << nm << "+0x" << std::hex << disp << std::dec << std::endl;
        }
        std::cout.flush();
    }
}

struct GuestThreadArgs {
    uint32_t startAddress;
    uint32_t startContext;
    uint32_t guestStack;
    uint32_t guestPCR;
    uint8_t* base;
};

static void InitGuestPCR(uint8_t* base, uint32_t pcr, uint32_t threadId) {
    // Layout mirrors SetupThreadContext (main thread): PCR at P, TLS at
    // P+0xAB0, TEB at P+0xBB0. Zero a page and wire big-endian pointers.
    constexpr uint32_t TLS_OFF = 0xAB0;
    constexpr uint32_t TEB_OFF = 0xBB0;
    uint8_t* p = base + pcr;
    memset(p, 0, 0x1000);
    *reinterpret_cast<uint32_t*>(p) = _byteswap_ulong(pcr + TLS_OFF);
    *reinterpret_cast<uint32_t*>(p + 0x100) = _byteswap_ulong(pcr + TEB_OFF);
    p[0x10C] = 0;
    *reinterpret_cast<uint32_t*>(base + pcr + TEB_OFF + 0x14C) = _byteswap_ulong(threadId);
}

static LONG SecondaryThreadFilter(PEXCEPTION_POINTERS p, uint32_t startAddr) {
    DWORD code = p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionCode : 0;
    void* fault = nullptr;
    ULONG_PTR guest = 0;
    bool isGuest = false;
    // g_guestHeapOffset-adjacent base unknown here; caller passes base via TLS? Use best effort: no base.
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    std::cout << "[GuestThread " << GetCurrentThreadId() << "] Exception 0x" << std::hex << code << std::dec
              << " in secondary thread (start 0x" << std::hex << startAddr << std::dec << ")." << std::endl;
    if (p && p->ExceptionRecord && p->ExceptionRecord->NumberParameters >= 2) {
        fault = (void*)p->ExceptionRecord->ExceptionInformation[1];
        std::cout << "[GuestThread " << GetCurrentThreadId() << "] fault host=" << fault
                  << " at host " << p->ExceptionRecord->ExceptionAddress << std::endl;
    }
    if (p && p->ContextRecord) {
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SymInitialize(hProcess, nullptr, TRUE);
        STACKFRAME64 sf = {};
        sf.AddrPC.Offset = p->ContextRecord->Rip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = p->ContextRecord->Rbp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = p->ContextRecord->Rsp;
        sf.AddrStack.Mode = AddrModeFlat;
        CONTEXT ctxCopy = *p->ContextRecord;
        for (int frame = 0; frame < 10; frame++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &ctxCopy,
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
            std::cout << "   [S#" << frame << "] " << nm << "+0x" << std::hex << disp << std::dec << std::endl;
        }
    }
    std::cout.flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI GuestThreadProc(LPVOID param) {
    GuestThreadArgs* args = reinterpret_cast<GuestThreadArgs*>(param);
    PPCContext threadCtx = {};
    threadCtx.r1.u64 = args->guestStack;
    threadCtx.r3.u64 = args->startContext;
    threadCtx.r13.u64 = args->guestPCR;

    uint32_t startAddr = args->startAddress;
    uint8_t* base = args->base;
    uint32_t startCtx = args->startContext;
    delete args;

    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        DarkRecomp::RegisterWorkerTid(static_cast<uint32_t>(GetCurrentThreadId()));
        std::cout << "[GuestThread " << GetCurrentThreadId() << "] worker enter start=0x"
                  << std::hex << startAddr << " ctx=0x" << startCtx << std::dec << std::endl;
        std::cout.flush();
    }
    __try {
        auto func = PPC_LOOKUP_FUNC(base, startAddr);
        if (func) {
            func(threadCtx, base);
        } else {
            std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
            std::cout << "[GuestThread " << GetCurrentThreadId() << "] No recompiled function for guest 0x"
                      << std::hex << startAddr << std::dec << std::endl;
            std::cout.flush();
        }
    } __except (SecondaryThreadFilter(GetExceptionInformation(), startAddr)) {
        std::cout << "[GuestThread] secondary start 0x" << std::hex << startAddr << std::dec
                  << " terminated via exception filter." << std::endl;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        std::cout << "[GuestThread " << GetCurrentThreadId() << "] worker exit start=0x"
                  << std::hex << startAddr << std::dec << std::endl;
        std::cout.flush();
    }
    return 0;
}

static uint32_t g_guestThreadCount = 1;

// Deferred worker threads, saved for later real launch. Boot stays
// single-threaded/deterministic; workers launch on demand when the game
// thread first blocks on an async job completion (see sub_820C88B0 hook).
struct DeferredThread {
    uint32_t startAddress;
    uint32_t startContext;
    uint32_t guestStack;
    uint32_t guestPCR;
    uint8_t* base;
};
static std::vector<DeferredThread> g_deferredThreads;
static bool g_workersLaunched = false;
// Owned worker handles: kept open (not CloseHandle'd at creation) so
// ShutdownWorkers can terminate the post-_xstart retry-loop spinners that
// otherwise flood IndirectMiss at ~400k/sec after the game thread exits.
static std::vector<HANDLE> g_workerHandles;

static void LaunchDeferredWorkers() {
    std::vector<DeferredThread> toLaunch;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        if (g_workersLaunched) return;
        g_workersLaunched = true;
        toLaunch = g_deferredThreads;
        g_deferredThreads.clear();
    }
    for (auto& dt : toLaunch) {
        GuestThreadArgs* args = new GuestThreadArgs{
            dt.startAddress, dt.startContext, dt.guestStack, dt.guestPCR, dt.base};
        HANDLE h = CreateThread(NULL, 0, GuestThreadProc, args, 0, NULL);
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        std::cout << "[Thread] Launched deferred worker start=0x" << std::hex
                  << dt.startAddress << " ctx=0x" << dt.startContext
                  << (h ? " OK" : " FAILED") << std::dec << std::endl;
        std::cout.flush();
        if (h) g_workerHandles.push_back(h);
        else delete args;
    }
}

PPC_FUNC(__imp__ExCreateThread) {
    KTRACE();
    uint32_t pHandle = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32;
    uint32_t pThreadId = ctx.r5.u32;
    uint32_t xApiStartup = ctx.r6.u32;
    uint32_t startAddress = ctx.r7.u32;
    uint32_t startContext = ctx.r8.u32;
    uint32_t flags = ctx.r9.u32;

    if (startAddress == 0) startAddress = xApiStartup;

    uint32_t threadIdx = 0;
    uint32_t guestStack = 0;
    uint32_t guestPCR = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        threadIdx = g_guestThreadCount++;
        guestStack = 0x71000000 + (threadIdx * 0x100000) - 0x100;
        // Main thread owns 0x70040000; secondaries get 0x70041000, 0x70042000...
        guestPCR = 0x70040000 + (threadIdx * 0x1000);
    }
    // Guest-memory init outside the lock (SEH fault must not leak it held).
    InitGuestPCR(base, guestPCR, threadIdx + 1);

    // Bring-up determinism: do NOT start real host threads yet. Concurrent
    // workers race the main thread through heap bumps (addresses change per
    // run -> different cmplw branches -> fast-abort some runs, deep-init
    // others). Single-threaded boot is fully deterministic: same alloc order
    // every run, so each hook iteration lands on the same frontier.
    // Workers are recorded and will be started once main reaches display.
    uint32_t fakeTid = 0x2000 + threadIdx;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        if (g_workersLaunched) {
            // Workers already live: start this one for real immediately.
            // Keep the handle in g_workerHandles for ShutdownWorkers.
            GuestThreadArgs* args = new GuestThreadArgs{
                startAddress, startContext, guestStack, guestPCR, base};
            HANDLE h = CreateThread(NULL, 0, GuestThreadProc, args, 0, NULL);
            std::cout << "[Thread] ExCreateThread LIVE start=0x" << std::hex
                      << startAddress << " ctx=0x" << startContext
                      << (h ? " OK" : " FAILED") << std::dec << std::endl;
            std::cout.flush();
            if (h) g_workerHandles.push_back(h);
            else delete args;
        } else {
            g_deferredThreads.push_back(DeferredThread{
                startAddress, startContext, guestStack, guestPCR, base});
        }
        std::cout << "[Thread] ExCreateThread DEFERRED guestStack=0x" << std::hex << guestStack
                  << " guestPCR=0x" << guestPCR << " start=0x" << startAddress
                  << " ctx=0x" << startContext << std::dec << " fakeTid=" << fakeTid << std::endl;
        std::cout.flush();
    }

    uint32_t guestHandle = 0x80000000 | fakeTid;
    if (pHandle) {
        *reinterpret_cast<uint32_t*>(base + pHandle) = _byteswap_ulong(guestHandle);
    }
    if (pThreadId) {
        *reinterpret_cast<uint32_t*>(base + pThreadId) = _byteswap_ulong(fakeTid);
    }
    ctx.r3.u64 = 0; // STATUS_SUCCESS
}

PPC_FUNC(__imp__ExTerminateThread) {
    KTRACE();
    ExitThread(ctx.r3.u32);
}

// 6. Starbreeze CDisplayContext Factory Hook (0x822432D8)
// ABI NOTE (2026-09-03): The guest is 32-bit big-endian PPC; the host is
// 64-bit x86_64. We MUST NOT return a truncated host pointer in r3. Instead we
// allocate a small guest-side stub object, return its 32-bit guest address,
// and keep a host-side map from guest address -> native display. When the
// engine later calls virtual methods through the guest vtable, the recompiled
// callees can look up the native object. For bring-up we only need the factory
// to succeed and return a non-null, guest-readable pointer; full vtable
// interception is Phase 4 work.
static DarkRecomp::CDisplayContextD3D11* g_pNativeDisplay = nullptr;
static std::unordered_map<uint32_t, DarkRecomp::CDisplayContextD3D11*> g_displayMap;

PPC_FUNC(sub_822432D8) {
    KTRACE();
    // Original: malloc(4768) + construct CDisplayContextXenon.
    // Native: allocate 4768 bytes from the guest heap so guest code can do
    // `stw` field stores / vtable pointer writes without faulting.
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + 4768 + 0xFFF) & ~0xFFF;
    }
    // Zero the stub object in guest memory.
    memset(base + guestObj, 0, 4768);
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        g_displayMap[guestObj] = g_pNativeDisplay;
    }
    std::cout << "[Hook] CDisplayContext factory (0x822432D8): guest obj 0x"
              << std::hex << guestObj << std::dec
              << " -> native " << (void*)g_pNativeDisplay << std::endl;
    std::cout.flush();
    ctx.r3.u64 = guestObj;
}

// 6b. Diagnostic heap hooks (2026-09-03)
// sub_821080E8 is a red-black-tree lookup inside the Starbreeze heap manager.
// It spins forever during boot because the heap metadata (in .data, built from
// zeroed low-memory reads) forms a cycle. Hook it to log inputs once and
// return 0 ("not found") so the caller falls through instead of hanging.
// This is a bring-up bypass, not a final heap implementation.
static uint64_t g_heapHookLogged = 0;
PPC_FUNC(sub_821080E8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_heapHookLogged;
    }
    if (n <= 5) {
        std::cout << "[HeapHook] sub_821080E8 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32
                  << " r4=0x" << ctx.r4.u32
                  << " r5=0x" << ctx.r5.u32 << std::dec
                  << " -> returning 0" << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = 0;
}
// sub_821F0B20: per-entry worker from 821F2840's table loop. A bypass here
// leaves job locks in a state the entry worker was supposed to resolve
// (game later spins forever in 820C87F8 lock acquire on the same objects).
// Call through to the original: its delay callees (828AC000) are bypassed
// and KeDelay is zeroed, so the 14-min livelock it once had should now be
// fast while doing the real per-entry work. Hot path: lock-free counter.
static volatile LONG64 g_hook1F0B20Count = 0;
PPC_FUNC(sub_821F0B20) {
    LONG64 n = InterlockedIncrement64(&g_hook1F0B20Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_821F0B20 passthrough #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << std::dec << std::endl;
        std::cout.flush();
    }
    __imp__sub_821F0B20(ctx, base);
}
// sub_820C88B0: refcount-release + spin-wait for async job completion
// ([r3+16] set by loader worker threads, start 0x821FBB00). Boot defers
// workers for determinism, so the first unsatisfied wait launches them for
// real, then runs the original wait. Hot path: lock-free counter.
static volatile LONG64 g_hook0C88B0Count = 0;
PPC_FUNC(sub_820C88B0) {
    LONG64 n = InterlockedIncrement64(&g_hook0C88B0Count);
    uint32_t job = ctx.r3.u32;
    uint32_t completion = 0;
    __try {
        completion = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + job + 16));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        completion = 0;
    }
    if (n <= 5) {
        std::cout << "[Hook] sub_820C88B0 wait #" << n
                  << " job=0x" << std::hex << job << " completion=0x" << completion
                  << std::dec << std::endl;
        std::cout.flush();
    }
    if (completion == 0) {
        LaunchDeferredWorkers();
    }
    __imp__sub_820C88B0(ctx, base);
}
// sub_8289FEF8: worker-pool sweep called from 820C88B0's tail. Entry/exit
// log only (PPCContext carries volatile regs; the sweep's table base is in
// non-volatiles). Tells whether the sweep returns or itself is the stall.
static volatile LONG64 g_hook9FEF8Count = 0;
PPC_FUNC(sub_8289FEF8) {
    LONG64 n = InterlockedIncrement64(&g_hook9FEF8Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_8289FEF8 sweep enter #" << n << std::endl;
        std::cout.flush();
    }
    __imp__sub_8289FEF8(ctx, base);
    if (n <= 3) {
        std::cout << "[Hook] sub_8289FEF8 sweep #" << n << " returned" << std::endl;
        std::cout.flush();
    }
}
// sub_82107EA0 is the heap allocator entry (called with sizes 96/232/1024).
// Its tree lookup always misses (see sub_821080E8 bypass) so it returns 0,
// poisoning all downstream heaps to null and causing null-vtable faults
// (ctr=0 -> lookup wraps to host 0x7E990000). Hook it to return fresh
// zeroed guest memory so construction can proceed.
static uint64_t g_heap7EA0Logged = 0;
PPC_FUNC(sub_82107EA0) {
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;
    // Heuristic: size is max(r3,r4) clamped to [32, 65536]; the observed
    // calls use r4 as size (0x60/0xE8/0x400) with r3 as heap handle.
    uint32_t size = a4;
    if (size < 32) size = 1024;
    if (size > 65536) size = 65536;
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + size + 0xFFF) & ~0xFFF;
        g_heap7EA0Logged++;
    }
    if (g_heap7EA0Logged <= 5) {
        std::cout << "[HeapHook] sub_82107EA0 alloc #" << g_heap7EA0Logged
                  << " r3=0x" << std::hex << a3 << " r4=0x" << a4
                  << " -> guest 0x" << guestObj << " size 0x" << size << std::dec << std::endl;
        std::cout.flush();
    }
    memset(base + guestObj, 0, size);
    ctx.r3.u64 = guestObj;
}
// sub_82215978 / sub_82216628 are the second-stage heap allocator (called via
// sub_821F1A80 during boot). Their RB-tree walk spins on zeroed/cyclic
// metadata. Hook both to return fresh guest memory keyed off r4 (size).
static uint64_t g_heap2215978Logged = 0;
// .rdata integrity probe: format string at 0x82007898 must stay intact.
// Logs the FIRST hook that observes it zeroed, narrowing the corrupter to
// the game-code interval between the previous hook and this one.
static uint64_t g_rdataCorruptLogged = 0;
static void CheckRdata(uint8_t* base, const char* who) {
    if (g_rdataCorruptLogged) return;
    const char* s = (const char*)(base + 0x82007898);
    if (s[0] == '\0') {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        if (!g_rdataCorruptLogged) {
            g_rdataCorruptLogged = 1;
            std::cout << "[Corrupt] .rdata format string ZEROED, first observed in "
                      << who << std::endl;
            std::cout.flush();
        }
    }
}
static uint32_t AllocGuestHeap(uint8_t* base, uint32_t size, const char* who, uint64_t* ctr) {
    if (size < 16) size = 1024;
    if (size > 0x100000) size = 0x100000;
    CheckRdata(base, who);
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        if (g_guestHeapOffset + size + 0xFFF >= kGuestHeapCap) {
            std::cout << "[Heap] CAP HIT in " << who << " size=0x" << std::hex << size << std::dec << " -> 0" << std::endl;
            return 0;
        }
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + size + 0xFFF) & ~0xFFF;
        (*ctr)++;
    }
    memset(base + guestObj, 0, size);
    return guestObj;
}
PPC_FUNC(sub_82215978) {
    uint32_t size = ctx.r4.u32;
    uint32_t guestObj = AllocGuestHeap(base, size, "82215978", &g_heap2215978Logged);
    if (g_heap2215978Logged <= 5) {
        std::cout << "[HeapHook] sub_82215978 alloc #" << g_heap2215978Logged
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << size
                  << " -> guest 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
static uint64_t g_heap2216628Logged = 0;
PPC_FUNC(sub_82216628) {
    uint32_t size = ctx.r4.u32;
    uint32_t guestObj = AllocGuestHeap(base, size, "82216628", &g_heap2216628Logged);
    if (g_heap2216628Logged <= 5) {
        std::cout << "[HeapHook] sub_82216628 alloc #" << g_heap2216628Logged
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << size
                  << " -> guest 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_821EFDA8 is the Starbreeze 28-byte allocator used by sub_8289FBB8.
// Un-hooked it returns 0 (tree miss on zeroed metadata), causing FBB8 to call
// FCF8(10) which calls FBB8 again -> infinite mutual recursion -> host stack
// overflow. Hook it to return fresh guest memory (size in r3).
static uint64_t g_heap1EFDA8Logged = 0;
PPC_FUNC(sub_821EFDA8) {
    uint32_t size = ctx.r3.u32;
    if (size == 0) size = 28;
    uint32_t guestObj = AllocGuestHeap(base, size, "821EFDA8", &g_heap1EFDA8Logged);
    if (g_heap1EFDA8Logged <= 5) {
        std::cout << "[HeapHook] sub_821EFDA8 alloc #" << g_heap1EFDA8Logged
                  << " r3(size)=0x" << std::hex << size
                  << " -> guest 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_821FCBD0 is a calloc-like wrapper (r3=count, r4=size) that mallocs via
// sub_82215978 then memsets via sub_82899950 with a length derived from
// sub_822160E8. With null vtables that query returns 0, yielding length
// 0xFFFFFFF0 (4GB) which wipes the image (.rdata format string at
// 0x82007898) and faults under .rdata protection (or corrupts silently
// without it). Hook as safe calloc: clamp total, allocate, zero exactly that.
static uint64_t g_heap1FCBD0Logged = 0;
PPC_FUNC(sub_821FCBD0) {
    uint64_t count = ctx.r3.u32, esize = ctx.r4.u32;
    uint64_t total = count * (esize ? esize : 1);
    if (total == 0) total = 64;
    if (total > 0x100000) total = 0x100000;
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + (uint32_t)total + 0xFFF) & ~0xFFF;
        g_heap1FCBD0Logged++;
    }
    memset(base + guestObj, 0, (size_t)total);
    if (g_heap1FCBD0Logged <= 5) {
        std::cout << "[HeapHook] sub_821FCBD0 calloc #" << g_heap1FCBD0Logged
                  << " count=0x" << std::hex << (uint32_t)count << " esize=0x" << (uint32_t)esize
                  << " total=0x" << (uint32_t)total << " -> guest 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_8289FCF8 is the heap-table lookup (index in r3) that recurses into
static uint64_t g_hookFCF8Logged = 0;
PPC_FUNC(sub_8289FCF8) {
    uint64_t n = 0;
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + 256 + 0xFFF) & ~0xFFF;
        n = ++g_hookFCF8Logged;
    }
    memset(base + guestObj, 0, 256);
    if (n <= 3) {
        std::cout << "[Hook] sub_8289FCF8 bypass #" << n
                  << " idx=0x" << std::hex << ctx.r3.u32
                  << " -> guest 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_8289C658 performs TLS alloc + indirect calls through .data function
// tables that are still null in bring-up (heap bypasses skipped the
// constructors that would fill them), faulting with ctr=0.
// Bypass for now (return 1 = success) so _xstart can proceed to file and
// display stages where the native port work actually lives.
static uint64_t g_hook89C658Logged = 0;
PPC_FUNC(sub_8289C658) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook89C658Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8289C658 bypass #" << n << " -> returning 1" << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = 1;
}
// sub_8223C4D0 is a tagged-pointer list insert used during engine init
// (via sub_828AC6A0 -> ... -> sub_8223B198). It faults on null list heads
// left by heap bypasses (zeroed objects, no constructed sentinels).
// Bypass for bring-up (log object, return it) so init can reach display.
static uint64_t g_hook23C4D0Logged = 0;
PPC_FUNC(sub_8223C4D0) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook23C4D0Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8223C4D0 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
    // Return object unchanged (skip list link).
}
// sub_8223C1A8: sibling list primitive faulting on static heads in the
// protected .rdata/.pdata window (guest 0x8209FFE8). Same bypass.
static uint64_t g_hook23C1A8Logged = 0;
PPC_FUNC(sub_8223C1A8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook23C1A8Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8223C1A8 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_82267040: display-construction list leaf (via 82241200 ctor).
// Same tagged-pointer pattern, same protected-head fault (0x8209FFF0).
static uint64_t g_hook267040Logged = 0;
PPC_FUNC(sub_82267040) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook267040Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_82267040 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_822672F0: next sibling list leaf (fault 0x8209FFF8, +8 past 67040).
static uint64_t g_hook2672F0Logged = 0;
PPC_FUNC(sub_822672F0) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook2672F0Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_822672F0 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_8220B2F8: resource-init list leaf via 82104DC8 (fault 0x8209FFB4).
static uint64_t g_hook20B2F8Logged = 0;
PPC_FUNC(sub_8220B2F8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook20B2F8Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8220B2F8 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_82214ED0: heap RB-tree find-or-create spinning on cyclic metadata
// (watchdog: endless tree walk, no crash). Hook to return a fresh node.
static uint64_t g_hook214ED0Logged = 0;
PPC_FUNC(sub_82214ED0) {
    uint64_t n = 0;
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + 128 + 0xFFF) & ~0xFFF;
        n = ++g_hook214ED0Logged;
    }
    memset(base + guestObj, 0, 128);
    if (n <= 3) {
        std::cout << "[Hook] sub_82214ED0 bypass #" << n
                  << " heap=0x" << std::hex << ctx.r3.u32 << " key=0x" << ctx.r4.u32
                  << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_8223B9A8: resource list leaf via 82104DC8 (fault 0x82055640, .rdata).
static uint64_t g_hook23B9A8Logged = 0;
PPC_FUNC(sub_8223B9A8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook23B9A8Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8223B9A8 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_82214030: spun (watchdog: same RSP/RIP window 20s) when called from
// 82216270+0x108 during resource init. Fake-node bypass strands later jobs
// (same trap as 82216270). Try original under SEH; fresh node on fault.
static volatile LONG64 g_hook214030Count = 0;
static volatile LONG64 g_hook214030Faults = 0;
PPC_FUNC(sub_82214030) {
    LONG64 n = InterlockedIncrement64(&g_hook214030Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_82214030 try-original #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                  << " r5=0x" << ctx.r5.u32 << std::dec << std::endl;
        std::cout.flush();
    }
    __try {
        __imp__sub_82214030(ctx, base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LONG64 f = InterlockedIncrement64(&g_hook214030Faults);
        uint32_t guestObj = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
            guestObj = g_guestHeapOffset;
            g_guestHeapOffset = (g_guestHeapOffset + 256 + 0xFFF) & ~0xFFF;
        }
        memset(base + guestObj, 0, 256);
        if (f <= 3) {
            std::cout << "[Hook] sub_82214030 original FAULTED #" << f
                      << " code=0x" << std::hex << GetExceptionCode()
                      << " -> node 0x" << guestObj << std::dec << " (fallback)" << std::endl;
            std::cout.flush();
        }
        ctx.r3.u64 = guestObj;
        return;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_82214030 original returned r3=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
static volatile LONG64 g_hook216088Count = 0;
// sub_82216088: RB-tree search (heap=r3, key=r4). Original spins forever on
// cyclic metadata (SEH cannot catch a non-faulting infinite loop).
// Survey 1/3/4 (2026-09-04) proved fresh-node bypass reaches clean _xstart
// exit (heap 0x145a9000); try-original/native regressed to early hangs.
// Restore bypass: return a fresh zeroed node so callers advance.
static uint64_t g_hook216088Logged = 0;
PPC_FUNC(sub_82216088) {
    LONG64 n = InterlockedIncrement64(&g_hook216088Count);
    uint32_t heap = ctx.r3.u32, key = ctx.r4.u32, r5 = ctx.r5.u32;
    uint32_t guestObj = AllocGuestHeap(base, 256, "82216088", &g_hook216088Logged);
    if (n <= 3) {
        std::cout << "[Hook] sub_82216088 bypass #" << n
                  << " r3=0x" << std::hex << heap << " r4=0x" << key
                  << " r5=0x" << r5 << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_82216270: wrapper around the 14030/16088 tree finds.
// Survey 1/3/4 proved bypass reaches clean _xstart; try-original hangs.
// Restore fresh-node bypass.
static volatile LONG64 g_hook216270Count = 0;
static uint64_t g_hook216270Logged = 0;
PPC_FUNC(sub_82216270) {
    LONG64 n = InterlockedIncrement64(&g_hook216270Count);
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
    uint32_t guestObj = AllocGuestHeap(base, 256, "82216270", &g_hook216270Logged);
    if (n <= 3) {
        std::cout << "[Hook] sub_82216270 bypass #" << n
                  << " r3=0x" << std::hex << r3 << " r4=0x" << r4
                  << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
}
// sub_8220B5B0: game-world list leaf via 82104DC8 (fault in .rdata window).
static uint64_t g_hook20B5B0Logged = 0;
PPC_FUNC(sub_8220B5B0) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook20B5B0Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8220B5B0 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_8220D168: object initializer reached with r3=0xFFFFFFFF (-1, error
// sentinel propagated from a failed lookup) that stores to the object.
// Bypass (no touch) so the caller takes its fail path instead of faulting
// one-past the guest mapping.
static uint64_t g_hook20D168Logged = 0;
PPC_FUNC(sub_8220D168) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook20D168Logged;
    }
    if (n <= 5) {
        std::cout << "[Hook] sub_8220D168 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_822062B8: resource-init step livelocked in refcount/list cycling
// (watchdog: static heap, same RSP/RIP window minutes-long). Bypass leaving
// regs unchanged so caller 82104DC8 continues to display/file stages.
static uint64_t g_hook2062B8Logged = 0;
PPC_FUNC(sub_822062B8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook2062B8Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_822062B8 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_822181C0: tagged-list walk livelocked on bypass-induced cycles
// (watchdog 10min identical: +0x28d -> +0x48e slow crawl, heap static).
// Bypass leaving r3 unchanged so caller 822152C8 continues.
// NOTE: hot path (per-node during traversal): lock-free counter, no mutex,
// to avoid contending with the very traversal being bypassed.
static volatile LONG64 g_hook2181C0Count = 0;
PPC_FUNC(sub_822181C0) {
    LONG64 n = InterlockedIncrement64(&g_hook2181C0Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_822181C0 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_822152C8: resource-init step; depth-guarded passthrough runs the real
// code with cycle breaking (see t_152C8Depth).
static volatile LONG64 g_hook2152C8Count = 0;
static thread_local int t_152C8Depth = 0;
PPC_FUNC(sub_822152C8) {
    LONG64 n = InterlockedIncrement64(&g_hook2152C8Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_822152C8 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << std::dec << std::endl;
        std::cout.flush();
    }
    if (t_152C8Depth >= 500) {
        if (n <= 10) {
            std::cout << "[Hook] sub_822152C8 depth cap hit, returning empty" << std::endl;
        }
        ctx.r3.u64 = 0;
        return;
    }
    t_152C8Depth++;
    __imp__sub_822152C8(ctx, base);
    t_152C8Depth--;
}
// sub_821EFC78: find-type lookup livelocked 15min (same window Offsets,
// heap static). Return a fresh node like the other find hooks.
static volatile LONG64 g_hook1EFC78Count = 0;
PPC_FUNC(sub_821EFC78) {
    LONG64 n = InterlockedIncrement64(&g_hook1EFC78Count);
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        if (g_guestHeapOffset + 0x1000 >= kGuestHeapCap) {
            std::cout << "[Heap] CAP HIT in 821EFC78 -> 0" << std::endl;
            ctx.r3.u64 = 0;
            return;
        }
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + 256 + 0xFFF) & ~0xFFF;
    }
    memset(base + guestObj, 0, 256);
    if (n <= 3) {
        std::cout << "[Hook] sub_821EFC78 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
    return;
}
// sub_821F27D0: outer resource enumerator cycling one node forever (watchdog:
// identical (r3,r4) retries, heap explodes without memoize, static with it).
// Bypass so 82104DC8 advances; resource requests then miss to disk streaming
// (file bridge handles opens/reads) instead of spinning the registry.
static volatile LONG64 g_hook21F27D0Count = 0;
PPC_FUNC(sub_821F27D0) {
    LONG64 n = InterlockedIncrement64(&g_hook21F27D0Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_821F27D0 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_82243710: display-region init step livelocked (watchdog 10min same
// +0x2d3 window, heap static). Bypass leaving regs unchanged so caller
// 82241618 continues toward present/file stages. Hot path: lock-free.
static volatile LONG64 g_hook243710Count = 0;
PPC_FUNC(sub_82243710) {
    LONG64 n = InterlockedIncrement64(&g_hook243710Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_82243710 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_82216B98: indexed remove. Always-empty makes caller 822152C8 retry
// forever (watchdog: same +0x193 window, heap static). Return a fresh node
// instead so the caller advances (faults downstream get hooked as usual).
// Hot path: lock-free counter.
static volatile LONG64 g_hook216B98Count = 0;
PPC_FUNC(sub_82216B98) {
    LONG64 n = InterlockedIncrement64(&g_hook216B98Count);
    // Memoize on (r3,r4): the caller retries identical lookups hundreds of
    // thousands of times; fresh allocs per retry exploded the heap through
    // stacks/image/dispatch (observed 0x14A04000 -> 0xB9482000 in 15 min).
    // Same key returns the same node: bounded, stable, still advances.
    uint32_t keyHi = ctx.r3.u32, keyLo = ctx.r4.u32;
    uint64_t key = ((uint64_t)keyHi << 32) | keyLo;
    uint32_t guestObj = 0;
    {
        static std::mutex s_memoMutex;
        static std::unordered_map<uint64_t, uint32_t> s_memo;
        std::lock_guard<std::mutex> lk(s_memoMutex);
        auto it = s_memo.find(key);
        if (it != s_memo.end()) {
            guestObj = it->second;
        } else {
            std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
            if (g_guestHeapOffset + 0x1000 >= kGuestHeapCap) {
                std::cout << "[Heap] CAP HIT in 82216B98, key=0x" << std::hex << keyHi
                          << "/0x" << keyLo << std::dec << " -> 0" << std::endl;
                ctx.r3.u64 = 0;
                return;
            }
            guestObj = g_guestHeapOffset;
            g_guestHeapOffset = (g_guestHeapOffset + 128 + 0xFFF) & ~0xFFF;
            s_memo[key] = guestObj;
            memset(base + guestObj, 0, 128);
        }
    }
    if (n <= 5) {
        std::cout << "[Hook] sub_82216B98 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
    return;
}
// sub_820CA1F8: resource-cache lookup. Miss-returns-0 poisons caller
// 822152C8 (indexes result-1/-4 -> one-past-end fault); it never null-checks
// because on hardware these indices always hit. Return a fresh node instead.
static volatile LONG64 g_hook0CA1F8Count = 0;
PPC_FUNC(sub_820CA1F8) {
    LONG64 n = InterlockedIncrement64(&g_hook0CA1F8Count);
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + 256 + 0xFFF) & ~0xFFF;
    }
    memset(base + guestObj, 0, 256);
    if (n <= 5) {
        std::cout << "[Hook] sub_820CA1F8 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32
                  << " r5=0x" << ctx.r5.u32 << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
    return;
}
// sub_8223A7D8: resource-chain leaf faulting with -1-derived dest (same
// pattern via 822062B8). Bypass.
static uint64_t g_hook23A7D8Logged = 0;
PPC_FUNC(sub_8223A7D8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook23A7D8Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8223A7D8 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_821FD6D8: worker job-queue pop (all 3 workers fault here). Same list
// family; on empty queue it should return null so the worker waits instead
// of faulting. Return 0.
static uint64_t g_hook1FD6D8Logged = 0;
PPC_FUNC(sub_821FD6D8) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook1FD6D8Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_821FD6D8 bypass #" << n
                  << " queue=0x" << std::hex << ctx.r3.u32 << " -> empty (0)" << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = 0;
}
// sub_8210C138: content-load list leaf writing to image base (0x82000000).
// Same family as 8223B9A8/8220B5B0; bypass.
static uint64_t g_hook10C138Logged = 0;
PPC_FUNC(sub_8210C138) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook10C138Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8210C138 bypass #" << n
                  << " obj=0x" << std::hex << ctx.r3.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_82250B88: heap RB-tree lookup. Miss-returns-0 poisoned callers that
// index result-1/-4 (one-past-end faults); they never null-check because on
// hardware these indices always hit. Return a fresh node like the other
// find-type hooks so callers advance (faults downstream get hooked).
static volatile LONG64 g_hook250B88Count = 0;
PPC_FUNC(sub_82250B88) {
    LONG64 n = InterlockedIncrement64(&g_hook250B88Count);
    uint32_t guestObj = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        guestObj = g_guestHeapOffset;
        g_guestHeapOffset = (g_guestHeapOffset + 256 + 0xFFF) & ~0xFFF;
    }
    memset(base + guestObj, 0, 256);
    if (n <= 3) {
        std::cout << "[Hook] sub_82250B88 bypass #" << n
                  << " heap=0x" << std::hex << ctx.r3.u32 << " key=0x" << ctx.r4.u32
                  << " size=0x" << ctx.r5.u32 << " -> node 0x" << guestObj << std::dec << std::endl;
        std::cout.flush();
    }
    ctx.r3.u64 = guestObj;
    return;
}
// sub_8220DC10: worker idle loop (all workers spin here asserting on the
// empty job queue at ~500k/sec each). Throttle with Sleep(1) and skip the
// assert so workers idle cleanly until main queues real work.
static uint64_t g_hook20DC10Logged = 0;
PPC_FUNC(sub_8220DC10) {
    uint64_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        n = ++g_hook20DC10Logged;
    }
    if (n <= 3) {
        std::cout << "[Hook] sub_8220DC10 worker-idle bypass #" << n << std::endl;
        std::cout.flush();
    }
    Sleep(1);
}
// sub_827A72D8: 5904-line resource state machine reached only via workers
// (821FC540 path; no direct callers in the tree, entry is indirect). Its hot
// section polls null-vtable virtuals (0xC/0x20/0x2C: ~7.9M of 8M misses in a
// 20s run) in a non-faulting retry loop SEH cannot catch. Throttle like
// sub_8220DC10 (Sleep + return, regs unchanged): the caller retries either
// way since every virtual inside already misses to r3=0. Worker-only path,
// so the game-thread boot frontier is unaffected. 821F35F8 deliberately NOT
// hooked: 78-line leaf with ~1000 direct callers including game-thread
// paths; its 2-miss cost per call is negligible next to 827A72D8.
// 8289C410 (3 misses, early init) and 8275CE18 (leaf, 1-2 misses) likewise
// left alone.
static volatile LONG64 g_hook27A72D8Count = 0;
PPC_FUNC(sub_827A72D8) {
    LONG64 n = InterlockedIncrement64(&g_hook27A72D8Count);
    if (n <= 3) {
        char buf[128];
        int m = snprintf(buf, sizeof(buf),
            "[Hook] sub_827A72D8 worker-throttle #%lld r3=0x%08X r4=0x%08X\n",
            (long long)n, ctx.r3.u32, ctx.r4.u32);
        if (m > 0) {
            std::cout << std::string(buf, (size_t)m);
            std::cout.flush();
        }
    }
    Sleep(1);
}
// sub_821F35F8: read-only probe (NOT a bypass). 78-line leaf with ~1000 direct
// callers on both worker and game-thread paths: [obj]+76 virtual, then +28 or
// +48 virtual, then 821F3350/821F33E8 follow-up. Behavior is fully preserved
// (original always runs); the probe only answers: is the object's vtable ptr
// null ("never constructed") or heap garbage ("wrong heap"/stale link)? All
// guest reads are SEH-guarded; on fault we still run the original.
static volatile LONG64 g_hook21F35F8Count = 0;
PPC_FUNC(sub_821F35F8) {
    LONG64 n = InterlockedIncrement64(&g_hook21F35F8Count);
    if (n <= 3 || (n % 20000) == 0) {
        uint32_t obj = ctx.r3.u32, r4 = ctx.r4.u32;
        uint32_t vtbl = 0, slot76 = 0;
        const char* cls = "ok";
        __try {
            vtbl = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + obj));
            if (vtbl == 0) cls = "null-vtbl";
            else if (vtbl < 0x82000000 || vtbl >= 0x82B10000) cls = "wild-vtbl";
            else {
                slot76 = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + vtbl + 76));
                cls = "mapped-vtbl";
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { cls = "unreadable"; }
        char caller[96] = {};
        if (n <= 3) {
            void* ret = _ReturnAddress();
            HANDLE hp = GetCurrentProcess();
            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            PSYMBOL_INFO ps = reinterpret_cast<PSYMBOL_INFO>(symBuf);
            ps->SizeOfStruct = sizeof(SYMBOL_INFO);
            ps->MaxNameLen = 256;
            DWORD64 disp = 0;
            if (SymFromAddr(hp, (DWORD64)ret, &disp, ps)) {
                snprintf(caller, sizeof(caller), " from %s+0x%llX", ps->Name, disp);
            } else {
                snprintf(caller, sizeof(caller), " from 0x%p", ret);
            }
        }
        char buf[256];
        int m = snprintf(buf, sizeof(buf),
            "[Probe35F8] #%lld obj=0x%08X r4=0x%08X vtbl=0x%08X slot76=0x%08X %s%s\n",
            (long long)n, obj, r4, vtbl, slot76, cls, caller);
        if (m > 0) {
            std::cout << std::string(buf, (size_t)m);
            std::cout.flush();
        }
    }
    __imp__sub_821F35F8(ctx, base);
}
// sub_82899950: PPC memset(dest=r3, value=r4 low byte, size=r5). Game code
// (via 821FC7B0 display-adjacent paths) issues wipes starting at the image
// base (0x82000000) that would destroy headers/.rdata (format string) and
// fault under protection (or corrupt silently without it). Guard: skip writes
// overlapping the protected image window, clamp absurd sizes, else host
// memset (fast path for legitimate object clears).
static uint64_t g_memsetSkipLogged = 0;
PPC_FUNC(sub_82899950) {
    uint32_t dest = ctx.r3.u32;
    uint8_t val = (uint8_t)(ctx.r4.u32 & 0xFF);
    uint32_t size = ctx.r5.u32;
    constexpr uint32_t kProtStart = 0x82000000;
    constexpr uint32_t kProtEnd = 0x820A0000;
    bool overlap = (dest < kProtEnd) && ((uint64_t)dest + size > kProtStart);
    if (overlap || size > 0x1000000) {
        uint64_t n = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
            n = ++g_memsetSkipLogged;
        }
        if (n <= 5) {
            std::cout << "[Memset] SKIP dest=0x" << std::hex << dest << " val=0x" << (unsigned)val
                      << " size=0x" << size << std::dec
                      << (overlap ? " (protected image window)" : " (absurd size)") << std::endl;
            std::cout.flush();
        }
        return; // leave r3 = dest (memset return convention)
    }
    memset(base + dest, val, size);
    // r3 already = dest; return.
}
// sub_8210A738: worker job-claim step (intrusive list/refcount family).
// Survey 1/3/4 proved pure bypass reaches clean _xstart; try-original hangs
// in 82207850/820C88B0 spin. Restore bypass (leave regs, let caller advance).
static volatile LONG64 g_hook10A738Count = 0;
PPC_FUNC(sub_8210A738) {
    LONG64 n = InterlockedIncrement64(&g_hook10A738Count);
    if (n <= 5) {
        std::cout << "[Hook] sub_8210A738 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_828AC000: delay wrapper reached indirectly (vtable/bctrl) from heap
// init; single long Sleep stalls deterministic boot for minutes (watchdog:
// identical SleepEx/RSP 100s+). Bypass leaving regs unchanged (callers that
// needed the delay use it as pacing; bring-up needs progress, not pacing).
// Hot path: lock-free.
static volatile LONG64 g_hook28AC000Count = 0;
PPC_FUNC(sub_828AC000) {
    LONG64 n = InterlockedIncrement64(&g_hook28AC000Count);
    if (n <= 3) {
        std::cout << "[Hook] sub_828AC000 bypass #" << n
                  << " r3=0x" << std::hex << ctx.r3.u32 << " r4=0x" << ctx.r4.u32 << std::dec << std::endl;
        std::cout.flush();
    }
}
// sub_821F2840: heap-init driver. A bypass here skips the real heap build
// and the boot takes a fast-abort path (clean exit 0, no files/display).
// Let the original run: KeDelay is zeroed and 828AC000 bypassed, so its
// delay loops are fast; leaf faults/spins are hooked individually.

// 7. File System Hooks
PPC_FUNC(__imp__NtOpenFile) {
    KTRACE();
    DarkRecomp::Hook_NtOpenFile(ctx, base);
}

PPC_FUNC(__imp__NtCreateFile) {
    KTRACE();
    DarkRecomp::Hook_NtCreateFile(ctx, base);
}

PPC_FUNC(__imp__NtReadFile) {
    KTRACE();
    DarkRecomp::Hook_NtReadFile(ctx, base);
}

PPC_FUNC(__imp__NtReadFileScatter) {
    KTRACE();
    DarkRecomp::Hook_NtReadFileScatter(ctx, base);
}

PPC_FUNC(__imp__NtWriteFile) {
    KTRACE();
    DarkRecomp::Hook_NtWriteFile(ctx, base);
}

PPC_FUNC(__imp__NtClose) {
    KTRACE();
    DarkRecomp::Hook_NtClose(ctx, base);
}

PPC_FUNC(__imp__NtSetInformationFile) {
    KTRACE();
    DarkRecomp::Hook_NtSetInformationFile(ctx, base);
}

PPC_FUNC(__imp__NtQueryInformationFile) {
    KTRACE();
    DarkRecomp::Hook_NtQueryInformationFile(ctx, base);
}

PPC_FUNC(__imp__NtQueryFullAttributesFile) {
    KTRACE();
    DarkRecomp::Hook_NtQueryFullAttributesFile(ctx, base);
}

PPC_FUNC(__imp__NtFlushBuffersFile) {
    KTRACE();
    DarkRecomp::Hook_NtFlushBuffersFile(ctx, base);
}

PPC_FUNC(__imp__HalReturnToFirmware) {
    KTRACE();
    // NOTE: Do NOT call ExitProcess here: our IAT hook for ExitProcess
    // re-enters itself (no trampoline), causing infinite recursion.
    // Log and return; the caller (sub_828AC8D0) will return to _xstart.
    // A future change should implement a proper trampoline or a clean
    // shutdown path. For bring-up, returning lets boot continue past the
    // reboot check so we can discover the next stage.
    std::cout << "[Kernel] HalReturnToFirmware called! Type = " << ctx.r3.u32
              << " -- IGNORING reboot request during bring-up (returning)." << std::endl;
    std::cout.flush();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamLoaderTerminateTitle) {
    KTRACE();
    std::cout << "[Kernel] XamLoaderTerminateTitle called! Title requested clean shutdown -- IGNORING during bring-up."
              << std::endl;
    // Log host stack once to find which _xstart stage requested terminate.
    bool wantSleep = false;
    {
        static uint64_t s_termLogged = 0;
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        if (++s_termLogged == 1) {
            wantSleep = true;
        }
    }
    if (wantSleep) {
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
            std::cout << "[Terminate] caller stack:" << std::endl;
            for (int frame = 0; frame < 14; frame++) {
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
                std::cout << "   [T#" << frame << "] " << nm << "+0x" << std::hex << disp << std::dec << std::endl;
            }
            // Guest PPC stack backtrace: walk r1 chain, print return addresses.
            // PPC prologue saves LR at -8(old SP) and links SP via 0(new SP)=old SP.
            std::cout << "[Terminate] guest r1=0x" << std::hex << ctx.r1.u32 << std::dec << std::endl;
            uint32_t sp = ctx.r1.u32;
            for (int d = 0; d < 16 && sp != 0; d++) {
                __try {
                    uint32_t back = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + sp));
                    uint32_t lr = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + sp + 8));
                    // LR slot may be at sp+8 after stwu? Print both +4/+8 candidates.
                    uint32_t cand4 = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + sp + 4));
                    std::cout << "   [G#" << d << "] sp=0x" << std::hex << sp
                              << " back=0x" << back << " +8=0x" << lr << " +4=0x" << cand4 << std::dec << std::endl;
                    if (back == 0 || back == sp) break;
                    // Sanity: back chain should stay within guest stack region.
                    if (back < 0x70000000 || back > 0x71000000) break;
                    sp = back;
                } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            }
            std::cout.flush();
            // Hold the game thread here so the watchdog (4s period) can
            // suspend it and dump the PPC host stack, revealing the true
            // caller above this import (RtlCaptureContext in-thread cannot
            // walk optimized PPC frames).
            Sleep(6000);
    }
    ctx.r3.u64 = 0;
}

namespace DarkRecomp {

uint32_t GetGuestHeapOffset() {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    return g_guestHeapOffset;
}

static std::vector<uint32_t> g_workerTids;
void RegisterWorkerTid(uint32_t tid) {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    g_workerTids.push_back(tid);
}
std::vector<uint32_t> GetWorkerTids() {
    std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
    return g_workerTids;
}

void ShutdownWorkers() {
    std::vector<HANDLE> toKill;
    {
        std::lock_guard<std::recursive_mutex> lock(g_kernelMutex);
        toKill.swap(g_workerHandles);
    }
    // Terminate outside the lock: a spinner holds no mutex in its hot loop
    // (Interlocked counters + its own miss-log mutex), so the wedge risk is
    // minimal, and the precedent (game-thread TerminateThread in main.cpp)
    // already accepts it for bring-up.
    uint64_t killed = 0;
    for (HANDLE h : toKill) {
        DWORD ec = 0;
        if (GetExitCodeThread(h, &ec) && ec == STILL_ACTIVE) {
            TerminateThread(h, 0);
            killed++;
        }
        CloseHandle(h);
    }
    {
        char buf[128];
        int n = snprintf(buf, sizeof(buf),
            "[Thread] ShutdownWorkers: terminated %llu/%llu workers\n",
            (unsigned long long)killed, (unsigned long long)toKill.size());
        if (n > 0) {
            std::cout << std::string(buf, (size_t)n);
            std::cout.flush();
        }
    }
}

void SetNativeDisplayContext(CDisplayContextD3D11* display) {
    g_pNativeDisplay = display;
    std::cout << "[Kernel] Registered native Direct3D 11 display context at " << (void*)display << std::endl;
}

void InitializeKernelSubsystem(uint8_t* base) {
    DarkRecomp::InitializeFileSystem();
    // Eager symbol init so LogMissedIndirect's _ReturnAddress resolution
    // works from the first worker thread (lazy SymInitialize elsewhere runs
    // after the early misses are already logged as raw addresses).
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::cout << "[Kernel] Initialized Xbox 360 Kernel & XAM Subsystem for The Darkness." << std::endl;
}

} // namespace DarkRecomp
// Missed virtual/indirect calls (PPCSafeIndirect fallback). During 82104DC8
// bring-up most vtables are null (heap bypasses skipped constructors), so
// virtual calls silently return 0. Log the distinct guest targets so the
// renderer hook work knows which functions the engine actually reaches.
void LogMissedIndirect(uint32_t guest) {
    static std::unordered_map<uint32_t, uint64_t> s_missed;
    static std::mutex s_missedMutex;
    static uint64_t s_total = 0;
    uint64_t c = 0, total = 0;
    {
        std::lock_guard<std::mutex> lk(s_missedMutex);
        c = ++s_missed[guest];
        total = ++s_total;
    }
    // Line-atomic single write: the shared log locks per xsputn call, so a
    // chained << sequence interleaves with other threads. Format first.
    // Caller symbol (host return address -> PPC function+offset) is resolved
    // only for the first 3 hits per target: it maps which engine caller
    // attempts which virtual without paying SymFromAddr on the hot path.
    if (c <= 3 || (total % 20000) == 0) {
        char caller[96] = {};
        if (c <= 3) {
            void* ret = _ReturnAddress();
            HANDLE hp = GetCurrentProcess();
            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            PSYMBOL_INFO ps = reinterpret_cast<PSYMBOL_INFO>(symBuf);
            ps->SizeOfStruct = sizeof(SYMBOL_INFO);
            ps->MaxNameLen = 256;
            DWORD64 disp = 0;
            if (SymFromAddr(hp, (DWORD64)ret, &disp, ps)) {
                snprintf(caller, sizeof(caller), " from %s+0x%llX", ps->Name, disp);
            } else {
                snprintf(caller, sizeof(caller), " from 0x%p", ret);
            }
        }
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
            "[IndirectMiss] guest=0x%08X (#%llu for target, #%llu total)%s -> r3=0\n",
            guest, (unsigned long long)c, (unsigned long long)total, caller);
        if (n > 0) {
            std::cout << std::string(buf, (size_t)n);
            std::cout.flush();
        }
    }
}
extern "C" float roundevenf(float x) {
    return std::nearbyintf(x);
}
extern "C" double roundeven(double x) {
    return std::nearbyint(x);
}
