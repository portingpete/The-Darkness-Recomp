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
        std::puts("vertices,iterations,decode_us,capture_changing_us,capture_repeated_us,validate_us");
        for(unsigned count:{4u,128u,1024u,16384u}) {
            const unsigned iterations=(std::max)(64u,131072u/count);
            constexpr unsigned descriptor=64,positions=256;
            const unsigned uv=positions+count*12,color=uv+count*8,indices=color+count*4;
            std::vector<uint8_t> memory(indices+6);
            auto* base=memory.data();
            put(base+descriptor,count<<16);
            put(base+descriptor+4,positions);put(base+descriptor+8,uv);
            base[descriptor+40]=2;put(base+descriptor+52,color);
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
            std::printf("%u,%u,%.6f,%.6f,%.6f,%.6f\n",count,iterations,decode,changing,repeated,validate);
        }
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"World geometry benchmark failed: %s\n",error.what());return 1;
    }
}
