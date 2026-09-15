#pragma once
#include <cstdint>
#include <vector>
#include "runtime/guest/guest_types.h"

namespace DarkRecomp {
    class CDisplayContextD3D11;
    void InitializeKernelSubsystem(uint8_t* base);
    void SetNativeDisplayContext(CDisplayContextD3D11* display);
    uint32_t GetGuestHeapOffset();
    void RegisterWorkerTid(uint32_t tid);
    std::vector<uint32_t> GetWorkerTids();
    void ShutdownWorkers();
}
