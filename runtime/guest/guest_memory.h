#pragma once
#include "guest_types.h"
#include <vector>
#include <memory>
#include <cassert>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif

namespace DarkRecomp {

class GuestMemory {
public:
    GuestMemory() : m_base(nullptr), m_size(0x100000000ULL) {} // 4 GB space
    ~GuestMemory() {
        Shutdown();
    }

    bool Initialize() {
#if defined(_WIN32)
        m_base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (m_base == nullptr) {
            m_base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        // NOTE (2026-09-03): Xbox low memory (0x0-0xFFFF) contains kernel
        // structures that retail code reads very early (e.g. *(0x193) via
        // *(0x82000814)). Guarding it faults before we emulate those
        // structures. Keep it mapped as zeroed memory for now (reads return
        // 0, matching the pre-guard behavior that reached heap init); a
        // future change should emulate the specific kernel pointers and
        // guard only NULL writes (e.g. `stw r0,0(0)` asserts) via a vectored
        // handler that distinguishes read vs write.
        return m_base != nullptr;
#else
        return false;
#endif
    }

    void Shutdown() {
#if defined(_WIN32)
        if (m_base) {
            VirtualFree(m_base, 0, MEM_RELEASE);
            m_base = nullptr;
        }
#endif
    }

    uint8_t* GetBase() const { return m_base; }

    inline void* Translate(uint32_t guestAddr) const {
        return m_base + guestAddr;
    }

    inline uint32_t MapVirtual(const void* hostPtr) const {
        return static_cast<uint32_t>(static_cast<const uint8_t*>(hostPtr) - m_base);
    }

    template<typename T>
    inline T Read(uint32_t guestAddr) const {
        return *reinterpret_cast<const be<T>*>(Translate(guestAddr));
    }

    template<typename T>
    inline void Write(uint32_t guestAddr, T value) {
        *reinterpret_cast<be<T>*>(Translate(guestAddr)) = value;
    }

private:
    uint8_t* m_base;
    size_t   m_size;
};

extern GuestMemory g_guestMemory;

} // namespace DarkRecomp
