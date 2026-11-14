#pragma once
#include "renderer/engine/world_mesh.h"
#include <cmath>
#include <span>

namespace DarkRecomp::WorldEyeGlow {
// The two Darkness eye volumes contain seven 130-vertex copies of a
// 600-index surface. This recognizes their topology, not a runtime texture ID.
inline bool layeredIndices(std::span<const uint16_t> indices) noexcept {
    if(indices.size()<4200)return false;
    for(unsigned k=0;k<7;++k)for(unsigned i=0;i<600;++i)
        if(indices[i]>=130 || indices[k*600+i]!=indices[i]+130*k)return false;
    return true;
}
inline float projection(const Native::WorldDraw& draw,const Native::WorldGeometry& g,bool layered) noexcept {
    if(!layered || draw.fragmentName!="XRUtil_RenderSurface" || draw.fragmentFlags!=8 || draw.options.weights!=8 ||
       g.indexCount!=600 || g.firstIndex>3600 || g.firstIndex%600 || draw.depthRange[0]!=1 || draw.depthRange[1]!=0 ||
       draw.attributes[144]!=2 || draw.attributes[145]!=2)return 0;
    const auto* a=draw.attributes.data();
    const uint32_t flags=uint32_t(a[92])<<24|uint32_t(a[93])<<16|uint32_t(a[94])<<8|a[95];
    if(!(flags&8))return 0;
    // For a standard perspective projection Z=a*W+b, the guest near plane
    // is W=-b/a. The reversed native depth is d=1-a-b/W, so
    // (W-near)/near = a/(d+a-1)-1. Retain finite-far projections too.
    const auto& z=draw.constants.vectors[2];const auto& w=draw.constants.vectors[3];
    double norm=0,dot=0;for(unsigned i=0;i<3;++i){norm+=double(w[i])*w[i];dot+=double(z[i])*w[i];}
    if(!std::isfinite(norm) || norm<=0)return 0;
    const double scale=dot/norm;
    if(!std::isfinite(scale) || scale<1 || scale>2)return 0;
    const double basis=std::sqrt(norm);
    for(unsigned i=0;i<3;++i)if(!std::isfinite(z[i]) ||
        std::abs(double(z[i])-scale*w[i])>1e-5*(std::abs(scale)*basis+std::abs(double(z[i]))))return 0;
    const double offset=double(z[3])-scale*w[3];
    if(!std::isfinite(offset) || offset>=0)return 0;
    return float(scale);
}
// Recover an orthonormal camera ray from the known world perspective path.
// Its metric is independent of camera translation and uniform matrix scale.
inline Native::EngineVector ray(const Native::WorldDraw& draw,unsigned scale) noexcept {
    const auto* a=draw.attributes.data();
    const uint32_t flags=uint32_t(a[92])<<24|uint32_t(a[93])<<16|uint32_t(a[94])<<8|a[95];
    if((flags&0x40000) || !scale || !draw.viewport[2] || !draw.viewport[3])return {};
    const unsigned rows[3]{0,1,3};double norm[3]{};
    for(unsigned i=0;i<3;++i) {
        for(unsigned j=0;j<3;++j)norm[i]+=double(draw.constants.vectors[rows[i]][j])*draw.constants.vectors[rows[i]][j];
        if(!std::isfinite(norm[i]) || norm[i]<=0)return {};
        norm[i]=std::sqrt(norm[i]);
    }
    for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j) {
        double dot=0;for(unsigned k=0;k<3;++k)dot+=double(draw.constants.vectors[rows[i]][k])*draw.constants.vectors[rows[j]][k];
        if(std::abs(dot)>1e-5*norm[i]*norm[j])return {};
    }
    const double inverseX=norm[2]/norm[0],inverseY=norm[2]/norm[1];
    const double width=double(draw.viewport[2])*scale,height=double(draw.viewport[3])*scale;
    Native::EngineVector result{float(2*inverseX/width),float(2*inverseY/height),
        float(-(1+2*double(draw.viewport[0])*scale/width)*inverseX),
        float(-(1+2*double(draw.viewport[1])*scale/height)*inverseY)};
    for(float v:result)if(!std::isfinite(v))return {};
    return result[0]>0 && result[1]>0?result:Native::EngineVector{};
}
}
