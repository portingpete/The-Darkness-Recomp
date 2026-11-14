#pragma once
#include "renderer/d3d11/world_eye_glow.h"

// Self-authored geometry: one visible quad and degenerate padding in each
// replicated layer. No original vertex, index, texture or shader asset data.
static std::shared_ptr<StoredGeometry> eyeGlowGeometry(float distance,bool recognized=true,float tilt=0) {
    auto g=std::make_shared<StoredGeometry>();g->vertexCount=910;g->stride=84;
    g->formats[0]=3;g->formats[1]=2;g->formats[12]=g->formats[13]=g->formats[14]=g->formats[15]=4;
    g->vertices.resize(size_t(910)*84);g->indices.resize(4200);
    const float xy[4][2]{{-.4f,-.4f},{-.4f,.4f},{.4f,.4f},{.4f,-.4f}};
    const uint16_t quad[6]{0,1,2,0,2,3};
    for(unsigned layer=0;layer<7;++layer) {
        for(unsigned i=0;i<130;++i) {
            auto* p=g->vertices.data()+size_t(layer*130+i)*84;
            const float extent=tilt?.25f:1.0f;
            const float x=i<4?xy[i][0]*distance*extent:0,y=i<4?xy[i][1]*distance*extent:0;
            const float data[21]{x,y,distance+tilt*x,.5f,.5f,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0};
            for(unsigned k=0;k<21;++k)put(p+k*4,std::bit_cast<uint32_t>(data[k]));
        }
        for(unsigned i=0;i<600;++i)g->indices[layer*600+i]=uint16_t(layer*130+(i<6?quad[i]:0));
    }
    if(!recognized)g->indices[599]=1; // Still a degenerate triangle, different topology.
    return g;
}
static WorldDraw eyeGlowDraw(float distance,float projectionA=1,bool recognized=true,float tilt=0) {
    auto draw=colorGradeDraw(eyeGlowGeometry(distance,recognized,tilt),7901,64,64);
    draw.geometry.indexCount=600;draw.fragmentName="XRUtil_RenderSurface";draw.fragmentFlags=8;
    draw.options.weights=8;draw.options.modes[7]=8;draw.constants.references[0][0]=96;draw.constants.vectors[8]={0,1,0,3};
    draw.constants.vectors[96]={1,0,0,0};draw.constants.vectors[97]={0,1,0,0};draw.constants.vectors[98]={0,0,1,0};
    draw.constants.vectors[2]={0,0,projectionA,-projectionA};draw.constants.vectors[3]={0,0,1,0};
    draw.depthRange={1,0,0,0};
    auto white=std::make_shared<ColorImage>();white->width=white->height=1;white->pixels={255,255,255,255};
    draw.textures[0]=white;draw.textureIds[0]=9700;draw.textureObjects[0]={9700,9700*4096,1,1,24,0};
    draw.samplers[0]=colorGradeSampler(false,2);draw.textureMask=1;
    draw.fragmentConstants[0]={.07f,.1f,.03f,.2f};
    // A finite position and pow(0,1) explicitly disable original distance fog.
    draw.fragmentConstants[1]={0,0,-7,1.0f/512};draw.fragmentConstants[2]={0,0,0,1};
    draw.fragmentConstants[4]={0,0,0,1};
    put(draw.attributes.data()+92,0x01100008);draw.attributes[144]=draw.attributes[145]=2;
    return draw;
}
static std::array<double,4> eyeGlowPixel(WorldRendererD3D11& renderer,const WorldDraw& draw,unsigned scale) {
    const auto bytes=renderer.readSurface(draw.targets[0],false);const size_t width=64*scale;
    require(bytes.size()==width*width*8,"Eye glow target size changed");
    std::array<double,4> result{};
    for(unsigned c=0;c<4;++c) {
        uint16_t v=0;std::memcpy(&v,bytes.data()+(width*(32*scale)+32*scale)*8+c*2,2);result[c]=materialHalf(v);
    }
    return result;
}
static void eyeGlowContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    {
        auto d=eyeGlowDraw(4);d.viewport={16,24,64,64};
        const auto r=DarkRecomp::WorldEyeGlow::ray(d,2);
        require(std::abs(r[0]*96.5f+r[2]-1.0f/128)<1e-6f &&
                std::abs(r[1]*112.5f+r[3]-1.0f/128)<1e-6f,"Eye ray lost viewport origin/scale");
        d.constants.vectors[0][2]=.25f;
        require(DarkRecomp::WorldEyeGlow::ray(d,2)[0]==0,"Nonorthogonal camera ray accepted");
        d.constants.vectors[0][2]=0;put(d.attributes.data()+92,0x01140008);
        require(DarkRecomp::WorldEyeGlow::ray(d,2)[0]==0,"Depth-biased camera ray accepted");
    }
    for(float scale:{1.0f,1e-6f,1e6f}) {
        auto d=eyeGlowDraw(1.25f,1.125f);
        for(auto& row:d.constants.vectors)for(auto& value:row)value*=scale;
        require(DarkRecomp::WorldEyeGlow::projection(d,d.geometry,true)!=0,"Scaled perspective rejected");
        d.constants.vectors[2][0]=5*scale;
        require(DarkRecomp::WorldEyeGlow::projection(d,d.geometry,true)==0,"Scaled oblique projection accepted");
    }
    for(unsigned scale:{1u,2u,3u}) {
        WorldRendererD3D11 renderer(device,context,scale);
        auto clear=[&](const WorldDraw& d) {WorldClear c;c.targets=d.targets;c.viewport=d.viewport;c.flags=1;c.color={0,0,0,.3125f};renderer.clear(c);};
        auto checkGain=[&](WorldDraw draw,double gain,const char* label) {
            renderer.beginFrame();clear(draw);require(renderer.draw(draw),label);
            for(unsigned layer=1;layer<7;++layer) {
                auto repeat=draw.geometry;repeat.firstIndex=layer*600;
                require(renderer.draw(draw,&repeat,true),"Eye layer repeat rejected");
            }
            const auto p=eyeGlowPixel(renderer,draw,scale);
            const auto* position=draw.geometry.vertices->vertices.data()+8;
            const float distance=std::bit_cast<float>(uint32_t(position[0])<<24|uint32_t(position[1])<<16|uint32_t(position[2])<<8|position[3]);
            for(unsigned c=0;c<3;++c) {
                const double expected=double(draw.fragmentConstants[0][c])*7*gain;
                if(std::abs(p[c]-expected)>=.002)std::fprintf(stderr,"Eye test scale=%u a=%g distance=%g classified=%g channel=%u actual=%g expected=%g alpha=%g\n",scale,draw.constants.vectors[2][2],distance,DarkRecomp::WorldEyeGlow::projection(draw,draw.geometry,DarkRecomp::WorldEyeGlow::layeredIndices(draw.geometry.indices->indices)),c,p[c],expected,p[3]);
                require(std::abs(p[c]-expected)<.002,label);
            }
            require(std::abs(p[3]-(.3125+7*.2))<.004,"Near fade changed authored alpha");
            renderer.endFrame();
        };
        for(float projectionA:{1.0f,1.125f})for(float distance:{1.025f,1.25f,1.5f,2.0f,4.0f}) {
            // Independent physical-distance oracle: near=1, full gain at 2.
            const double margin=std::clamp(double(distance)-1,0.0,1.0);
            const double gain=margin*margin*(3-2*margin);
            checkGain(eyeGlowDraw(distance,projectionA),gain,"Eye glow differs from physical near-distance oracle");
        }
        for(float projectionA:{1.0f,1.125f})for(float distance:{1.5f,2.0f,4.0f})for(double cosine:{.4,.5,.625,.75,.9}) {
            const float tilt=float(std::sqrt(1/(cosine*cosine)-1));
            const double ray=1.0/(64*scale);
            const double facing=std::abs(1-tilt*ray)/std::sqrt((1+double(tilt)*tilt)*(1+2*ray*ray));
            const double t=std::clamp((facing-.5)/.25,0.0,1.0);
            const double ratio=distance/(1-tilt*ray);
            const double nearMargin=std::clamp(ratio-1,0.0,1.0),farMargin=std::clamp(ratio-1.5,0.0,1.0);
            const double nearGain=nearMargin*nearMargin*(3-2*nearMargin),farGain=farMargin*farMargin*(3-2*farMargin);
            const double angle=t*t*(3-2*t);
            checkGain(eyeGlowDraw(distance,projectionA,true,tilt),nearGain*(farGain+(1-farGain)*angle),
                "Eye glow differs from near-camera geometric angle oracle");
        }
        checkGain(eyeGlowDraw(1.25f,1,false),1,"Unrelated topology was attenuated");
        auto orthographic=eyeGlowDraw(1.25f);orthographic.constants.vectors[2]={0,0,0,.5f};orthographic.constants.vectors[3]={0,0,0,1};
        checkGain(orthographic,1,"Orthographic eye draw was attenuated");
        auto ordinaryRange=eyeGlowDraw(4.0f);ordinaryRange.depthRange={0,1,0,0};
        checkGain(ordinaryRange,1,"Different depth range was attenuated");

        // State-identical geometry continuations can change classification.
        // Test both directions without a clear/readback invalidating bindings.
        auto eye=eyeGlowDraw(1.25f),other=eyeGlowDraw(1.25f,1,false);
        renderer.beginFrame();clear(eye);require(renderer.draw(eye),"Eye transition anchor rejected");
        require(renderer.draw(eye,&other.geometry,true),"Eye-to-other continuation rejected");
        require(renderer.draw(eye,&eye.geometry,true),"Other-to-eye continuation rejected");
        const auto p=eyeGlowPixel(renderer,eye,scale);const double gain=.25*.25*(3-2*.25);
        for(unsigned c=0;c<3;++c)require(std::abs(p[c]-eye.fragmentConstants[0][c]*(1+2*gain))<.001,
            "Eye classification leaked through repeated geometry");
        renderer.endFrame();
    }
}
