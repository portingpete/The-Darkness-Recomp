#include "renderer/d3d11/world_vertex_metadata.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
static void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}

// The renderer's former eight full passes, retained as a correctness oracle
// and a timing baseline. Input comes from the actual fetch decoder below.
__declspec(noinline) static WorldVertexMetadata previous(std::span<const WorldVertex> vertices) {
    WorldVertexMetadata result;result.count=uint32_t(vertices.size());
    if(vertices.empty())return result;
    for(unsigned lane=0;lane<8;++lane) {
        auto value=[&](const auto& vertex){return lane<4?vertex.indices[lane]:vertex.indices2[lane-4];};
        auto& bounds=result.indexBounds[lane];bounds.fill(value(vertices.front()));
        for(const auto& vertex:vertices) {
            bounds[0]=(std::min)(bounds[0],value(vertex));bounds[1]=(std::max)(bounds[1],value(vertex));
        }
    }
    return result;
}
__declspec(noinline) static WorldVertexMetadata current(std::span<const WorldVertex> vertices,
                                                       const std::array<uint8_t,16>& formats) {
    return worldVertexMetadata(vertices,formats);
}
static bool same(const WorldVertexMetadata& first,const WorldVertexMetadata& second) {
    return first.count==second.count && std::memcmp(first.indexBounds.data(),second.indexBounds.data(),sizeof(first.indexBounds))==0;
}
static void put(uint8_t* destination,uint32_t value) {
    for(unsigned i=0;i<4;++i)destination[i]=uint8_t(value>>(24-i*8));
}
static StoredGeometry source(unsigned firstFormat,unsigned secondFormat) {
    static constexpr unsigned sizes[]{0,4,8,12,16,0,0,0,0,2,4,6,8,0,4,4,4,4,4,4};
    StoredGeometry geometry;geometry.vertexCount=19;
    geometry.formats[0]=4;geometry.formats[1]=10;geometry.formats[9]=14;
    geometry.formats[12]=uint8_t(firstFormat);geometry.formats[14]=uint8_t(secondFormat);
    for(auto format:geometry.formats)geometry.stride+=sizes[format];
    geometry.vertices.resize(size_t(geometry.vertexCount)*geometry.stride);
    for(unsigned vertex=0;vertex<geometry.vertexCount;++vertex) {
        auto* field=geometry.vertices.data()+size_t(vertex)*geometry.stride;
        for(auto format:geometry.formats) {
            if(format>=1 && format<=4)for(unsigned lane=0;lane<format;++lane) {
                float value=float(int((vertex*13+lane*7)%29)-14)/8;
                if(vertex<2)value=std::bit_cast<float>(vertex?0x80000000u:0u);
                put(field+lane*4,std::bit_cast<uint32_t>(value));
            }
            else if(format>=9 && format<=12)for(unsigned byte=0;byte<sizes[format];byte+=2) {
                const unsigned value=0x3000+vertex*13+byte;
                field[byte]=uint8_t(value>>8);field[byte+1]=uint8_t(value);
            }
            else if(format)put(field,0xA1030405u^(vertex*0x01710309u));
            field+=sizes[format];
        }
    }
    return geometry;
}
static void contract() {
    require(same(previous({}),current({},{})),"Empty metadata changed");
    constexpr unsigned formats[]{0,1,2,3,4,9,10,11,12,14,15,16,17,18,19};
    for(auto firstFormat:formats)for(auto secondFormat:formats) {
        const auto packed=source(firstFormat,secondFormat);
        std::vector<WorldVertex> decoded;
        require(decodeWorldVertices(packed,decoded),"Metadata fixture was not accepted by the fetch decoder");
        require(same(previous(decoded),current(decoded,packed.formats)),"Metadata changed an index extremum or signed-zero bit");
        const uint16_t selected[]{18,4,18};
        std::vector<WorldVertex> sampled;
        require(sampleWorldVertices(packed,selected,sampled) && sampled.size()==std::size(selected),"Valid diagnostic subset failed");
        for(unsigned i=0;i<std::size(selected);++i)
            require(std::memcmp(&sampled[i],&decoded[selected[i]],sizeof(WorldVertex))==0,
                    "Diagnostic subset changed packed fetch values or duplicate index order");
        const auto original=sampled;
        const uint16_t invalid[]{19};
        require(!sampleWorldVertices(packed,invalid,sampled) && std::memcmp(sampled.data(),original.data(),original.size()*sizeof(WorldVertex))==0,
                "Invalid diagnostic index changed the output");
    }
    // Test finite canonicalized holes, and failure if a sampled vertex still
    // contains nonfinite data. Reconstruction cannot change draw acceptance.
    auto packed=source(4,4);
    std::fill_n(packed.vertices.begin()+packed.stride,packed.stride,uint8_t(0));
    const uint16_t hole[]{1,18,1};
    std::vector<WorldVertex> full,sampled;
    require(decodeWorldVertices(packed,full) && sampleWorldVertices(packed,hole,sampled),"Finite recovered hole failed");
    for(unsigned i=0;i<std::size(hole);++i)
        require(std::memcmp(&sampled[i],&full[hole[i]],sizeof(WorldVertex))==0,"Recovered hole diagnostic changed GPU fetch values");
    const auto saved=sampled;
    auto unchanged=[&] {return sampled.size()==saved.size() && std::memcmp(sampled.data(),saved.data(),saved.size()*sizeof(WorldVertex))==0;};
    put(packed.vertices.data()+packed.stride,0x7F800001u);
    require(!sampleWorldVertices(packed,hole,sampled) && unchanged(),"Sampled NaN changed diagnostic output");
    packed.vertices.pop_back();
    require(!sampleWorldVertices(packed,hole,sampled) && unchanged(),"Truncated source was accepted by diagnostic reconstruction");
    require(!sampleWorldVertices(packed,{},sampled) && unchanged(),"Empty diagnostic subset changed output");
    std::vector<WorldVertex> extremes(3);
    for(unsigned lane=0;lane<4;++lane) {
        extremes[0].indices[lane]=extremes[0].indices2[lane]=std::numeric_limits<float>::max();
        extremes[1].indices[lane]=extremes[1].indices2[lane]=std::numeric_limits<float>::lowest();
    }
    std::array<uint8_t,16> streams{};streams[12]=streams[14]=4;
    require(same(previous(extremes),current(extremes,streams)),"Finite extreme index values changed");
    for(unsigned lane=0;lane<4;++lane) {
        extremes[0].indices[lane]=extremes[0].indices2[lane]=std::numeric_limits<float>::denorm_min();
        extremes[1].indices[lane]=extremes[1].indices2[lane]=-std::numeric_limits<float>::denorm_min();
    }
    require(same(previous(extremes),current(extremes,streams)),"Subnormal index extrema changed");
    struct MxcsrRestore {unsigned value=_mm_getcsr();~MxcsrRestore(){_mm_setcsr(value);}} restore;
    for(unsigned flags:{0u,0x40u,0x8000u,0x8040u}) {
        _mm_setcsr((restore.value&~0x8040u)|flags);
        require(same(previous(extremes),current(extremes,streams)),"DAZ/FTZ changed subnormal index extrema");
    }
    std::puts("WorldVertexMetadata passed: all 225 index format pairs, bitwise extrema, subset/duplicate diagnostic fetches, recovered finite holes, transactional invalid sources.");
}
static uint64_t checksum(const WorldVertexMetadata& metadata) {
    uint64_t result=metadata.count;
    for(const auto& bounds:metadata.indexBounds)for(float value:bounds)result+=std::bit_cast<uint32_t>(value);
    return result;
}
static void benchmark() {
    std::puts("vertices,index_lanes,old_metadata_us,new_metadata_us,old_retained_cpu_bytes,new_retained_cpu_bytes");
    for(unsigned count:{4u,128u,1024u,16384u})for(unsigned streams:{0u,4u,8u}) {
        std::vector<WorldVertex> vertices(count);
        std::array<uint8_t,16> formats{};formats[12]=streams>=4?4:0;formats[14]=streams==8?4:0;
        for(unsigned vertex=0;vertex<count;++vertex)for(unsigned lane=0;lane<4;++lane) {
            vertices[vertex].indices[lane]=streams>=4?float((vertex*13+lane*7)%256)/255.0f:float(lane==3);
            vertices[vertex].indices2[lane]=streams==8?float((vertex*17+lane*11)%256)/255.0f:float(lane==3);
        }
        const unsigned repetitions=(std::max)(64u,131072u/count);
        const uint64_t expected=checksum(previous(vertices))*repetitions;
        std::array<double,9> before{},after{};
        auto measure=[&](bool old) {
            uint64_t sum=0;const auto begin=std::chrono::steady_clock::now();
            for(unsigned repeat=0;repeat<repetitions;++repeat) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
                sum+=checksum(old?previous(vertices):current(vertices,formats));
            }
            const double elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/repetitions;
            require(sum==expected,"Metadata benchmark changed results");return elapsed;
        };
        for(unsigned trial=0;trial<before.size();++trial) {
            if(trial%2) {after[trial]=measure(false);before[trial]=measure(true);}
            else {before[trial]=measure(true);after[trial]=measure(false);}
        }
        std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
        std::printf("%u,%u,%.6f,%.6f,%zu,%zu\n",count,streams,before[4],after[4],
                    vertices.size()*sizeof(WorldVertex),sizeof(WorldVertexMetadata));
    }
}
int main(int argc,char** argv) {
    try {
        require(argc==1 || (argc==2 && std::string_view(argv[1])=="--benchmark"),"Expected optional --benchmark");
        contract();if(argc==2)benchmark();return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"WorldVertexMetadata failed: %s\n",error.what());return 1;}
}
