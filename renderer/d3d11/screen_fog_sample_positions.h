#pragma once

namespace DarkRecomp {
// Exact D3D11_STANDARD_MULTISAMPLE_PATTERN offsets, in sixteenths of a pixel.
// attachMultisample explicitly selects this pattern for the fog depth surface.
// See Microsoft's DirectXShaderCompiler docs/SPIR-V.rst, GetSamplePosition.
inline constexpr char screenFogSamplePositionSource[]=R"(
#if SCREEN_FOG_MULTISAMPLE
float2 screenFogSamplePosition(uint count, uint index) {
    // Literal branches avoid FXC's uninitialized-array warnings for dynamic
    // indexing, while also compiling with Wine's built-in HLSL compiler.
    if(count==2)return (index==0?float2(4,4):float2(-4,-4))/16.0;
    if(count==4) {
        switch(index) {
        case 0:return float2(-2,-6)/16.0;
        case 1:return float2(6,-2)/16.0;
        case 2:return float2(-6,2)/16.0;
        case 3:return float2(2,6)/16.0;
        }
    }
    if(count==8) {
        switch(index) {
        case 0:return float2(1,-3)/16.0;
        case 1:return float2(-1,3)/16.0;
        case 2:return float2(5,1)/16.0;
        case 3:return float2(-3,-5)/16.0;
        case 4:return float2(-5,5)/16.0;
        case 5:return float2(-7,-1)/16.0;
        case 6:return float2(3,7)/16.0;
        case 7:return float2(7,-7)/16.0;
        }
    }
    return float2(0,0);
}
#endif
)";
}
