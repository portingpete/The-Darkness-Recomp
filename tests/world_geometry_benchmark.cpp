#include "renderer/engine/world_mesh.h"
#include "renderer/engine/scene_work.h"
#include <string_view>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
using Clock = std::chrono::steady_clock;

static void put(uint8_t* destination, uint32_t value) {
    for (unsigned i=0;i<4;++i) destination[i]=uint8_t(value>>(24-8*i));
}
static void putHalf(uint8_t* destination,uint16_t value) {
    destination[0]=uint8_t(value>>8);destination[1]=uint8_t(value);
}

template<class Work> static double measure(Work work, unsigned iterations) {
    for(unsigned i=0;i<32;++i) work(i);
    std::array<double,9> samples{};
    for(auto& sample:samples) {
        const auto start=Clock::now();
        for(unsigned i=0;i<iterations;++i) work(i);
        sample=std::chrono::duration<double,std::micro>(Clock::now()-start).count()/iterations;
    }
    std::sort(samples.begin(),samples.end());
    return samples[samples.size()/2];
}

int main(int argc,char** argv) {
    try {
        if(argc>2 || (argc==2 && std::string_view(argv[1])!="--serial"))throw std::runtime_error("Expected optional --serial");
        initializeSceneWorkers(argc==2?0:UINT32_MAX);
        std::puts("layout,vertices,indices,iterations,decode_us,capture_changing_us,capture_repeated_us,validate_us");
        for(bool positionsOnly:{false,true})for(unsigned count:{4u,128u,1024u,16384u}) {
            const unsigned iterations=(std::max)(64u,131072u/count);
            constexpr unsigned descriptor=64,positions=256;
            const unsigned uv=positions+count*12,color=uv+count*8,indices=color+count*4;
            std::vector<uint8_t> memory(indices+6);
            auto* base=memory.data();
            put(base+descriptor,count<<16);
            put(base+descriptor+4,positions);
            if(!positionsOnly) {
                put(base+descriptor+8,uv);base[descriptor+40]=2;put(base+descriptor+52,color);
            }
            base[indices+3]=1;base[indices+5]=2;
            for(unsigned i=0;i<count;++i) {
                put(base+positions+i*12,std::bit_cast<uint32_t>(float(i)*0.125f));
                put(base+color+i*4,0xFFFFFFFF);
            }
            StoredDraw draw;
            auto capture=[&] {
                if(!snapshotImmediateWorldGeometry(base,descriptor,indices,3,{},draw))
                    throw std::runtime_error("Immediate capture rejected valid benchmark geometry");
            };
            capture();
            const auto source=draw.vertices;
            const double decode=measure([&](unsigned) {
                std::vector<WorldVertex> decoded;
                if(!decodeWorldVertices(*source,decoded) || decoded.size()!=count)
                    throw std::runtime_error("Vertex decode failed");
            },iterations);
            unsigned revision=0;
            const double changing=measure([&](unsigned) {
                put(base+positions,std::bit_cast<uint32_t>(float(++revision)));
                capture();
            },iterations);
            const double repeated=measure([&](unsigned) {capture();},iterations);
            const double validate=measure([&](unsigned) {
                if(!validateWorldVertices(*source))throw std::runtime_error("Vertex validation failed");
            },iterations);
            std::printf("%s,%u,3,%u,%.6f,%.6f,%.6f,%.6f\n",positionsOnly?"position":"position-uv-color",count,iterations,decode,changing,repeated,validate);
        }
        // A verified stored VB can be combined with a long immediate IB. This
        // isolates the draw-time BE16 conversion/range check from vertex
        // copying; the existing rows above still cover tiny complete captures.
        auto vertices=std::make_shared<StoredGeometry>();
        vertices->vertexCount=65535;vertices->stride=12;vertices->formats[0]=3;
        vertices->vertices.resize(size_t(vertices->vertexCount)*vertices->stride);
        for(unsigned count:{3u,6u,21u,24u,27u,96u,384u,1536u,6144u,24576u,49152u}) {
            constexpr unsigned indices=64;
            const unsigned iterations=(std::max)(64u,131072u/count);
            std::vector<uint8_t> memory(indices+count*2);
            std::vector<uint16_t> expected(count);
            for(unsigned i=0;i<count;++i) {
                expected[i]=uint16_t((i*13u)%vertices->vertexCount);
                putHalf(memory.data()+indices+i*2,expected[i]);
            }
            StoredDraw draw;
            auto capture=[&] {
                if(!snapshotImmediateWorldGeometry(memory.data(),0,indices,count,vertices,draw) ||
                   draw.vertices!=vertices || draw.indexCount!=count || draw.indices->indices.size()!=count)
                    throw std::runtime_error("Stored-VB immediate index capture failed");
            };
            capture();
            if(draw.indices->indices!=expected)throw std::runtime_error("Stored-VB immediate index bytes differ");
            unsigned revision=0;
            const double changing=measure([&](unsigned) {
                expected[0]=uint16_t(++revision%vertices->vertexCount);
                putHalf(memory.data()+indices,expected[0]);capture();
            },iterations);
            const double repeated=measure([&](unsigned) {capture();},iterations);
            if(draw.indices->indices!=expected)throw std::runtime_error("Changing immediate index bytes differ");
            std::printf("stored-index,%u,%u,%u,0,%.6f,%.6f,0\n",vertices->vertexCount,count,iterations,changing,repeated);
        }
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"World geometry benchmark failed: %s\n",error.what());return 1;
    }
}
