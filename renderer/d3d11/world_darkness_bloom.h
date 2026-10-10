#pragma once
#include "renderer/engine/world_mesh.h"

namespace DarkRecomp {
// The original Sign and Signs_Levels textures form the additive glow shells
// over Hugin/Munin's power tattoos. Calibrate their radiance for a soft halo
// with bright cores; retain the authored blur, palette, alpha and geometry.
// This is a visual calibration, not an extra multiplier from the Xbox shader.
inline Native::EngineVector darknessArmBloomColor(const Native::WorldDraw& draw,bool bloom) {
    auto color=draw.fragmentConstants[0];
    if(!bloom || draw.fragmentName!="XRUtil_RenderSurface" || draw.fragmentFlags)
        return color;
    const auto id=draw.textureIds[0];
    // Shipped cache: Sign_4, Sign_3, Sign_1, Sign_2 and Signs_Levels.
    if(id!=9187 && id!=9188 && id!=9189 && id!=9190 && id!=9192)
        return color;
    const auto& texture=draw.textureObjects[0];
    if(texture.format!=20 || texture.width!=512 || texture.height!=512 || texture.faces!=1)
        return color;
    const auto* a=draw.attributes.data();
    const uint32_t flags=uint32_t(a[92])<<24|uint32_t(a[93])<<16|uint32_t(a[94])<<8|a[95];
    // Only the RGB-writing ONE/ONE shells qualify. Tattoo/base surfaces and
    // other uses of the image retain their original material constants.
    if((flags&(8u|4u|0x100000u))!=(8u|0x100000u) || a[144]!=2 || a[145]!=2)
        return color;
    constexpr float emissionGain=3.0f;
    for(unsigned lane=0;lane<3;++lane)color[lane]*=emissionGain;
    return color;
}
}
