#include "renderer/engine/world_mesh.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <atomic>
#include <string_view>
#include <thread>

using namespace DarkRecomp::Native;
using Clock=std::chrono::steady_clock;
static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static void put(uint8_t* base,uint32_t address,uint32_t value) {
    for(unsigned i=0;i<4;++i)base[address+i]=uint8_t(value>>(24-8*i));
}
template<class Work> static double median(Work work,unsigned repetitions) {
    for(unsigned i=0;i<2;++i)work(i);
    std::array<double,7> batches{};
    for(auto& batch:batches) {
        const auto start=Clock::now();
        for(unsigned i=0;i<repetitions;++i)work(i);
        batch=std::chrono::duration<double,std::micro>(Clock::now()-start).count()/repetitions;
    }
    std::sort(batches.begin(),batches.end());return batches[batches.size()/2];
}
// The optional consumer exercises the real streaming queue and releases draws
// on another thread. No GPU work or scheduler-dependent timing assertions.
class Consumer {
    std::atomic<bool> stop_{false},valid_{true};
    std::atomic<unsigned> completed_{0};
    std::thread worker_;
    unsigned submitted_=0;
public:
    explicit Consumer(bool enabled) {
        if(!enabled)return;
        setPreviewFrameBackpressure(true,true);
        worker_=std::thread([this] {
            std::vector<DarkRecomp::SimpleMesh> frame;
            unsigned draws=0;
            while(!stop_.load()) {
                PreviewFramePart part;
                if(!takePreviewFrame(frame,8,&part))continue;
                if(part.first)draws=0;
                for(const auto& command:frame) {
                    if(!command.world || !command.world->geometry || command.world->geometry.indexCount!=3 ||
                       command.world->geometry.vertices->id!=command.world->geometry.indices->id ||
                       command.world->material!=WorldMaterial::depth || command.world->textureMask)
                        valid_=false;
                    ++draws;
                }
                frame.clear();
                if(part.last) {
                    if(draws!=3072)valid_=false;
                    completed_.fetch_add(1);completed_.notify_one();
                }
            }
        });
    }
    bool active() const {return worker_.joinable();}
    void finishFrame() {
        ++submitted_;
        for(auto done=completed_.load();done<submitted_;done=completed_.load())completed_.wait(done);
        require(valid_.load(),"Threaded submission changed or dropped benchmark draws");
    }
    ~Consumer() {
        if(worker_.joinable()) {stop_=true;worker_.join();setPreviewFrameBackpressure(false);}
    }
};
int main(int argc,char** argv) {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    try {
        require(argc==1 || (argc==2 && std::string_view(argv[1])=="--threaded"),"Expected optional --threaded");
        require(base!=nullptr,"Reserve private guest address space");
        require(VirtualAlloc(base+0x82A60000,0x10000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Commit render context");
        require(VirtualAlloc(base+0x01000000,0x100000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Commit private resource fixtures");
        constexpr uint32_t context=0x82A69B00,table=0x01000000,validity=0x01050000,manager=0x01061000;
        constexpr uint32_t device=0x01070000,resources=0x01080000,resourceCount=2048,drawsPerFrame=3072;
        put(base,context+16636,table);put(base,context+160,manager);put(base,manager+24,validity);
        StoredGeometryCache cache;
        for(unsigned id=1;id<=resourceCount;++id) {
            const auto resource=resources+id*128;
            put(base,table+(id+1)*4,resource);base[validity+id]=1;
            put(base,resource+92,(id<<16)|1);base[resource+96]=12;
            put(base,resource+12,1);put(base,resource+52,1);
            auto upload=cache.begin(base,resource);require(bool(upload.geometry),"Begin benchmark resource");
            auto& geometry=*upload.geometry;
            geometry.vertexCount=3;geometry.stride=12;geometry.formats[0]=3;
            geometry.vertices.resize(36);geometry.indices={0,1,2};geometry.maximumIndex=2;
            require(cache.finish(std::move(upload),base),"Publish benchmark resource");
        }
        std::puts("resources,stored_lookup_us,depth_submission_us_per_draw");
        enableEnginePreview();
        base[context+16896+97]=8;
        put(base,context+17160,1280);put(base,context+17164,720);put(base,context+17172,0x3F800000);
        EngineVertexBindingSnapshot binding;binding.deviceAddress=device;
        binding.descriptor.flags=0x01000000;binding.descriptor.modes.fill(4);
        const auto encoded=encodeEngineVertexDescriptor(binding.descriptor);
        for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
            uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
        std::vector<DarkRecomp::SimpleMesh> frame;
        Consumer consumer(argc==2);
        for(unsigned workingSet:{64u,512u,2048u}) {
            const double lookup=median([&](unsigned i) {
                const auto id=1+(i*13)%workingSet;
                auto draw=cache.draw(base,id,id,0,3,resources+id*128+56);
                require(bool(draw),"Stored lookup failed");
            },100000);
            const double submission=median([&](unsigned frameIndex) {
                for(unsigned i=0;i<drawsPerFrame;++i) {
                    const auto id=1+((i+frameIndex)*13)%workingSet;
                    auto draw=cache.draw(base,id,id,0,3,resources+id*128+56);
                    draw.vertexBindings=binding;
                    previewObserveWorld(base,draw);
                }
                previewEndFrame();
                if(consumer.active())consumer.finishFrame();
                else {
                    require(takePreviewFrame(frame) && frame.size()==drawsPerFrame,"Submission dropped benchmark draws");
                    frame.clear();
                }
            },12)/drawsPerFrame;
            std::printf("%u,%.6f,%.6f\n",workingSet,lookup,submission);
        }
        VirtualFree(base,0,MEM_RELEASE);return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Scene submission benchmark: %s\n",error.what());
        if(base)VirtualFree(base,0,MEM_RELEASE);return 1;
    }
}
