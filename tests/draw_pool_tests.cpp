#include "renderer/engine/world_mesh.h"
#include "renderer/engine/native_layer.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace DarkRecomp::Native;
using DarkRecomp::ColorImage;
static void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
static void put(uint8_t* base,uint32_t address,uint32_t value) {
    for(unsigned i=0;i<4;++i)base[address+i]=uint8_t(value>>(24-i*8));
}
struct HeldDraw {
    std::shared_ptr<WorldDraw> draw;
    std::weak_ptr<const StoredGeometry> geometry;
    std::weak_ptr<const ColorImage> image;
    unsigned id=0;
};
// Release a captured draw after main and TLS cleanup, while static objects
// are being destroyed. The recycler must still own its synchronization/storage.
struct ExitCheck {
    std::shared_ptr<WorldDraw> draw;
    std::weak_ptr<const StoredGeometry> geometry;
    ~ExitCheck() {
        draw.reset();
        if(!geometry.expired()) {std::fputs("Draw pool pinned geometry at static teardown\n",stderr);std::abort();}
    }
} exitCheck;
static void drawTransportContract(const EngineVertexBindingSnapshot& binding) {
    auto geometry=std::make_shared<StoredGeometry>();geometry->id=0xD123;
    StoredDraw rich{geometry,geometry,7,21};rich.transforms.emplace();rich.vertexBindings=binding;
    rich.transforms->matrixAddress=0xABCD;rich.transforms->constants.vectors[7][3]=-17;
    for(size_t i=0;i<rich.vertexBindings->constantBytes.size();++i)rich.vertexBindings->constantBytes[i]=uint8_t(i*73+19);
    auto check=[&](const StoredDraw& draw) {
        require(draw.vertices==geometry && draw.indices==geometry && draw.firstIndex==7 && draw.indexCount==21 &&
                draw.transforms && draw.transforms->matrixAddress==0xABCD && draw.transforms->constants.vectors[7][3]==-17 &&
                draw.vertexBindings==rich.vertexBindings,"Draw transport lost an engaged owned snapshot");
    };
    StoredDraw copied(rich),assigned;assigned=rich;check(copied);check(assigned);
    StoredDraw moved(std::move(copied));check(moved);
    require(!copied.vertices && !copied.indices && copied.vertexBindings==rich.vertexBindings,
            "Draw move changed snapshot value or retained geometry");
    assigned=std::move(moved);check(assigned);
    assigned=assigned;check(assigned);assigned=std::move(assigned);check(assigned);

    // Protect an entire page inside an EMPTY optional's inactive snapshot. None
    // of the transport operations may inspect that storage. This catches the
    // former full optional memcpy while exercising ordinary value semantics.
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    StoredDraw layout;layout.vertexBindings.emplace();
    const auto bankOffset=reinterpret_cast<const uint8_t*>(layout.vertexBindings->constantBytes.data())-
                          reinterpret_cast<const uint8_t*>(&layout);
    const auto snapshotOffset=reinterpret_cast<const uint8_t*>(&*layout.vertexBindings)-
                              reinterpret_cast<const uint8_t*>(&layout);
    // The byte bank need not share StoredDraw's alignment. Include a few
    // inactive descriptor bytes so the placement-new object stays aligned.
    const auto offset=bankOffset-bankOffset%alignof(StoredDraw);
    require(page==4096 && offset>=snapshotOffset && offset+page<=bankOffset+4096 &&
            offset<page && sizeof(StoredDraw)+page-offset<=page*3,"Unexpected guarded draw layout");
    auto* allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,page*3,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    require(allocation!=nullptr,"Allocate guarded draw transport fixture");
    auto* source=new(allocation+page-offset) StoredDraw(geometry,geometry,3,9);
    DWORD previous=0;
    require(VirtualProtect(allocation+page,page,PAGE_NOACCESS,&previous)!=0,"Protect inactive optional bank");
    StoredDraw emptyCopy(*source),emptyMove(std::move(*source));
    source->vertices=geometry;source->indices=geometry;
    assigned=*source;
    require(assigned && assigned.firstIndex==3 && assigned.indexCount==9 && !assigned.vertexBindings && !assigned.transforms,
            "Empty source did not clear an engaged destination");
    assigned=std::move(*source);
    const bool valid=assigned && emptyCopy && emptyMove && !source->vertices && !source->indices &&
                     !emptyCopy.vertexBindings && !emptyMove.vertexBindings && !assigned.vertexBindings;
    DWORD ignored=0;const bool restored=VirtualProtect(allocation+page,page,previous,&ignored)!=0;
    source->~StoredDraw();VirtualFree(allocation,0,MEM_RELEASE);
    require(valid && restored,"Empty draw transport changed ownership or snapshots");
}
static void guardedFaultPoolContract(uint8_t* base,const EngineVertexBindingSnapshot& templateBinding) {
    // This runs before any other draw capture on this thread. The only plan
    // warmed below is the clean descriptor, so the different fault descriptor
    // must take the plan-cache miss/build path.
    constexpr uint32_t device=0x01000400,bank=device+1920,guard=0x01001000;
    constexpr unsigned dirtyRow=30,faultRow=72;
    static_assert(bank+faultRow*16==guard);
    SYSTEM_INFO system{};GetSystemInfo(&system);
    require(system.dwPageSize==4096,"Unexpected guest page size in guarded pool fixture");

    auto clean=templateBinding;
    clean.deviceAddress=device;
    auto fault=clean;
    fault.descriptor.color=dirtyRow;
    fault.descriptor.modes[0]=1;
    fault.descriptor.parameters[0][0]=faultRow;
    const auto encoded=encodeEngineVertexDescriptor(fault.descriptor);
    for(unsigned i=0;i<5;++i)fault.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
        uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
    // Color row 30 is copied before mode 1 reads the inaccessible row 72.
    put(base,bank+dirtyRow*16,0x41540000); // 13.25f
    auto geometry=std::make_shared<StoredGeometry>();
    // Validate the fault descriptor while all pages are readable, on another
    // thread so this does not populate the test thread's plan cache.
    std::exception_ptr controlError;
    {
        std::jthread control([&] {
            try {
                StoredDraw input{geometry,geometry,0,3};
                auto draw=captureWorldDrawFast(base,fault,input);
                require(draw && draw->constants.vectors[dirtyRow][0]==13.25f,
                        "Readable fault descriptor did not copy its color constant");
            } catch(...) {controlError=std::current_exception();}
        });
    }
    if(controlError)std::rethrow_exception(controlError);
    StoredDraw freshInput{geometry,geometry,0,3};freshInput.vertexBindings=clean;
    WorldDraw fresh;
    require(snapshotWorldDraw(base,freshInput,fresh),"Fresh guarded-pool comparison capture failed");
    require(fresh.constants.vectors[dirtyRow]==EngineVector{},"Fresh capture unexpectedly uses dirty row");

    StoredDraw seedInput{geometry,geometry,0,3};
    auto seed=captureWorldDrawFast(base,clean,seedInput);
    require(bool(seed),"Cannot seed guarded-pool fixture");
    const auto* pooledAddress=seed.get();
    seed.reset();

    struct GuardPage {
        uint8_t* address;size_t size;DWORD previous=0;
        GuardPage(uint8_t* p,size_t n):address(p),size(n) {
            require(VirtualProtect(address,size,PAGE_NOACCESS,&previous)!=0,"Cannot protect guest constant page");
        }
        ~GuardPage() {DWORD ignored=0;VirtualProtect(address,size,previous,&ignored);}
    };
    StoredDraw faultInput{geometry,geometry,0,3};
    {
        GuardPage protectedPage(base+guard,system.dwPageSize);
        auto rejected=captureWorldDrawFast(base,fault,faultInput);
        require(!rejected && faultInput.vertices==geometry && faultInput.indices==geometry,
                "Guarded fast capture did not reject without consuming geometry");
    }

    StoredDraw reuseInput{geometry,geometry,0,3};
    auto reused=captureWorldDrawFast(base,clean,reuseInput);
    require(bool(reused) && reused.get()==pooledAddress,"Faulted draw did not return to the expected pool slot");
    require(reused->constants.vectors[dirtyRow]==fresh.constants.vectors[dirtyRow],
            "Faulted fast capture left a stale constant in the recycled draw");

    put(base,bank+dirtyRow*16,0);
}
int main() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    try {
        require(base!=nullptr,"Reserve private guest space");
        require(VirtualAlloc(base+0x82A60000,0x10000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Commit context");
        require(VirtualAlloc(base+0x01000000,0x10000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Commit device");
        constexpr uint32_t context=0x82A69B00;
        base[context+16896+97]=8;
        put(base,context+17160,1280);put(base,context+17164,720);put(base,context+17172,0x3F800000);
        EngineVertexBindingSnapshot binding;
        binding.deviceAddress=0x01000000;binding.descriptor.flags=0x01000000;binding.descriptor.modes.fill(4);
        const auto encoded=encodeEngineVertexDescriptor(binding.descriptor);
        for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
            uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
        drawTransportContract(binding);
        guardedFaultPoolContract(base,binding);
        {
            auto geometry=std::make_shared<StoredGeometry>();
            StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
            for(unsigned pass=0;pass<3;++pass) {
                auto draw=captureWorldDraw(base,input);
                require(bool(draw),"Cannot capture unused-texture pool fixture");
                for(const auto& texture:draw->textureObjects)
                    require(texture.faces==WorldTexture{}.faces && texture.mipLevels==WorldTexture{}.mipLevels,
                            "Recycled draw changed unused texture defaults");
            }
        }
        {
            auto vertices=std::make_shared<StoredGeometry>();
            auto indices=std::make_shared<StoredGeometry>();
            vertices->id=41;indices->id=42;
            StoredDraw input{vertices,indices,1,2};input.vertexBindings=binding;
            auto copied=captureWorldDraw(base,input);
            require(copied && input.vertices==vertices && input.indices==indices &&
                    copied->geometry.vertices==vertices && copied->geometry.indices==indices &&
                    input.firstIndex==1 && input.indexCount==2 && input.vertexBindings->key==binding.key,
                    "Const capture consumed or changed its input");
            // Reject after vertex preparation and guest state capture, before
            // publishing geometry. Even a late failure must preserve ownership.
            put(base,context+17160,0);
            auto rejected=captureWorldDraw(base,std::move(input));
            put(base,context+17160,1280);
            require(!rejected && input.vertices==vertices && input.indices==indices &&
                    input.firstIndex==1 && input.indexCount==2 && input.vertexBindings->key==binding.key,
                    "Rejected owned capture consumed or changed its input");
            const std::weak_ptr<const StoredGeometry> weakVertices=vertices,weakIndices=indices;
            auto transferred=captureWorldDraw(base,std::move(input));
            require(transferred && !input.vertices && !input.indices &&
                    transferred->geometry.vertices==vertices && transferred->geometry.indices==indices &&
                    transferred->geometry.firstIndex==1 && transferred->geometry.indexCount==2 &&
                    input.firstIndex==1 && input.indexCount==2 && input.vertexBindings->key==binding.key,
                    "Successful owned capture did not transfer exactly its geometry references");
            copied.reset();vertices.reset();indices.reset();
            require(!weakVertices.expired() && !weakIndices.expired(),"Transferred draw lost its geometry");
            transferred.reset();
            require(weakVertices.expired() && weakIndices.expired(),"Released transferred draw pinned its geometry");
        }
        {
            enableEnginePreview();
            auto geometry=std::make_shared<StoredGeometry>();
            geometry->vertexCount=3;geometry->vertices.resize(36);geometry->indices={0,1,2};
            StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
            put(base,context+17160,0);
            previewObserveWorld(base,std::move(input));
            put(base,context+17160,1280);
            require(input.vertices==geometry && input.indices==geometry,"Rejected owned preview consumed its input");
            const std::weak_ptr<const StoredGeometry> weak=geometry;
            previewObserveWorld(base,std::move(input));
            require(!input.vertices && !input.indices,"Owned preview did not transfer its geometry");
            geometry.reset();previewEndFrame();
            std::vector<DarkRecomp::SimpleMesh> frame;
            require(takePreviewFrame(frame) && frame.size()==1 && frame[0].world &&
                    frame[0].world->geometry.indexCount==3 && !weak.expired(),
                    "Queued transferred draw lost its geometry");
            frame.clear();
            require(weak.expired(),"Preview accounting pinned transferred geometry after release");
        }
        const auto beforeRounds=worldCaptureCounters();
        for(unsigned round=0;round<3;++round) {
            std::array<std::vector<HeldDraw>,8> held;
            std::array<std::exception_ptr,8> errors{};
            std::vector<std::jthread> producers;
            for(unsigned thread=0;thread<held.size();++thread)producers.emplace_back([&,thread] {
                try {
                    // More live snapshots than the pool can retain, then
                    // reuse them with new threads after the producers exit.
                    for(unsigned i=0;i<600;++i) {
                        auto geometry=std::make_shared<StoredGeometry>();
                        geometry->id=1+round*4800+thread*600+i;geometry->vertexCount=3;geometry->stride=12;
                        geometry->formats[0]=3;geometry->vertices.resize(36);geometry->indices={0,1,2};
                        StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
                        if(i%31==0) {
                            input.vertexBindings->deviceAddress=0;
                            require(!captureWorldDraw(base,std::move(input)) && input.vertices==geometry &&
                                    input.indices==geometry,"Invalid owned capture consumed its input or was published");
                            input.vertexBindings=binding;
                        }
                        auto draw=captureWorldDraw(base,std::move(input));
                        require(draw && !input.vertices && !input.indices &&
                                draw->geometry.vertices==geometry && draw->geometry.indices==geometry,
                                "Capture changed geometry ownership");
                        require(draw->constants.vectors[255][0]==0 && draw->fragmentConstants[15][0]==0 &&
                                !draw->textureObjects[15].object && !draw->textureIds[15] && !draw->samplers[15].lodValid &&
                                !draw->textures[15] && !draw->fragmentFlags && draw->fragmentName.empty(),
                                "Recycled draw inherited an unused bank or texture");
                        auto image=std::make_shared<ColorImage>();image->width=image->height=1;
                        image->pixels={uint8_t(geometry->id),0,0,255};draw->textures[15]=image;
                        // Poison banks that this depth fixture will not fill.
                        draw->constants.vectors[255][0]=91;draw->fragmentConstants[15][0]=92;
                        draw->textureObjects[15].object=93;draw->textureIds[15]=94;draw->samplers[15].lodValid=true;
                        draw->fragmentFlags=95;draw->fragmentName="retained draw must not be recycled early";
                        held[thread].push_back({std::move(draw),geometry,image,geometry->id});
                    }
                } catch(...) {errors[thread]=std::current_exception();}
            });
            producers.clear(); // Join all producers before validating their retained results.
            for(const auto& error:errors)if(error)std::rethrow_exception(error);
            for(auto& batch:held)for(auto& item:batch) {
                require(item.draw->geometry.vertices->id==item.id && item.draw->textures[15]->pixels[0]==uint8_t(item.id) &&
                        item.draw->constants.vectors[255][0]==91 && item.draw->fragmentConstants[15][0]==92 &&
                        item.draw->textureObjects[15].object==93 && item.draw->fragmentFlags==95 &&
                        item.draw->fragmentName=="retained draw must not be recycled early",
                        "Retained draw changed after its producing thread exited");
                item.draw.reset();
                require(item.geometry.expired() && item.image.expired(),"Pool pinned an image or geometry after draw release");
            }
        }
        const auto afterRounds=worldCaptureCounters();
        require(afterRounds.observed-beforeRounds.observed==14880 &&
                afterRounds.captured-beforeRounds.captured==14400 &&
                afterRounds.rejected-beforeRounds.rejected==480,
                "Draw statistics lost counts after producer thread exit");
        {
            // Exhaust the bounded private lanes and race multiple writers on
            // the fallback lane. Final totals must remain exact after join.
            const auto before=worldCaptureCounters();
            std::vector<std::jthread> writers;
            for(unsigned writer=0;writer<80;++writer)writers.emplace_back([&,writer] {
                auto geometry=std::make_shared<StoredGeometry>();
                StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
                input.vertexBindings->deviceAddress=0;
                for(unsigned i=0;i<32;++i) {
                    if(captureWorldDraw(base,input))std::abort();
                    recordWorldSubmission(WorldMaterial(writer%5));
                }
            });
            writers.clear();
            const auto after=worldCaptureCounters();
            require(after.observed-before.observed==2560 && after.rejected-before.rejected==2560 &&
                    after.captured==before.captured,"Shared fallback lane lost capture counts");
            for(unsigned material=0;material<5;++material)
                require(after.submitted[material]-before.submitted[material]==512,"Shared fallback lane lost submission counts");
        }
        {
            auto geometry=std::make_shared<StoredGeometry>();
            StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
            exitCheck.draw=captureWorldDraw(base,input);exitCheck.geometry=geometry;
            require(bool(exitCheck.draw),"Capture retained for static teardown");
        }
        VirtualFree(base,0,MEM_RELEASE);
        std::puts("DrawPoolContract passed: 14,400 retained draws, 104 producer lifetimes, exact private/fallback counters, overflow, failed captures, unused bank reset and prompt resource release.");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"DrawPoolContract failed: %s\n",error.what());
        if(base)VirtualFree(base,0,MEM_RELEASE);return 1;
    }
}
