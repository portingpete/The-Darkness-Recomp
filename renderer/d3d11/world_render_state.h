#pragma once
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace DarkRecomp::WorldRenderState {
inline bool depthBias(uint32_t flags,float slope,float units,unsigned scale,D3D11_RASTERIZER_DESC& raster) {
    raster.DepthBias=0;raster.SlopeScaledDepthBias=0;
    if(!(flags&0x40000))return true;
    // Original822487D0 negates the slope and multiplies units by the fixed
    // context+17064 value initialized from8209F114: -2^-19.8285F490's
    // factor16 is canceled by Xenos's 1/16-pixel slope unit. D24 needs an
    // integer count of representable depth steps, rounded away from zero.
    const float offset=units*-0x1p-19f;
    const float scaledSlope=-slope*float(scale);
    if(!std::isfinite(offset) || !std::isfinite(scaledSlope))return false;
    const double steps=std::copysign(std::ceil(std::abs(double(offset))*16777215.0),offset);
    raster.DepthBias=int(std::clamp(steps,double((std::numeric_limits<int>::min)()),
        double((std::numeric_limits<int>::max)())));
    // Higher resolution reduces the slope per physical pixel; preserve the
    // guest's bias per logical pixel at every rendering scale.
    raster.SlopeScaledDepthBias=scaledSlope;
    return true;
}
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
