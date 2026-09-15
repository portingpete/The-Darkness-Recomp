#include "renderer/d3d11/world_renderer.h"
#include "renderer/engine/prompt_icons.h"
#include "renderer/engine/prompt_origin.h"
#include "renderer/engine/texture_mip_layout.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <d3d11sdklayers.h>
using namespace DarkRecomp;
using namespace DarkRecomp::Native;
using Microsoft::WRL::ComPtr;
static void require(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}
static void check(HRESULT hr,const char* what) {require(SUCCEEDED(hr),what);}
#include "world_sampler_cache_tests.h"
using Result=std::array<float,40>;
static void put(uint8_t* p,uint32_t x) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(x>>(24-i*8));}
#include "darkness_vision_tests.h"
#include "world_palette_usage_tests.h"
#include "world_position_usage_tests.h"
#include "world_vertex_validation_tests.h"
static void formats() {
    StoredGeometry g;g.vertexCount=1;g.stride=12;g.formats[0]=15;g.formats[1]=10;g.formats[9]=14;
    g.conversionMask=3;g.vertices.resize(12);
    put(g.vertices.data(),2047|(1023u<<11)|(511u<<22));
    put(g.vertices.data()+4,0x1234FFFF);put(g.vertices.data()+8,1024|(2047u<<11)|(511u<<22));
    std::vector<WorldVertex> decoded;require(decodeWorldVertices(g,decoded),"Original packed fetch rejected");
    require(decoded[0].position[0]==1 && decoded[0].tex[0][0]==4660 && decoded[0].tex[0][1]==65535,
            "Position/UV fetch applied CPU conversion before shader conversion");
    require(decoded[0].normal[0]==-1 && std::abs(decoded[0].normal[1]+1.0f/1023)<1e-8f && decoded[0].normal[2]==1,
            "Signed 11:11:10 fetch failed");
    {
        StoredGeometry f;f.vertexCount=7;f.stride=8;f.formats[0]=19;f.formats[1]=19;
        f.conversionMask=31;f.vertices.resize(size_t(f.vertexCount)*f.stride);
        const uint32_t xs[7]{0,2047,1024,0,0,1,123},ys[7]{0,2047,0,1024,0,2,456},zs[7]{0,1023,0,0,512,3,789};
        for(unsigned i=0;i<7;++i) {
            put(f.vertices.data()+size_t(i)*8,(zs[i]<<22)|(ys[i]<<11)|xs[i]);
            put(f.vertices.data()+size_t(i)*8+4,(zs[6-i]<<22)|(ys[6-i]<<11)|xs[6-i]);
        }
        std::vector<WorldVertex> out;require(decodeWorldVertices(f,out),"Format19 packed fetch rejected");
        for(unsigned i=0;i<7;++i) {
            require(out[i].position[0]==float(xs[i]) && out[i].position[1]==float(ys[i]) && out[i].position[2]==float(zs[i]) && out[i].position[3]==1,
                    "Format19 position raw lanes differ");
            require(out[i].tex[0][0]==float(xs[6-i]) && out[i].tex[0][1]==float(ys[6-i]) && out[i].tex[0][2]==float(zs[6-i]) && out[i].tex[0][3]==1,
                    "Format19 generic slot raw lanes differ");
        }
        require(out[1].position[0]==2047 && out[1].position[1]==2047 && out[1].position[2]==1023,
                "Format19 all-ones/max lanes differ");
        require(out[2].position[0]==1024 && out[2].position[1]==0 && out[2].position[2]==0,
                "Format19 X high-bit lane differs");
        require(out[3].position[1]==1024 && out[5].position[2]==3,
                "Format19 Y high-bit/cross-lane lanes differ");
        const auto kept=out;
        auto same=[&](const std::vector<WorldVertex>& a,const std::vector<WorldVertex>& b) {
            if(a.size()!=b.size())return false;
            for(size_t i=0;i<a.size();++i)
                if(std::memcmp(&a[i],&b[i],sizeof(WorldVertex)))return false;
            return true;
        };
        f.vertices.pop_back();
        require(!decodeWorldVertices(f,out) && same(out,kept),"Truncated format19 changed output");
        f.vertices.pop_back();
        require(!decodeWorldVertices(f,out) && same(out,kept),"Truncated format19 stride changed output");
        StoredGeometry bad=f;bad.vertices.resize(size_t(bad.vertexCount)*bad.stride);
        for(unsigned i=0;i<7;++i) {
            put(bad.vertices.data()+size_t(i)*8,(zs[i]<<22)|(ys[i]<<11)|xs[i]);
            put(bad.vertices.data()+size_t(i)*8+4,(zs[6-i]<<22)|(ys[6-i]<<11)|xs[6-i]);
        }
        bad.formats[2]=20;bad.stride+=2;bad.vertices.resize(size_t(bad.vertexCount)*bad.stride);
        require(!decodeWorldVertices(bad,out) && same(out,kept),"Unknown format20 changed output");
        bad.formats[2]=0;bad.stride-=2;bad.vertices.resize(size_t(bad.vertexCount)*bad.stride);bad.stride+=1;
        require(!decodeWorldVertices(bad,out) && same(out,kept),"Invalid stride changed output");
    }
    {
        const auto kept=decoded;
        auto same=[&](const std::vector<WorldVertex>& a,const std::vector<WorldVertex>& b) {
            if(a.size()!=b.size())return false;
            for(size_t i=0;i<a.size();++i)
                if(std::memcmp(&a[i],&b[i],sizeof(WorldVertex)))return false;
            return true;
        };
        g.vertices.pop_back();require(!decodeWorldVertices(g,decoded) && same(decoded,kept),"Truncated fetch changed output");
        g.vertices.push_back(0);
        StoredGeometry bad=g;bad.formats[2]=20;bad.stride+=2;bad.vertices.resize(size_t(bad.vertexCount)*bad.stride);
        require(!decodeWorldVertices(bad,decoded) && same(decoded,kept),"Unknown format20 changed output");
        bad.formats[2]=0;bad.stride=g.stride+1;
        require(!decodeWorldVertices(bad,decoded) && same(decoded,kept),"Invalid stride changed output");
    }
    std::vector<uint8_t> memory(2048);auto* b=memory.data();const uint32_t image=64,pixels=128;
    put(b+image,0x82097610);put(b+image+8,pixels);put(b+image+12,16);put(b+image+16,4);put(b+image+20,4);
    put(b+image+24,8);put(b+image+28,2);put(b+image+32,0x20000);put(b+image+40,0x11014);
    b[pixels]=255;b[pixels+1]=0;b[pixels+8]=0;b[pixels+9]=255;
    uint64_t selectors=0;for(unsigned i=0;i<16;++i)selectors|=uint64_t(i%8)<<(3*i);
    for(unsigned i=0;i<6;++i)b[pixels+2+i]=b[pixels+10+i]=uint8_t(selectors>>(8*i));
    ColorImage normal;require(!decodeUploadImage(b,image,pixels,normal),"DXN image rejected");
    const uint8_t luma[]{255,0,218,182,145,109,72,36},alpha[]{0,255,51,102,153,204,0,255};
    for(unsigned i=0;i<16;++i)require(normal.pixels[i*4]==luma[i%8] && normal.pixels[i*4+1]==luma[i%8] && normal.pixels[i*4+2]==luma[i%8] && normal.pixels[i*4+3]==alpha[i%8],"DXN L,L,L,A decoding differs");
    put(b+image+12,15);normal.width=99;require(decodeUploadImage(b,image,pixels,normal) && normal.width==99,"Truncated DXN changed output");
    put(b+image+12,12);put(b+image+16,3);put(b+image+20,2);put(b+image+24,6);put(b+image+28,1);put(b+image+32,0x2000);put(b+image+40,0x814);
    for(unsigned i=0;i<12;++i)b[pixels+i]=uint8_t(i+1);
    ColorImage gray;require(!decodeUploadImage(b,image,pixels,gray) && gray.pixels.size()==24,"Luminance image rejected");
    require(gray.pixels[0]==1 && gray.pixels[12]==7 && gray.pixels[23]==255,"Luminance pitch/alpha failed");
    put(b+image+12,24);put(b+image+16,2);put(b+image+20,2);put(b+image+24,12);put(b+image+28,4);put(b+image+32,0x40);
    const uint8_t raw[]{1,2,3,4,5,6,7,8,99,99,99,99,9,10,11,12,13,14,15,16,99,99,99,99};
    std::memcpy(b+pixels,raw,sizeof(raw));ColorImage rgb;
    require(!decodeUploadImage(b,image,pixels,rgb) && rgb.pixels==std::vector<uint8_t>({3,2,1,255,7,6,5,255,11,10,9,255,15,14,13,255}),
            "Original XRGB upload pitch/channel order differs");
    put(b+image+32,0x800);
    require(!decodeUploadImage(b,image,pixels,rgb) && rgb.pixels[3]==4 && rgb.pixels[11]==12,"Original ARGB alpha lost");
    put(b+image+12,23);rgb.width=99;
    require(decodeUploadImage(b,image,pixels,rgb) && rgb.width==99,"Truncated uncompressed upload changed output");
    const uint32_t descriptor=512,positions=600,indices=700;
    put(b+descriptor,3u<<16);put(b+descriptor+4,positions);
    for(unsigned i=0;i<9;++i)put(b+positions+i*4,std::bit_cast<uint32_t>(float(i)/8));
    b[indices+3]=1;b[indices+5]=2;
    StoredDraw immediate;
    // Replacing an existing draw must release its resources and discard both
    // optional snapshots, just as aggregate assignment from geometry alone did.
    immediate.vertices=immediate.indices=std::make_shared<StoredGeometry>();
    const std::weak_ptr<const StoredGeometry> replaced=immediate.vertices;
    immediate.firstIndex=9;immediate.indexCount=12;
    immediate.transforms.emplace().matrixAddress=0x12345678;
    immediate.vertexBindings.emplace().constantBytes.fill(0xA5);
    require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},immediate) && immediate.vertices->vertexCount==3 &&
            immediate.indices->indices==std::vector<uint16_t>({0,1,2}),"Immediate depth geometry rejected");
    require(!immediate.transforms && !immediate.vertexBindings && immediate.firstIndex==0 && immediate.indexCount==3 && replaced.expired(),
            "Successful immediate capture retained stale snapshots, range or resource ownership");
    const auto retained=immediate.vertices;
    const auto retainedIndices=immediate.indices;
    require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},immediate) &&
            immediate.vertices==retained && immediate.indices==retainedIndices,"Identical immediate data was uploaded twice");
    put(b+positions,std::bit_cast<uint32_t>(0.25f));
    require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},immediate) &&
            immediate.vertices!=retained && immediate.indices==retainedIndices && retained->vertices[0]==0,
            "Changed immediate vertices reused or mutated a queued snapshot");
    put(b+positions,0);
    b[indices+5]=1;
    require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},immediate) &&
            immediate.indices!=retainedIndices && retainedIndices->indices[2]==2,
            "Changed immediate indices reused or mutated a queued snapshot");
    b[indices+5]=2;
    require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},immediate),"Restoring valid immediate data failed");
    immediate.firstIndex=17;immediate.indexCount=29;
    auto& transform=immediate.transforms.emplace();
    transform.matrixAddress=0x12345678;transform.deviceAddress=0x87654321;
    transform.input.modelView[2][1]=3.5f;transform.input.projection[1][3]=-2.25f;
    transform.constants.vectors[7][2]=42;transform.originalComparison=TransformComparison::equal;
    auto& binding=immediate.vertexBindings.emplace();
    binding.key[3]=0x13579BDF;binding.deviceAddress=0x2468ACE0;binding.constantBytes.fill(0x5A);
    const auto beforeInvalid=immediate;
    auto unchanged=[&] {
        return immediate.vertices==beforeInvalid.vertices && immediate.indices==beforeInvalid.indices &&
            immediate.firstIndex==beforeInvalid.firstIndex && immediate.indexCount==beforeInvalid.indexCount &&
            immediate.vertexBindings==beforeInvalid.vertexBindings && immediate.transforms &&
            immediate.transforms->matrixAddress==beforeInvalid.transforms->matrixAddress &&
            immediate.transforms->deviceAddress==beforeInvalid.transforms->deviceAddress &&
            immediate.transforms->input.modelView==beforeInvalid.transforms->input.modelView &&
            immediate.transforms->input.projection==beforeInvalid.transforms->input.projection &&
            immediate.transforms->constants.vectors==beforeInvalid.transforms->constants.vectors &&
            immediate.transforms->originalComparison==beforeInvalid.transforms->originalComparison;
    };
    b[positions]=0xFF;b[indices+5]=3;
    require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},immediate) && unchanged(),
            "Malformed immediate geometry changed owned output");
    require(!snapshotImmediateWorldGeometry(b,0,indices,3,retained,immediate) && unchanged(),
            "Invalid immediate index changed retained geometry, range or optional snapshots");
    b[indices+5]=2;
    require(snapshotImmediateWorldGeometry(b,0,indices,3,retained,immediate) && immediate.vertices==retained &&
            immediate.indices && immediate.indices->indices==retainedIndices->indices && immediate.firstIndex==0 && immediate.indexCount==3 &&
            !immediate.transforms && !immediate.vertexBindings,
            "Stored vertices were not retained for immediate indices");
}
static EngineVertexBindingSnapshot fixture(unsigned weights,bool normalizing) {
    EngineVertexBindingSnapshot b;auto& d=b.descriptor;
    d.flags=0x47600001|(weights<<16)|(normalizing?0x800000:0);d.positionConversion=76;
    d.coordinateMapping=0xFAC44000;d.declarationFlags=0x301F;
    d.modes={0,8,4,20,20,4,4,1};d.conversions.fill(78);d.parameters[3][0]=12;d.parameters[4][0]=13;d.parameters[7][0]=14;
    const auto bytes=encodeEngineVertexDescriptor(d);
    for(unsigned i=0;i<5;++i) for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
    auto constant=[&](unsigned n,EngineVector v){for(unsigned lane=0;lane<4;++lane)put(b.constantBytes.data()+n*16+lane*4,std::bit_cast<uint32_t>(v[lane]));};
    for(unsigned row=0;row<4;++row) {EngineVector v{};v[row]=1;constant(row,v);}
    constant(7,{0.1f,-0.2f,0.3f,0});constant(8,{0,1,.5f,765.0003f});constant(10,{.2f,.4f,.6f,.8f});
    constant(12,{2,3,4,.25f});constant(13,{-3,4,5,.5f});
    for(unsigned row=0;row<4;++row) {EngineVector v{};v[row]=float(row+1);constant(14+row,v);}
    constant(76,{2,3,4,1});constant(77,{1,-1,.5f,0});constant(78,{.5f,.25f,1,1});constant(79,{.1f,-.2f,0,0});
    for(unsigned bone=0;bone<52;++bone) for(unsigned row=0;row<3;++row) {
        EngineVector v{};v[row]=float(row+2);v[3]=float(bone+row)/8;constant(96+bone*3+row,v);
    }
    return b;
}
static void constantCopyContract() {
    auto b=fixture(8,false);WorldVertexOptions options;WorldVertexConstants constants;
    const auto vector=b.descriptor.palette;
    for(auto bits:{0u,0x80000000u,1u,0x80000001u,0x7F7FFFFFu,0xFF7FFFFFu}) {
        put(b.constantBytes.data()+vector*16+8,bits);
        require(prepareWorldVertexProgram(b,options,constants) && std::bit_cast<uint32_t>(constants.vectors[vector][2])==bits,
                "Native constant copy changed finite endian/float bits");
    }
    for(auto bits:{0x7F800000u,0xFF800000u,0x7FC00000u,0xFFFFFFFFu}) {
        const auto savedOptions=options;const auto savedConstants=constants;
        put(b.constantBytes.data()+vector*16+8,bits);
        require(!prepareWorldVertexProgram(b,options,constants),"Native vertex constants accepted infinity or NaN");
        require(options==savedOptions && constants.vectors==savedConstants.vectors && constants.references==savedConstants.references,
                "Rejected vertex preparation changed published constants");
    }
}
// Independent analytic oracle for diagonal affine palette matrices. Each bone
// transforms the point before weighting; tangent-space vectors are derived from
// the resulting basis, rather than executing the generated template instructions.
static std::array<double,40> expected(const WorldVertex& v,unsigned weights,bool normalize) {
    double p[4]{v.position[0]*2+1,v.position[1]*3-1,v.position[2]*4+.5,1};
    double basis[3]{1,1,1};
    if(weights) {
        double skinned[3]{},weightSum=0;
        for(unsigned b=0;b<weights;++b) {
            const auto weight=b<4?v.weights[b]:v.weights2[b-4];weightSum+=weight;
            for(unsigned r=0;r<3;++r) skinned[r]+=weight*(p[r]*(r+2)+double(b+r)/8);
        }
        for(unsigned r=0;r<3;++r) {p[r]=skinned[r];basis[r]=normalize?1:weightSum*(r+2);}
    }
    std::array<double,40> out{};out[0]=p[0]+.1f;out[1]=p[1]-.2f;out[2]=p[2]+.3f;out[3]=1;
    out[4]=v.tex[0][0]*.5f+.1f;out[5]=v.tex[0][1]*.25f-.2f;out[6]=v.tex[0][2];out[7]=v.tex[0][3];
    for(unsigned r=0;r<4;++r){out[8+r]=p[r];out[32+r]=p[r]*(r+1);out[36+r]=double(float(r+1)*.2f);}
    for(unsigned stage=3;stage<=4;++stage) {
        const double light[3]{stage==3?2.0:-3.0,stage==3?3.0:4.0,stage==3?4.0:5.0};
        const double scale=stage==3?.25:.5;
        double n=0,t1=0,t0=0;
        for(unsigned r=0;r<3;++r){const double delta=(light[r]-p[r])*basis[r];n+=v.normal[r]*delta;t1+=v.tex[2][r]*delta;t0+=v.tex[1][r]*delta;}
        const unsigned first=4+stage*4;out[first]=n*scale;out[first+1]=t1*scale;out[first+2]=t0*scale;
    }
    return out;
}
static double lightAttenuation(double px,double py,double pz,double lx,double ly,double lz,double invRange) {
    const double dx=lx-px,dy=ly-py,dz=lz-pz;
    const double dist=std::sqrt(dx*dx+dy*dy+dz*dz);
    const double a=1.0-dist*invRange;
    return a>0?a:0;
}
static void lightingValidationContract() {
    auto setLambda=[&](EngineVertexBindingSnapshot& b,unsigned reg,EngineVector v){
        for(unsigned lane=0;lane<4;++lane)put(b.constantBytes.data()+reg*16+lane*4,std::bit_cast<uint32_t>(v[lane]));
    };
    auto makeBinding=[&](unsigned weights){
        auto b=fixture(weights,false);
        auto& d=b.descriptor;
        d.modes[3]=16;d.modes[4]=9;d.modes[5]=9;
        d.parameters[3][0]=32;d.parameters[4][0]=34;d.parameters[5][0]=35;
        setLambda(b,32,{1,-1,.5f,.25f});setLambda(b,33,{2,.5f,-1,.25f});
        setLambda(b,34,{-3,4,5,.5f});setLambda(b,35,{1,2,3,4});
        const auto bytes=encodeEngineVertexDescriptor(d);b.key={};
        for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
        return b;
    };
    WorldVertexOptions options;WorldVertexConstants constants;
    {
        auto b=makeBinding(0);
        require(prepareWorldVertexProgram(b,options,constants),"One-light (16,9,9) binding rejected");
        require(options.modes[3]==16 && options.modes[5]==9,"Lighting modes not published");
        require(constants.vectors[33][3]==.25f,"Second lighting vector (non-unit W) not copied");
    }
    {
        auto b=makeBinding(0);
        setLambda(b,33,{2,.5f,-1,std::numeric_limits<float>::infinity()});
        const auto savedO=options;const auto savedC=constants;
        const auto bytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
        for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
        require(!prepareWorldVertexProgram(b,options,constants),"Nonfinite second lighting vector (W) accepted");
        require(options==savedO && constants.vectors==savedC.vectors && constants.references==savedC.references,
                "Rejected lighting preparation changed published constants");
    }
    {
        auto b=makeBinding(0);
        b.descriptor.parameters[3][0]=255;
        const auto bytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
        for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
        require(!prepareWorldVertexProgram(b,options,constants),"Parameter-base255 two-vector overflow accepted");
    }
    for(unsigned s:{0u,1u,2u,6u,7u}) {
        auto b=makeBinding(0);
        b.descriptor.modes[3]=4;b.descriptor.modes[s]=16;b.descriptor.parameters[s][0]=32;
        const auto bytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
        for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
        require(!prepareWorldVertexProgram(b,options,constants),"Unproven mode16 stage accepted");
    }
    {
        auto b=makeBinding(0);
        b.descriptor.modes[2]=9;b.descriptor.parameters[2][0]=34;
        const auto bytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
        for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
        require(!prepareWorldVertexProgram(b,options,constants),"Unproven mode9 stage accepted");
    }
    std::puts("LightingValidation passed: two-vector copy, nonfinite-W, base255 overflow, unproven stages rejected.");
}
static void partialClears(WorldRendererD3D11& renderer,ID3D11DeviceContext* context,const WorldDraw& queryDraw) {
    WorldClear base;base.targets={501,502,0,503,504};base.viewport={0,0,64,64};base.flags=49;
    base.color={.125f,.25f,.5f,.75f};base.depth=.25f;base.stencil=42;
    renderer.clear(base);const auto oldColor=renderer.readSurface(501,false),oldDepth=renderer.readSurface(504,true);
    WorldClear expected=base;expected.color={2,-.5f,.75f,1};expected.depth=1.0f;expected.stencil=0x1A5;
    renderer.clear(expected);const auto newColor=renderer.readSurface(501,false),newDepth=renderer.readSurface(504,true);
    const std::array<int32_t,4> rectangles[]{{-3,7,22,39},{12,15,13,16},{8,6,8,33},{70,80,90,100},{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX}};
    for(unsigned flags:{1u,16u,32u,17u,33u,48u,49u})for(const auto& rect:rectangles) {
        renderer.clear(base);WorldClear clear=expected;clear.flags=flags;clear.depth=1.25f;clear.rectangle=rect;renderer.clear(clear);
        for(unsigned target:{501u,502u,503u}) {
            const auto pixels=renderer.readSurface(target,false);
            for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
                const bool inside=int(x)>=rect[0] && int(x)<rect[2] && int(y)>=rect[1] && int(y)<rect[3];
                const auto& reference=inside && (flags&1)?newColor:oldColor;
                const auto offset=(y*64+x)*8;
                require(!std::memcmp(pixels.data()+offset,reference.data()+offset,8),"Rectangular color clear changed the wrong pixels or HDR values");
            }
        }
        const auto pixels=renderer.readSurface(504,true);
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
            const bool inside=int(x)>=rect[0] && int(x)<rect[2] && int(y)>=rect[1] && int(y)<rect[3];
            uint32_t before,after,actual;const auto offset=(y*64+x)*4;
            std::memcpy(&before,oldDepth.data()+offset,4);std::memcpy(&after,newDepth.data()+offset,4);std::memcpy(&actual,pixels.data()+offset,4);
            const uint32_t mask=inside?((flags&16?0xFFFFFFu:0)|(flags&32?0xFF000000u:0)):0;
            if(actual!=((after&mask)|(before&~mask))) {
                std::fprintf(stderr,"Clear mismatch flags=%u rect=%d,%d,%d,%d pixel=%u,%u before=%08X after=%08X actual=%08X mask=%08X\n",
                    flags,rect[0],rect[1],rect[2],rect[3],x,y,before,after,actual,mask);
                throw std::runtime_error("Rectangular depth/stencil clear changed an unselected plane or outside pixel");
            }
        }
    }
    // A retained attachment may be larger than the current viewport. Explicit
    // viewport-sized rectangles must not clear its border or require equal MRT sizes.
    WorldClear large=base;large.targets={502,0,0,0,0};large.viewport={0,0,90,80};renderer.clear(large);
    renderer.clear(base);WorldClear regionClear=expected;regionClear.rectangle=std::array<int32_t,4>{0,0,64,64};renderer.clear(regionClear);
    const auto grown=renderer.readSurface(502,false);
    require(grown.size()==90*80*8,"Clear fixture lost its larger retained attachment");
    for(unsigned y=0;y<80;++y)for(unsigned x=0;x<90;++x)
        require(!std::memcmp(grown.data()+(y*90+x)*8,(x<64 && y<64?newColor:oldColor).data(),8),
                "Rectangular clear erased a retained attachment border");
    // Helpers are not scene geometry: split and resume a real GPU query, and
    // count only the three ordinary triangles around two rectangular clears.
    auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
    renderer.histogram(query,true);
    for(unsigned n=0;n<3;++n) {
        require(renderer.draw(queryDraw),"Query draw after rectangular clear failed to restore native bindings");
        if(n<2) {regionClear.rectangle=std::array<int32_t,4>{5,7,31,33};renderer.clear(regionClear);}
    }
    renderer.histogram(query,false);context->Flush();const auto deadline=GetTickCount64()+5000;
    while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
    require(query->result->samples.load()==3*1152,"Clear helper geometry contaminated exposure queries or lost a resumed segment");
    std::puts("Rectangular clears passed: all7 color/depth/stencil masks,35 clipped/empty/full regions, HDR pixels, retained borders, and resumed GPU queries.");
}
static void retainedDepthGrowth(WorldRendererD3D11& renderer) {
    // An atlas first visits its left tile, then a tile extending the retained
    // allocation. Earlier depth and every stencil bit must survive the growth.
    WorldClear base;base.targets={0,0,0,0,701};base.viewport={0,0,64,32};base.flags=48;
    base.depth=.25f;base.stencil=0x35;renderer.clear(base);
    auto patch=base;patch.rectangle=std::array<int32_t,4>{4,7,20,23};patch.depth=.75f;patch.stencil=0xCA;
    renderer.clear(patch);const auto before=renderer.readSurface(701,true);
    require(before.size()==64*32*4,"Depth growth fixture extent");
    auto next=base;next.viewport={64,0,32,64};next.rectangle=std::array<int32_t,4>{64,0,96,64};
    next.depth=.5f;next.stencil=0x96;renderer.clear(next);
    const auto after=renderer.readSurface(701,true);
    require(after.size()==96*64*4,"Depth growth did not retain the new atlas extent");
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<96;++x) {
        uint32_t actual=0;std::memcpy(&actual,after.data()+(y*96+x)*4,4);
        if(x<64 && y<32) {
            uint32_t expected=0;std::memcpy(&expected,before.data()+(y*64+x)*4,4);
            if(actual!=expected) {
                std::fprintf(stderr,"Depth growth pixel=(%u,%u) actual=%08X expected=%08X\n",x,y,actual,expected);
                throw std::runtime_error("Growing an atlas erased earlier depth/stencil pixels");
            }
        } else if(x>=64) {
            require((actual>>24)==0x96 && (actual&0xFFFFFF)>=0x7FFFFF && (actual&0xFFFFFF)<=0x800001,
                    "New atlas tile did not receive its depth/stencil clear");
        } else require(actual==0,"Depth growth left uninitialized pixels outside the old atlas and new tile");
    }
}
static void retainedAttachmentPair(WorldRendererD3D11& renderer,ID3D11DeviceContext* context,const WorldDraw& queryDraw) {
    // Exercise larger depth, larger color, and crossed width/height histories.
    // All must retain the union without discarding either attachment's pixels.
    const std::array<std::array<uint32_t,4>,3> extents{{{64,64,96,80},{96,80,64,64},{96,64,64,80}}};
    uint32_t identity=711;
    for(const auto& e:extents) {
        const uint32_t colorId=identity++,depthId=identity++;
        WorldClear color;color.targets={colorId,0,0,0,0};color.viewport={0,0,e[0],e[1]};color.flags=1;
        color.color={.125f,.25f,.5f,.75f};renderer.clear(color);
        const auto oldColor=renderer.readSurface(colorId,false);
        WorldClear depth;depth.targets={0,0,0,0,depthId};depth.viewport={0,0,e[2],e[3]};depth.flags=48;
        depth.depth=.25f;depth.stencil=0xA6;renderer.clear(depth);
        const auto oldDepth=renderer.readSurface(depthId,true);
        auto draw=queryDraw;draw.targets={colorId,0,0,0,depthId};draw.material=WorldMaterial::motion;
        draw.options.modes[0]=draw.options.modes[1]=7;
        draw.constants.references[1][2]=draw.constants.references[2][2]=20;
        draw.constants.vectors[20]={0,0,1,1};
        put(draw.attributes.data()+92,0x01100002);draw.attributes[96]=draw.attributes[97]=8;
        require(renderer.draw(draw),"Retained target motion draw was rejected");
        ComPtr<ID3D11RenderTargetView> boundColor;ComPtr<ID3D11DepthStencilView> boundDepth;
        context->OMGetRenderTargets(1,&boundColor,&boundDepth);
        require(boundColor.Get() && boundDepth.Get(),"Retained attachment extents caused D3D11 to drop the motion targets");
        ComPtr<ID3D11Resource> colorResource,depthResource;boundColor->GetResource(&colorResource);boundDepth->GetResource(&depthResource);
        ComPtr<ID3D11Texture2D> colorTexture,depthTexture;check(colorResource.As(&colorTexture),"Motion color resource");
        check(depthResource.As(&depthTexture),"Motion depth resource");
        D3D11_TEXTURE2D_DESC cd{},dd{};colorTexture->GetDesc(&cd);depthTexture->GetDesc(&dd);
        require(cd.Width==dd.Width && cd.Height==dd.Height,"Motion draw bound incompatible retained color/depth extents");
        require(cd.Width==(std::max)(e[0],e[2]) && cd.Height==(std::max)(e[1],e[3]),
                "Matching attachments discarded a retained extent");
        const auto pixels=renderer.readSurface(colorId,false);uint16_t pixel[4]{};
        const auto retainedDepth=renderer.readSurface(depthId,true);
        require(pixels.size()==size_t(cd.Width)*cd.Height*8 && retainedDepth.size()==size_t(cd.Width)*cd.Height*4,
                "Retained motion readback extent");
        std::memcpy(pixel,pixels.data()+(32*cd.Width+32)*8,8);
        require(pixel[0]==0x3800 && pixel[1]==0x3800 && pixel[2]==0,
                "Motion pass disappeared after reusing retained targets");
        for(unsigned y=0;y<cd.Height;++y)for(unsigned x=0;x<cd.Width;++x) {
            uint32_t actual=0,expected=0;
            std::memcpy(&actual,retainedDepth.data()+(y*cd.Width+x)*4,4);
            if(x<e[2] && y<e[3])std::memcpy(&expected,oldDepth.data()+(y*e[2]+x)*4,4);
            require(actual==expected,"Matching motion attachments changed retained depth/stencil or uninitialized the border");
            if(x>=64 || y>=64) {
                const uint8_t zero[8]{};
                const auto* reference=x<e[0] && y<e[1]?oldColor.data()+(y*e[0]+x)*8:zero;
                require(!std::memcmp(pixels.data()+(y*cd.Width+x)*8,reference,8),
                        "Matching motion attachments changed color outside the draw viewport");
            }
        }
    }
}
static void resolveSourceClearRegion(WorldRendererD3D11& renderer,ID3D11Device* device,ID3D11DeviceContext* context) {
    auto halfFloat=[](uint16_t h) {const unsigned e=(h>>10)&31,m=h&1023;return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    WorldClear color;color.targets={731,0,732,0,0};color.viewport={0,0,96,80};color.flags=1;
    color.color={.125f,.25f,.375f,.5f};
    WorldClear depth;depth.targets[4]=734;depth.viewport={0,0,80,96};depth.flags=48;depth.depth=.25f;depth.stencil=0x35;
    WorldClear reference=color;reference.targets={741,0,0,0,744};reference.flags=49;reference.depth=.75f;reference.stencil=0xCA;
    reference.color={.75f,.5f,.25f,1};renderer.clear(reference);
    const auto clearColor=renderer.readSurface(741,false),clearDepth=renderer.readSurface(744,true);
    unsigned cases=0;
    for(unsigned attachment:{0u,2u,4u})for(unsigned flags:{0u,0x100u,0x200u,0x300u})
    for(unsigned shape=0;shape<5;++shape) {
        const std::array<uint32_t,4> rect=shape==0?std::array<uint32_t,4>{16,8,48,40}:
            shape==1?std::array<uint32_t,4>{0,0,64,64}:
            shape==3?std::array<uint32_t,4>{64,64,attachment==4?72u:88u,attachment==4?88u:72u}:
            std::array<uint32_t,4>{16,8,45,37};
        renderer.clear(color);renderer.clear(depth);
        auto pattern=color;pattern.targets[2]=0;pattern.rectangle=std::array<int32_t,4>{32,32,64,64};
        pattern.color={.625f,.375f,.125f,.875f};renderer.clear(pattern);
        auto depthPattern=depth;depthPattern.rectangle=pattern.rectangle;depthPattern.depth=.5f;depthPattern.stencil=0xA6;
        renderer.clear(depthPattern);
        const auto old0=renderer.readSurface(731,false),old2=renderer.readSurface(732,false),oldDepth=renderer.readSurface(734,true);
        WorldResolve copy;copy.targets=color.targets;copy.targets[4]=734;copy.viewport={0,0,64,64};
        copy.rectangle=rect;copy.offset={8,16};copy.flags=attachment|flags;
        copy.destination={749+cases,0x749000,shape==4?8+rect[2]-rect[0]:96,shape==4?16+rect[3]-rect[1]:96,
                          attachment==4?23u:26u,0,1};
        copy.color=reference.color;copy.depth=reference.depth;copy.stencil=reference.stencil;
        require(renderer.resolve(copy),"Resolve with source-clear flags rejected");
        for(unsigned target:{0u,2u,4u}) {
            const bool isDepth=target==4;
            const unsigned width=isDepth?80:96,height=isDepth?96:80,bytes=isDepth?4:8;
            const auto pixels=renderer.readSurface(copy.targets[target],isDepth);
            const auto& old=isDepth?oldDepth:target==0?old0:old2;
            require(pixels.size()==old.size(),"Resolve clear changed a retained attachment extent");
            const bool selected=isDepth?(flags&0x200)!=0:(flags&0x100)!=0 && attachment==target;
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
                const bool inside=x>=rect[0] && y>=rect[1] && x<((rect[2]+7u)&~7u) && y<((rect[3]+7u)&~7u);
                const auto at=(size_t(y)*width+x)*bytes;
                const auto* expected=selected && inside?(isDepth?clearDepth.data():clearColor.data()):old.data()+at;
                if(std::memcmp(pixels.data()+at,expected,bytes)) {
                    std::fprintf(stderr,"ResolveClear[attachment=%u flags=%03X target=%u pixel=%u,%u inside=%u]\n",attachment,flags,target,x,y,unsigned(inside));
                    throw std::runtime_error("Resolve source clear changed pixels outside its copied rectangle or an unselected attachment");
                }
            }
        }
        // Verify every destination texel, including expanded right/bottom
        // fringes, untouched borders and clipping at the logical destination.
        D3D11_TEXTURE2D_DESC desc{};desc.Width=copy.destination.width;desc.Height=copy.destination.height;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=attachment==4?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16B16A16_UNORM;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging),"Resolve-clear staging");
        require(renderer.present(copy.destination,staging.Get()),"Resolve-clear destination readback failed");
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Resolve-clear readback map");
        const unsigned sourceWidth=attachment==4?80:96,sourceHeight=attachment==4?96:80;
        const unsigned right=(std::min)((rect[2]+7u)&~7u,sourceWidth),bottom=(std::min)((rect[3]+7u)&~7u,sourceHeight);
        bool copied=true;
        for(unsigned y=0;y<desc.Height && copied;++y)for(unsigned x=0;x<desc.Width && copied;++x) {
            const auto* pixel=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+x*(attachment==4?4:8);
            const bool inside=x>=copy.offset[0] && y>=copy.offset[1] &&
                x<copy.offset[0]+right-rect[0] && y<copy.offset[1]+bottom-rect[1];
            const size_t at=inside?(size_t(rect[1]+y-copy.offset[1])*sourceWidth+rect[0]+x-copy.offset[0]):0;
            if(attachment==4) {
                uint32_t packed{};float value{};if(inside)std::memcpy(&packed,oldDepth.data()+at*4,4);std::memcpy(&value,pixel,4);
                copied=std::abs(value-float(packed&0xffffff)/16777215.0f)<1e-6f;
            } else {
                const auto& old=attachment==0?old0:old2;uint16_t input[4]{},actual[4]{};
                if(inside)std::memcpy(input,old.data()+at*8,8);std::memcpy(actual,pixel,8);
                for(unsigned c=0;c<4;++c)copied&=std::abs(float(actual[c])/65535.0f-halfFloat(input[c]))<2e-5f;
            }
            if(!copied)std::fprintf(stderr,"ResolveCopyBoundary[attachment=%u flags=%03X shape=%u pixel=%u,%u inside=%u]\n",
                                   attachment,flags,shape,x,y,unsigned(inside));
        }
        context->Unmap(staging.Get(),0);require(copied,"Resolve omitted expanded copy pixels or cleared before copying");++cases;
    }
    std::printf("ResolveSourceClear: %u color/depth/flag/rectangle cases preserve retained borders, packed stencil and copy-before-clear ordering.\n",cases);
}
static void resolveHistogramIsolation(WorldRendererD3D11& renderer,ID3D11DeviceContext* context,const WorldDraw& queryDraw) {
    WorldClear clear;clear.targets={721,0,0,0,0};clear.viewport={0,0,64,64};clear.flags=1;clear.color={.25f,.5f,.75f,1};
    renderer.clear(clear);
    WorldClear depth;depth.targets[4]=724;depth.viewport={0,0,80,96};depth.flags=48;renderer.clear(depth);
    WorldResolve resolve;resolve.targets=clear.targets;resolve.targets[4]=724;resolve.flags=0x300;
    resolve.destination={722,0x72000,64,64,6,0,1};
    resolve.rectangle={4,6,16,16};resolve.offset={20,30};
    auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
    renderer.histogram(query,true);
    require(renderer.draw(queryDraw),"Pre-resolve exposure triangle rejected");
    require(renderer.resolve(resolve),"First in-query resolve rejected");
    require(renderer.draw(queryDraw),"Exposure triangle after first resolve rejected");
    resolve.rectangle={2,3,22,11};resolve.offset={0,0};
    require(renderer.resolve(resolve),"Second in-query resolve rejected");
    require(renderer.draw(queryDraw),"Exposure triangle after second resolve rejected");
    renderer.histogram(query,false);context->Flush();const auto deadline=GetTickCount64()+5000;
    while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
    const auto samples=query->result->samples.load();
    if(samples!=3*1152)std::fprintf(stderr,"In-query resolves: samples=%llu expected=%u\n",
                                  static_cast<unsigned long long>(samples),3*1152);
    require(samples==3*1152,"Resolve helper pixels contaminated exposure or lost a resumed query segment");
}
static void deeperResources(WorldRendererD3D11& renderer,ID3D11Device* device,ID3D11DeviceContext* context,const WorldDraw& queryDraw) {
    // Report each independent baseline failure in one invocation so the parent
    // can establish all regressions before any production fixes are applied.
    unsigned failures=0;ComPtr<ID3D11InfoQueue> messages;device->QueryInterface(IID_PPV_ARGS(&messages));
    auto run=[&](const char* name,auto test) {
        context->ClearState();renderer.invalidateBindings();
        const auto firstMessage=messages?messages->GetNumStoredMessagesAllowedByRetrievalFilter():0;
        try {
            test();
            if(messages)for(UINT64 i=firstMessage;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
                SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
                auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                check(messages->GetMessage(i,message,&size),"Deeper GPU debug message retrieval");
                require(message->Severity>D3D11_MESSAGE_SEVERITY_ERROR,"D3D11 rejected a retained-resource command");
            }
            std::printf("DeeperGPU[%s] passed\n",name);
        } catch(const std::exception& e) {++failures;std::fprintf(stderr,"DeeperGPU[%s] failed: %s\n",name,e.what());}
    };
    run("retained-depth-growth",[&]{retainedDepthGrowth(renderer);});
    run("retained-attachment-pair",[&]{retainedAttachmentPair(renderer,context,queryDraw);});
    run("resolve-histogram-isolation",[&]{resolveHistogramIsolation(renderer,context,queryDraw);});
    run("resolve-source-clear-region",[&]{resolveSourceClearRegion(renderer,device,context);});
    require(failures==0,"Deeper GPU resource regressions failed (see individual DeeperGPU diagnostics)");
}
static void deadNormalContract(WorldRendererD3D11& renderer) {
    // Live XRUtil_RenderSurface tuple (boot log step6 flags65/67): formats
    // 3,2 with color18, missing normal; modes 0,4,8,16,16,9,4,4; coords 0..7;
    // weights0 normal1 tangents0 normalize0 color1. All outputs are proven
    // normal-independent, so the missing stream must render exactly like
    // finite-normal geometry. Dependent modes and excluded configs stay strict.
    auto binding=fixture(0,false);
    binding.descriptor.flags=0x46200001;
    binding.descriptor.modes={0,4,8,16,16,9,4,4};
    binding.descriptor.coordinateMapping=0xFAC68800;
    binding.descriptor.parameters[3][0]=32;binding.descriptor.parameters[4][0]=34;binding.descriptor.parameters[5][0]=36;
    auto setLambda=[&](unsigned reg,EngineVector v){for(unsigned lane=0;lane<4;++lane)
        put(binding.constantBytes.data()+reg*16+lane*4,std::bit_cast<uint32_t>(v[lane]));};
    setLambda(32,{1,-1,.5f,.25f});setLambda(33,{2,.5f,1,.25f});
    setLambda(34,{0,0,2,.25f});setLambda(35,{1,2,3,1});setLambda(36,{.5f,.25f,.125f,1});
    // Clip path is R4=R8+c[BASE+7] through identity MVP (VP.xrg:1068-1075) with
    // DepthClipEnable TRUE, and D3D11 clips 0<=z<=w: zero c7 so the z=.5 below
    // survives instead of going negative (reversed depth would exceed w).
    setLambda(7,{0,0,0,0});
    const auto bytes=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)binding.key[i+1]=(binding.key[i+1]<<8)|bytes[i*4+n];
    WorldVertexOptions live;WorldVertexConstants constants;
    require(prepareWorldVertexProgram(binding,live,constants),"Dead-normal live binding rejected");
    require(live.modes==std::array<uint8_t,8>{0,4,8,16,16,9,4,4} &&
            live.coordinates==std::array<uint8_t,8>{0,1,2,3,4,5,6,7} &&
            !live.weights && live.normal && !live.tangents && !live.normalizeNormal && live.vertexColor,
            "Dead-normal live options differ from the proven tuple");
    // StoredGeometry holds big-endian words (decodeWorldVertices reads word()),
    // so every float lane uses put()+bit_cast exactly like passes()/formats().
    // Position conversion (c76={2,3,4,1},c77={1,-1,.5,0}) plus zeroed c7 maps
    // these inputs onto the passes() clip triangle (-.75,-.75),(0,.75),(.75,-.75)
    // at clip z=.5: R8z=0*4+.5, R4z=R8z+0, w=1, reversed depth 1-.5=.5, all inside
    // the D3D11 0<=z<=w clip range. Exact fractions: y=(cy+1)/3 gives 1/12,7/12.
    auto geometry=[&](bool withNormal) {
        auto g=std::make_shared<StoredGeometry>();
        g->vertexCount=3;g->formats[0]=3;g->formats[1]=2;g->formats[10]=18;
        if(withNormal)g->formats[9]=4;
        const uint32_t stride=withNormal?40:24;
        g->stride=stride;g->vertices.resize(size_t(3)*stride);g->indices={0,1,2};
        const EngineVector positions[]{{-.875f,1.0f/12,0,1},{-.5f,7.0f/12,0,1},{-.125f,1.0f/12,0,1}};
        const EngineVector uvs[]{{0,0,0,1},{.5f,1,0,1},{1,0,0,1}};
        const EngineVector normal{0,0,1,0};
        for(unsigned i=0;i<3;++i) {
            auto* v=g->vertices.data()+size_t(i)*stride;
            for(unsigned l=0;l<3;++l)put(v+l*4,std::bit_cast<uint32_t>(positions[i][l]));
            for(unsigned l=0;l<2;++l)put(v+12+l*4,std::bit_cast<uint32_t>(uvs[i][l]));
            unsigned colorAt=20;
            if(withNormal) {
                for(unsigned l=0;l<4;++l)put(v+20+l*4,std::bit_cast<uint32_t>(normal[l]));
                colorAt=36;
            }
            v[colorAt]=255;v[colorAt+1]=128;v[colorAt+2]=64;v[colorAt+3]=255;
        }
        return g;
    };
    auto white=std::make_shared<ColorImage>();
    white->width=white->height=1;white->pixels={255,255,255,255};
    // XRUtil_RenderSurface_65 samples 2D slot0 and CUBE slot2 (proj0+lighting);
    // _67 additionally samples CUBE slot3 (proj1). Uniform-white cube faces keep
    // projection lookups deterministic while lighting stays nonzero.
    auto whiteCube=[] {
        auto t=std::make_shared<ColorImage>();
        t->width=t->height=1;t->faces=6;t->pixels.assign(24,255);return t;
    };
    WorldDraw draw;draw.viewport={0,0,64,64};draw.targets={901,0,0,0,0};
    draw.material=WorldMaterial::fixed;draw.fragmentName="XRUtil_RenderSurface";
    draw.options=live;draw.constants=constants;draw.textures[0]=white;
    draw.textures[2]=whiteCube();draw.textures[3]=whiteCube();
    // Fragment env layout (XRUtil_RenderSurface.fp, no-fog branch): env0 Color,
    // env1..3 ProjMap0 rows, env4..6 ProjMap1 rows. Identity projection rows map
    // the mspos interpolant straight into well-defined nonzero cube coordinates.
    draw.fragmentConstants[0]={1,1,1,1};
    draw.fragmentConstants[1]={1,0,0,0};draw.fragmentConstants[2]={0,1,0,0};draw.fragmentConstants[3]={0,0,1,0};
    draw.fragmentConstants[4]={1,0,0,0};draw.fragmentConstants[5]={0,1,0,0};draw.fragmentConstants[6]={0,0,1,0};
    put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
    for(unsigned flags:{65u,67u}) {
        draw.fragmentFlags=flags;
        draw.geometry.vertices=draw.geometry.indices=geometry(false);
        draw.geometry.indexCount=3;
        renderer.clear(clear);
        require(renderer.draw(draw),"Proven normal-independent missing-normal draw rejected");
        const auto missing=renderer.readSurface(901,false);
        draw.geometry.vertices=draw.geometry.indices=geometry(true);
        renderer.clear(clear);
        require(renderer.draw(draw),"Equivalent finite-normal draw rejected");
        const auto present=renderer.readSurface(901,false);
        require(missing.size()==64*64*8 && present.size()==64*64*8,"Dead-normal readback extent");
        require(missing==present,"Missing normal changed pixels of a proven independent tuple");
        size_t colored=0;
        for(size_t i=0;i<missing.size();i+=8)if(missing[i] || missing[i+1])++colored;
        require(colored>0,"Proven tuple rendered two empty frames instead of meaningful pixels");
    }
    // Normal-dependent outputs must still reject a missing normal stream. These
    // attempts reuse the fully valid bindings above, so they fail at the step6
    // normal check rather than an unrelated texture/decode rejection.
    auto strict=draw;
    strict.geometry.vertices=strict.geometry.indices=geometry(false);
    strict.geometry.indexCount=3;strict.fragmentFlags=65;
    for(auto mode:{13u,17u,18u,20u,22u,27u}) {
        auto attempt=strict;attempt.options=live;
        attempt.options.modes[mode==17 || mode==18?5:0]=uint8_t(mode);
        if(!renderer.draw(attempt))continue;
        std::fprintf(stderr,"DeadNormal[mode=%u] accepted a missing normal\n",mode);
        throw std::runtime_error("Normal-dependent or unknown mode accepted a missing normal");
    }
    for(bool tangents:{false,true})for(bool normalize:{false,true}) {
        if(!tangents && !normalize)continue;
        auto attempt=strict;attempt.options=live;
        attempt.options.tangents=tangents;attempt.options.normalizeNormal=normalize;
        require(!renderer.draw(attempt),"Excluded tangents/normalize configuration bypassed normal validation");
    }
    std::puts("DeadNormal passed: RenderSurface65/67 missing-normal matches finite-normal pixels; dependent/unknown modes and tangents/normalize stay strict.");
}
static void immediateCanonicalContract(WorldRendererD3D11& renderer) {
    // Live smoke signature from capture-61 (tex1371 frames 3977+): immediate
    // position float3 + UV float2 + color18, stride 24, firstIndex 0.
    // NaN/Inf allocator leftovers (rawBE FFFFFFFF) in vertices no index
    // references must not reject visible triangles; referenced bad data,
    // OOB indices and bad formats stay strict and transactional.
    std::vector<uint8_t> memory(4096,0);auto* b=memory.data();
    const uint32_t descriptor=512,positions=640,uvs=1024,colors=1152,indices=1280;
    auto put16=[&](uint32_t address,uint16_t v){b[address]=uint8_t(v>>8);b[address+1]=uint8_t(v&255);};
    auto fill=[&](unsigned vc,const EngineVector* pos,const EngineVector* uv,const uint8_t col[][4],
                  const uint16_t* idx,unsigned count) {
        std::fill(memory.begin(),memory.end(),uint8_t(0));
        put(b+descriptor,vc<<16);
        put(b+descriptor+4,positions);
        put(b+descriptor+8,uvs);b[descriptor+40]=2;
        put(b+descriptor+52,colors);
        for(unsigned i=0;i<vc;++i) {
            for(unsigned l=0;l<3;++l)put(b+positions+i*12+l*4,std::bit_cast<uint32_t>(pos[i][l]));
            for(unsigned l=0;l<2;++l)put(b+uvs+i*8+l*4,std::bit_cast<uint32_t>(uv[i][l]));
            for(unsigned l=0;l<4;++l)b[colors+i*4+l]=col[i][l];
        }
        for(unsigned i=0;i<count;++i)put16(indices+i*2,idx[i]);
    };
    const EngineVector triPos[]{{-.875f,1.0f/12,0,1},{-.5f,7.0f/12,0,1},{-.125f,1.0f/12,0,1}};
    const EngineVector triUv[]{{0,0,0,1},{.5f,1,0,1},{1,0,0,1}};
    const uint8_t triColor[][4]{{255,128,64,255},{255,128,64,255},{255,128,64,255}};
    auto unchangedProbe=[] {
        StoredDraw probe;probe.vertices=probe.indices=std::make_shared<StoredGeometry>();
        probe.firstIndex=9;probe.indexCount=12;
        auto& t=probe.transforms.emplace();t.matrixAddress=0x12345678;t.deviceAddress=0x87654321;
        t.input.modelView[2][1]=3.5f;t.input.projection[1][3]=-2.25f;
        t.constants.vectors[7][2]=42;t.originalComparison=TransformComparison::equal;
        probe.vertexBindings.emplace().constantBytes.fill(0x5A);return probe;
    };
    auto unchangedEqual=[&](const StoredDraw& probe,const StoredDraw& before) {
        if(probe.vertices!=before.vertices || probe.indices!=before.indices ||
            probe.firstIndex!=before.firstIndex || probe.indexCount!=before.indexCount ||
            probe.vertexBindings!=before.vertexBindings)return false;
        if(bool(probe.transforms)!=bool(before.transforms))return false;
        if(probe.transforms) {
            const auto& a=*probe.transforms;const auto& e=*before.transforms;
            if(a.matrixAddress!=e.matrixAddress || a.deviceAddress!=e.deviceAddress ||
                a.input.modelView!=e.input.modelView || a.input.projection!=e.input.projection ||
                a.constants.vectors!=e.constants.vectors || a.originalComparison!=e.originalComparison)return false;
        }
        return true;
    };
    // Tail NaN succeeds; tail canonicalized, live bytes and source intact.
    {
        EngineVector pos[4]={triPos[0],triPos[1],triPos[2],triPos[0]};
        EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
        uint8_t col[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t idx[]={0,1,2};
        fill(4,pos,uv,col,idx,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        const auto memBefore=memory;
        StoredDraw draw;
        require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},draw),"NaN tail rejected visible triangle");
        require(draw.vertices->vertexCount==4 && draw.vertices->stride==24 && draw.indexCount==3 && draw.firstIndex==0 &&
                !draw.transforms && !draw.vertexBindings,"Canonicalized draw lost range, snapshots or dimensions");
        const auto& bytes=draw.vertices->vertices;
        for(unsigned i=0;i<12;++i)require(bytes[3*24+i]==0,"Unreferenced NaN tail not canonicalized");
        std::vector<WorldVertex> decoded;require(decodeWorldVertices(*draw.vertices,decoded),"Canonicalized geometry undecodable");
        for(unsigned i=0;i<3;++i)for(unsigned l=0;l<3;++l)require(decoded[i].position[l]==triPos[i][l],"Live position changed by canonicalization");
        require(memory==memBefore,"Guest source mutated by canonicalization");
    }
    // Interior Inf hole with duplicates succeeds; order/winding preserved.
    {
        EngineVector pos[5]={triPos[0],triPos[1],triPos[2],triPos[0],{0.5f,0.5f,0.5f,1}};
        EngineVector uv[5]={triUv[0],triUv[1],triUv[2],triUv[0],triUv[1]};
        uint8_t col[5][4];for(unsigned i=0;i<5;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t idx[]={0,1,2,0,2,4};
        fill(5,pos,uv,col,idx,6);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0x7F800000u);
        const auto memBefore=memory;
        StoredDraw draw;
        require(snapshotImmediateWorldGeometry(b,descriptor,indices,6,{},draw),"Inf interior hole rejected visible triangles");
        require(draw.indices->indices==std::vector<uint16_t>({0,1,2,0,2,4}),"Duplicate index order/winding changed");
        const auto& bytes=draw.vertices->vertices;
        for(unsigned i=0;i<12;++i)require(bytes[3*24+i]==0,"Unreferenced Inf hole not canonicalized");
        std::vector<WorldVertex> decoded;require(decodeWorldVertices(*draw.vertices,decoded),"Holed geometry undecodable");
        require(decoded[4].position[0]==0.5f,"Live interior vertex changed");
        require(memory==memBefore,"Guest source mutated by interior canonicalization");
    }
    // Referenced NaN still fails transactionally.
    {
        EngineVector pos[4]={triPos[0],triPos[1],triPos[2],triPos[0]};
        EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
        uint8_t col[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t idx[]={0,1,3};
        fill(4,pos,uv,col,idx,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        auto probe=unchangedProbe();const auto before=probe;const auto memBefore=memory;
        require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},probe),"Referenced NaN accepted");
        require(unchangedEqual(probe,before),"Referenced NaN changed owned output");
        require(memory==memBefore,"Referenced NaN mutated guest source");
    }
    // Out-of-range index still rejects transactionally on clean data.
    {
        EngineVector pos[4]={triPos[0],triPos[1],triPos[2],triPos[0]};
        EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
        uint8_t col[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t idx[]={0,1,9};
        fill(4,pos,uv,col,idx,3);
        auto probe=unchangedProbe();const auto before=probe;const auto memBefore=memory;
        require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},probe),"Out-of-range index accepted");
        require(unchangedEqual(probe,before),"Out-of-range index changed owned output");
        require(memory==memBefore,"Out-of-range index mutated guest source");
    }
    // Mixed unused NaN then used Inf rejects with membership for the final used vertex.
    {
        EngineVector pos[5]={{0,0,0,1},triPos[1],triPos[0],triPos[1],triPos[2]};
        EngineVector uv[5]={triUv[0],triUv[1],triUv[0],triUv[1],triUv[2]};
        uint8_t col[5][4];for(unsigned i=0;i<5;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t idx[]={2,3,4};
        fill(5,pos,uv,col,idx,3);
        for(unsigned l=0;l<3;++l)put(b+positions+0*12+l*4,0xFFFFFFFFu);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0x7F800000u);
        auto probe=unchangedProbe();const auto before=probe;const auto memBefore=memory;
        ImmediateCaptureReason reason;
        require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},probe,&reason),"Mixed unused NaN plus used Inf accepted");
        require(reason.stage==ImmediateCaptureReason::Stage::vertexDecode,"Mixed failure did not report vertexDecode");
        require(reason.badVertex==3 && reason.membershipKnown && reason.referenced && reason.firstRefPos==1,
                "Mixed failure membership does not point at the used vertex");
        require(reason.rawBE==0x7F800000u,"Mixed failure raw bits do not point at the used Inf");
        require(unchangedEqual(probe,before),"Mixed failure changed owned output");
        require(memory==memBefore,"Mixed failure mutated guest source");
    }
    // Dirty unused vertex plus OOB index rejects as OOB without recovery credit.
    {
        EngineVector pos[4]={triPos[0],triPos[1],triPos[2],triPos[0]};
        EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
        uint8_t col[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t idx[]={0,1,9};
        fill(4,pos,uv,col,idx,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        auto probe=unchangedProbe();const auto before=probe;const auto memBefore=memory;
        ImmediateCaptureReason reason;
        require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},probe,&reason),"Dirty unused plus OOB accepted");
        require(reason.stage==ImmediateCaptureReason::Stage::indexOob,"Dirty OOB did not report indexOob");
        require(reason.detail0==2 && reason.detail1==9 && reason.detail2==4,"Dirty OOB position/value/count differ");
        require(unchangedEqual(probe,before),"Dirty OOB changed owned output");
        require(memory==memBefore,"Dirty OOB mutated guest source");
    }
    // Changed referenced index set cannot reuse a stale canonicalized cache,
    // in either order; identical canonicalized bytes still intern.
    {
        EngineVector pos[4]={triPos[0],triPos[1],triPos[2],triPos[0]};
        EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
        uint8_t col[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t avoid[]={0,1,2},hit[]={0,1,3};
        fill(4,pos,uv,col,avoid,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        StoredDraw good;
        require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},good),"Canonicalizable bytes rejected");
        const auto canonical=good.vertices;
        fill(4,pos,uv,col,avoid,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        StoredDraw same;
        require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},same) && same.vertices==canonical,
                "Identical canonicalized bytes not interned");
        fill(4,pos,uv,col,hit,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        StoredDraw bad;
        require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},bad),"Hit set reused stale canonicalized cache");
        require(canonical->vertices[3*24]==0,"Queued canonicalized snapshot mutated");
    }
    // Reverse order on distinct filler bytes: hit fails first, avoid succeeds.
    {
        EngineVector pos[4]={{1.5f,0.5f,0,1},{1.75f,0.75f,0,1},{2.0f,0.5f,0,1},{1.5f,0.5f,0,1}};
        EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
        uint8_t col[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)col[i][l]=triColor[i<3?i:0][l];
        const uint16_t avoid[]={0,1,2},hit[]={0,1,3};
        fill(4,pos,uv,col,hit,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        StoredDraw bad;
        require(!snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},bad),"Referenced NaN accepted before any cache entry");
        fill(4,pos,uv,col,avoid,3);
        for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
        StoredDraw good;
        require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},good),"Avoid set rejected after unrelated failure");
    }
    // End to end: tail and interior dirty slots through the RenderSurface65/67
    // fixed path and the dark RenderSurface0 alpha-blend path render pixels
    // identical to the clean finite reference (same binding as
    // deadNormalContract: no-normal independent modes over pos3/uv2/color18).
    {
        auto binding=fixture(0,false);
        binding.descriptor.flags=0x46200001;
        binding.descriptor.modes={0,4,8,16,16,9,4,4};
        binding.descriptor.coordinateMapping=0xFAC68800;
        binding.descriptor.parameters[3][0]=32;binding.descriptor.parameters[4][0]=34;binding.descriptor.parameters[5][0]=36;
        auto setLambda=[&](unsigned reg,EngineVector v){for(unsigned lane=0;lane<4;++lane)
            put(binding.constantBytes.data()+reg*16+lane*4,std::bit_cast<uint32_t>(v[lane]));};
        setLambda(32,{1,-1,.5f,.25f});setLambda(33,{2,.5f,1,.25f});
        setLambda(34,{0,0,2,.25f});setLambda(35,{1,2,3,1});setLambda(36,{.5f,.25f,.125f,1});
        setLambda(7,{0,0,0,0});
        const auto bytes=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
        for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)binding.key[i+1]=(binding.key[i+1]<<8)|bytes[i*4+n];
        WorldVertexOptions live;WorldVertexConstants constants;
        require(prepareWorldVertexProgram(binding,live,constants),"Canonical pixel binding rejected");
        auto white=std::make_shared<ColorImage>();
        white->width=white->height=1;white->pixels={255,255,255,255};
        auto whiteCube=[] {
            auto t=std::make_shared<ColorImage>();
            t->width=t->height=1;t->faces=6;t->pixels.assign(24,255);return t;
        };
        WorldDraw draw;draw.viewport={0,0,64,64};draw.targets={901,0,0,0,0};
        draw.material=WorldMaterial::fixed;draw.fragmentName="XRUtil_RenderSurface";
        draw.options=live;draw.constants=constants;draw.textures[0]=white;
        draw.textures[2]=whiteCube();draw.textures[3]=whiteCube();
        draw.fragmentConstants[0]={1,1,1,1};
        draw.fragmentConstants[1]={1,0,0,0};draw.fragmentConstants[2]={0,1,0,0};draw.fragmentConstants[3]={0,0,1,0};
        draw.fragmentConstants[4]={1,0,0,0};draw.fragmentConstants[5]={0,1,0,0};draw.fragmentConstants[6]={0,0,1,0};
        put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
        WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
        auto snapTail=[&](bool dirty,const uint8_t col[][4]) {
            EngineVector pos[4]={triPos[0],triPos[1],triPos[2],{5,5,5,1}};
            EngineVector uv[4]={triUv[0],triUv[1],triUv[2],triUv[0]};
            uint8_t c[4][4];for(unsigned i=0;i<4;++i)for(unsigned l=0;l<4;++l)c[i][l]=col[i<3?i:0][l];
            const uint16_t idx[]={0,1,2};
            fill(4,pos,uv,c,idx,3);
            if(dirty)for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0xFFFFFFFFu);
            StoredDraw geometry;
            require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},geometry),
                    dirty?"NaN-tail smoke batch rejected":"Clean smoke batch rejected");
            require(geometry.vertices->vertexCount==4 && geometry.indexCount==3,"Smoke batch dimensions differ");
            return geometry;
        };
        auto snapInterior=[&](bool dirty,const uint8_t col[][4]) {
            EngineVector pos[5]={triPos[0],triPos[1],triPos[2],{5,5,5,1},triPos[2]};
            EngineVector uv[5]={triUv[0],triUv[1],triUv[2],triUv[0],triUv[2]};
            uint8_t c[5][4];for(unsigned i=0;i<5;++i)for(unsigned l=0;l<4;++l)c[i][l]=col[i<4?i:0][l];
            const uint16_t idx[]={0,1,4};
            fill(5,pos,uv,c,idx,3);
            if(dirty)for(unsigned l=0;l<3;++l)put(b+positions+3*12+l*4,0x7F800000u);
            StoredDraw geometry;
            require(snapshotImmediateWorldGeometry(b,descriptor,indices,3,{},geometry),
                    dirty?"Inf-hole smoke batch rejected":"Clean interior batch rejected");
            require(geometry.vertices->vertexCount==5 && geometry.indexCount==3,"Interior batch dimensions differ");
            return geometry;
        };
        auto pixelsEqual=[&](const StoredDraw& dirty,const StoredDraw& clean,const char* what) {
            draw.geometry.vertices=dirty.vertices;draw.geometry.indices=dirty.indices;draw.geometry.indexCount=3;
            renderer.clear(clear);
            require(renderer.draw(draw),"Canonicalized smoke batch rejected");
            const auto recovered=renderer.readSurface(901,false);
            draw.geometry.vertices=clean.vertices;draw.geometry.indices=clean.indices;
            renderer.clear(clear);
            require(renderer.draw(draw),"Clean smoke batch rejected");
            const auto reference=renderer.readSurface(901,false);
            require(recovered.size()==64*64*8 && recovered==reference,what);
            size_t colored=0;
            for(size_t i=0;i<recovered.size();i+=8)if(recovered[i] || recovered[i+1])++colored;
            require(colored>0,"Smoke pixel fixtures rendered two empty frames");
        };
        const uint8_t bright[4][4]{{255,128,64,255},{255,128,64,255},{255,128,64,255},{255,128,64,255}};
        const uint8_t dark[4][4]{{32,16,8,255},{32,16,8,255},{32,16,8,255},{32,16,8,255}};
        const uint8_t dark5[5][4]{{32,16,8,255},{32,16,8,255},{32,16,8,255},{32,16,8,255},{32,16,8,255}};
        for(unsigned flags:{65u,67u}) {
            draw.fragmentFlags=flags;
            put(draw.attributes.data()+92,0x01100000);draw.attributes[144]=0;draw.attributes[145]=0;
            draw.fragmentConstants[0]={1,1,1,1};
            pixelsEqual(snapTail(true,bright),snapTail(false,bright),"Canonicalized tail changed visible pixels");
            pixelsEqual(snapInterior(true,bright),snapInterior(false,bright),"Canonicalized interior hole changed visible pixels");
        }
        draw.fragmentFlags=0;
        put(draw.attributes.data()+92,0x01100008);draw.attributes[144]=5;draw.attributes[145]=6;
        draw.fragmentConstants[0]={0.25f,0.25f,0.25f,1};
        pixelsEqual(snapTail(true,dark),snapTail(false,dark),"Dark RenderSurface0 tail changed visible pixels");
        pixelsEqual(snapInterior(true,dark5),snapInterior(false,dark5),"Dark RenderSurface0 interior hole changed visible pixels");
    }
    std::puts("ImmediateCanonical passed: NaN/Inf tail and interior holes canonicalized with identical pixels on 65/67 and dark RenderSurface0 blend5/6; mixed unused+used, dirty OOB, referenced NaN, OOB indices and stale cache reuse stay strict and transactional.");
}
#include "darkness_effect_tests.h"
#include "world_alpha_coverage_tests.h"
static void passes(ID3D11Device* device,ID3D11DeviceContext* context) {
    context->ClearState();WorldRendererD3D11 renderer(device,context);
    WorldClear clear;clear.targets={1,0,0,0,2};clear.viewport={0,0,64,64};clear.flags=49;renderer.clear(clear);
    auto g=std::make_shared<StoredGeometry>();g->vertexCount=3;g->stride=16;g->formats[0]=4;g->vertices.resize(48);g->indices={0,1,2};
    const EngineVector positions[]{{-.75f,-.75f,.5f,1},{0,.75f,.5f,1},{.75f,-.75f,.5f,1}};
    for(unsigned i=0;i<3;++i)for(unsigned l=0;l<4;++l)put(g->vertices.data()+i*16+l*4,std::bit_cast<uint32_t>(positions[i][l]));
    WorldDraw draw;draw.geometry.vertices=g;draw.geometry.indices=g;draw.geometry.indexCount=3;draw.viewport=clear.viewport;draw.targets=clear.targets;
    draw.attributes[97]=8; // Original alpha test ALWAYS.
    auto b=fixture(0,false);b.descriptor.flags=0x03000000;b.descriptor.modes.fill(4);
    const auto bytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
    for(unsigned i=0;i<16;++i)b.constantBytes[7*16+i]=0;
    require(prepareWorldVertexProgram(b,draw.options,draw.constants),"Pass vertex binding failed");
    auto histogramDraw=draw;
    // A depth-only query draw needs rasterization enabled, with an always-pass
    // comparison and no depth writes, independent of earlier fixture pixels.
    put(histogramDraw.attributes.data()+92,2);histogramDraw.attributes[96]=8;
    put(draw.attributes.data()+92,0x4006);draw.attributes[96]=4;draw.attributes[120]=0x80;draw.attributes[121]=2;
    draw.attributes[124]=127;draw.attributes[125]=255;draw.attributes[126]=255;
    require(renderer.draw(draw),"Depth pass not submitted");
    const auto depth=renderer.readSurface(2,true);require(depth.size()==64*64*4,"Depth readback extent");
    uint32_t center=0,corner=0;std::memcpy(&center,depth.data()+(32*64+32)*4,4);std::memcpy(&corner,depth.data(),4);
    require((center>>24)==127 && (center&0xFFFFFF)>=0x7FFFFF && (center&0xFFFFFF)<=0x800001 && corner==0,
            "Depth/stencil pass did not write the expected covered pixels");
    // An immutable IB can contain another draw's larger indices. Accept the
    // fitting subset, and still reject a range that actually references them.
    auto subset=std::make_shared<StoredGeometry>();subset->indices={0,1,2,65535};
    draw.geometry.indices=subset;
    require(renderer.draw(draw),"Valid shared-index subset was rejected by whole-buffer bounds");
    draw.geometry.firstIndex=1;
    require(!renderer.draw(draw),"Cached index bounds accepted an out-of-range vertex");
    draw.geometry.firstIndex=0;draw.geometry.indices=g;
    // GUI/external users can replace every binding on the shared context.
    // Invalidating the cache must restore the native pass, including states
    // and constant buffers whose contents have not changed.
    context->ClearState();renderer.invalidateBindings();
    renderer.clear(clear);
    require(renderer.draw(draw),"World pass did not restore bindings after external context changes");
    const auto rebound=renderer.readSurface(2,true);
    std::memcpy(&center,rebound.data()+(32*64+32)*4,4);
    require((center>>24)==127 && (center&0xFFFFFF)>=0x7FFFFF && (center&0xFFFFFF)<=0x800001,
            "Invalidated native bindings changed depth/stencil pixels");
    // Change only the stencil operation to prove depth-fail is a distinct nibble.
    draw.attributes[121]=0x20;draw.attributes[124]=83;draw.attributes[96]=5; // LESS fails against the equal depth.
    require(renderer.draw(draw),"Depth-fail pass not submitted");
    const auto failed=renderer.readSurface(2,true);std::memcpy(&center,failed.data()+(32*64+32)*4,4);
    require((center>>24)==83,"Depth-fail stencil replacement used the wrong operation");
    // Original 82066AFC keeps ordinary stencil ordering, independent of reversed Z.
    draw.attributes[96]=8;draw.attributes[120]=0x20;draw.attributes[121]=2;draw.attributes[124]=128;
    require(renderer.draw(draw),"Rejected LESS stencil pass not submitted");
    const auto rejectedLess=renderer.readSurface(2,true);std::memcpy(&center,rejectedLess.data()+(32*64+32)*4,4);
    require((center>>24)==83,"Stencil LESS accepted a reference greater than the stored value");
    draw.attributes[124]=42;
    require(renderer.draw(draw),"Less stencil pass not submitted");
    const auto less=renderer.readSurface(2,true);std::memcpy(&center,less.data()+(32*64+32)*4,4);
    require((center>>24)==42,"Stencil LESS rejected a reference below the stored value");
    draw.attributes[120]=0x50;draw.attributes[124]=128;
    require(renderer.draw(draw),"Greater stencil pass not submitted");
    const auto greater=renderer.readSurface(2,true);std::memcpy(&center,greater.data()+(32*64+32)*4,4);
    require((center>>24)==128,"Stencil GREATER rejected a reference above the stored value");
    // Motion pass reads the same depth surface; constant generated coordinates
    // make its analytic velocity (.5,.5), independent of triangle interpolation.
    b.descriptor.modes[0]=b.descriptor.modes[1]=7;b.descriptor.parameters[0][0]=b.descriptor.parameters[1][0]=20;
    put(b.constantBytes.data()+20*16+8,std::bit_cast<uint32_t>(1.0f));
    const auto motionBytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|motionBytes[i*4+n];
    require(prepareWorldVertexProgram(b,draw.options,draw.constants),"Motion binding");
    draw.material=WorldMaterial::motion;put(draw.attributes.data()+92,0x01100002);draw.attributes[96]=3;
    require(renderer.draw(draw),"Motion pass not submitted");
    const auto motion=renderer.readSurface(1,false);uint16_t velocity[4]{};std::memcpy(velocity,motion.data()+(32*64+32)*8,8);
    require(velocity[0]==0x3800 && velocity[1]==0x3800 && velocity[2]==0,"Motion color did not survive the shared depth test");
    // Full NDSP: original diffuse, specular, reconstructed normal, attenuation
    // and cube projection with an independent scalar lighting oracle.
    b.descriptor.modes.fill(4);
    for(unsigned s:{0u,1u,3u,4u,7u}) {b.descriptor.modes[s]=7;b.descriptor.parameters[s][0]=uint8_t(20+s);}
    auto constant=[&](unsigned n,EngineVector v){for(unsigned l=0;l<4;++l)put(b.constantBytes.data()+n*16+l*4,std::bit_cast<uint32_t>(v[l]));};
    constant(20,{.5f,.5f,0,1});constant(21,{0,0,0,1});constant(23,{1,0,0,0});constant(24,{1,0,0,0});constant(27,{1,0,0,0});
    const auto litBytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|litBytes[i*4+n];
    require(prepareWorldVertexProgram(b,draw.options,draw.constants),"Lighting binding");
    auto texture=[](std::array<uint8_t,4> color,unsigned faces) {
        auto t=std::make_shared<ColorImage>();t->width=t->height=1;t->faces=faces;t->pixels.resize(faces*4);
        for(unsigned f=0;f<faces;++f)for(unsigned c=0;c<4;++c)t->pixels[f*4+c]=f?0:color[c];return t;
    };
    draw.textures[0]=texture({255,128,64,255},1);draw.textures[1]=texture({64,128,192,255},1);
    draw.textures[2]=texture({128,128,128,128},1);draw.textures[4]=texture({128,255,64,255},6);
    draw.fragmentConstants[0]={1,0,0,0};draw.fragmentConstants[1]={.25f,4,.0625f,16};
    draw.fragmentConstants[2]={.7f,.5f,.3f,0};draw.fragmentConstants[3]={.2f,.4f,.6f,8};
    clear.flags=1;renderer.clear(clear);draw.material=WorldMaterial::ndsp;put(draw.attributes.data()+92,0x0010000A);draw.attributes[144]=draw.attributes[145]=2;
    const double component=128.0/255*2-1,nx=std::sqrt(1-2*component*component),spec=std::pow(2*nx*nx-1,8),attenuation=std::pow(1-1.0/16,2);
    auto halfFloat=[](uint16_t h) {const unsigned e=(h>>10)&31,m=h&1023;return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    for(unsigned pass=1;pass<=2;++pass) {
        require(renderer.draw(draw),"Lighting pass rejected");const auto lit=renderer.readSurface(1,false);uint16_t pixel[4]{};
        std::memcpy(pixel,lit.data()+(32*64+32)*8,8);
        for(unsigned c=0;c<3;++c) {
            const double diffuse=draw.textures[0]->pixels[c]/255.0,specular=draw.textures[1]->pixels[c]/255.0,projection=draw.textures[4]->pixels[c]/255.0;
            const double expected=(nx*draw.fragmentConstants[2][c]*diffuse+spec*draw.fragmentConstants[3][c]*specular)*attenuation*projection*pass;
            require(std::abs(halfFloat(pixel[c])-expected)<.002,"NDSP pixel or additive blending differs from lighting oracle");
        }
    }
    {
        // NDS retains diffuse/specular and attenuation without a projector.
        auto nds=draw;nds.fragmentName="XRShader_FP20_NDS";nds.textures[4].reset();
        renderer.clear(clear);
        for(unsigned pass=1;pass<=2;++pass) {
            require(renderer.draw(nds),"Original NDS material rejected");
            const auto lit=renderer.readSurface(1,false);uint16_t pixel[4]{};
            std::memcpy(pixel,lit.data()+(32*64+32)*8,8);
            for(unsigned c=0;c<3;++c) {
                const double diffuse=nds.textures[0]->pixels[c]/255.0,specular=nds.textures[1]->pixels[c]/255.0;
                const double expected=(nx*nds.fragmentConstants[2][c]*diffuse+spec*nds.fragmentConstants[3][c]*specular)*attenuation*pass;
                require(std::abs(halfFloat(pixel[c])-expected)<.002,"NDS pixel or additive blending differs from lighting oracle");
            }
        }
    }
    {
        auto optional=draw;optional.material=WorldMaterial::post;optional.fragmentName="XRShader_FP20_NDSEATP";
        optional.options.modes[2]=7;optional.constants.references[3][2]=22;optional.constants.vectors[22]={1,0,0,0};
        for(unsigned slot=3;slot<16;++slot)optional.textures[slot].reset();
        require(renderer.draw(optional),"Inactive original material fetch required an unbound texture");
        optional.fragmentFlags=4;
        require(!renderer.draw(optional),"Active anisotropic texture was not required");
        optional.textures[8]=texture({128,128,255,255},1);
        require(renderer.draw(optional),"Original anisotropic cross-product material rejected");
        const auto anisotropic=renderer.readSurface(1,false);uint16_t pixel[4]{};
        std::memcpy(pixel,anisotropic.data()+(32*64+32)*8,8);
        for(unsigned c=0;c<3;++c)require(std::isfinite(halfFloat(pixel[c])) && halfFloat(pixel[c])>0,
                                      "Anisotropic material produced an invalid covered pixel");
        // The original XDK linker supplies zero XYZ for missing tangent
        // semantics; the independent NativeRuntime cache-record13 oracle
        // verifies that behavior. They must not reject an otherwise valid draw.
        optional.options.tangents=true;optional.options.coordinates[2]=2;optional.options.coordinates[3]=3;
        optional.options.modes[2]=optional.options.modes[3]=4;
        require(renderer.draw(optional),"Missing original tangent defaults rejected a draw");
    }
    const auto lightingDraw=draw;
    samplerCachePressure(device,context,lightingDraw);renderer.invalidateBindings();
    // Resolve HDR with the original signed -4 exponent, then sample with +4.
    clear.color={8,4,2,1};clear.flags=1;renderer.clear(clear);
    WorldResolve resolve;resolve.targets=clear.targets;resolve.viewport=clear.viewport;resolve.rectangle={0,0,64,64};
    resolve.destination={99,4096,64,64,26,4};resolve.flags=0xF0000000;resolve.exponent=-4;
    require(renderer.resolve(resolve),"HDR resolve rejected");
    clear.color={0,0,0,0};renderer.clear(clear);
    draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    draw.fragmentConstants[0]={0,0,0,0};draw.textureObjects[0]=resolve.destination;draw.textures[0].reset();
    draw.constants.vectors[10]={1,1,1,1};put(draw.attributes.data()+92,0x01100000);
    require(renderer.draw(draw),"Resolved HDR sampling rejected");
    const auto hdr=renderer.readSurface(1,false);uint16_t h[4]{};std::memcpy(h,hdr.data()+(32*64+32)*8,8);
    require(std::abs(halfFloat(h[0])-8)<.01 && std::abs(halfFloat(h[1])-4)<.01 && std::abs(halfFloat(h[2])-2)<.01,
            "Resolve/sample exponent pair lost HDR intensity");
    // Fetch-result exponents apply to CPU-owned images as well as resolved
    // views. Reuse each image across bindings to detect baking or stale scales.
    {
        std::vector<uint8_t> memory(16384);auto* base=memory.data();
        constexpr uint32_t header=64,raw=4096,cimage=256,upload=8192;
        constexpr std::array<uint8_t,4> rgba{32,64,96,128};
        const uint32_t fetch[]{2u|(1u<<22),raw|6u,0,0xd10u,0,0x200u};
        for(unsigned i=0;i<6;++i)put(base+header+28+i*4,fetch[i]);
        std::memcpy(base+raw,rgba.data(),4);
        auto native=std::make_shared<ColorImage>();
        require(!decodeWorldTextureImage(base,header,*native),"Exponent native image decode failed");
        put(base+cimage,0x82097610);put(base+cimage+8,upload);put(base+cimage+12,4);
        put(base+cimage+16,1);put(base+cimage+20,1);put(base+cimage+24,4);
        put(base+cimage+28,4);put(base+cimage+32,0x800);put(base+cimage+40,0x814);
        base[upload]=rgba[2];base[upload+1]=rgba[1];base[upload+2]=rgba[0];base[upload+3]=rgba[3];
        auto cpu=std::make_shared<ColorImage>();
        require(!decodeUploadImage(base,cimage,upload,*cpu) && cpu->pixels==native->pixels,
                "Exponent CImage and native resource fixtures differ");
        auto sampled=draw;sampled.samplers[0]=decodeWorldSampler({2,0,0,0,0,0});
        auto sourceClear=clear;sourceClear.flags=1;
        WorldResolve copy;copy.targets=clear.targets;copy.viewport=clear.viewport;copy.rectangle={0,0,1,1};
        copy.destination={799,0x79000,1,1,6,0,1};
        unsigned failures=0,checks=0;
        auto verify=[&](const char* path,int exponent) {
            const auto pixels=renderer.readSurface(1,false);uint16_t actual[4]{};
            std::memcpy(actual,pixels.data()+(32*64+32)*8,8);
            for(unsigned channel=0;channel<4;++channel) {
                const float expected=std::ldexp(float(rgba[channel])/255.0f,exponent);
                ++checks;
                if(std::abs(halfFloat(actual[channel])-expected)>.001f*(std::max)(1.0f,expected)) {
                    std::fprintf(stderr,"FetchExponent[%s exp=%d channel=%u] actual=%.9g expected=%.9g\n",
                        path,exponent,channel,halfFloat(actual[channel]),expected);
                    ++failures;break;
                }
            }
        };
        for(int exponent:{-8,-2,0,1,4,8,0}) {
            put(base+header+40,0xd10u|((uint32_t(exponent)&63u)<<13));
            WorldTexture selected;require(snapshotWorldTexture(base,header,selected) && selected.exponent==exponent,
                                          "Signed fetch exponent snapshot differs");
            for(unsigned c=0;c<4;++c)sourceClear.color[c]=float(rgba[c])/255.0f;
            renderer.clear(sourceClear);require(renderer.resolve(copy),"Exponent reference resolve failed");
            sampled.textureObjects[0]=copy.destination;sampled.textureObjects[0].exponent=exponent;
            sampled.textures[0].reset();renderer.clear(clear);
            require(renderer.draw(sampled),"Exponent reference sample failed");verify("resolved",exponent);
            sampled.textureObjects[0]=selected;
            for(unsigned source=0;source<2;++source) {
                sampled.textures[0]=source?cpu:native;renderer.clear(clear);
                require(renderer.draw(sampled),"CPU exponent sample failed");verify(source?"CImage":"native-resource",exponent);
            }
        }
        require(native->pixels==std::vector<uint8_t>(rgba.begin(),rgba.end()) && cpu->pixels==native->pixels,
                "Per-binding exponent modified immutable source pixels");
        require(!failures,"CPU-owned texture sampling discarded fetch exponent (see FetchExponent diagnostics)");
        std::printf("FetchExponent: %u RGBA comparisons across resolved/native/CImage paths and signed binding changes passed.\n",checks);
    }
    // Sample beyond the edge of a red/blue texture. Wrapping must select red,
    // while clamping and mirrored repeat select blue; linear filtering at the
    // middle must produce half of each. This checks actual rendered pixels.
    {
        auto sampled=draw;sampled.textureObjects[0]={};
        auto stripes=std::make_shared<ColorImage>();stripes->width=2;stripes->height=1;
        stripes->pixels={255,0,0,255,0,0,255,255};sampled.textures[0]=stripes;
        for(unsigned mode:{0u,2u,1u,8u}) {
            std::array<uint32_t,6> descriptor{};
            descriptor[0]=2|((mode==8?0:mode)<<10)|(2<<13)|(2<<16);
            if(mode==8)descriptor[3]=(1<<19)|(1<<21);
            sampled.samplers[0]=decodeWorldSampler(descriptor);
            require(sampled.samplers[0].valid,"Original sampler descriptor rejected");
            sampled.constants.vectors[20]={mode==8?.5f:1.25f,.5f,0,1};
            renderer.clear(clear);require(renderer.draw(sampled),"Native sampler draw rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            const float red=mode==0?1:mode==8?.5f:0,blue=1-red;
            require(std::abs(halfFloat(h[0])-red)<.01 && std::abs(halfFloat(h[2])-blue)<.01,
                    "Native sampling lost original address/filter state");
        }
        const auto biased=decodeWorldSampler({2,0,0,5u<<25,(2u<<2)|(7u<<6)|(976u<<12),1});
        require(biased.valid && biased.bias==-1.5f && biased.anisotropy==16 && biased.minLevel==2 && biased.maxLevel==7 && biased.border==1,
                "Native sampler lost signed LOD bias, bounds or anisotropy");
        auto checker=std::make_shared<ColorImage>();checker->width=checker->height=8;checker->pixels.resize(8*8*4);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
            auto* pixel=checker->pixels.data()+(y*8+x)*4;pixel[((x+y)&1)?0:2]=255;pixel[3]=255;
        }
        sampled.textures[0]=checker;sampled.samplers[0]=decodeWorldSampler({2,0,0,0,(3u<<2)|(3u<<6),0});
        renderer.clear(clear);require(renderer.draw(sampled),"Minified checker draw rejected");
        const auto minified=renderer.readSurface(1,false);std::memcpy(h,minified.data()+(32*64+32)*8,8);
        require(std::abs(halfFloat(h[0])-.5f)<.01 && std::abs(halfFloat(h[2])-.5f)<.01,
                "Minification sampled an aliased base level instead of the averaged mip");
        sampled.material=WorldMaterial::post;sampled.fragmentName="GUIFadeToWhite";
        sampled.fragmentConstants[0]={0,0,std::bit_cast<float>(0xFFFFFFFFu),std::bit_cast<float>(0xFFFFFFFFu)};
        sampled.fragmentConstants[1]={0,0,0,0};
        renderer.clear(clear);require(renderer.draw(sampled),"Original GUI fade shader data rejected");
        const auto fade=renderer.readSurface(1,false);std::memcpy(h,fade.data()+(32*64+32)*8,8);
        require(std::abs(halfFloat(h[0])-.5f)<.01 && std::abs(halfFloat(h[2])-.5f)<.01,
                "Original zero-blur GUI constants lost the source image");
    }
    // Separate Xbox base and mip allocations contain deliberately different
    // authored colors. Original 828AE188 places nonpacked mip1 at offset zero
    // of the mip allocation; subsequent small RGBA levels each occupy 4 KiB.
    // Force each LOD so regenerating the lower levels from red cannot pass.
    {
        std::vector<uint8_t> memory(0xc000);
        constexpr unsigned resource=64,baseAddress=0x1000,mipAddress=0x6000;
        auto* b=memory.data();
        put(b+resource+28,0x80800002);put(b+resource+32,baseAddress|6);
        put(b+resource+36,63|(63u<<13));put(b+resource+40,0xd10);
        put(b+resource+44,6u<<6);put(b+resource+48,mipAddress|(1u<<9));
        const std::array<std::array<uint8_t,4>,7> colors{{
            {255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},
            {255,0,255,255},{0,255,255,255},{255,255,255,255}}};
        for(unsigned mip=0;mip<colors.size();++mip) {
            const unsigned size=64u>>mip,pitch=(std::max)(32u,size);
            const unsigned address=mip?mipAddress+(mip-1)*4096:baseAddress;
            for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
                std::memcpy(b+address+worldTextureTiledOffset(x,y,pitch,4),colors[mip].data(),4);
        }
        auto image=std::make_shared<ColorImage>();
        require(!decodeWorldTextureImage(b,resource,*image),"Authored mip fixture decode rejected");
        auto sampled=draw;sampled.textureObjects[0]={};sampled.textures[0]=image;
        sampled.constants.vectors[20]={.5f,.5f,0,1};
        for(unsigned mip=0;mip<colors.size();++mip) {
            sampled.samplers[0]=decodeWorldSampler({2,0,0,0,(mip<<2)|(mip<<6),0});
            renderer.clear(clear);require(renderer.draw(sampled),"Authored mip draw rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            for(unsigned c=0;c<4;++c)if(std::abs(halfFloat(h[c])-colors[mip][c]/255.0f)>.01f) {
                std::fprintf(stderr,"AuthoredMip[level=%u channel=%u] actual=%g expected=%g\n",
                    mip,c,halfFloat(h[c]),colors[mip][c]/255.0f);
                throw std::runtime_error("Authored texture mip replaced by generated base-level pixels");
            }
        }
        auto sparse=std::make_shared<ColorImage>(*image);
        sparse->firstMip=2;sparse->pixels.clear();sparse->mips[0].clear();sampled.textures[0]=sparse;
        for(unsigned mip=2;mip<colors.size();++mip) {
            sampled.samplers[0]=decodeWorldSampler({2,0,0,0,(mip<<2)|(mip<<6),0});
            renderer.clear(clear);require(renderer.draw(sampled),"Resident-tail image rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            for(unsigned c=0;c<4;++c)require(std::abs(halfFloat(h[c])-colors[mip][c]/255.0f)<.01f,"Resident mip sampled a rebased or absent level");
        }
        // Base-only uses the captured minimum, including a stricter minimum
        // than firstMip; it must not select the absent resource mip0.
        for(unsigned mip:{2u,3u}) {
            sampled.samplers[0]=decodeWorldSampler({2,0,0,2u<<23,(mip<<2)|(6u<<6),0});
            require(sampled.samplers[0].valid && sampled.samplers[0].baseOnly,"Base-only resident sampler rejected");
            renderer.clear(clear);require(renderer.draw(sampled),"Base-only resident-tail draw rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            for(unsigned c=0;c<4;++c)require(std::abs(halfFloat(h[c])-colors[mip][c]/255.0f)<.01f,
                "Base-only sampling ignored the captured resident minimum");
        }
        // Unsupported address modes use the existing fallback addressing,
        // but must retain valid LOD limits independently. Missing/invalid LOD
        // state must still respect the owned image's first resident level.
        for(unsigned mode=0;mode<7;++mode) {
            const unsigned minimum=mode==1 || mode==2 || mode==6?3:2;
            sampled.samplers[0]=mode==3?WorldSampler{}:decodeWorldSampler({
                mode==5?0u:2u|(4u<<10),0,0,mode==2?2u<<23:mode==6?3u<<23:0u,
                mode==4?(6u<<2)|(2u<<6):mode==5?(7u<<2)|(8u<<6)|(448u<<12):
                    (minimum<<2)|(6u<<6)|(mode==1?976u<<12:0u),0});
            require(!sampled.samplers[0].valid,"Fallback regression did not select invalid sampler state");
            renderer.clear(clear);require(renderer.draw(sampled),"Fallback resident-tail draw rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            for(unsigned c=0;c<4;++c)require(std::abs(halfFloat(h[c])-colors[minimum][c]/255.0f)<.01f,
                "Fallback sampler selected an absent mip or discarded valid LOD bounds");
            ComPtr<ID3D11SamplerState> selected;context->PSGetSamplers(0,1,&selected);
            require(selected!=nullptr,"Fallback sampler missing");
            D3D11_SAMPLER_DESC state{};selected->GetDesc(&state);
            require(state.MinLOD==minimum && state.MaxLOD==(mode==2?minimum:mode==3 || mode==4 || mode==5?15:6) &&
                    state.MipLODBias==(mode==1?-1.5f:0.0f),"Fallback sampler corrupted independent LOD state");
        }
        sampled.samplers[0]=decodeWorldSampler({2u|(4u<<10),0,0,0,1u<<6,0});
        require(!renderer.draw(sampled),"Fallback sampled outside a captured interval containing no resident mip");
        // Valid sampler state also cannot read an absent prefix. Intersect
        // its range with residency while preserving its original sampling mode.
        for(bool baseOnly:{false,true}) {
            sampled.samplers[0]=decodeWorldSampler({2u|(1u<<10),0,0,
                (1u<<19)|(1u<<21)|(baseOnly?2u<<23:0u),(1u<<2)|(6u<<6),0});
            require(sampled.samplers[0].valid,"Valid resident-interval fixture rejected");
            renderer.clear(clear);require(renderer.draw(sampled),"Overlapping resident interval rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            for(unsigned c=0;c<4;++c)require(std::abs(halfFloat(h[c])-colors[2][c]/255.0f)<.01f,
                "Valid sampler selected an absent mip");
            ComPtr<ID3D11SamplerState> selected;context->PSGetSamplers(0,1,&selected);
            require(selected!=nullptr,"Valid resident sampler missing");
            D3D11_SAMPLER_DESC state{};selected->GetDesc(&state);
            require(state.MinLOD==2 && state.MaxLOD==(baseOnly?2:6) &&
                    state.AddressU==D3D11_TEXTURE_ADDRESS_MIRROR && state.Filter==D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT &&
                    sampled.samplers[0].minLevel==1,"Resident intersection changed sampling mode or captured state");
        }
        sampled.samplers[0]=decodeWorldSampler({2,0,0,0,0,0});
        require(sampled.samplers[0].valid && !renderer.draw(sampled),
                "Valid sampler accepted an interval containing no resident mip");
        std::puts("ResidentMipInterval: valid and fallback samplers intersect residency without changing captured state.");
        std::puts("ResidentMipFallback: unsupported addressing, base-only, missing state and invalid bounds preserve residency.");
        ComPtr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(0,1,&view);
        require(view!=nullptr,"Resident-tail SRV missing");
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};view->GetDesc(&viewDesc);
        ComPtr<ID3D11Resource> boundResource;view->GetResource(&boundResource);ComPtr<ID3D11Texture2D> texture;
        check(boundResource.As(&texture),"Resident image is not Texture2D");D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        require(desc.Width==64 && desc.Height==64 && desc.MipLevels==7 &&
                viewDesc.Texture2D.MostDetailedMip==0 && viewDesc.Texture2D.MipLevels==7,
                "Resident-tail GPU resource rebased dimensions or mip indices");
        // A newly completed prefix is a different immutable image; revisiting
        // the old queued generation must still sample its retained tail.
        for(const auto& current:{image,sparse,image}) {
            sampled.textures[0]=current;const unsigned first=current->firstMip;
            sampled.samplers[0]=decodeWorldSampler({2,0,0,0,(first<<2)|(6u<<6),0});
            renderer.clear(clear);require(renderer.draw(sampled),"Streaming generation draw rejected");
            const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            require(halfFloat(h[first?2:0])>.99f && halfFloat(h[first?0:2])==0,"Streaming generation or sampler clamp leaked");
        }
        std::puts("ResidentMip: original 64x64 dimensions and seven indices, first-resident 2, immutable complete/sparse generations passed.");
        unsigned sampledLevels=7;
        for(unsigned format:{2u,6u,18u,20u,49u})for(bool packed:{false,true})
            for(bool tiled:{false,true})for(unsigned dimension:{8u,64u}) {
                std::vector<uint8_t> data(0x80000);
                constexpr unsigned mipStorage=0x20000;
                const unsigned block=format<=6?1:4,bytes=format==2?1:format==6?4:format==18?8:16;
                const unsigned endian=format==2?0:format==6?2:1,swap=endian==2?3:endian==1?1:0;
                const unsigned pitch=(std::max)(32u*block,dimension),last=unsigned(std::bit_width(dimension))-1;
                uint32_t fetch[6]{(tiled?0x80000002u:2u)|((pitch/32)<<22),baseAddress|format|(endian<<6),
                    (dimension-1)|((dimension-1)<<13),format==2?0x1400u:0xd10u,last<<6,
                    mipStorage|0x200u|(packed?0x800u:0)};
                for(unsigned i=0;i<6;++i)put(data.data()+resource+28+i*4,fetch[i]);
                for(unsigned mip=0;mip<=last;++mip) {
                    TextureMipLayout layout;
                    require(!getTextureMipLayout(fetch,0,mip,layout),"GPU authored layout fixture rejected");
                    uint8_t encoded[16]{};
                    if(format==2)encoded[0]=uint8_t(20+mip*30);
                    else if(format==6)std::memcpy(encoded,colors[mip].data(),4);
                    else if(format==49) {encoded[0]=encoded[1]=colors[mip][0];encoded[8]=encoded[9]=colors[mip][1];}
                    else {
                        auto* color=encoded+(format==20?8:0);
                        const uint16_t rgb=uint16_t((colors[mip][0]?0xf800:0)|(colors[mip][1]?0x7e0:0)|(colors[mip][2]?31:0));
                        color[0]=color[2]=uint8_t(rgb);color[1]=color[3]=uint8_t(rgb>>8);
                        if(format==20)encoded[0]=encoded[1]=255;
                    }
                    for(unsigned y=0;y<layout.blocksHigh;++y)for(unsigned x=0;x<layout.blocksWide;++x) {
                        const unsigned sx=x+layout.originBlockX,sy=y+layout.originBlockY;
                        const unsigned offset=tiled?worldTextureTiledOffset(sx,sy,layout.pitchBlocks,bytes):
                            sy*layout.rowPitchBytes+sx*bytes;
                        for(unsigned lane=0;lane<bytes;++lane)
                            data[layout.allocationAddress+layout.surfaceOffsetBytes+((offset+lane)^swap)]=encoded[lane];
                    }
                }
                auto authored=std::make_shared<ColorImage>();
                require(!decodeWorldTextureImage(data.data(),resource,*authored),"Packed/linear/compressed authored texture decode rejected");
                sampled.textures[0]=authored;
                for(unsigned mip=0;mip<=last;++mip) {
                    auto expected=colors[mip];
                    if(format==2)expected={uint8_t(20+mip*30),uint8_t(20+mip*30),uint8_t(20+mip*30),255};
                    if(format==49)expected[2]=0;
                    sampled.samplers[0]=decodeWorldSampler({2,0,0,0,(mip<<2)|(mip<<6),0});
                    renderer.clear(clear);require(renderer.draw(sampled),"Packed/linear/compressed authored mip draw rejected");
                    const auto pixels=renderer.readSurface(1,false);std::memcpy(h,pixels.data()+(32*64+32)*8,8);
                    for(unsigned c=0;c<4;++c)if(std::abs(halfFloat(h[c])-expected[c]/255.0f)>.01f) {
                        std::fprintf(stderr,"AuthoredMip[format=%u packed=%u tiled=%u dimension=%u mip=%u channel=%u] actual=%g expected=%g\n",
                            format,packed,tiled,dimension,mip,c,halfFloat(h[c]),expected[c]/255.0f);
                        throw std::runtime_error("Authored packed/linear/compressed mip pixel differs");
                    }
                    ++sampledLevels;
                }
            }
        std::printf("AuthoredMip: %u forced-LOD pixel checks; RGBA/L8/BC1/BC3/BC5, tiled/linear, packed bases/tails.\n",sampledLevels);
    }
    // Two source regions in one destination retain unrelated pixels. Check the
    // actual RGBA8 presentation resource, including asymmetric crop and offset.
    resolve.destination={100,8192,64,64,6,0};resolve.flags=0;resolve.exponent=0;
    clear.color={.2f,.4f,.6f,1};renderer.clear(clear);require(renderer.resolve(resolve),"Color resolve rejected");
    clear.color={.8f,.1f,.3f,1};renderer.clear(clear);
    resolve.rectangle={5,7,15,17};resolve.offset={20,30};require(renderer.resolve(resolve),"Partial resolve rejected");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> display,readback;check(device->CreateTexture2D(&td,nullptr,&display),"Present fixture");
    require(renderer.present(resolve.destination,display.Get()),"Resolved present rejected");
    td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&td,nullptr,&readback),"Present readback");
    context->CopyResource(readback.Get(),display.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Present map");
    const auto* pixels=static_cast<const uint8_t*>(mapped.pData);
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        // Original82865FD0 rounds 15/17 to 16/24, preserving the source
        // origin 5/7 and destination offset 20/30: the copy is 11x17.
        const bool patch=x>=20 && x<31 && y>=30 && y<47;
        const uint8_t expected[3]{uint8_t(patch?204:51),uint8_t(patch?26:102),uint8_t(patch?77:153)};
        for(unsigned c=0;c<3;++c)require(std::abs(int(pixels[size_t(y)*mapped.RowPitch+x*4+c])-expected[c])<=1,"Partial resolve overwrote the wrong region");
    }
    context->Unmap(readback.Get(),0);
    // Presentation unbinds OM targets. A later identical pass in this frame
    // must restore them even when none of its cached draw state has changed.
    auto afterPresent=histogramDraw;
    WorldClear presentClear=clear;presentClear.targets={0,0,0,0,102};presentClear.flags=48;presentClear.depth=0;
    afterPresent.targets=presentClear.targets;
    put(afterPresent.attributes.data()+92,0x4006);afterPresent.attributes[96]=4;
    afterPresent.attributes[120]=0x80;afterPresent.attributes[121]=2;
    afterPresent.attributes[124]=99;afterPresent.attributes[125]=afterPresent.attributes[126]=255;
    renderer.clear(presentClear);require(renderer.draw(afterPresent),"Pre-presentation binding fixture failed");
    require(renderer.present(resolve.destination,display.Get()),"Repeated presentation fixture failed");
    renderer.clear(presentClear);require(renderer.draw(afterPresent),"Post-presentation pass rejected");
    const auto presentedDepth=renderer.readSurface(102,true);
    std::memcpy(&center,presentedDepth.data()+(32*64+32)*4,4);
    require((center>>24)==99 && (center&0xFFFFFF)>=0x7FFFFF && (center&0xFFFFFF)<=0x800001,
            "Presentation left an identical later pass without its depth/stencil target");
    resolve.offset={63,63};require(!renderer.resolve(resolve),"Out-of-bounds resolve accepted");
    // Original resolve argument r9 selects a cube face; each face must retain
    // its own pixels and be consumable by the original projected-light shader.
    resolve.destination={101,12288,64,64,6,0,6};resolve.rectangle={0,0,64,64};resolve.offset={0,0};
    for(unsigned face=0;face<6;++face) {
        clear.color=face?EngineVector{0,1,0,1}:EngineVector{1,0,0,1};renderer.clear(clear);resolve.face=face;
        require(renderer.resolve(resolve),"Cube face resolve rejected");
    }
    draw=lightingDraw;draw.textureObjects[4]=resolve.destination;draw.textures[4].reset();
    for(unsigned face=0;face<2;++face) {
        clear.color={0,0,0,0};renderer.clear(clear);draw.constants.vectors[27]={face?-1.0f:1.0f,0,0,0};
        require(renderer.draw(draw),"Resolved cube projection rejected");const auto cube=renderer.readSurface(1,false);
        std::memcpy(h,cube.data()+(32*64+32)*8,8);
        require(halfFloat(h[face])>.01 && halfFloat(h[1-face])==0 && halfFloat(h[2])==0,"Resolved cube faces alias or select wrong axis");
    }
    {
        auto cube=std::make_shared<ColorImage>();cube->width=cube->height=8;cube->faces=6;
        cube->authoredMips=true;cube->firstMip=1;cube->mips.resize(3);
        for(unsigned mip=1;mip<4;++mip) {
            const unsigned side=8u>>mip;auto& pixels=cube->mips[mip-1];pixels.resize(size_t(side)*side*24);
            for(unsigned face=0;face<6;++face)for(unsigned p=0;p<side*side;++p) {
                auto* pixel=pixels.data()+(size_t(face)*side*side+p)*4;pixel[face==0?0:1]=255;pixel[3]=255;
            }
        }
        draw=lightingDraw;draw.textureObjects[4]={};draw.textures[4]=cube;
        draw.samplers[4]=decodeWorldSampler({2,0,0,0,(1u<<2)|(1u<<6),0});
        for(unsigned face=0;face<2;++face) {
            renderer.clear(clear);draw.constants.vectors[27]={face?-1.0f:1.0f,0,0,0};
            require(renderer.draw(draw),"Resident cube draw rejected");const auto pixels=renderer.readSurface(1,false);
            std::memcpy(h,pixels.data()+(32*64+32)*8,8);
            require(halfFloat(h[face])>.01f && halfFloat(h[1-face])==0 && halfFloat(h[2])==0,"Resident cube face/mip indexing differs");
        }
        ComPtr<ID3D11ShaderResourceView> srv;context->PSGetShaderResources(4,1,&srv);require(srv!=nullptr,"Resident cube view missing");
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};srv->GetDesc(&desc);
        require(desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURECUBE && desc.TextureCube.MostDetailedMip==0 && desc.TextureCube.MipLevels==4,
                "Resident cube view rebased authored levels");
        std::puts("ResidentMip: cube faces preserve original four-level indices with missing base.");
    }
    // A clear of another engine identity must preserve the original surface.
    clear.targets[4]=3;clear.depth=.75f;clear.flags=49;renderer.clear(clear);
    require(renderer.readSurface(2,true)==greater,"Distinct engine depth targets alias");
    // The original viewport (1,0) maps clip depth .25 to stored depth .75.
    clear.targets[4]=2;clear.flags=49;clear.depth=0;renderer.clear(clear);
    draw.material=WorldMaterial::depth;draw.fragmentName.clear();draw.targets=clear.targets;draw.depthRange={1,0,0,0};
    draw.constants.vectors[2][2]=.5f;put(draw.attributes.data()+92,6);draw.attributes[96]=2;
    require(renderer.draw(draw),"Reversed viewport draw rejected");
    const auto reversed=renderer.readSurface(2,true);std::memcpy(&center,reversed.data()+(32*64+32)*4,4);
    require(std::abs(double(center&0xFFFFFF)/0xFFFFFF-.75)<1e-6,"Original reversed viewport depth was lost");
    // Original histogram counts fragments passing the alpha comparison. Its
    // triangle covers exactly48*48/2 pixels on this64x64 single-sample target.
    draw=histogramDraw;draw.material=WorldMaterial::post;draw.fragmentName="XREngine_Histogram";
    draw.fragmentConstants[0]={0,1,0,0};draw.fragmentConstants[1]={1,0,0,0};
    auto white=std::make_shared<ColorImage>();white->width=white->height=1;white->pixels={255,255,255,255};draw.textures[0]=white;
    draw.attributes.fill(0);put(draw.attributes.data()+92,0x01100000);
    const bool expected[3][9]{{false,false,false,false,false,true,true,true,true},
                            {false,false,false,true,true,false,false,true,true},
                            {false,false,true,false,true,false,true,false,true}};
    for(unsigned r=0;r<3;++r)for(unsigned comparison=0;comparison<=8;++comparison) {
        const unsigned reference=r==0?0:r==1?255:256;
        draw.attributes[97]=uint8_t(comparison);draw.attributes[98]=uint8_t(reference>>8);draw.attributes[99]=uint8_t(reference);
        clear.color={0,0,0,0};renderer.clear(clear);
        auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
        renderer.histogram(query,true);require(renderer.draw(draw),"Histogram draw rejected");renderer.histogram(query,false);
        context->Flush();const auto deadline=GetTickCount64()+5000;
        while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
        const auto samples=query->result->samples.load();
        require(samples==(expected[r][comparison]?1152u:0u),"Histogram query ignored original alpha comparison/reference");
        const auto pixels=renderer.readSurface(1,false);size_t colored=0;
        for(size_t i=0;i<pixels.size();i+=8)if(pixels[i] || pixels[i+1])++colored;
        require(colored==samples,"Histogram samples differ from actual surviving pixels");
    }
    std::puts("Original alpha comparisons and27 actual GPU histogram measurements passed.");
    // Original822482CC emits face=1 (clockwise front), cull_back=1 for
    // engine0x800 without0x1000. With0x1000 it emits face=0 (CCW front).
    // These are front-face conventions, not directions to preserve by name.
    // A clockwise triangle must therefore survive the first configuration.
    draw.attributes[97]=8;
    auto reverse=std::make_shared<StoredGeometry>(*g);reverse->indices={0,2,1};
    for(bool reversed:{false,true})for(bool ccwFront:{false,true}) {
        draw.geometry.indices=reversed?reverse:g;
        put(draw.attributes.data()+92,0x01100800|(ccwFront?0x1000:0));
        renderer.clear(clear);require(renderer.draw(draw),"Original cull-mode fixture rejected");
        const auto pixels=renderer.readSurface(1,false);size_t colored=0;
        for(size_t i=0;i<pixels.size();i+=8)if(pixels[i] || pixels[i+1])++colored;
        require(colored==((reversed==ccwFront)?1152u:0u),"Native culling reverses the original front-face convention");
    }
    deadNormalContract(renderer);
    darknessEffectContract(renderer);
    immediateCanonicalContract(renderer);
    context->ClearState();renderer.invalidateBindings();
    std::weak_ptr<const StoredGeometry> temporary;
    {
        auto immediate=std::make_shared<StoredGeometry>(*g);temporary=immediate;
        auto oneFrame=draw;oneFrame.geometry.vertices=oneFrame.geometry.indices=immediate;
        require(renderer.draw(oneFrame),"Temporary geometry fixture rejected");
    }
    renderer.beginFrame();
    require(temporary.expired(),"Renderer retained temporary geometry through both vertex and index caches");
    paletteFallbackPass(renderer);
    partialClears(renderer,context,histogramDraw);
    deeperResources(renderer,device,context,histogramDraw);
    alphaCoveragePass(renderer,histogramDraw,1);
    // A diagonal must resolve to different coverage values inside individual
    // logical pixels. Enlarging a 64x64 raster would repeat each pixel instead.
    for(unsigned scale:{2u,3u}) {
        context->ClearState();WorldRendererD3D11 scaled(device,context,scale);
        WorldClear background;background.targets={2901,0,0,0,0};background.viewport={0,0,64,64};background.flags=1;scaled.clear(background);
        auto triangle=histogramDraw;triangle.targets=background.targets;
        triangle.material=WorldMaterial::post;triangle.fragmentName="XREngine_Histogram";
        triangle.fragmentConstants[0]={0,1,0,0};triangle.fragmentConstants[1]={1,0,0,0};
        auto white=std::make_shared<ColorImage>();white->width=white->height=1;white->pixels={255,255,255,255};triangle.textures[0]=white;
        put(triangle.attributes.data()+92,0x01100000);triangle.attributes[97]=8;
        require(scaled.draw(triangle),"Physical triangle raster rejected");
        const unsigned size=64*scale;
        const auto raster=scaled.readSurface(2901,false);
        require(raster.size()==size_t(size)*size*8,"Physical triangle readback extent differs");
        auto covered=[&](unsigned x,unsigned y){uint16_t red=0;std::memcpy(&red,raster.data()+(size_t(y)*size+x)*8,2);return red==0x3c00;};
        unsigned physicalCoverage=0,mixedBlocks=0;
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
            unsigned count=0;
            for(unsigned dy=0;dy<scale;++dy)for(unsigned dx=0;dx<scale;++dx)count+=covered(x*scale+dx,y*scale+dy);
            physicalCoverage+=count;mixedBlocks+=count>0 && count<scale*scale;
        }
        require(physicalCoverage==1152*scale*scale && mixedBlocks>0,"High-resolution triangle is an enlarged logical raster");
        WorldResolve resolve;resolve.targets=background.targets;resolve.rectangle={0,0,64,64};resolve.destination={2902,4096,64,64,6,0};
        require(scaled.resolve(resolve),"Physical triangle resolve rejected");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=size;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D11Texture2D> output,readback;check(device->CreateTexture2D(&desc,nullptr,&output),"Physical output texture");
        require(scaled.present(resolve.destination,output.Get()),"Physical triangle presentation rejected");
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc,nullptr,&readback),"Physical output staging");context->CopyResource(readback.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Physical output map");
        const auto* pixels=static_cast<const uint8_t*>(mapped.pData);
        bool matched=true;
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
            matched&=pixels[size_t(y)*mapped.RowPitch+x*4]==(covered(x,y)?255:0);
        context->Unmap(readback.Get(),0);
        require(matched,"Resolve/present lost native subpixel edge coverage");
        std::printf("PhysicalRaster%u: %u covered pixels, %u mixed logical edge blocks retained through resolve/present.\n",scale,physicalCoverage,mixedBlocks);
        alphaCoveragePass(scaled,histogramDraw,scale);
    }
}
// Page-in pacing: a burst of fresh large uploads in one frame must defer the
// overflow (not freeze decoding it all), complete it in later frames, and
// never starve a single huge mesh. Depth-only draws isolate the budget from
// texture work; each owns fresh geometry so every draw is a cache miss.
static void budgetContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    context->ClearState();WorldRendererD3D11 renderer(device,context);
    WorldClear clear;clear.targets={1,0,0,0,2};clear.viewport={0,0,64,64};clear.flags=49;renderer.clear(clear);
    auto b=fixture(0,false);b.descriptor.flags=0x03000000;b.descriptor.modes.fill(4);
    const auto bytes=encodeEngineVertexDescriptor(b.descriptor);b.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)b.key[i+1]=(b.key[i+1]<<8)|bytes[i*4+n];
    for(unsigned i=0;i<16;++i)b.constantBytes[7*16+i]=0;
    WorldVertexOptions options;WorldVertexConstants constants;
    require(prepareWorldVertexProgram(b,options,constants),"Budget vertex binding failed");
    // 64 draws x 3000 verts x 240B decoded ~= 46MB, far past the 4MB/frame
    // budget; each draw alone (~720KB) fits, so progress is always possible.
    constexpr unsigned draws=64,verts=3000;
    const EngineVector corners[]{{-.75f,-.75f,.5f,1},{0,.75f,.5f,1},{.75f,-.75f,.5f,1}};
    std::vector<WorldDraw> batch;batch.reserve(draws);
    // Extra ownership like the engine's retained queue snapshots: without it
    // beginFrame correctly recycles single-frame-owned geometry instead.
    std::vector<std::shared_ptr<StoredGeometry>> retain;retain.reserve(draws);
    for(unsigned n=0;n<draws;++n) {
        auto g=std::make_shared<StoredGeometry>();
        // Stored-tagged (nonzero address/id): transient immediate snapshots
        // bypass the budget, so the burst must look like a level page-in.
        g->address=0x10000+n*64;g->id=200+n;
        g->vertexCount=verts;g->stride=16;g->formats[0]=4;
        g->vertices.resize(size_t(verts)*16);
        for(unsigned i=0;i<verts;++i)for(unsigned l=0;l<4;++l)
            put(g->vertices.data()+i*16+l*4,std::bit_cast<uint32_t>(corners[i%3][l]));
        g->indices.resize(300);for(unsigned i=0;i<300;++i)g->indices[i]=uint16_t(i);
        WorldDraw draw;draw.geometry.vertices=g;draw.geometry.indices=g;draw.geometry.indexCount=300;
        draw.viewport=clear.viewport;draw.targets=clear.targets;draw.options=options;draw.constants=constants;
        draw.attributes[97]=8;
        put(draw.attributes.data()+92,0x4006);draw.attributes[96]=4;draw.attributes[120]=0x80;draw.attributes[121]=2;
        draw.attributes[124]=127;draw.attributes[125]=255;draw.attributes[126]=255;
        retain.push_back(g);batch.push_back(std::move(draw));
    }
    renderer.beginFrame();
    unsigned accepted=0;
    for(auto& draw:batch)if(renderer.draw(draw))++accepted;
    require(accepted>0 && accepted<draws,"Upload budget never deferred a fresh-upload burst");
    // Drain over later frames: cache hits always pass, and the budget admits
    // at least one fresh upload per frame (starvation guard), so the pending
    // set must shrink every frame until the burst completes. Shared ownership
    // in batch/retain survives every beginFrame eviction meanwhile.
    unsigned completed=0,frames=1;
    for(;frames<=20 && completed<draws;++frames) {
        renderer.beginFrame();
        const unsigned before=completed;completed=0;
        for(auto& draw:batch)if(renderer.draw(draw))++completed;
        require(completed>before,"Upload budget starved a frame with pending draws");
    }
    require(completed==draws,"Deferred burst draws did not complete once uploaded");
    const auto depth=renderer.readSurface(2,true);require(depth.size()==64*64*4,"Budget depth readback extent");
    uint32_t center=0;std::memcpy(&center,depth.data()+(32*64+32)*4,4);
    require((center>>24)==127,"Deferred burst changed depth/stencil pixels");
}
static void promptWorldContract() {
    using namespace DarkRecomp::Prompts;
    auto owned = [](uint8_t r, uint8_t g, uint8_t b, uint8_t origin, unsigned firstMip = 0) {
        auto image = std::make_shared<ColorImage>();
        image->width = image->height = 32; image->faces = 1;
        image->promptOrigin = origin; image->sourceCodec = 0; image->firstMip = firstMip;
        image->pixels.resize(firstMip ? 0 : size_t(32) * 32 * 4, 0);
        if (!firstMip) for (size_t i = 0; i < image->pixels.size(); i += 4) {
            image->pixels[i] = r; image->pixels[i + 1] = g; image->pixels[i + 2] = b; image->pixels[i + 3] = 255;
        } else {
            image->authoredMips = true; image->mips.resize(1);
            image->mips[0].resize(size_t(16) * 16 * 4, 128);
        }
        return image;
    };
    auto prompt = owned(200, 40, 40, uint8_t(Origin::A));
    const auto snapshot = prompt;
    const auto icon = replacementFor(9, prompt, Source::KeyboardMouse, Context::Menu);
    require(icon && icon->valid() && icon->width == 32 && prompt == snapshot &&
            prompt->pixels[0] == 200, "World prompt substitution mutated its draw snapshot");
    require(replacementFor(9, prompt, Source::Controller, Context::Menu) == nullptr,
            "World controller source did not preserve original artwork");
    auto unknown = owned(40, 40, 200, 0);
    require(replacementFor(9, unknown, Source::KeyboardMouse, Context::Menu) == nullptr,
            "World unknown image was substituted");
    auto cube = owned(200, 40, 40, uint8_t(Origin::A));
    cube->faces = 6; cube->pixels.assign(size_t(32) * 32 * 6 * 4, 128);
    require(replacementFor(9, cube, Source::KeyboardMouse, Context::Menu) == nullptr,
            "World cubemap prompt was substituted");
    auto partial = owned(200, 40, 40, uint8_t(Origin::B), 1);
    const auto partialIcon = replacementFor(9, partial, Source::KeyboardMouse, Context::Menu);
    require(partialIcon && partialIcon->firstMip == 0, "Partial-mip prompt lost coherent replacement LOD");
    const auto again = replacementFor(9, prompt, Source::KeyboardMouse, Context::Menu);
    require(again == icon, "World prompt icon was not reused across slots");
    require(buttonFromOrigin(Origin::DRL) == Button::DRL && buttonFromOrigin(Origin::DUD) == Button::DUD,
            "World paired DRL/DUD lost");
    require(originFromName("GUI_Arrow_Left") == Origin::Unknown, "World decorative arrow classified");
    std::puts("PromptWorld passed: owned origin, controller preserve, unknown/cube passthrough, partial-mip LOD and snapshot immutability.");
}
static std::vector<Result> captureWorldPositions(ID3D11Device* device,ID3D11DeviceContext* context,const std::vector<WorldVertex>& vertices,const WorldVertexOptions& options,const WorldVertexConstants& constants) {
    WorldVertexShaderD3D11 shader(device,options);
    std::vector<D3D11_SO_DECLARATION_ENTRY> declaration{{0,"SV_Position",0,0,4,0}};
    for(unsigned s=0;s<8;++s)declaration.push_back({0,"TEXCOORD",s,0,4,0});declaration.push_back({0,"COLOR",0,0,4,0});
    const UINT strideOut=sizeof(Result);ComPtr<ID3D11GeometryShader> stream;
    check(device->CreateGeometryShaderWithStreamOutput(shader.bytecode()->GetBufferPointer(),shader.bytecode()->GetBufferSize(),declaration.data(),UINT(declaration.size()),&strideOut,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&stream),"Format19 stream capture");
    ComPtr<ID3D11Buffer> input,output,staging;D3D11_BUFFER_DESC bd{};
    bd.ByteWidth=UINT(vertices.size()*sizeof(WorldVertex));bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{vertices.data(),0,0};check(device->CreateBuffer(&bd,&init,&input),"Format19 VB");
    bd.ByteWidth=UINT(vertices.size()*sizeof(Result));bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;bd.Usage=D3D11_USAGE_DEFAULT;bd.CPUAccessFlags=0;
    check(device->CreateBuffer(&bd,nullptr,&output),"Format19 SO");
    bd.BindFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateBuffer(&bd,nullptr,&staging),"Format19 staging");
    std::array<std::array<float,2>,8> bounds{};
    for(unsigned lane=0;lane<8;++lane)bounds[lane].fill(float(lane)/255.0f);
    require(shader.bind(context,constants,vertices,&bounds),"Format19 binding from cached index extrema failed");
    context->GSSetShader(stream.Get(),nullptr,0);
    UINT stride=sizeof(WorldVertex),offset=0;ID3D11Buffer* in=input.Get();ID3D11Buffer* out=output.Get();
    context->IASetVertexBuffers(0,1,&in,&stride,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->SOSetTargets(1,&out,&offset);context->Draw(UINT(vertices.size()),0);context->SOSetTargets(0,nullptr,nullptr);
    context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Format19 readback");
    std::vector<Result> actual(vertices.size());std::memcpy(actual.data(),mapped.pData,actual.size()*sizeof(Result));context->Unmap(staging.Get(),0);
    context->GSSetShader(nullptr,nullptr,0);context->ClearState();
    return actual;
}
static void format19GpuContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    const uint32_t xs[3]{10,100,1},ys[3]{20,200,2},zs[3]{30,300,3};
    StoredGeometry packed;packed.vertexCount=3;packed.stride=4;packed.formats[0]=19;packed.vertices.resize(12);
    StoredGeometry floats;floats.vertexCount=3;floats.stride=12;floats.formats[0]=3;floats.vertices.resize(36);
    for(unsigned i=0;i<3;++i) {
        put(packed.vertices.data()+i*4,(zs[i]<<22)|(ys[i]<<11)|xs[i]);
        put(floats.vertices.data()+i*12+0,std::bit_cast<uint32_t>(float(xs[i])));
        put(floats.vertices.data()+i*12+4,std::bit_cast<uint32_t>(float(ys[i])));
        put(floats.vertices.data()+i*12+8,std::bit_cast<uint32_t>(float(zs[i])));
    }
    std::vector<WorldVertex> fromPacked,fromFloats;
    require(decodeWorldVertices(packed,fromPacked) && decodeWorldVertices(floats,fromFloats),"Format19 GPU fixtures rejected");
    for(unsigned i=0;i<3;++i)
        require(fromPacked[i].position==fromFloats[i].position,"Format19 CPU position differs from FLOAT3");
    auto binding=fixture(0,false);
    WorldVertexOptions options;WorldVertexConstants constants;
    require(prepareWorldVertexProgram(binding,options,constants),"Format19 GPU binding rejected");
    const auto actualPacked=captureWorldPositions(device,context,fromPacked,options,constants);
    const auto actualFloats=captureWorldPositions(device,context,fromFloats,options,constants);
    unsigned comparisons=0;
    for(unsigned n=0;n<3;++n) {
        const auto ref=expected(fromFloats[n],0,false);
        for(unsigned l=0;l<40;++l) {
            require(std::isfinite(actualPacked[n][l]) && std::abs(actualPacked[n][l]-actualFloats[n][l])<=3e-5*(1+std::abs(actualFloats[n][l])),
                    "Format19 GPU output differs from FLOAT3");
            require(std::isfinite(actualPacked[n][l]) && std::abs(actualPacked[n][l]-ref[l])<=3e-5*(1+std::abs(ref[l])),
                    "Format19 GPU position conversion differs from analytic oracle");
            ++comparisons;
        }
        require(actualPacked[n][8]==double(xs[n])*2+1 && actualPacked[n][9]==double(ys[n])*3-1 && actualPacked[n][10]==double(zs[n])*4+.5,
                "Format19 GPU position missed nonuniform conversion plus translation");
    }
    require(actualPacked[0][8]!=actualPacked[1][8] && actualPacked[0][4]!=0,"Format19 GPU outputs not nonzero/converted");
    std::printf("Format19GPU passed: 3 packed vs 3 FLOAT3 vertices, %u output comparisons, nonuniform conversion analytically verified.\n",comparisons);
}
int main(int argc,char** argv) {
    constantCopyContract();
    lightingValidationContract();
    promptWorldContract();
    ComPtr<ID3D11Device> device;
    try {
        formats();paletteUsageContract();paletteArithmeticContract();worldPositionUsageContract();
        worldVertexValidationContract();immediateIndexOwnershipContract();
        if(argc>1 && std::strcmp(argv[1],"--cpu-only")==0) {
            std::puts("World vertex preparation CPU contracts passed; no D3D device created.");
            return 0;
        }
        const bool warp=argc>1 && std::strcmp(argv[1],"--warp")==0;
        ComPtr<ID3D11DeviceContext> context;const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        const auto deviceFlags=GetEnvironmentVariableA("DARK_D3D_DEBUG",nullptr,0)?D3D11_CREATE_DEVICE_DEBUG:0;
        check(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,deviceFlags,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D device");
        std::vector<WorldVertex> vertices(7);
        for(unsigned i=0;i<vertices.size();++i) {
            auto& v=vertices[i];v.position={float(i)/8,-float(i)/16,float(i)/32,1};v.normal={0,0,1,0};
            v.tex[0]={float(i),float(i+1),.3f,1};v.tex[1]={1,0,0,0};v.tex[2]={0,1,0,0};
            for(unsigned w=0;w<4;++w){v.indices[w]=float(w)/255;v.indices2[w]=float(w+4)/255;
                v.weights[w]=float(w+1)/16;v.weights2[w]=float(w+5)/16;}
        }
        ComPtr<ID3D11Buffer> input,output,staging;D3D11_BUFFER_DESC bd{};bd.ByteWidth=UINT(vertices.size()*sizeof(WorldVertex));bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{vertices.data(),0,0};check(device->CreateBuffer(&bd,&init,&input),"VB");
        bd.ByteWidth=UINT(vertices.size()*sizeof(Result));bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;check(device->CreateBuffer(&bd,nullptr,&output),"SO");
        bd.BindFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateBuffer(&bd,nullptr,&staging),"staging");
        unsigned comparisons=0;
        for(unsigned w=0;w<=8;++w) for(bool normalizing:{false,true}) for(unsigned family=0;family<9;++family) {
            const bool bumpCube=family==1;
            WorldVertexOptions options;WorldVertexConstants constants;
            auto binding=fixture(w,normalizing);
            const EngineVector basisRows[]{{1,2,3,0},{4,5,6,0},{-3,2,.5f,0}};
            if(bumpCube) {
                auto& d=binding.descriptor;d.modes[5]=18;d.modes[6]=d.modes[7]=4;d.parameters[5][0]=32;
                const auto bytes=encodeEngineVertexDescriptor(d);binding.key={};
                for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)binding.key[i+1]=(binding.key[i+1]<<8)|bytes[i*4+n];
                for(unsigned row=0;row<3;++row)for(unsigned lane=0;lane<4;++lane)
                    put(binding.constantBytes.data()+(32+row)*16+lane*4,std::bit_cast<uint32_t>(basisRows[row][lane]));
            }
            auto setConstant=[&](unsigned reg,EngineVector value) {for(unsigned lane=0;lane<4;++lane)
                put(binding.constantBytes.data()+reg*16+lane*4,std::bit_cast<uint32_t>(value[lane]));};
            auto& d=binding.descriptor;
            if(family==2) {
                d.modes[0]=13;d.modes[3]=d.modes[4]=9;d.modes[5]=17;d.parameters[5][0]=32;
                setConstant(4,{2,-1,3,0});setConstant(5,{-4,5,-2,0});
                for(unsigned row=0;row<6;++row)setConstant(32+row,{float(row+1)/10,float(row+1)/5,float(row+1)*.3f,0});
            } else if(family==3) {
                d.modes[1]=18;d.modes[2]=d.modes[3]=4;d.parameters[1][0]=32;
                for(unsigned row=0;row<3;++row){auto value=basisRows[row];value[3]=float(row+1)/4;setConstant(32+row,value);}
                setConstant(35,{.3f,.5f,.7f,0});setConstant(39,{6,7,8,0});
            } else if(family==4 || family==5) {
                // Original cache37/144: depth-offset UVs use the camera ray
                // projected into the tangent plane, followed by the UV matrix.
                d.modes[0]=22;d.parameters[0][0]=32;
                setConstant(32,{6,-3,10,1});setConstant(33,{.2f,-.35f,0,0});
                if(family==4) {
                    d.flags|=0x100;d.matrices[0]=36;
                    setConstant(36,{2,.5f,0,.1f});setConstant(37,{-.25f,3,0,-.2f});
                    setConstant(38,{0,0,1,0});setConstant(39,{0,0,0,1});
                } else d.flags&=~1u;
            } else if(family>=6) {
                // Proven one/two/three-light tuples: stage3..5 Lighting_Nonormal
                // (distance-only, all-lane multiply) plus constant-9 passthrough.
                // Rec324 (16,9,9), rec326 (16,16,9), rec328 (16,16,16).
                if(family==6) {
                    d.modes[3]=16;d.modes[4]=9;d.modes[5]=9;
                    d.parameters[3][0]=32;d.parameters[4][0]=34;d.parameters[5][0]=35;
                    setConstant(32,{1,-1,.5f,.25f});setConstant(33,{2,.5f,-1,.25f});
                    setConstant(34,{-3,4,5,.5f});setConstant(35,{1,2,3,4});
                } else if(family==7) {
                    d.modes[3]=16;d.modes[4]=16;d.modes[5]=9;
                    d.parameters[3][0]=32;d.parameters[4][0]=34;d.parameters[5][0]=36;
                    setConstant(32,{1,-1,.5f,.25f});setConstant(33,{2,.5f,-1,.25f});
                    setConstant(34,{100,100,100,1});setConstant(35,{.5f,2,1,2});
                    setConstant(36,{.5f,-1,2,.75f});
                } else {
                    d.modes[3]=16;d.modes[4]=16;d.modes[5]=16;
                    d.parameters[3][0]=32;d.parameters[4][0]=34;d.parameters[5][0]=36;
                    setConstant(32,{1,-1,.5f,.25f});setConstant(33,{2,.5f,-1,.25f});
                    setConstant(34,{100,100,100,1});setConstant(35,{.5f,2,1,2});
                    setConstant(36,{1.75f,-1.5625f,.875f,.5f});setConstant(37,{-1,2,.5f,3});
                }
            }
            const auto finalBytes=encodeEngineVertexDescriptor(d);binding.key={};
            for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)binding.key[i+1]=(binding.key[i+1]<<8)|finalBytes[i*4+n];
            require(prepareWorldVertexProgram(binding,options,constants),"World binding rejected");
            WorldVertexShaderD3D11 shader(device.Get(),options);
            std::vector<D3D11_SO_DECLARATION_ENTRY> declaration{{0,"SV_Position",0,0,4,0}};
            for(unsigned s=0;s<8;++s)declaration.push_back({0,"TEXCOORD",s,0,4,0});declaration.push_back({0,"COLOR",0,0,4,0});
            const UINT strideOut=sizeof(Result);ComPtr<ID3D11GeometryShader> stream;
            check(device->CreateGeometryShaderWithStreamOutput(shader.bytecode()->GetBufferPointer(),shader.bytecode()->GetBufferSize(),declaration.data(),UINT(declaration.size()),&strideOut,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&stream),"World stream capture");
            std::array<std::array<float,2>,8> bounds{};
            for(unsigned lane=0;lane<8;++lane)bounds[lane].fill(float(lane)/255.0f);
            require(shader.bind(context.Get(),constants,vertices,&bounds),"World binding from cached index extrema failed");context->GSSetShader(stream.Get(),nullptr,0);
            UINT stride=sizeof(WorldVertex),offset=0;ID3D11Buffer* in=input.Get();ID3D11Buffer* out=output.Get();
            context->IASetVertexBuffers(0,1,&in,&stride,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->SOSetTargets(1,&out,&offset);context->Draw(UINT(vertices.size()),0);context->SOSetTargets(0,nullptr,nullptr);
            context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"World readback");
            std::vector<Result> actual(vertices.size());std::memcpy(actual.data(),mapped.pData,actual.size()*sizeof(Result));context->Unmap(staging.Get(),0);
            for(unsigned n=0;n<vertices.size();++n) {
                auto ref=expected(vertices[n],w,normalizing);
                double sum=0;for(unsigned k=0;k<w;++k)sum+=double(k+1)/16;
                const auto scale=[&](unsigned axis){return w && !normalizing ? sum*(axis+2) : 1.0;};
                if(bumpCube) {
                    // Analytic transformed basis, also matching the original
                    // cache's nine DP3 operations for unskinned/4/8-weight keys.
                    for(unsigned row=0;row<3;++row) {
                        ref[24+row*4]=basisRows[row][2]*scale(2);
                        ref[25+row*4]=basisRows[row][1]*scale(1);
                        ref[26+row*4]=basisRows[row][0]*scale(0);
                    }
                } else if(family==2) {
                    ref[4]=3*scale(2)*.5+.5;ref[5]=scale(2)+.5;ref[6]=double(vertices[n].tex[0][2])*.5+.5;ref[7]=1;
                    const EngineVector parameters[]{{2,3,4,.25f},{-3,4,5,.5f}};
                    for(unsigned s=0;s<2;++s)for(unsigned lane=0;lane<4;++lane)ref[16+s*4+lane]=parameters[s][lane];
                    ref[24]=1.2*scale(2);ref[25]=2.4*scale(2);ref[26]=3.6*scale(2);ref[27]=1;
                } else if(family==3) {
                    const double position[]{ref[8],ref[9],ref[10]};
                    for(unsigned row=0;row<3;++row) {
                        ref[8+row*4]=basisRows[row][2]*scale(2)*.3f;
                        ref[9+row*4]=basisRows[row][1]*scale(1)*.5f;
                        ref[10+row*4]=basisRows[row][0]*scale(0)*.7f;
                        ref[11+row*4]=6+row-double(row+1)/4;
                        for(unsigned axis=0;axis<3;++axis)ref[11+row*4]-=basisRows[row][axis]*position[axis];
                    }
                } else if(family==4 || family==5) {
                    // Independent ray/plane intersection: normalization cancels
                    // in the ratio between tangent and normal components.
                    const double dx=(6-ref[8])*scale(0),dy=(-3-ref[9])*scale(1),dz=(10-ref[10])*scale(2);
                    double u=(family==4?ref[4]:vertices[n].tex[0][0])+dx/dz*.2f;
                    double v=(family==4?ref[5]:vertices[n].tex[0][1])-dy/dz*.35f;
                    ref[4]=family==4?2*u+.5*v+.1f:u;
                    ref[5]=family==4?-.25*u+3*v-.2f:v;
                } else if(family>=6) {
                    // Independent distance attenuation (no normal use):
                    // atten=max(1-dist*invRange,0), out=color*atten on all lanes.
                    const double px=ref[8],py=ref[9],pz=ref[10];
                    const double a3=lightAttenuation(px,py,pz,1.0,-1.0,0.5,0.25);
                    ref[16]=2.0*a3;ref[17]=0.5*a3;ref[18]=-1.0*a3;ref[19]=0.25*a3;
                    if(family==6) {
                        ref[20]=-3.0;ref[21]=4.0;ref[22]=5.0;ref[23]=0.5;
                        ref[24]=1.0;ref[25]=2.0;ref[26]=3.0;ref[27]=4.0;
                    } else if(family==7) {
                        const double a4=lightAttenuation(px,py,pz,100.0,100.0,100.0,1.0);
                        ref[20]=0.5*a4;ref[21]=2.0*a4;ref[22]=1.0*a4;ref[23]=2.0*a4;
                        ref[24]=0.5;ref[25]=-1.0;ref[26]=2.0;ref[27]=0.75;
                    } else {
                        const double a4=lightAttenuation(px,py,pz,100.0,100.0,100.0,1.0);
                        ref[20]=0.5*a4;ref[21]=2.0*a4;ref[22]=1.0*a4;ref[23]=2.0*a4;
                        const double a5=lightAttenuation(px,py,pz,1.75,-1.5625,0.875,0.5);
                        ref[24]=-1.0*a5;ref[25]=2.0*a5;ref[26]=0.5*a5;ref[27]=3.0*a5;
                    }
                }
                for(unsigned l=0;l<40;++l) {
                    // Original bumpcubeenv only defines/consumes XYZ here.
                    if(bumpCube && (l==27 || l==31 || l==35))continue;
                    if(!std::isfinite(actual[n][l]) || std::abs(actual[n][l]-ref[l])>3e-5*(1+std::abs(ref[l]))) {
                        std::fprintf(stderr,"family=%u weights=%u normalize=%u vertex=%u lane=%u actual=%g expected=%g\n",family,w,normalizing,n,l,actual[n][l],ref[l]);
                        throw std::runtime_error("World shader differs from independent affine/basis oracle");
                    }++comparisons;
                }
            }
        }
        format19GpuContract(device.Get(),context.Get());
        passes(device.Get(),context.Get());
        budgetContract(device.Get(),context.Get());
        darknessVisionContract(device.Get(),context.Get());
        ComPtr<ID3D11InfoQueue> messages;
        if(SUCCEEDED(device.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            check(messages->GetMessage(i,message,&size),"D3D debug message retrieval");
            require(message->Severity>D3D11_MESSAGE_SEVERITY_ERROR,"D3D debug layer reported invalid rendering commands");
        }
        context->ClearState();std::printf("WorldRendererContract passed: %s,162 variants,1134 vertices,%u output comparisons; original stage-1/5 bump-cube basis, environment mapping, light field, depth-offset, constant and Lighting_Nonormal distance modes; packed fetch; DXN/L8; depth/stencil, motion, cube-projected lighting and additive pixels; separate targets.\n",warp?"WARP":"hardware",comparisons);return 0;
    } catch(const std::exception& e){
        ComPtr<ID3D11InfoQueue> messages;
        if(device && SUCCEEDED(device.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            if(SUCCEEDED(messages->GetMessage(i,message,&size)) && message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)
                std::fprintf(stderr,"D3D11[%u]: %s\n",message->ID,message->pDescription);
        }
        std::fprintf(stderr,"WorldRendererContract failed: %s\n",e.what());return 1;
    }
}
