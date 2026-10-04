#pragma once
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#include <array>
#include <cstdint>
#include "ppc_config.h"

namespace DarkRecomp::Native {
enum class DispatcherImageSpanStatus { fallback, accepted, denied };
struct DispatcherImageSpanResult {
    DispatcherImageSpanStatus status = DispatcherImageSpanStatus::fallback;
    uint8_t* checkedEnd = nullptr;
    unsigned pages = 0;
    bool apiFailure = false;
    bool nonresident = false;
};
inline bool dispatcherPageAccessible(DWORD protection, bool writable) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD basic = protection & 0xff;
    const bool canWrite = basic == PAGE_READWRITE || basic == PAGE_WRITECOPY ||
        basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
    return writable ? canWrite : canWrite || basic == PAGE_READONLY || basic == PAGE_EXECUTE_READ;
}
// Only fully bounded image spans of at most two pages are eligible. Every
// call queries current page attributes; uncertain residency/API results use
// the caller's original VirtualQuery path. No page content is touched here.
template<class Query> inline DispatcherImageSpanResult dispatcherImageSpan(
    uint8_t* base, uint32_t address, uint32_t bytes, bool writable, Query&& query) {
    DispatcherImageSpanResult result;
    const uint64_t end = uint64_t(address) + bytes;
    if (!base || !bytes || address < PPC_IMAGE_BASE || end > PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
        return result;
    constexpr uint32_t pageBytes = 4096;
    const uint32_t first = address & ~(pageBytes - 1);
    const uint32_t last = uint32_t(end - 1) & ~(pageBytes - 1);
    if (last - first > pageBytes) return result;
    result.pages = first == last ? 1 : 2;
    std::array<PSAPI_WORKING_SET_EX_INFORMATION, 2> pages{};
    pages[0].VirtualAddress = base + first;
    if (result.pages == 2) pages[1].VirtualAddress = base + last;
    if (!query(pages.data(), DWORD(result.pages * sizeof(pages[0])))) {
        result.apiFailure = true;
        return result;
    }
    for (unsigned i = 0; i < result.pages; ++i) {
        if (!pages[i].VirtualAttributes.Valid) {
            result.nonresident = true;
            return result;
        }
    }
    for (unsigned i = 0; i < result.pages; ++i) {
        if (!dispatcherPageAccessible(DWORD(pages[i].VirtualAttributes.Win32Protection), writable)) {
            result.status = DispatcherImageSpanStatus::denied;
            return result;
        }
    }
    result.status = DispatcherImageSpanStatus::accepted;
    result.checkedEnd = base + uint64_t(last) + pageBytes;
    return result;
}
}
