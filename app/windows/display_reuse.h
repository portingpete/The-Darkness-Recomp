#pragma once
#include <windows.h>

// The renderer owns the image independently of the flip-discard backbuffer.
// A complete copy accepted by DXGI can be retained until the next invalidation.
class NativeDisplayReuse {
    bool pending_ = true;
public:
    void invalidate() noexcept { pending_ = true; }
    bool shouldCopy(bool frameOpen) const noexcept { return pending_ && !frameOpen; }
    void presented(HRESULT status) noexcept { pending_ = status != S_OK; }
};
