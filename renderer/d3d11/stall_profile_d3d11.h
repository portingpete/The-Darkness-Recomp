#pragma once
#include <d3d11.h>
#include "runtime/native/stall_profiler.h"

namespace DarkRecomp {
// Observe the existing driver call; keep flags, resource, subresource and return
// status intact. Map can block for GPU use even with WRITE_DISCARD.
inline HRESULT stallProfileMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, D3D11_MAP mode, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped,
    const char* function = "ID3D11DeviceContext::Map") {
    Native::StallProfiler::Scope scope(Native::StallProfiler::Section::Wait,
        function, 0, 0, reinterpret_cast<uint64_t>(resource), "D3D11 resource");
    return context->Map(resource, subresource, mode, flags, mapped);
}
}
