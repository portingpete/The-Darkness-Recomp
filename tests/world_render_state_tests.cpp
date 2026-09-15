#include "renderer/d3d11/world_render_state.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>

using namespace DarkRecomp::WorldRenderState;
static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}

template<size_t Count>
static std::array<uint32_t,Count> originalTable(std::ifstream& image,std::streamoff offset) {
    std::array<unsigned char,Count*4> bytes{};
    image.seekg(offset);
    image.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    require(bool(image),"Cannot read original render-state table");
    std::array<uint32_t,Count> values{};
    for(size_t i=0;i<Count;++i) {
        values[i]=uint32_t(bytes[i*4])<<24|uint32_t(bytes[i*4+1])<<16|
            uint32_t(bytes[i*4+2])<<8|bytes[i*4+3];
        require(values[i]<=7,"Invalid original render-state encoding");
    }
    return values;
}

static bool accepts(D3D11_COMPARISON_FUNC comparison,unsigned reference,unsigned stored) {
    switch(comparison) {
    case D3D11_COMPARISON_NEVER:return false;
    case D3D11_COMPARISON_LESS:return reference<stored;
    case D3D11_COMPARISON_EQUAL:return reference==stored;
    case D3D11_COMPARISON_LESS_EQUAL:return reference<=stored;
    case D3D11_COMPARISON_GREATER:return reference>stored;
    case D3D11_COMPARISON_NOT_EQUAL:return reference!=stored;
    case D3D11_COMPARISON_GREATER_EQUAL:return reference>=stored;
    case D3D11_COMPARISON_ALWAYS:return true;
    default:throw std::runtime_error("Invalid D3D11 comparison");
    }
}

int main(int argc,char** argv) {
    try {
        require(argc==2,"Supply the original Darkness/basefile.exe path");
        std::ifstream image(argv[1],std::ios::binary);
        require(bool(image),"Cannot open original executable");
        // In the supported executable, .rdata maps file offset N to 82000000+N.
        // Read the original big-endian tables as the oracle, independently of
        // the native renderer's translation. No game code or GPU work executes.
        const auto stencil=originalTable<9>(image,0x66AFC);
        const auto depth=originalTable<9>(image,0x66B20);
        const auto operations=originalTable<8>(image,0x66B44);
        unsigned faces=0;
        for(unsigned comparison=0;comparison<stencil.size();++comparison) {
            require(unsigned(depthComparison(comparison))==depth[comparison]+1,
                    "Depth comparison differs from original reversed-Z table");
            for(unsigned fail=0;fail<8;++fail)for(unsigned pass=0;pass<8;++pass)for(unsigned depthFail=0;depthFail<8;++depthFail) {
                const uint8_t packed[]{uint8_t((comparison<<4)|fail),uint8_t((depthFail<<4)|pass)};
                const auto face=stencilFace(packed);
                require(unsigned(face.StencilFunc)==stencil[comparison]+1,
                        "Stencil comparison differs from original ordinary-order table");
                require(unsigned(face.StencilFailOp)==operations[fail]+1,"Wrong stencil-fail operation");
                require(unsigned(face.StencilPassOp)==operations[pass]+1,"Wrong stencil-pass operation");
                require(unsigned(face.StencilDepthFailOp)==operations[depthFail]+1,"Wrong depth-fail operation");
                ++faces;
            }
        }
        for(unsigned comparison=9;comparison<16;++comparison) {
            const uint8_t packed[]{uint8_t(comparison<<4),0};
            require(stencilFace(packed).StencilFunc==D3D11_COMPARISON_NEVER,"Invalid stencil comparison must reject");
        }
        for(unsigned comparison=9;comparison<256;++comparison)
            require(depthComparison(comparison)==D3D11_COMPARISON_NEVER,"Invalid depth comparison must reject");

        struct Case {unsigned comparison,reference,stored;bool accepted;};
        const Case cases[]{
            {0,83,83,false},{1,83,83,false},
            {2,42,83,true},{2,83,83,false},{2,128,83,false},
            {3,83,83,true},{3,42,83,false},
            {4,42,83,true},{4,83,83,true},{4,128,83,false},
            {5,42,83,false},{5,83,83,false},{5,128,83,true},
            {6,83,83,false},{6,42,83,true},
            {7,42,83,false},{7,83,83,true},{7,128,83,true},
            {8,42,83,true},{8,128,83,true}
        };
        for(const auto& test:cases) {
            const uint8_t packed[]{uint8_t(test.comparison<<4),2};
            require(accepts(stencilFace(packed).StencilFunc,test.reference,test.stored)==test.accepted,
                    "Stencil mask accepts the wrong reference ordering");
        }
        std::printf("WorldRenderStateContract passed: original depth/stencil/operation tables, %u packed faces, 20 mask-order cases; no rendering.\n",faces);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"WorldRenderStateContract failed: %s\n",e.what());
        return 1;
    }
}
