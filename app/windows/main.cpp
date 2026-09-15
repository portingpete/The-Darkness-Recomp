#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <iostream>
#include <thread>
#include <filesystem>
#include <io.h>
#include "runtime/guest/guest_memory.h"
#include "runtime/guest/guest_types.h"
#include "runtime/loader/pe_image_loader.h"
#include "runtime/kernel/kernel_imports.h"
#include "renderer/d3d11/display_context_d3d11.h"

void _xstart(PPCContext& __restrict ctx, uint8_t* base);

namespace DarkRecomp {
    GuestMemory g_guestMemory;
    PPCContext  g_mainContext;
    PEImageLoader g_imageLoader;
    CDisplayContextD3D11 g_displayContext;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
        }
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

LONG WINAPI CrashHandler(PEXCEPTION_POINTERS p) {
    if (p && p->ExceptionRecord) {
        DWORD code = p->ExceptionRecord->ExceptionCode;
        if (code == 0x406D1388 || code == 0x000006BA || code == 0x40010006 || code == 0x80000003) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // Minimal raw crash record FIRST (no cout/dbghelp that can fault or
        // deadlock when multiple threads crash during concurrent logging).
        {
            char buf[256];
            void* ea = p->ExceptionRecord->ExceptionAddress;
            void* fa = (void*)(p->ExceptionRecord->NumberParameters >= 2
                ? p->ExceptionRecord->ExceptionInformation[1] : 0);
            DWORD tid = GetCurrentThreadId();
            int n = 0;
            // Manual hex to avoid printf-family faults; best-effort only.
            __try {
                n = sprintf_s(buf, sizeof(buf),
                    "CRASH tid=%lu code=0x%08X at=0x%p fault=0x%p rip=0x%llX rsp=0x%llX\n",
                    (unsigned long)tid, (unsigned)code, ea, fa,
                    p->ContextRecord ? p->ContextRecord->Rip : 0,
                    p->ContextRecord ? p->ContextRecord->Rsp : 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
            if (n > 0) {
                HANDLE hf = CreateFileA("darkrecomp_crash_min.txt", GENERIC_WRITE, FILE_SHARE_READ,
                                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hf != INVALID_HANDLE_VALUE) {
                    DWORD w = 0;
                    WriteFile(hf, buf, (DWORD)n, &w, nullptr);
                    FlushFileBuffers(hf);
                    CloseHandle(hf);
                }
            }
        }
        if (code >= 0x80000000) {
            FILE* f = nullptr;
            fopen_s(&f, "darkrecomp_crash.txt", "w");
            if (f) {
                fprintf(f, "=================== DARKRECOMP CRASH REPORT ===================\n");
                fprintf(f, "Exception Code:    0x%08X\n", (unsigned int)p->ExceptionRecord->ExceptionCode);
                fprintf(f, "Exception Address: 0x%p\n", p->ExceptionRecord->ExceptionAddress);
                if (p->ExceptionRecord->NumberParameters >= 2) {
                    const char* op = p->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" :
                                     p->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXECUTE";
                    fprintf(f, "Access Type:       %s at 0x%p\n", op, (void*)p->ExceptionRecord->ExceptionInformation[1]);
                }
                if (p->ContextRecord) {
                    fprintf(f, "RIP: 0x%016llX  RSP: 0x%016llX\n", p->ContextRecord->Rip, p->ContextRecord->Rsp);
                    fprintf(f, "RAX: 0x%016llX  RCX: 0x%016llX  RDX: 0x%016llX\n", p->ContextRecord->Rax, p->ContextRecord->Rcx, p->ContextRecord->Rdx);
                    fprintf(f, "RBX: 0x%016llX  RSI: 0x%016llX  RDI: 0x%016llX\n", p->ContextRecord->Rbx, p->ContextRecord->Rsi, p->ContextRecord->Rdi);
                    fprintf(f, "R8:  0x%016llX  R9:  0x%016llX  R10: 0x%016llX\n", p->ContextRecord->R8, p->ContextRecord->R9, p->ContextRecord->R10);
                    fprintf(f, "R11: 0x%016llX  R12: 0x%016llX  R13: 0x%016llX\n", p->ContextRecord->R11, p->ContextRecord->R12, p->ContextRecord->R13);
                    fprintf(f, "R14: 0x%016llX  R15: 0x%016llX\n", p->ContextRecord->R14, p->ContextRecord->R15);

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
                    fprintf(f, "\nCall Stack:\n");
                    for (int frame = 0; frame < 32; frame++) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &ctxCopy, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                        if (sf.AddrPC.Offset == 0) break;

                        char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
                        PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
                        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
                        pSym->MaxNameLen = MAX_SYM_NAME;

                        DWORD64 disp = 0;
                        if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) {
                            IMAGEHLP_LINE64 line = {};
                            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                            DWORD lineDisp = 0;
                            if (SymGetLineFromAddr64(hProcess, sf.AddrPC.Offset, &lineDisp, &line)) {
                                fprintf(f, "  [%02d] %s (0x%llX) in %s:%lu\n", frame, pSym->Name, sf.AddrPC.Offset, line.FileName, line.LineNumber);
                            } else {
                                fprintf(f, "  [%02d] %s (0x%llX)+0x%llX\n", frame, pSym->Name, sf.AddrPC.Offset, disp);
                            }
                        } else {
                            fprintf(f, "  [%02d] 0x%llX\n", frame, sf.AddrPC.Offset);
                        }
                    }
                }
                fprintf(f, "===============================================================\n");
                fflush(f);
                fclose(f);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void (WINAPI *g_origExitProcess)(UINT) = ExitProcess;
static BOOL (WINAPI *g_origTerminateProcess)(HANDLE, UINT) = TerminateProcess;

static void PrintCurrentStackTrace() {
    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();
    SymInitialize(hProcess, nullptr, TRUE);

    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);

    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;

    for (int frame = 0; frame < 32; frame++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        if (sf.AddrPC.Offset == 0) break;

        char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
        PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
        pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSym->MaxNameLen = MAX_SYM_NAME;

        DWORD64 disp = 0;
        if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) {
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(hProcess, sf.AddrPC.Offset, &lineDisp, &line)) {
                std::cout << "  [" << frame << "] " << pSym->Name << " (" << line.FileName << ":" << line.LineNumber << ")" << std::endl;
            } else {
                std::cout << "  [" << frame << "] " << pSym->Name << "+0x" << std::hex << disp << std::dec << std::endl;
            }
        } else {
            std::cout << "  [" << frame << "] 0x" << std::hex << sf.AddrPC.Offset << std::dec << std::endl;
        }
    }
}

static void WINAPI HookedExitProcess(UINT uExitCode) {
    std::cout << "\n==========================================================" << std::endl;
    std::cout << "[INTERCEPT] ExitProcess called! ExitCode: 0x" << std::hex << uExitCode << std::dec << std::endl;
    PrintCurrentStackTrace();
    std::cout << "==========================================================" << std::endl;
    std::cout.flush();
    Sleep(3000);
    g_origExitProcess(uExitCode);
}

static BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode) {
    std::cout << "\n==========================================================" << std::endl;
    std::cout << "[INTERCEPT] TerminateProcess called! ExitCode: 0x" << std::hex << uExitCode << std::dec << std::endl;
    PrintCurrentStackTrace();
    std::cout << "==========================================================" << std::endl;
    std::cout.flush();
    Sleep(3000);
    return g_origTerminateProcess(hProcess, uExitCode);
}

static void HookMemoryFunction(void* target, void* hook) {
    DWORD oldProtect;
    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
    uint8_t jmp[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    uint64_t addr = reinterpret_cast<uint64_t>(hook);
    memcpy(jmp + 6, &addr, 8);
    memcpy(target, jmp, 14);
    VirtualProtect(target, 14, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 14);
}

typedef LONG NTSTATUS;
static NTSTATUS (NTAPI *g_origNtTerminateProcess)(HANDLE, NTSTATUS) = nullptr;

static NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus) {
    FILE* f = nullptr;
    fopen_s(&f, "darkrecomp_terminate.txt", "w");
    if (f) {
        fprintf(f, "=================== TERMINATION REPORT ===================\n");
        fprintf(f, "ExitStatus: 0x%08X (%d)\n", (unsigned int)ExitStatus, (int)ExitStatus);
        fprintf(f, "ProcessHandle: 0x%p\n", ProcessHandle);

        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();
        SymInitialize(hProcess, nullptr, TRUE);

        CONTEXT ctx = {};
        RtlCaptureContext(&ctx);

        STACKFRAME64 sf = {};
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;
        sf.AddrStack.Mode = AddrModeFlat;

        fprintf(f, "\nCall Stack:\n");
        for (int frame = 0; frame < 32; frame++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            if (sf.AddrPC.Offset == 0) break;

            char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
            PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
            pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
            pSym->MaxNameLen = MAX_SYM_NAME;

            DWORD64 disp = 0;
            if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) {
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisp = 0;
                if (SymGetLineFromAddr64(hProcess, sf.AddrPC.Offset, &lineDisp, &line)) {
                    fprintf(f, "  [%02d] %s in %s:%lu\n", frame, pSym->Name, line.FileName, line.LineNumber);
                } else {
                    fprintf(f, "  [%02d] %s + 0x%llX\n", frame, pSym->Name, disp);
                }
            } else {
                fprintf(f, "  [%02d] 0x%llX\n", frame, sf.AddrPC.Offset);
            }
        }
        fprintf(f, "==========================================================\n");
        fflush(f);
        fclose(f);
    }
    Sleep(2000);
    return 0;
}

static void HookTerminationAPIs() {
    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (hK32) {
        void* pExit = (void*)GetProcAddress(hK32, "ExitProcess");
        if (pExit) HookMemoryFunction(pExit, (void*)HookedExitProcess);
        void* pTerm = (void*)GetProcAddress(hK32, "TerminateProcess");
        if (pTerm) HookMemoryFunction(pTerm, (void*)HookedTerminateProcess);
    }
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
        void* pNtTerm = (void*)GetProcAddress(hNtdll, "NtTerminateProcess");
        if (pNtTerm) HookMemoryFunction(pNtTerm, (void*)HookedNtTerminateProcess);
    }
    std::cout << "[Hook] Installed ExitProcess, TerminateProcess, and NtTerminateProcess hooks." << std::endl;
}

static LONG GameThreadFilter(PEXCEPTION_POINTERS p) {
    DWORD code = p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionCode : 0;
    void* fault = nullptr;
    ULONG_PTR guest = 0;
    bool isGuest = false;
    if (p && p->ExceptionRecord && p->ExceptionRecord->NumberParameters >= 2) {
        fault = (void*)p->ExceptionRecord->ExceptionInformation[1];
        uint8_t* base = DarkRecomp::g_guestMemory.GetBase();
        if (base && fault >= base && fault < base + 0x100000000ULL) {
            guest = (ULONG_PTR)((uint8_t*)fault - base);
            isGuest = true;
        }
    }
    std::cout << "[GameThread] Exception 0x" << std::hex << code << std::dec
              << " at host " << (p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionAddress : nullptr);
    if (fault) {
        std::cout << " fault host=" << fault;
        if (isGuest) std::cout << " guest=0x" << std::hex << guest << std::dec;
    }
    std::cout << std::endl;
    // Dump registers + faulting instruction to distinguish guest vs host faults.
    if (p && p->ContextRecord) {
        CONTEXT* c = p->ContextRecord;
        std::cout << "[GameThread] Regs RIP=0x" << std::hex << c->Rip
                  << " RSP=0x" << c->Rsp << " RBP=0x" << c->Rbp
                  << " RAX=0x" << c->Rax << " RBX=0x" << c->Rbx
                  << " RCX(ctx)=0x" << c->Rcx << " RDX(base)=0x" << c->Rdx
                  << " RSI=0x" << c->Rsi << " RDI=0x" << c->Rdi
                  << " R8=0x" << c->R8 << " R9=0x" << c->R9
                  << " R10=0x" << c->R10 << " R11=0x" << c->R11
                  << " R12=0x" << c->R12 << " R13=0x" << c->R13
                  << " R14=0x" << c->R14 << " R15=0x" << c->R15 << std::dec << std::endl;
        uint8_t* base = DarkRecomp::g_guestMemory.GetBase();
        std::cout << "[GameThread] Expected base=0x" << std::hex << (void*)base << std::dec << std::endl;
        // Best-effort dump of faulting host instruction bytes.
        __try {
            uint8_t* rip = (uint8_t*)c->Rip;
            std::cout << "[GameThread] Code at RIP:";
            for (int i = 0; i < 32; i++) {
                char tmp[8]; sprintf_s(tmp, " %02X", rip[i]);
                std::cout << tmp;
            }
            std::cout << std::endl;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Dump host stack at fault using the exception context (game thread's own context).
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
        std::cout << "[GameThread] Fault stack:" << std::endl;
        for (int frame = 0; frame < 24; frame++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &ctxCopy,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrPC.Offset == 0) break;
            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
            pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
            pSym->MaxNameLen = 256;
            DWORD64 disp = 0;
            std::string name;
            if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) name = pSym->Name;
            else { char tmp[32]; sprintf_s(tmp, "0x%llX", sf.AddrPC.Offset); name = tmp; }
            std::cout << "   [#" << frame << "] " << name << "+0x" << std::hex << disp << std::dec << std::endl;
        }
    }
    std::cout.flush();
    FILE* f = nullptr;
    fopen_s(&f, "darkrecomp_game_exception.txt", "w");
    if (f) {
        fprintf(f, "GameThread exception 0x%08X fault host=%p", (unsigned)code, fault);
        if (isGuest) fprintf(f, " guest=0x%08X", (unsigned)guest);
        fprintf(f, "\n");
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void RunGameThread() {
    std::cout << "[GameThread] Starting recompiled PowerPC entry point (_xstart at 0x828AA3E8)..." << std::endl;
    std::cout.flush();
    __try {
        _xstart(DarkRecomp::g_mainContext, DarkRecomp::g_guestMemory.GetBase());
        std::cout << "[GameThread] _xstart finished execution cleanly." << std::endl;
    } __except (GameThreadFilter(GetExceptionInformation())) {
        std::cout << "[GameThread] _xstart terminated via exception filter." << std::endl;
    }
    // Game init is done (clean exit via unconditional TerminateTitle). Kill
    // worker retry-loop spinners now: otherwise they flood IndirectMiss at
    // ~400k/sec on null vtables for the life of the window pump.
    DarkRecomp::ShutdownWorkers();
    std::cout.flush();
}

static HANDLE g_hGameThread = nullptr;
static DWORD g_gameThreadId = 0;

static void PrintStackForThread(HANDLE hThread, const char* tag) {
    if (SuspendThread(hThread) == (DWORD)-1) return;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(hThread, &ctx)) {
        std::cout << "[Watchdog] " << tag << " game-thread RIP=0x" << std::hex << ctx.Rip
                  << " RSP=0x" << ctx.Rsp << " RCX=0x" << ctx.Rcx << std::dec << std::endl;
        // Dump code bytes at RIP to identify spin loops (which guest addrs
        // are polled). Best-effort; RIP is user-mode PPC lib code.
        {
            __try {
                uint8_t* rip = (uint8_t*)ctx.Rip;
                std::cout << "[Watchdog] Code at RIP:";
                for (int i = 0; i < 24; i++) {
                    char tmp[8]; sprintf_s(tmp, " %02X", rip[i]);
                    std::cout << tmp;
                }
                std::cout << std::endl;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        // Walk host stack of the suspended thread.
        HANDLE hProcess = GetCurrentProcess();
        SymInitialize(hProcess, nullptr, TRUE);
        STACKFRAME64 sf = {};
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;
        sf.AddrStack.Mode = AddrModeFlat;
        CONTEXT ctxCopy = ctx;
        for (int frame = 0; frame < 24; frame++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &sf, &ctxCopy,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrPC.Offset == 0) break;
            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
            pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
            pSym->MaxNameLen = 256;
            DWORD64 disp = 0;
            std::string name;
            if (SymFromAddr(hProcess, sf.AddrPC.Offset, &disp, pSym)) name = pSym->Name;
            else { char tmp[32]; sprintf_s(tmp, "0x%llX", sf.AddrPC.Offset); name = tmp; }
            std::cout << "   [#" << frame << "] " << name << "+0x" << std::hex << disp << std::dec << std::endl;
            // Stop at PPC entry to keep log short.
            if (name.find("_xstart") != std::string::npos && frame > 2) break;
        }
        std::cout.flush();
    }
    ResumeThread(hThread);
}

static DWORD WINAPI WatchdogProc(LPVOID) {
    for (int i = 1; i <= 450; i++) {
        Sleep(4000);
        if (!g_hGameThread) break;
        DWORD exitCode = 0;
        if (GetExitCodeThread(g_hGameThread, &exitCode) && exitCode != STILL_ACTIVE) {
            std::cout << "[Watchdog] game thread exited with code " << exitCode << std::endl;
            break;
        }
        // Log every 4s for the first minute, then every ~5th to reduce spam
        // during long resource-init phases (refcount-heavy, minutes long).
        if (i <= 15 || (i % 5) == 0) {
        char tag[64];
        sprintf_s(tag, "heartbeat %ds - game thread still alive", i * 4);
        PrintStackForThread(g_hGameThread, tag);
        {
            uint32_t ho = DarkRecomp::GetGuestHeapOffset();
            std::cout << "[Watchdog] guest heap offset=0x" << std::hex << ho << std::dec << std::endl;
        }
        // Every ~40s, snapshot ALL threads (3 frames each) to catch lock
        // holders / workers stuck while holding kernel mutexes.
        if ((i % 10) == 0) {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                DWORD pid = GetCurrentProcessId();
                std::vector<uint32_t> workerTids = DarkRecomp::GetWorkerTids();
                THREADENTRY32 te = { sizeof(te) };
                std::cout << "[Watchdog] thread snapshot:" << std::endl;
                if (Thread32First(hSnap, &te)) do {
                    if (te.th32OwnerProcessID != pid) continue;
                    if (te.th32ThreadID == GetCurrentThreadId()) continue;
                    bool isWorker = false;
                    for (uint32_t wt : workerTids) { if (wt == te.th32ThreadID) { isWorker = true; break; } }
                    HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                    if (!ht) continue;
                    if (SuspendThread(ht) == (DWORD)-1) { CloseHandle(ht); continue; }
                    CONTEXT c = {};
                    c.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(ht, &c)) {
                        HANDLE hp = GetCurrentProcess();
                        CONTEXT cc = c;
                        STACKFRAME64 sf = {};
                        sf.AddrPC.Offset = c.Rip;
                        sf.AddrPC.Mode = AddrModeFlat;
                        sf.AddrFrame.Offset = c.Rbp;
                        sf.AddrFrame.Mode = AddrModeFlat;
                        sf.AddrStack.Offset = c.Rsp;
                        sf.AddrStack.Mode = AddrModeFlat;
                        std::cout << "   [TID " << te.th32ThreadID << (isWorker ? " worker]" : "]");
                        int maxFrames = isWorker ? 12 : 3;
                        for (int f = 0; f < maxFrames; f++) {
                            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hp, ht, &sf, &cc,
                                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                                break;
                            if (sf.AddrPC.Offset == 0) break;
                            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
                            PSYMBOL_INFO ps = reinterpret_cast<PSYMBOL_INFO>(symBuf);
                            ps->SizeOfStruct = sizeof(SYMBOL_INFO);
                            ps->MaxNameLen = 256;
                            DWORD64 disp = 0;
                            std::string nm;
                            if (SymFromAddr(hp, sf.AddrPC.Offset, &disp, ps)) nm = ps->Name;
                            else { char tmp[32]; sprintf_s(tmp, "0x%llX", sf.AddrPC.Offset); nm = tmp; }
                            std::cout << " " << nm << "+0x" << std::hex << disp << std::dec << ";";
                        }
                        std::cout << std::endl;
                    }
                    ResumeThread(ht);
                    CloseHandle(ht);
                } while (Thread32Next(hSnap, &te));
                CloseHandle(hSnap);
                std::cout.flush();
            }
        }
        }
    }
    return 0;
}

static HANDLE g_hSharedLog = INVALID_HANDLE_VALUE;
// Global log serialization: game + 7 workers + watchdog write concurrently.
// The previous lock-free streambuf raced (interleaved lines + heap corruption
// crashes mid-line). All output paths funnel through here.
static CRITICAL_SECTION g_logLock;
static bool g_logLockInit = false;

class SharedLogBuf : public std::streambuf {
protected:
    int overflow(int c) override {
        if (c != EOF) {
            if (g_logLockInit) EnterCriticalSection(&g_logLock);
            char ch = static_cast<char>(c);
            DWORD written = 0;
            if (g_hSharedLog != INVALID_HANDLE_VALUE) {
                WriteFile(g_hSharedLog, &ch, 1, &written, nullptr);
            }
            char str[2] = { ch, 0 };
            OutputDebugStringA(str);
            if (ch == '\n' && g_hSharedLog != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(g_hSharedLog);
            }
            if (g_logLockInit) LeaveCriticalSection(&g_logLock);
        }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (g_logLockInit) EnterCriticalSection(&g_logLock);
        DWORD written = 0;
        if (g_hSharedLog != INVALID_HANDLE_VALUE && n > 0) {
            WriteFile(g_hSharedLog, s, static_cast<DWORD>(n), &written, nullptr);
        }
        if (n > 0) {
            std::string tmp(s, n);
            OutputDebugStringA(tmp.c_str());
        }
        if (g_logLockInit) {
            if (g_hSharedLog != INVALID_HANDLE_VALUE) FlushFileBuffers(g_hSharedLog);
            LeaveCriticalSection(&g_logLock);
        }
        return n;
    }
};
static SharedLogBuf g_sharedLogBuf;
static void OnExitCleanup() {
    std::cout << "[Runtime] Process exiting via atexit / exit()." << std::endl;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    InitializeCriticalSection(&g_logLock);
    g_logLockInit = true;
    atexit(OnExitCleanup);
    SetUnhandledExceptionFilter(CrashHandler);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path root = std::filesystem::path(exePath).parent_path();
    if (!std::filesystem::exists(root / "Darkness")) {
        if (std::filesystem::exists(root.parent_path().parent_path() / "Darkness")) {
            root = root.parent_path().parent_path();
        }
    }
    std::error_code ec;
    std::filesystem::current_path(root, ec);

    std::string logPath = (root / "darkrecomp.log").string();
    g_hSharedLog = CreateFileA(
        logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    std::cout.rdbuf(&g_sharedLogBuf);
    std::cerr.rdbuf(&g_sharedLogBuf);

    HookTerminationAPIs();

    std::cout << "==========================================================" << std::endl;
    std::cout << " The Darkness (2007) - 100% Native PC Port (DarkRecomp)   " << std::endl;
    std::cout << " Engine: Starbreeze P5 (Recompiled Native x86_64)         " << std::endl;
    std::cout << " Renderer: Direct3D 11 Native Deferred Pipeline           " << std::endl;
    std::cout << "==========================================================" << std::endl;
    std::cout << "[Init] Working directory resolved to: " << root.string() << std::endl;

    // 1. Initialize 4GB virtual address space
    if (!DarkRecomp::g_guestMemory.Initialize()) {
        std::cerr << "Fatal: Failed to map 4GB guest virtual address space!" << std::endl;
        return 1;
    }
    std::cout << "[Init] 4GB Guest memory space reserved at: " << (void*)DarkRecomp::g_guestMemory.GetBase() << std::endl;

    // 2. Load The Darkness PE Image (Sections, Data, Classes, Vtables)
    if (!DarkRecomp::g_imageLoader.Load("Darkness/basefile.exe", DarkRecomp::g_guestMemory)) {
        std::cerr << "Fatal: Failed to load Darkness PE binary image!" << std::endl;
        return 1;
    }
    // Integrity check: format string at 0x82007898 must be "[XAPI RETURN VALUE]".
    {
        uint8_t* b = DarkRecomp::g_guestMemory.GetBase();
        const char* s = (const char*)(b + 0x82007898);
        std::cout << "[Init] Format-string check @0x82007898: \""
                  << std::string(s, strnlen(s, 32)) << "\"" << std::endl;
    }
    // Write-protect headers + .rdata + .pdata (guest 0x82000000-0x820A0000).
    // The format string at 0x82007898 gets zeroed mid-boot by a stray game
    // memset; protecting it turns silent corruption into a fault with a
    // PPC stack identifying the writer. (.text/.data stay writable.)
    {
        uint8_t* b = DarkRecomp::g_guestMemory.GetBase();
        DWORD old = 0;
        if (VirtualProtect(b + 0x82000000, 0xA0000, PAGE_READONLY, &old)) {
            std::cout << "[Init] Write-protected guest .rdata (0x82000000-0x820A0000)." << std::endl;
        } else {
            std::cout << "[Init] WARNING: failed to protect guest .rdata (" << GetLastError() << ")." << std::endl;
        }
    }

    // 3. Populate Recompiled Function Lookup Table (for indirect calls / vtables)
    DarkRecomp::g_imageLoader.SetupLookupTable(DarkRecomp::g_guestMemory.GetBase());

    // 4. Initialize Primary Guest Thread Execution Context
    DarkRecomp::g_imageLoader.SetupThreadContext(DarkRecomp::g_mainContext, DarkRecomp::g_guestMemory.GetBase());

    // 5. Initialize Kernel & XAM Subsystem
    DarkRecomp::InitializeKernelSubsystem(DarkRecomp::g_guestMemory.GetBase());

    // 6. Create Win32 Window
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, LoadCursor(nullptr, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr, "DarkRecompWnd", nullptr };
    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowExA(
        0,
        "DarkRecompWnd", "The Darkness (2007) - Native PC Port",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 1280, 720,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (!hWnd) {
        std::cerr << "Fatal: Failed to create window, error: " << GetLastError() << std::endl;
        return 1;
    }
    std::cout << "[Init] Native game window created (HWND = 0x" << std::hex << (uintptr_t)hWnd << std::dec << ")." << std::endl;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // 7. Initialize Direct3D 11 Display Context
    DarkRecomp::g_displayContext.Init(hWnd, 1280, 720);
    DarkRecomp::SetNativeDisplayContext(&DarkRecomp::g_displayContext);
    std::cout << "[Init] Direct3D 11 Display Context initialized and hooked to P5 engine." << std::endl;

    // 8. Start Game Execution on Dedicated Thread with 64MB Host Stack
    // Keep the handle open so the watchdog can sample the game thread.
    DWORD gameThreadId = 0;
    HANDLE hGameThread = CreateThread(
        nullptr,
        64 * 1024 * 1024,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(RunGameThread),
        nullptr,
        STACK_SIZE_PARAM_IS_A_RESERVATION,
        &gameThreadId
    );
    if (hGameThread) {
        g_hGameThread = hGameThread;
        g_gameThreadId = gameThreadId;
        HANDLE hWatchdog = CreateThread(nullptr, 0, WatchdogProc, nullptr, 0, nullptr);
        if (hWatchdog) CloseHandle(hWatchdog);
    }

    // 9. Windows Message Pump & Presentation Loop
    std::cout << "[Runtime] Main window active, entering presentation pump..." << std::endl;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            DarkRecomp::g_displayContext.BeginScene();
            DarkRecomp::g_displayContext.Clear(0, 0.04f, 0.04f, 0.08f, 1.0f, 1.0f);
            DarkRecomp::g_displayContext.EndScene();
            DarkRecomp::g_displayContext.Present();
            Sleep(16); // ~60 FPS presentation pacing
        }
    }

    std::cout << "[Runtime] Window message pump exited with WM_QUIT." << std::endl;
    // Stop the game thread BEFORE tearing down display/guest memory.
    // Otherwise it faults on freed/unmapped guest RAM during teardown and
    // pollutes the log with a bogus GameThread exception.
    if (g_hGameThread) {
        TerminateThread(g_hGameThread, 0);
        CloseHandle(g_hGameThread);
        g_hGameThread = nullptr;
    }
    DarkRecomp::ShutdownWorkers();
    DarkRecomp::g_displayContext.Destroy();
    DarkRecomp::g_guestMemory.Shutdown();
    return 0;
}
