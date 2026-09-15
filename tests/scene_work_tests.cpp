#include "renderer/engine/scene_work.h"
#include "renderer/engine/world_mesh.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string_view>
#include <xmmintrin.h>
using namespace DarkRecomp::Native;
using DarkRecomp::ColorImage;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static void put(uint8_t* p,uint32_t word) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(word>>(24-i*8));}
static void little(uint8_t* p,uint32_t word) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(word>>(i*8));}

static void executorContract() {
    SceneWorkPool pool(3);
    require(pool.workers()==3,"Test pool could not create workers");
    std::mutex mutex;std::condition_variable arrived;
    unsigned participants=0;std::set<DWORD> threads;
    require(pool.run(4,1,[&](size_t first,size_t end) {
        std::unique_lock lock(mutex);threads.insert(GetCurrentThreadId());++participants;arrived.notify_all();
        return first+1==end && arrived.wait_for(lock,std::chrono::seconds(5),[&]{return participants==4;});
    }),"Ranges did not execute concurrently");
    require(threads.size()==4,"Pool failed to engage three workers and the caller");
    std::vector<std::atomic<unsigned>> hits(4099);
    require(pool.run(hits.size(),127,[&](size_t first,size_t end) {
        return pool.run(end-first,3,[&](size_t begin,size_t stop) {
            for(size_t n=begin;n<stop;++n)++hits[first+n];return true;
        });
    }),"Recursive work deadlocked or failed");
    for(auto& n:hits)require(n==1,"Range overlap, omission, or tail error");
    require(!pool.run(10000,32,[](size_t,size_t){throw std::runtime_error("expected");return true;}),"Worker exception was lost");
    require(pool.run(10000,32,[](size_t,size_t){return true;}),"Failure poisoned the next batch");
    std::atomic<unsigned> completed=0;
    auto concurrent=[&] {
        for(unsigned repeat=0;repeat<100;++repeat)
            if(pool.run(1027,31,[](size_t first,size_t end){return first<end;}))++completed;
    };
    std::thread other(concurrent);concurrent();other.join();
    require(completed==200,"Concurrent producers lost work");
    SceneWorkPool serial(0);unsigned calls=0;
    require(serial.run(100,1,[&](size_t a,size_t b){++calls;return a==0 && b==100;}),"Serial fallback changed range");
    require(serial.run(0,1,[&](size_t,size_t){++calls;return false;}) && calls==1,"Empty range called callback");
}
static void geometryContract() {
    StoredGeometry g;g.vertexCount=8193;g.stride=56;
    g.formats[0]=3;g.formats[1]=15;g.formats[9]=14;g.formats[10]=18;
    g.formats[12]=12;g.formats[13]=4;g.formats[14]=17;g.formats[15]=16;
    g.vertices.resize(size_t(g.vertexCount)*g.stride);
    uint32_t random=0x13241342;
    for(auto& byte:g.vertices){random^=random<<13;random^=random>>17;random^=random<<5;byte=uint8_t(random);}
    for(unsigned v=0;v<g.vertexCount;++v) {
        auto* p=g.vertices.data()+size_t(v)*g.stride;
        for(unsigned n=0;n<3;++n)put(p+n*4,std::bit_cast<uint32_t>(float(int(v)-1024)/7.0f));
        for(unsigned n=0;n<4;++n)put(p+32+n*4,std::bit_cast<uint32_t>(float(n)/3.0f));
    }
    const auto original=g.vertices;const auto csr=_mm_getcsr();
    for(unsigned rounding:{0u,0x2000u,0x4000u,0x6000u}) {
        _mm_setcsr((csr&~0x6000u)|rounding);
        std::vector<WorldVertex> parallel;
        require(decodeWorldVertices(g,parallel),"Parallel geometry decode failed");
        StoredGeometry one=g;one.vertexCount=1;one.vertices.resize(g.stride);
        for(unsigned v=0;v<g.vertexCount;++v) {
            std::memcpy(one.vertices.data(),g.vertices.data()+size_t(v)*g.stride,g.stride);
            std::vector<WorldVertex> expected;
            require(decodeWorldVertices(one,expected) && !std::memcmp(&expected[0],&parallel[v],sizeof(WorldVertex)),
                "Parallel geometry differs from per-vertex serial decode");
        }
        require((_mm_getcsr()&0x6000u)==rounding,"Worker changed caller rounding mode");
    }
    _mm_setcsr(csr);
    require(g.vertices==original,"Workers changed source geometry");
    std::vector<WorldVertex> before(1);before[0].position[0]=123;
    auto output=before;put(g.vertices.data()+size_t(g.vertexCount-1)*g.stride,0x7FC00000);
    require(!decodeWorldVertices(g,output) && output.size()==1 && !std::memcmp(output.data(),before.data(),sizeof(WorldVertex)),
        "Failed parallel decode published partial output");
    g.vertexCount=65535;g.vertices.resize(size_t(g.vertexCount)*g.stride,0);
    require(!validateWorldVertices(g),"Parallel validator missed nonfinite input");
    put(g.vertices.data()+size_t(8192)*g.stride,0);
    require(validateWorldVertices(g),"Parallel validator rejected valid input");
}
static std::vector<uint8_t> imageFixture(unsigned codec,unsigned width,unsigned height) {
    const unsigned size=((width+3)/4)*((height+3)/4)*(codec?16:8);
    std::vector<uint8_t> bytes(256+16+size);
    put(bytes.data()+64,0x82097610);put(bytes.data()+72,256);put(bytes.data()+76,size+16);
    put(bytes.data()+80,width);put(bytes.data()+84,height);put(bytes.data()+92,4);
    put(bytes.data()+96,0x800);put(bytes.data()+104,0x5014);
    little(bytes.data()+256,codec);little(bytes.data()+260,size);little(bytes.data()+268,16);
    return bytes;
}
static void textureContract() {
    constexpr unsigned width=513,height=257,blocksX=(width+3)/4,blocksY=(height+3)/4;
    for(unsigned codec:{0u,4u}) {
        auto bytes=imageFixture(codec,width,height);const unsigned blockBytes=codec?16:8;
        uint32_t random=0x85664763;
        for(size_t n=272;n<bytes.size();++n){random^=random<<13;random^=random>>17;random^=random<<5;bytes[n]=uint8_t(random);}
        const auto original=bytes;ColorImage image;
        require(!decodeColorImage(bytes.data(),64,image),"Parallel BC decode failed");
        auto block=imageFixture(codec,4,4);
        for(unsigned by=0;by<blocksY;++by)for(unsigned bx=0;bx<blocksX;++bx) {
            std::memcpy(block.data()+272,bytes.data()+272+(by*blocksX+bx)*blockBytes,blockBytes);
            ColorImage expected;require(!decodeColorImage(block.data(),64,expected),"Serial BC oracle failed");
            for(unsigned y=0;y<4 && by*4+y<height;++y)for(unsigned x=0;x<4 && bx*4+x<width;++x)
                require(!std::memcmp(image.pixels.data()+((by*4+y)*width+bx*4+x)*4,expected.pixels.data()+(y*4+x)*4,4),
                    "Parallel BC decode changed a texel or edge crop");
        }
        require(bytes==original,"Parallel BC decode changed the source");
    }
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--default-serial") {
            auto& pool=sceneWorkPool();
            require(pool.workers()==0,"Lazy initialization started unrequested workers");
            initializeSceneWorkers();
            const DWORD caller=GetCurrentThreadId();unsigned calls=0;
            require(parallelSceneRange(65535,2048,[&](size_t first,size_t end) {
                ++calls;return GetCurrentThreadId()==caller && first==0 && end==65535;
            }),"Default scene work was split or moved off the caller");
            require(calls==1 && pool.workers()==0 && pool.batches()==0 && pool.assistedChunks()==0,
                "Default serial work used the parallel dispatcher");
            geometryContract();textureContract();printSceneWorkCounters();
            std::puts("Default scene work passed: no helpers, whole-range serial execution, geometry and texture parity.");
            return 0;
        }
        require(argc==1,"Expected optional --default-serial");
        initializeSceneWorkers(4);
        executorContract();geometryContract();textureContract();printSceneWorkCounters();
        std::puts("Scene work passed: concurrent execution, nested/concurrent callers, failure recovery, floating-point parity, geometry and BC1/BC3 pixel parity.");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"Scene work: %s\n",e.what());return 1;}
}
