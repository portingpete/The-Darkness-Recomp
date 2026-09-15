#include "kernel_imports.h"
#include <iostream>
#include <unordered_map>
#include <string>
#include <mutex>

// Weak stubs for all Xbox 360 Kernel and XAM imported functions
// Any strong implementation in kernel_imports.cpp will automatically override these.
// Each unimplemented import logs its first 3 calls (throttled) so the reachable
// unimplemented set is visible in darkrecomp.log instead of failing silently.
static std::unordered_map<std::string, unsigned long long> g_weakCallCounts;
static std::mutex g_weakTraceMutex;
static void WeakTrace(const char* name) {
    std::lock_guard<std::mutex> lk(g_weakTraceMutex);
    unsigned long long &c = g_weakCallCounts[name];
    c++;
    if (c <= 3 || (c % 20000) == 0) {
        std::cout << "[Unimpl] " << name << " call #" << c << " (returning 0)" << std::endl;
        std::cout.flush();
    }
}

PPC_WEAK_FUNC(__imp__DbgBreakPoint) {
    WeakTrace("__imp__DbgBreakPoint");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__DbgPrint) {
    WeakTrace("__imp__DbgPrint");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ExCreateThread) {
    WeakTrace("__imp__ExCreateThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ExGetXConfigSetting) {
    WeakTrace("__imp__ExGetXConfigSetting");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ExRegisterTitleTerminateNotification) {
    WeakTrace("__imp__ExRegisterTitleTerminateNotification");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ExTerminateThread) {
    WeakTrace("__imp__ExTerminateThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__FscSetCacheElementCount) {
    WeakTrace("__imp__FscSetCacheElementCount");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__HalReturnToFirmware) {
    WeakTrace("__imp__HalReturnToFirmware");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__IoDismountVolume) {
    WeakTrace("__imp__IoDismountVolume");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__IoDismountVolumeByFileHandle) {
    WeakTrace("__imp__IoDismountVolumeByFileHandle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) {
    WeakTrace("__imp__KeAcquireSpinLockAtRaisedIrql");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeBugCheck) {
    WeakTrace("__imp__KeBugCheck");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeDelayExecutionThread) {
    WeakTrace("__imp__KeDelayExecutionThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeEnableFpuExceptions) {
    WeakTrace("__imp__KeEnableFpuExceptions");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeEnterCriticalRegion) {
    WeakTrace("__imp__KeEnterCriticalRegion");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeGetCurrentProcessType) {
    WeakTrace("__imp__KeGetCurrentProcessType");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeInitializeSemaphore) {
    WeakTrace("__imp__KeInitializeSemaphore");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeLeaveCriticalRegion) {
    WeakTrace("__imp__KeLeaveCriticalRegion");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeLockL2) {
    WeakTrace("__imp__KeLockL2");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeQueryPerformanceFrequency) {
    WeakTrace("__imp__KeQueryPerformanceFrequency");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeQuerySystemTime) {
    WeakTrace("__imp__KeQuerySystemTime");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeRaiseIrqlToDpcLevel) {
    WeakTrace("__imp__KeRaiseIrqlToDpcLevel");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeReleaseSemaphore) {
    WeakTrace("__imp__KeReleaseSemaphore");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) {
    WeakTrace("__imp__KeReleaseSpinLockFromRaisedIrql");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeResetEvent) {
    WeakTrace("__imp__KeResetEvent");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeResumeThread) {
    WeakTrace("__imp__KeResumeThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeSetAffinityThread) {
    WeakTrace("__imp__KeSetAffinityThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeSetBasePriorityThread) {
    WeakTrace("__imp__KeSetBasePriorityThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeSetEvent) {
    WeakTrace("__imp__KeSetEvent");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeTlsAlloc) {
    WeakTrace("__imp__KeTlsAlloc");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeTlsFree) {
    WeakTrace("__imp__KeTlsFree");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeTlsGetValue) {
    WeakTrace("__imp__KeTlsGetValue");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeTlsSetValue) {
    WeakTrace("__imp__KeTlsSetValue");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeUnlockL2) {
    WeakTrace("__imp__KeUnlockL2");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeWaitForMultipleObjects) {
    WeakTrace("__imp__KeWaitForMultipleObjects");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KeWaitForSingleObject) {
    WeakTrace("__imp__KeWaitForSingleObject");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KfAcquireSpinLock) {
    WeakTrace("__imp__KfAcquireSpinLock");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KfLowerIrql) {
    WeakTrace("__imp__KfLowerIrql");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KfReleaseSpinLock) {
    WeakTrace("__imp__KfReleaseSpinLock");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__KiApcNormalRoutineNop) {
    WeakTrace("__imp__KiApcNormalRoutineNop");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
    WeakTrace("__imp__MmAllocatePhysicalMemoryEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__MmFreePhysicalMemory) {
    WeakTrace("__imp__MmFreePhysicalMemory");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__MmGetPhysicalAddress) {
    WeakTrace("__imp__MmGetPhysicalAddress");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__MmQueryAddressProtect) {
    WeakTrace("__imp__MmQueryAddressProtect");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__MmQueryAllocationSize) {
    WeakTrace("__imp__MmQueryAllocationSize");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__MmQueryStatistics) {
    WeakTrace("__imp__MmQueryStatistics");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_WSACleanup) {
    WeakTrace("__imp__NetDll_WSACleanup");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_WSAEventSelect) {
    WeakTrace("__imp__NetDll_WSAEventSelect");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_WSAGetLastError) {
    WeakTrace("__imp__NetDll_WSAGetLastError");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_WSAStartup) {
    WeakTrace("__imp__NetDll_WSAStartup");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetCreateKey) {
    WeakTrace("__imp__NetDll_XNetCreateKey");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetDnsLookup) {
    WeakTrace("__imp__NetDll_XNetDnsLookup");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetDnsRelease) {
    WeakTrace("__imp__NetDll_XNetDnsRelease");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
    WeakTrace("__imp__NetDll_XNetGetTitleXnAddr");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetInAddrToXnAddr) {
    WeakTrace("__imp__NetDll_XNetInAddrToXnAddr");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetQosListen) {
    WeakTrace("__imp__NetDll_XNetQosListen");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetQosLookup) {
    WeakTrace("__imp__NetDll_XNetQosLookup");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetQosRelease) {
    WeakTrace("__imp__NetDll_XNetQosRelease");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetRandom) {
    WeakTrace("__imp__NetDll_XNetRandom");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetRegisterKey) {
    WeakTrace("__imp__NetDll_XNetRegisterKey");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetStartup) {
    WeakTrace("__imp__NetDll_XNetStartup");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetUnregisterKey) {
    WeakTrace("__imp__NetDll_XNetUnregisterKey");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetXnAddrToInAddr) {
    WeakTrace("__imp__NetDll_XNetXnAddrToInAddr");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_XNetXnAddrToMachineId) {
    WeakTrace("__imp__NetDll_XNetXnAddrToMachineId");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll___WSAFDIsSet) {
    WeakTrace("__imp__NetDll___WSAFDIsSet");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_accept) {
    WeakTrace("__imp__NetDll_accept");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_bind) {
    WeakTrace("__imp__NetDll_bind");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_closesocket) {
    WeakTrace("__imp__NetDll_closesocket");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_connect) {
    WeakTrace("__imp__NetDll_connect");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_getpeername) {
    WeakTrace("__imp__NetDll_getpeername");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_ioctlsocket) {
    WeakTrace("__imp__NetDll_ioctlsocket");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_listen) {
    WeakTrace("__imp__NetDll_listen");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_recv) {
    WeakTrace("__imp__NetDll_recv");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_recvfrom) {
    WeakTrace("__imp__NetDll_recvfrom");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_select) {
    WeakTrace("__imp__NetDll_select");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_send) {
    WeakTrace("__imp__NetDll_send");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_sendto) {
    WeakTrace("__imp__NetDll_sendto");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_setsockopt) {
    WeakTrace("__imp__NetDll_setsockopt");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NetDll_socket) {
    WeakTrace("__imp__NetDll_socket");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtAllocateVirtualMemory) {
    WeakTrace("__imp__NtAllocateVirtualMemory");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtCancelTimer) {
    WeakTrace("__imp__NtCancelTimer");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtClearEvent) {
    WeakTrace("__imp__NtClearEvent");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtClose) {
    WeakTrace("__imp__NtClose");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtCreateEvent) {
    WeakTrace("__imp__NtCreateEvent");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtCreateFile) {
    WeakTrace("__imp__NtCreateFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtCreateSemaphore) {
    WeakTrace("__imp__NtCreateSemaphore");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtCreateTimer) {
    WeakTrace("__imp__NtCreateTimer");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtDeviceIoControlFile) {
    WeakTrace("__imp__NtDeviceIoControlFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtDuplicateObject) {
    WeakTrace("__imp__NtDuplicateObject");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtFlushBuffersFile) {
    WeakTrace("__imp__NtFlushBuffersFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtFreeVirtualMemory) {
    WeakTrace("__imp__NtFreeVirtualMemory");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtOpenFile) {
    WeakTrace("__imp__NtOpenFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtQueryDirectoryFile) {
    WeakTrace("__imp__NtQueryDirectoryFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtQueryFullAttributesFile) {
    WeakTrace("__imp__NtQueryFullAttributesFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtQueryInformationFile) {
    WeakTrace("__imp__NtQueryInformationFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtQueryVolumeInformationFile) {
    WeakTrace("__imp__NtQueryVolumeInformationFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtReadFile) {
    WeakTrace("__imp__NtReadFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtReadFileScatter) {
    WeakTrace("__imp__NtReadFileScatter");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtReleaseSemaphore) {
    WeakTrace("__imp__NtReleaseSemaphore");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtResumeThread) {
    WeakTrace("__imp__NtResumeThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtSetEvent) {
    WeakTrace("__imp__NtSetEvent");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtSetInformationFile) {
    WeakTrace("__imp__NtSetInformationFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtSetTimerEx) {
    WeakTrace("__imp__NtSetTimerEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtSuspendThread) {
    WeakTrace("__imp__NtSuspendThread");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtWaitForSingleObjectEx) {
    WeakTrace("__imp__NtWaitForSingleObjectEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__NtWriteFile) {
    WeakTrace("__imp__NtWriteFile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ObCreateSymbolicLink) {
    WeakTrace("__imp__ObCreateSymbolicLink");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ObDeleteSymbolicLink) {
    WeakTrace("__imp__ObDeleteSymbolicLink");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ObDereferenceObject) {
    WeakTrace("__imp__ObDereferenceObject");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ObLookupThreadByThreadId) {
    WeakTrace("__imp__ObLookupThreadByThreadId");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ObOpenObjectByPointer) {
    WeakTrace("__imp__ObOpenObjectByPointer");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__ObReferenceObjectByHandle) {
    WeakTrace("__imp__ObReferenceObjectByHandle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlEnterCriticalSection) {
    WeakTrace("__imp__RtlEnterCriticalSection");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlFillMemoryUlong) {
    WeakTrace("__imp__RtlFillMemoryUlong");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlFreeAnsiString) {
    WeakTrace("__imp__RtlFreeAnsiString");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlImageXexHeaderField) {
    WeakTrace("__imp__RtlImageXexHeaderField");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlInitAnsiString) {
    WeakTrace("__imp__RtlInitAnsiString");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlInitUnicodeString) {
    WeakTrace("__imp__RtlInitUnicodeString");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlInitializeCriticalSection) {
    WeakTrace("__imp__RtlInitializeCriticalSection");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlLeaveCriticalSection) {
    WeakTrace("__imp__RtlLeaveCriticalSection");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlLowerChar) {
    WeakTrace("__imp__RtlLowerChar");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlMultiByteToUnicodeN) {
    WeakTrace("__imp__RtlMultiByteToUnicodeN");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlNtStatusToDosError) {
    WeakTrace("__imp__RtlNtStatusToDosError");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlRaiseException) {
    WeakTrace("__imp__RtlRaiseException");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlTimeFieldsToTime) {
    WeakTrace("__imp__RtlTimeFieldsToTime");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlTimeToTimeFields) {
    WeakTrace("__imp__RtlTimeToTimeFields");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlTryEnterCriticalSection) {
    WeakTrace("__imp__RtlTryEnterCriticalSection");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlUnicodeStringToAnsiString) {
    WeakTrace("__imp__RtlUnicodeStringToAnsiString");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__RtlUnwind) {
    WeakTrace("__imp__RtlUnwind");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__StfsControlDevice) {
    WeakTrace("__imp__StfsControlDevice");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__StfsCreateDevice) {
    WeakTrace("__imp__StfsCreateDevice");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdCallGraphicsNotificationRoutines) {
    WeakTrace("__imp__VdCallGraphicsNotificationRoutines");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdEnableDisableClockGating) {
    WeakTrace("__imp__VdEnableDisableClockGating");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    WeakTrace("__imp__VdEnableRingBufferRPtrWriteBack");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdGetCurrentDisplayGamma) {
    WeakTrace("__imp__VdGetCurrentDisplayGamma");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdGetCurrentDisplayInformation) {
    WeakTrace("__imp__VdGetCurrentDisplayInformation");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdGetSystemCommandBuffer) {
    WeakTrace("__imp__VdGetSystemCommandBuffer");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdInitializeEngines) {
    WeakTrace("__imp__VdInitializeEngines");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdInitializeRingBuffer) {
    WeakTrace("__imp__VdInitializeRingBuffer");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    WeakTrace("__imp__VdInitializeScalerCommandBuffer");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdIsHSIOTrainingSucceeded) {
    WeakTrace("__imp__VdIsHSIOTrainingSucceeded");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdPersistDisplay) {
    WeakTrace("__imp__VdPersistDisplay");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdQueryVideoFlags) {
    WeakTrace("__imp__VdQueryVideoFlags");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdQueryVideoMode) {
    WeakTrace("__imp__VdQueryVideoMode");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdRetrainEDRAM) {
    WeakTrace("__imp__VdRetrainEDRAM");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdRetrainEDRAMWorker) {
    WeakTrace("__imp__VdRetrainEDRAMWorker");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdSetDisplayMode) {
    WeakTrace("__imp__VdSetDisplayMode");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    WeakTrace("__imp__VdSetGraphicsInterruptCallback");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    WeakTrace("__imp__VdSetSystemCommandBufferGpuIdentifierAddress");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdShutdownEngines) {
    WeakTrace("__imp__VdShutdownEngines");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__VdSwap) {
    WeakTrace("__imp__VdSwap");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
    WeakTrace("__imp__XAudioGetVoiceCategoryVolume");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
    WeakTrace("__imp__XAudioGetVoiceCategoryVolumeChangeMask");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    WeakTrace("__imp__XAudioRegisterRenderDriverClient");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    WeakTrace("__imp__XAudioSubmitRenderDriverFrame");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    WeakTrace("__imp__XAudioUnregisterRenderDriverClient");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XGetAVPack) {
    WeakTrace("__imp__XGetAVPack");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XGetGameRegion) {
    WeakTrace("__imp__XGetGameRegion");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XGetLanguage) {
    WeakTrace("__imp__XGetLanguage");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XGetVideoMode) {
    WeakTrace("__imp__XGetVideoMode");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XMACreateContext) {
    WeakTrace("__imp__XMACreateContext");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XMAReleaseContext) {
    WeakTrace("__imp__XMAReleaseContext");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XMsgCancelIORequest) {
    WeakTrace("__imp__XMsgCancelIORequest");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XMsgInProcessCall) {
    WeakTrace("__imp__XMsgInProcessCall");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XMsgStartIORequest) {
    WeakTrace("__imp__XMsgStartIORequest");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XMsgStartIORequestEx) {
    WeakTrace("__imp__XMsgStartIORequestEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XNotifyGetNext) {
    WeakTrace("__imp__XNotifyGetNext");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XNotifyPositionUI) {
    WeakTrace("__imp__XNotifyPositionUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamAlloc) {
    WeakTrace("__imp__XamAlloc");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamContentClose) {
    WeakTrace("__imp__XamContentClose");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamContentCreateEnumerator) {
    WeakTrace("__imp__XamContentCreateEnumerator");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamContentCreateEx) {
    WeakTrace("__imp__XamContentCreateEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamContentDelete) {
    WeakTrace("__imp__XamContentDelete");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamContentGetDeviceState) {
    WeakTrace("__imp__XamContentGetDeviceState");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamEnumerate) {
    WeakTrace("__imp__XamEnumerate");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamFree) {
    WeakTrace("__imp__XamFree");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamGetExecutionId) {
    WeakTrace("__imp__XamGetExecutionId");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamGetSystemVersion) {
    WeakTrace("__imp__XamGetSystemVersion");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamInputGetCapabilities) {
    WeakTrace("__imp__XamInputGetCapabilities");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamInputGetKeystrokeEx) {
    WeakTrace("__imp__XamInputGetKeystrokeEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamInputGetState) {
    WeakTrace("__imp__XamInputGetState");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamInputSetState) {
    WeakTrace("__imp__XamInputSetState");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamLoaderGetLaunchData) {
    WeakTrace("__imp__XamLoaderGetLaunchData");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamLoaderLaunchTitle) {
    WeakTrace("__imp__XamLoaderLaunchTitle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamLoaderSetLaunchData) {
    WeakTrace("__imp__XamLoaderSetLaunchData");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamLoaderTerminateTitle) {
    WeakTrace("__imp__XamLoaderTerminateTitle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamNotifyCreateListener) {
    WeakTrace("__imp__XamNotifyCreateListener");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamSessionCreateHandle) {
    WeakTrace("__imp__XamSessionCreateHandle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamSessionRefObjByHandle) {
    WeakTrace("__imp__XamSessionRefObjByHandle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowAchievementsUI) {
    WeakTrace("__imp__XamShowAchievementsUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowDeviceSelectorUI) {
    WeakTrace("__imp__XamShowDeviceSelectorUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowDirtyDiscErrorUI) {
    WeakTrace("__imp__XamShowDirtyDiscErrorUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowFriendRequestUI) {
    WeakTrace("__imp__XamShowFriendRequestUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowFriendsUI) {
    WeakTrace("__imp__XamShowFriendsUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowGamerCardUIForXUID) {
    WeakTrace("__imp__XamShowGamerCardUIForXUID");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowMessageBoxUIEx) {
    WeakTrace("__imp__XamShowMessageBoxUIEx");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowPlayerReviewUI) {
    WeakTrace("__imp__XamShowPlayerReviewUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamShowSigninUI) {
    WeakTrace("__imp__XamShowSigninUI");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamTaskCloseHandle) {
    WeakTrace("__imp__XamTaskCloseHandle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamTaskSchedule) {
    WeakTrace("__imp__XamTaskSchedule");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamTaskShouldExit) {
    WeakTrace("__imp__XamTaskShouldExit");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserAreUsersFriends) {
    WeakTrace("__imp__XamUserAreUsersFriends");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserCheckPrivilege) {
    WeakTrace("__imp__XamUserCheckPrivilege");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserCreateAchievementEnumerator) {
    WeakTrace("__imp__XamUserCreateAchievementEnumerator");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserCreateStatsEnumerator) {
    WeakTrace("__imp__XamUserCreateStatsEnumerator");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserGetName) {
    WeakTrace("__imp__XamUserGetName");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserGetSigninInfo) {
    WeakTrace("__imp__XamUserGetSigninInfo");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserGetSigninState) {
    WeakTrace("__imp__XamUserGetSigninState");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserGetXUID) {
    WeakTrace("__imp__XamUserGetXUID");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamUserReadProfileSettings) {
    WeakTrace("__imp__XamUserReadProfileSettings");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamVoiceClose) {
    WeakTrace("__imp__XamVoiceClose");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamVoiceCreate) {
    WeakTrace("__imp__XamVoiceCreate");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamVoiceHeadsetPresent) {
    WeakTrace("__imp__XamVoiceHeadsetPresent");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamVoiceSubmitPacket) {
    WeakTrace("__imp__XamVoiceSubmitPacket");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XamWriteGamerTile) {
    WeakTrace("__imp__XamWriteGamerTile");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XeCryptSha) {
    WeakTrace("__imp__XeCryptSha");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XeKeysConsolePrivateKeySign) {
    WeakTrace("__imp__XeKeysConsolePrivateKeySign");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XeKeysConsoleSignatureVerification) {
    WeakTrace("__imp__XeKeysConsoleSignatureVerification");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XexCheckExecutablePrivilege) {
    WeakTrace("__imp__XexCheckExecutablePrivilege");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XexGetModuleHandle) {
    WeakTrace("__imp__XexGetModuleHandle");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp__XexGetProcedureAddress) {
    WeakTrace("__imp__XexGetProcedureAddress");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp____C_specific_handler) {
    WeakTrace("__imp____C_specific_handler");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

PPC_WEAK_FUNC(__imp___vsnprintf) {
    uint32_t destination = ctx.r3.u32;
    uint32_t capacity = ctx.r4.u32;
    uint32_t formatAddress = ctx.r5.u32;
    uint32_t vaListAddress = ctx.r6.u32;
    if (!destination || !capacity || !formatAddress || !vaListAddress) {
        ctx.r3.u64 = static_cast<uint32_t>(-1);
        return;
    }
    // Xbox PPC va_list points at the register-save area. Each saved GPR
    // occupies an eight-byte slot, with the value in the low 32 bits.
    uint32_t cursor = PPC_LOAD_U32(vaListAddress);
    std::string output;
    output.reserve(capacity);
    const char* format = reinterpret_cast<const char*>(base + formatAddress);
    auto nextArgument = [&]() {
        uint32_t value = PPC_LOAD_U32(cursor);
        cursor += 8;
        return value;
    };
    for (size_t i = 0; format[i] && output.size() < capacity; ++i) {
        if (format[i] != '%') {
            output.push_back(format[i]);
            continue;
        }
        if (format[++i] == '%') {
            output.push_back('%');
            continue;
        }
        unsigned width = 0;
        if (format[i] == '0') ++i;
        while (std::isdigit(static_cast<unsigned char>(format[i])))
            width = width * 10 + unsigned(format[i++] - '0');
        while (format[i] == '.' || std::isdigit(static_cast<unsigned char>(format[i]))) ++i;
        if (format[i] == 'l' || format[i] == 'I') {
            while (format[i] == 'l' || format[i] == 'I' || format[i] == '6' || format[i] == '4') ++i;
        }
        char specifier = format[i];
        char rendered[128] = {};
        uint32_t argument = nextArgument();
        switch (specifier) {
        case 's': {
            const char* value = argument ? reinterpret_cast<const char*>(base + argument) : "(null)";
            output.append(value, strnlen(value, 4096));
            continue;
        }
        case 'c':
            rendered[0] = char(argument);
            rendered[1] = '\0';
            break;
        case 'd': case 'i':
            if (width) sprintf_s(rendered, "%0*d", int(width), static_cast<int32_t>(argument));
            else sprintf_s(rendered, "%d", static_cast<int32_t>(argument));
            break;
        case 'u':
            if (width) sprintf_s(rendered, "%0*u", int(width), argument);
            else sprintf_s(rendered, "%u", argument);
            break;
        case 'x': case 'X':
            if (width) sprintf_s(rendered, specifier == 'x' ? "%0*x" : "%0*X", int(width), argument);
            else sprintf_s(rendered, specifier == 'x' ? "%x" : "%X", argument);
            break;
        case 'p':
            sprintf_s(rendered, "0x%08X", argument);
            break;
        default:
            output.push_back('%');
            output.push_back(specifier);
            continue;
        }
        output += rendered;
    }
    size_t written = (std::min)(output.size(), size_t(capacity - 1));
    memcpy(base + destination, output.data(), written);
    PPC_STORE_U8(destination + uint32_t(written), 0);
    ctx.r3.s64 = output.size() < capacity ? static_cast<int32_t>(output.size()) : -1;
}

PPC_WEAK_FUNC(__imp__sprintf) {
    WeakTrace("__imp__sprintf");
    ctx.r3.u64 = 0; // Default STATUS_SUCCESS / ERROR_SUCCESS
}

