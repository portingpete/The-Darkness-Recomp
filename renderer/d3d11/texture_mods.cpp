#include "texture_mods.h"
#include "texture_mod_file_io.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/texture_mod_dds.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace DarkRecomp::TextureMods {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0)
            throw std::runtime_error("Texture SHA256 provider unavailable");
        if (BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm,0); algorithm = nullptr;
            throw std::runtime_error("Texture SHA256 initialization failed");
        }
    }
    ~Hash() { if(hash)BCryptDestroyHash(hash); if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0); }
    void add(std::span<const uint8_t> bytes) {
        if (bytes.size() > (std::numeric_limits<ULONG>::max)() ||
            BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0) < 0)
            throw std::runtime_error("Texture SHA256 update failed");
    }
    std::string finish() {
        std::array<uint8_t,32> bytes{};
        if(BCryptFinishHash(hash,bytes.data(),ULONG(bytes.size()),0) < 0)
            throw std::runtime_error("Texture SHA256 finish failed");
        constexpr char digits[] = "0123456789abcdef";
        std::string text; text.reserve(64);
        for(auto b:bytes) { text += digits[b>>4]; text += digits[b&15]; }
        return text;
    }
};
void put(std::span<uint8_t> bytes,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(value>>(i*8));
}
using Detail::transfer;
enum class ReadResult { missing, loaded, failed };
ReadResult readDds(const std::filesystem::path& path,HANDLE stop,size_t maximum,std::vector<uint8_t>& bytes) {
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!file) {
        const auto e=GetLastError();
        return e==ERROR_FILE_NOT_FOUND || e==ERROR_PATH_NOT_FOUND?ReadResult::missing:ReadResult::failed;
    }
    LARGE_INTEGER size{};
    if(GetFileType(file.value)!=FILE_TYPE_DISK || !GetFileSizeEx(file.value,&size) ||
        size.QuadPart<128 || uint64_t(size.QuadPart)>maximum)return ReadResult::failed;
    bytes.resize(size_t(size.QuadPart));
    if(!transfer(file.value,stop,false,bytes.data(),bytes.size()))return ReadResult::failed;
    LARGE_INTEGER after{};
    if(!GetFileSizeEx(file.value,&after) || after.QuadPart!=size.QuadPart)return ReadResult::failed;
    return ReadResult::loaded;
}
bool writeNew(const std::filesystem::path& path,HANDLE stop,std::vector<uint8_t>& bytes) {
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,
        FILE_FLAG_OVERLAPPED,nullptr));
    if(!file)return false; // Existing files, including partial prior output, stay intact.
    return transfer(file.value,stop,true,bytes.data(),bytes.size());
}
Replacement upload(ID3D11Device* device,const DdsImage& image) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=image.width; desc.Height=image.height; desc.MipLevels=UINT(image.levels.size());
    desc.ArraySize=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
    size_t bytes=0;
    for(unsigned mip=0;mip<image.levels.size();++mip) {
        const auto width=(std::max)(1u,image.width>>mip);
        initial.push_back({image.levels[mip].data(),width*4,UINT(image.levels[mip].size())});
        bytes+=image.levels[mip].size();
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Replacement result;
    if(FAILED(device->CreateTexture2D(&desc,initial.data(),&texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&result.view)))return {};
    result.width=image.width; result.height=image.height; result.mipLevels=desc.MipLevels; result.bytes=bytes;
    return result;
}
} // namespace

bool eligibleSource(const ColorImage& source) noexcept {
    return source.authoredMips && !source.firstMip && source.faces==1 && !source.promptOrigin &&
        !source.generatedSubtitleFont && source.valid();
}
std::string contentKey(const ColorImage& source) {
    if(!eligibleSource(source))throw std::invalid_argument("Texture source is not an owned authored2D base");
    Hash hash;
    static constexpr char domain[]="DarkRecomp.Texture.RGBA8.v1";
    hash.add({reinterpret_cast<const uint8_t*>(domain),sizeof(domain)}); // Includes terminal NUL.
    std::array<uint8_t,16> header{};
    put(header,0,source.width); put(header,4,source.height); put(header,8,source.sourceCodec);
    put(header,12,uint32_t(source.mips.size()+1)); hash.add(header);
    hash.add(source.pixels); for(const auto& mip:source.mips)hash.add(mip);
    return hash.finish();
}
std::vector<uint8_t> sourceDds(const ColorImage& source) {
    if(!eligibleSource(source))throw std::invalid_argument("Cannot dump this texture source");
    std::vector<std::vector<uint8_t>> levels; levels.push_back(source.pixels);
    levels.insert(levels.end(),source.mips.begin(),source.mips.end());
    const unsigned count=std::bit_width((std::max)(source.width,source.height));
    while(levels.size()<count) {
        const unsigned previous=unsigned(levels.size()-1);
        const unsigned sw=(std::max)(1u,source.width>>previous),sh=(std::max)(1u,source.height>>previous);
        const unsigned dw=(std::max)(1u,sw/2),dh=(std::max)(1u,sh/2);
        std::vector<uint8_t> next(size_t(dw)*dh*4);
        const auto& pixels=levels.back();
        for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x) {
            const unsigned x0=x*sw/dw,x1=(x+1)*sw/dw,y0=y*sh/dh,y1=(y+1)*sh/dh;
            const unsigned samples=(x1-x0)*(y1-y0); std::array<unsigned,4> sums{};
            for(unsigned sy=y0;sy<y1;++sy)for(unsigned sx=x0;sx<x1;++sx)
                for(unsigned c=0;c<4;++c)sums[c]+=pixels[(size_t(sy)*sw+sx)*4+c];
            for(unsigned c=0;c<4;++c)next[(size_t(y)*dw+x)*4+c]=uint8_t((sums[c]+samples/2)/samples);
        }
        levels.push_back(std::move(next));
    }
    std::vector<uint8_t> bytes(128,0);
    put(bytes,0,0x20534444); put(bytes,4,124); put(bytes,8,0x2100f);
    put(bytes,12,source.height); put(bytes,16,source.width); put(bytes,20,source.width*4);
    put(bytes,28,count); put(bytes,76,32); put(bytes,80,0x41); put(bytes,88,32);
    put(bytes,92,0xff); put(bytes,96,0xff00); put(bytes,100,0xff0000); put(bytes,104,0xff000000);
    put(bytes,108,count>1?0x401008:0x1000);
    for(const auto& mip:levels)bytes.insert(bytes.end(),mip.begin(),mip.end());
    return bytes;
}

struct Worker::Impl {
    enum class Phase { pending, absent, ready };
    struct Entry {
        std::weak_ptr<const ColorImage> owner;
        uint64_t ticket=0,use=0;
        Phase phase=Phase::pending;
        Replacement image;
    };
    struct Job { std::shared_ptr<const ColorImage> source; uint64_t ticket=0; size_t bytes=0; };
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Config config;
    Handle stopEvent;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::unordered_map<const ColorImage*,Entry> entries;
    std::deque<Job> jobs;
    std::vector<Replacement> retired,garbage;
    bool reclaim=false;
    uint64_t ticket=0,use=0;
    Stats counters;
    std::unordered_set<std::string> dumped;
    size_t dumpBudgetUsed=0;
    unsigned logs=0;

    Impl(ID3D11Device* d,const Config& c):device(d),config(c),stopEvent(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
        if(!device || (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) || !stopEvent)
            throw std::invalid_argument("Texture worker requires a thread-safe D3D11 device and stop event");
        if(!config.queueEntries || config.queueEntries>32 || !config.cacheEntries || config.cacheEntries>1024 ||
            config.queueEntries>config.cacheEntries || !config.sourceQueueBytes || config.sourceQueueBytes>128u*1024*1024 ||
            config.gpuBytes>ddsMaxBytes || config.dumpBytes>ddsMaxBytes)
            throw std::invalid_argument("Texture worker limits outside bounds");
        if(!config.packDirectory.empty()) {
            config.packDirectory=std::filesystem::absolute(config.packDirectory);
            if(!std::filesystem::is_directory(config.packDirectory))throw std::invalid_argument("Texture pack directory missing");
        }
        if(!config.dumpDirectory.empty()) {
            config.dumpDirectory=std::filesystem::absolute(config.dumpDirectory);
            if(!std::filesystem::create_directory(config.dumpDirectory))
                throw std::invalid_argument("Texture dump requires a new directory");
        }
        entries.reserve(config.cacheEntries);
        retired.reserve(size_t(config.cacheEntries)+1);
        garbage.reserve(size_t(config.cacheEntries)+1);
        thread=std::thread([this]{
            try {run();}
            catch(...) {
                stopping.store(true,std::memory_order_release);
                SetEvent(stopEvent.value); wake.notify_all();
                std::fprintf(stderr,"[TextureMods] worker stopped after an optional resource failure; retaining originals.\n");
            }
        });
    }
    ~Impl() {stop();}
    void stop() noexcept {
        stopping.store(true,std::memory_order_release); SetEvent(stopEvent.value); wake.notify_all();
        if(thread.joinable())thread.join();
        std::lock_guard lock(mutex);
        jobs.clear(); counters.pending=0; counters.queuedSourceBytes=0;
    }
    void retire(std::unordered_map<const ColorImage*,Entry>::iterator it) {
        if(it->second.image.view) {
            counters.gpuBytes-=it->second.image.bytes;
            retired.push_back(std::move(it->second.image));
        }
        entries.erase(it);
    }
    bool evict(const ColorImage* except=nullptr) {
        auto oldest=entries.end();
        for(auto it=entries.begin();it!=entries.end();++it)
            if(it->first!=except && it->second.phase!=Phase::pending &&
                (oldest==entries.end() || it->second.use<oldest->second.use))oldest=it;
        if(oldest==entries.end())return false;
        retire(oldest); return true;
    }
    Replacement request(const std::shared_ptr<const ColorImage>& source) {
        if(!source || stopping.load(std::memory_order_acquire))return {};
        std::lock_guard lock(mutex);
        if(stopping.load(std::memory_order_relaxed))return {};
        auto it=entries.find(source.get());
        if(it!=entries.end()) {
            if(it->second.owner.lock()==source) { it->second.use=++use; return it->second.image; }
            retire(it); wake.notify_one(); // A reused address never inherits the old generation.
        }
        if(!eligibleSource(*source))return {};
        const size_t bytes=source->bytes();
        if(entries.size()>=config.cacheEntries) { reclaim=true; wake.notify_one(); return {}; }
        if(counters.pending>=config.queueEntries || bytes>config.sourceQueueBytes ||
            counters.queuedSourceBytes>config.sourceQueueBytes-bytes || ticket==(std::numeric_limits<uint64_t>::max)())return {};
        const auto next=++ticket;
        // A draw's reference may own nothing (unowned_reference.h); the entry's
        // weak reference and the worker's job need the image's owner.
        const auto owner=ownerOf(source);
        entries.emplace(source.get(),Entry{owner,next,++use,Phase::pending,{}});
        try { jobs.push_back({owner,next,bytes}); }
        catch(...) {entries.erase(source.get()); throw;}
        ++counters.pending; counters.queuedSourceBytes+=bytes; wake.notify_one();
        return {};
    }
    Replacement process(const Job& job,bool& rejected) {
        const auto key=contentKey(*job.source);
        if(!config.dumpDirectory.empty() && !dumped.contains(key) && dumped.size()<config.cacheEntries) {
            dumped.insert(key);
            auto bytes=sourceDds(*job.source);
            if(bytes.size()<=config.dumpBytes && dumpBudgetUsed<=config.dumpBytes-bytes.size()) {
                dumpBudgetUsed+=bytes.size(); // Partial failed output also consumes the write budget.
                if(writeNew(config.dumpDirectory/(key+".dds"),stopEvent.value,bytes)) {
                    { std::lock_guard lock(mutex); counters.dumpedBytes+=bytes.size(); }
                    if(logs++<64)std::fprintf(stderr,"[TextureMods] dumped key=%s logical=%ux%u bytes=%zu\n",
                        key.c_str(),job.source->width,job.source->height,bytes.size());
                }
            }
        }
        if(config.packDirectory.empty() || stopping.load(std::memory_order_acquire))return {};
        std::vector<uint8_t> bytes;
        const auto read=readDds(config.packDirectory/(key+".dds"),stopEvent.value,
            (std::min)(ddsMaxBytes,config.gpuBytes)+148,bytes);
        if(read==ReadResult::missing)return {};
        if(read!=ReadResult::loaded) {rejected=true; return {};}
        DdsImage image;
        if(parseDds(bytes,image)) {rejected=true; return {};}
        size_t pixelBytes=0;
        for(const auto& level:image.levels)pixelBytes+=level.size();
        if(pixelBytes>config.gpuBytes) {rejected=true; return {};}
        const unsigned factor=image.width/job.source->width;
        if((factor!=1 && factor!=2 && factor!=4) || image.width!=job.source->width*factor ||
            image.height!=job.source->height*factor) {rejected=true; return {};}
        if(stopping.load(std::memory_order_acquire))return {};
        auto result=upload(device.Get(),image);
        if(!result.view) {rejected=true; return {};}
        if(logs++<64)std::fprintf(stderr,"[TextureMods] prepared key=%s logical=%ux%u physical=%ux%u mips=%u bytes=%zu\n",
            key.c_str(),job.source->width,job.source->height,result.width,result.height,result.mipLevels,result.bytes);
        return result;
    }
    void run() {
        for(;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return stopping.load(std::memory_order_acquire)||reclaim||!retired.empty()||!jobs.empty();});
                if(stopping.load(std::memory_order_relaxed))break;
                if(reclaim) {evict(); reclaim=false;}
                garbage.swap(retired);
                if(!jobs.empty()) {job=std::move(jobs.front()); jobs.pop_front();}
            }
            garbage.clear(); // GPU release is on this worker, outside the queue mutex.
            if(!job.source)continue;
            bool rejected=false; Replacement result;
            try {result=process(job,rejected);}
            catch(const std::exception& e) {rejected=true; if(logs++<64)std::fprintf(stderr,"[TextureMods] optional texture rejected: %s\n",e.what());}
            catch(...) {rejected=true;}
            {
                std::lock_guard lock(mutex);
                --counters.pending; counters.queuedSourceBytes-=job.bytes; ++counters.finished;
                if(rejected)++counters.rejected;
                auto it=entries.find(job.source.get());
                if(it!=entries.end() && it->second.ticket==job.ticket && it->second.owner.lock()==job.source) {
                    while(result.view && counters.gpuBytes+result.bytes>config.gpuBytes && evict(job.source.get())) {}
                    if(result.view && result.bytes<=config.gpuBytes && counters.gpuBytes<=config.gpuBytes-result.bytes &&
                        !stopping.load(std::memory_order_relaxed)) {
                        counters.gpuBytes+=result.bytes; ++counters.ready;
                        it->second.image=std::move(result); it->second.phase=Phase::ready;
                    } else it->second.phase=Phase::absent;
                }
            }
            wake.notify_one();
        }
        { std::lock_guard lock(mutex);
            jobs.clear(); garbage.swap(retired);
            counters.pending=0; counters.queuedSourceBytes=0;
            for(auto& [key,entry]:entries)if(entry.phase==Phase::pending)entry.phase=Phase::absent;
        }
        garbage.clear();
    }
    Stats stats() const {
        std::lock_guard lock(mutex); auto result=counters; result.entries=unsigned(entries.size()); return result;
    }
};
Worker::Worker(ID3D11Device* device,const Config& config):impl_(std::make_unique<Impl>(device,config)) {}
Worker::~Worker()=default;
Replacement Worker::request(const std::shared_ptr<const ColorImage>& source) noexcept {
    try {return impl_->request(source);}catch(...) {return {};}
}
Stats Worker::stats() const {return impl_->stats();}
void Worker::stop() noexcept {if(impl_)impl_->stop();}
std::unique_ptr<Worker> makeWorker(ID3D11Device* device,const Config& config) {
    if(config.packDirectory.empty() && config.dumpDirectory.empty())return {};
    return std::make_unique<Worker>(device,config);
}
} // namespace DarkRecomp::TextureMods
