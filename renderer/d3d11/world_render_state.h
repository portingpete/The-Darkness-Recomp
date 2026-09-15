#pragma once
#include <d3d11.h>
#include <cstdint>

namespace DarkRecomp::WorldRenderState {
// Original 82066B20 reverses comparison ordering for the engine's reversed Z.
constexpr D3D11_COMPARISON_FUNC depthComparison(unsigned c) {
    constexpr D3D11_COMPARISON_FUNC table[]{D3D11_COMPARISON_NEVER,D3D11_COMPARISON_NEVER,
        D3D11_COMPARISON_GREATER,D3D11_COMPARISON_EQUAL,D3D11_COMPARISON_GREATER_EQUAL,
        D3D11_COMPARISON_LESS,D3D11_COMPARISON_NOT_EQUAL,D3D11_COMPARISON_LESS_EQUAL,D3D11_COMPARISON_ALWAYS};
    return table[c<=8?c:0];
}
constexpr D3D11_COMPARISON_FUNC stencilComparison(unsigned c) {
    // 82247FE8 retains table 82066AFC in r29 for alpha and both stencil faces.
    // Its ordinary comparison order matches D3D11 values 1..8; only depth is reversed.
    return c>=1 && c<=8?static_cast<D3D11_COMPARISON_FUNC>(c):D3D11_COMPARISON_NEVER;
}
constexpr D3D11_DEPTH_STENCILOP_DESC stencilFace(const uint8_t* b) {
    // 82247FE8: comparison is high nibble of byte0; fail is low nibble;
    // pass is low nibble of byte1 (bits14..16 in the original state);
    // depth-fail is high nibble of byte1 (bits17..19).
    D3D11_DEPTH_STENCILOP_DESC r{};
    r.StencilFunc=stencilComparison(b[0]>>4);
    r.StencilFailOp=D3D11_STENCIL_OP((b[0]&7)+1);
    r.StencilPassOp=D3D11_STENCIL_OP((b[1]&7)+1);
    r.StencilDepthFailOp=D3D11_STENCIL_OP(((b[1]>>4)&7)+1);
    return r;
}
}
