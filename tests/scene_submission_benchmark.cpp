#include "renderer/engine/world_mesh.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
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
// Measure the complete pooled capture, including material constants and
// completed sampler/texture bindings, rather than an isolated byte operation.
static void captureBenchmark(uint8_t* base,uint32_t context,uint32_t device,const StoredDraw& original) {
    constexpr uint32_t program=0x01062000,name=0x01062100,objects=0x01063000;
    struct Case {const char* label;const char* fragment;WorldMaterial material;unsigned vectors,stages;};
    constexpr Case cases[]{
        {"depth",nullptr,WorldMaterial::depth,0,0},
        {"fixed-zero",nullptr,WorldMaterial::fixed,0,0},
        {"fixed-two",nullptr,WorldMaterial::fixed,1,2},
        {"motion","XRShader_MotionMap",WorldMaterial::motion,4,0},
        {"ndsp","XRShader_FP20_NDSP",WorldMaterial::ndsp,4,0},
        {"decal","XRShader_FP20_Decal",WorldMaterial::post,34,0},
        {"post-full","UnknownNativeFragment",WorldMaterial::post,64,0}
    };
    for(unsigned slot=0;slot<16;++slot) {
        const auto object=objects+slot*64;
        put(base,object+28,2);put(base,object+32,(0x01000000+slot*4096)|6);
        put(base,object+36,31u|(31u<<13));put(base,object+40,0xd10);put(base,object+44,5u<<6);
        put(base,device+12536+slot*4,object);
        put(base,device+1152+slot*24,2);put(base,device+1152+slot*24+12,(1u<<19)|(1u<<21)|(1u<<23));
        put(base,device+1152+slot*24+16,5u<<6);
    }
    std::puts("descriptor,material,fragment_vectors,texture_mask,prepare_us,capture_us");
    for(unsigned rich=0;rich<2;++rich) {
        auto geometry=original;
        auto& binding=*geometry.vertexBindings;
        auto& descriptor=binding.descriptor;
        if(rich) {
            descriptor.coordinateMapping=0x12345600;descriptor.declarationFlags=0xA1234400;
            descriptor.flags=0x01000B03;descriptor.color=12;
            descriptor.modes={0,10,4,16,9,17,4,4};
            descriptor.parameters[1][0]=20;descriptor.parameters[3][0]=32;
            descriptor.parameters[4][0]=36;descriptor.parameters[5][0]=40;
            descriptor.matrices[0]=48;descriptor.matrices[1]=52;descriptor.matrices[3]=56;
            descriptor.conversions[0]=64;descriptor.conversions[1]=66;
        }
        const auto encoded=encodeEngineVertexDescriptor(descriptor);
        for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
            uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
        for(unsigned vector=0;vector<256;++vector)for(unsigned lane=0;lane<4;++lane) {
            const auto bits=0x3E800000u+vector*1024+lane*16;
            for(unsigned byte=0;byte<4;++byte)binding.constantBytes[vector*16+lane*4+byte]=uint8_t(bits>>(24-byte*8));
        }
        WorldVertexOptions expectedOptions;WorldVertexConstants expectedConstants;
        require(prepareWorldVertexProgram(binding,expectedOptions,expectedConstants),"Capture benchmark descriptor rejected");
        const double prepare=median([&](unsigned) {
            WorldVertexOptions options;WorldVertexConstants constants;
            require(prepareWorldVertexProgram(binding,options,constants),"Capture benchmark preparation failed");
        },100000);
        for(const auto& fixture:cases) {
            std::memset(base+context+16896,0,160);base[context+16896+97]=8;
            if(fixture.fragment) {
                put(base,context+16896,program);put(base,program,5);put(base,program+4,name);
                put(base,program+16,fixture.vectors);std::memset(base+name,0,96);std::strcpy(reinterpret_cast<char*>(base+name),fixture.fragment);
            } else if(fixture.material==WorldMaterial::fixed) {
                put(base,context+16896+92,0x00100000);
                for(unsigned slot=0;slot<fixture.stages;++slot)base[context+16896+8+slot*2+1]=uint8_t(slot+1);
            }
            for(unsigned vector=0;vector<64;++vector)for(unsigned lane=0;lane<4;++lane)
                put(base,device+6016+vector*16+lane*4,0x3E000000+vector*1024+lane*16);
            const auto expectedMask=fixture.material==WorldMaterial::depth?0:worldFragmentTextureMask(
                fixture.fragment?fixture.fragment:fixture.stages?"MRenderXenon_Attrib_TexEnvMode02":"MRenderXenon_Attrib_TexEnvMode00",0);
            auto verify=[&](const std::shared_ptr<WorldDraw>& draw) {
                require(draw && draw->geometry.vertices==geometry.vertices && draw->geometry.indices==geometry.indices &&
                    draw->geometry.indexCount==3 && draw->material==fixture.material && draw->textureMask==expectedMask &&
                    draw->options==expectedOptions && draw->constants.vectors==expectedConstants.vectors &&
                    draw->constants.references==expectedConstants.references,"Capture benchmark changed geometry, descriptor or material");
                for(unsigned vector=0;vector<64;++vector)for(unsigned lane=0;lane<4;++lane) {
                    const uint32_t expected=vector<fixture.vectors?0x3E000000+vector*1024+lane*16:0;
                    require(std::bit_cast<uint32_t>(draw->fragmentConstants[vector][lane])==expected,"Capture benchmark changed fragment bank");
                }
                for(unsigned slot=0;slot<16;++slot)
                    require(draw->textureObjects[slot].object==((expectedMask&(1u<<slot))?objects+slot*64:0),"Capture benchmark changed completed textures");
            };
            auto first=captureWorldDraw(base,geometry);verify(first);
            const double capture=median([&](unsigned) {
                auto draw=captureWorldDraw(base,geometry);require(bool(draw),"Whole capture benchmark failed");
            },50000);
            verify(captureWorldDraw(base,geometry));verify(first);
            std::printf("%s,%s,%u,%u,%.6f,%.6f\n",rich?"multi":"simple",fixture.label,fixture.vectors,expectedMask,prepare,capture);
        }
    }
}
int main(int argc,char** argv) {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    try {
        require(argc==1 || (argc==2 && (std::string_view(argv[1])=="--threaded" || std::string_view(argv[1])=="--capture")),"Expected optional --threaded or --capture");
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
        enableEnginePreview();
        base[context+16896+97]=8;
        put(base,context+17160,1280);put(base,context+17164,720);put(base,context+17172,0x3F800000);
        EngineVertexBindingSnapshot binding;binding.deviceAddress=device;
        binding.descriptor.flags=0x01000000;binding.descriptor.modes.fill(4);
        const auto encoded=encodeEngineVertexDescriptor(binding.descriptor);
        for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
            uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
        if(argc==2 && std::string_view(argv[1])=="--capture") {
            auto draw=cache.draw(base,1,1,0,3,resources+128+56);draw.vertexBindings=binding;
            captureBenchmark(base,context,device,draw);VirtualFree(base,0,MEM_RELEASE);return 0;
        }
        std::puts("resources,stored_lookup_us,depth_submission_us_per_draw");
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
