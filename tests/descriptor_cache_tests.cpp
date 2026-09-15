#include "renderer/d3d11/descriptor_cache.h"
#include <d3d11.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace DarkRecomp;
static void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
struct CollisionHash {
    template<size_t N> size_t operator()(const DescriptorKey<N>&) const noexcept {return 0;}
};
template<class Descriptor,class Hash=DescriptorHash<sizeof(Descriptor)>>
static void distinctBytes() {
    DescriptorCache<Descriptor,unsigned,Hash> cache;
    Descriptor descriptor{};
    cache.entry(descriptor)=1;
    // Every byte matters, even bytes after embedded NULs or the first word.
    for(size_t byte=0;byte<sizeof(Descriptor);++byte) {
        std::memset(&descriptor,0,sizeof(descriptor));
        reinterpret_cast<unsigned char*>(&descriptor)[byte]=0x81;
        auto& value=cache.entry(descriptor);
        require(value==0,"Distinct descriptor bytes aliased an existing state");
        value=unsigned(byte)+2;
    }
    require(cache.size()==sizeof(Descriptor)+1,"Lost descriptor variants");
    for(size_t byte=sizeof(Descriptor);byte-->0;) {
        std::memset(&descriptor,0,sizeof(descriptor));
        reinterpret_cast<unsigned char*>(&descriptor)[byte]=0x81;
        require(cache.entry(descriptor)==byte+2,"Lookup lost a descriptor byte");
        require(cache.entry(descriptor)==byte+2,"Last-entry lookup changed the state");
    }
    std::memset(&descriptor,0,sizeof(descriptor));
    require(cache.entry(descriptor)==1,"Cache retained borrowed source storage");
    cache.clear();require(cache.size()==0 && cache.entry(descriptor)==0,"Clear retained an old entry");
}
static void ownershipAndRehash() {
    DescriptorCache<D3D11_SAMPLER_DESC,std::shared_ptr<unsigned>> cache;
    D3D11_SAMPLER_DESC descriptor{};
    auto& first=cache.entry(descriptor);first=std::make_shared<unsigned>(77);
    auto* stable=&first;std::weak_ptr<unsigned> weak=first;
    for(unsigned i=1;i<=1024;++i) {
        descriptor.MipLODBias=float(i);
        cache.entry(descriptor)=std::make_shared<unsigned>(i);
    }
    require(*(*stable)==77,"Map growth invalidated a retained value reference");
    require(*cache.entry(descriptor)==1024,"Map rehash invalidated the last-entry shortcut");
    descriptor.MipLODBias=0;
    require(&cache.entry(descriptor)==stable,"Lookup did not preserve entry identity");
    auto selected=first;
    cache.clear();require(!weak.expired() && *selected==77,"Clear invalidated a selected state");
    selected.reset();require(weak.expired(),"Cache leaked cleared state ownership");
    require(!cache.entry(descriptor),"Clear left a dangling shortcut");
}

struct StringHash {
    using is_transparent=void;
    size_t operator()(std::string_view key) const noexcept {return std::hash<std::string_view>{}(key);}
};
struct StringEqual {
    using is_transparent=void;
    bool operator()(std::string_view a,std::string_view b) const noexcept {return a==b;}
};
template<class Descriptor> struct PreviousCache {
    std::unordered_map<std::string,unsigned,StringHash,StringEqual> entries;
    unsigned& entry(const Descriptor& desc) {
        const std::string_view key(reinterpret_cast<const char*>(&desc),sizeof(desc));
        if(auto found=entries.find(key);found!=entries.end())return found->second;
        return entries.try_emplace(std::string(key)).first->second;
    }
};
template<class Cache,class Descriptor>
__declspec(noinline) static uint64_t lookup(Cache& cache,const std::vector<Descriptor>& descriptors,
                                          const std::vector<unsigned>& sequence) {
    uint64_t sum=0;
    for(auto index:sequence)sum+=cache.entry(descriptors[index]);
    return sum;
}
template<class Descriptor>
static void compare(const char* name,const std::vector<Descriptor>& descriptors) {
    PreviousCache<Descriptor> previous;DescriptorCache<Descriptor,unsigned> current;
    for(unsigned i=0;i<descriptors.size();++i)previous.entry(descriptors[i])=current.entry(descriptors[i])=i+1;
    for(const char* workload:{"repeated","material-runs","shuffled"}) {
        std::mt19937 rng(42);std::vector<unsigned> sequence(262144);unsigned selected=0;
        for(size_t i=0;i<sequence.size();++i) {
            if(workload==std::string_view("shuffled") || (workload==std::string_view("material-runs") && i%16==0))
                selected=rng()%unsigned(descriptors.size());
            sequence[i]=selected;
        }
        uint64_t expected=0;for(auto i:sequence)expected+=i+1;
        std::vector<double> before,after;
        auto measure=[&](auto& cache,auto& times) {
            const auto start=std::chrono::steady_clock::now();
            const auto sum=lookup(cache,descriptors,sequence);
            const auto end=std::chrono::steady_clock::now();
            require(sum==expected,"Benchmark cache returned the wrong state");
            times.push_back(std::chrono::duration<double,std::nano>(end-start).count()/sequence.size());
        };
        require(lookup(previous,descriptors,sequence)==expected && lookup(current,descriptors,sequence)==expected,
                "Benchmark warmup returned wrong states");
        for(unsigned repeat=0;repeat<7;++repeat) {
            if(repeat%2){measure(current,after);measure(previous,before);}
            else{measure(previous,before);measure(current,after);}
        }
        std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
        std::printf("DescriptorLookup type=%s bytes=%zu variants=%zu workload=%s oldNs=%.2f newNs=%.2f reduction=%.2f%% checksum=%llu\n",
            name,sizeof(Descriptor),descriptors.size(),workload,before[3],after[3],100*(1-after[3]/before[3]),expected);
    }
}
static void benchmark() {
    std::vector<D3D11_BLEND_DESC> blend(18);
    for(unsigned i=0;i<blend.size();++i) {
        auto& d=blend[i];auto& t=d.RenderTarget[0];t.RenderTargetWriteMask=15;t.BlendEnable=TRUE;
        t.SrcBlend=D3D11_BLEND(1+i%6);t.DestBlend=D3D11_BLEND(1+i/6);
        t.SrcBlendAlpha=D3D11_BLEND_ONE;t.DestBlendAlpha=D3D11_BLEND_ZERO;t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    }
    std::vector<D3D11_DEPTH_STENCIL_DESC> depth(26);
    for(unsigned i=0;i<depth.size();++i) {
        auto& d=depth[i];d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK(i%2);
        d.DepthFunc=D3D11_COMPARISON_FUNC(1+i%8);d.StencilEnable=i>=16;d.StencilReadMask=UINT8(i);
        d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};d.BackFace=d.FrontFace;
    }
    std::vector<D3D11_RASTERIZER_DESC> raster(6);
    for(unsigned i=0;i<raster.size();++i) {
        auto& d=raster[i];d.FillMode=D3D11_FILL_SOLID;d.CullMode=D3D11_CULL_MODE(1+i%3);
        d.FrontCounterClockwise=i/3;d.DepthClipEnable=TRUE;
    }
    std::vector<D3D11_SAMPLER_DESC> sampler(34);
    for(unsigned i=0;i<sampler.size();++i) {
        auto& d=sampler[i];d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxAnisotropy=1;
        d.ComparisonFunc=D3D11_COMPARISON_NEVER;d.MaxLOD=15;d.MipLODBias=float(i)/16;
    }
    compare("blend",blend);compare("depth",depth);compare("raster",raster);compare("sampler",sampler);
}
int main(int argc,char** argv) {
    try {
        distinctBytes<D3D11_BLEND_DESC>();distinctBytes<D3D11_DEPTH_STENCIL_DESC>();
        distinctBytes<D3D11_RASTERIZER_DESC>();distinctBytes<D3D11_SAMPLER_DESC>();
        distinctBytes<D3D11_BLEND_DESC,CollisionHash>();
        distinctBytes<std::array<unsigned char,13>>();
        ownershipAndRehash();
        std::puts("Descriptor cache: every byte, forced hash collisions, owned keys, stable rehash, clear and retained ownership passed.");
        if(argc>1 && std::string_view(argv[1])=="--benchmark")benchmark();
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}
}
