#include "world_mesh.h"
#include "engine_performance.h"
#include "scene_work.h"
#include "engine_world_fragment_bindings.generated.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <intrin.h>
#include <tmmintrin.h>
#include <mutex>
#include <vector>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00;
constexpr std::array<uint8_t,27> sizes{0,4,8,12,16,2,4,6,8,2,4,6,8,6,4,4,4,4,4,4,2,4,8,2,4,6,8};
std::atomic<uint64_t> observed{}, captured{}, rejected{}, submitted[5]{};
std::atomic<uint64_t> immediateCanonicalRecovered{}, immediateCanonicalRefused{};
std::atomic<uint64_t> immediateCanonicalRecoveredTex1371{}, immediateCanonicalRecoveredTex1477{};
std::atomic<unsigned> immediateCanonicalLog1371{}, immediateCanonicalLog1477{};
uint32_t word(const uint8_t* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
uint16_t half(const uint8_t* p) { return uint16_t(uint16_t(p[0])<<8 | p[1]); }
bool finite(const EngineVector& v) { return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);}); }
bool supportedMode(uint8_t m) { return m==0 || m==1 || m==4 || m==7 || m==8 || m==9 || m==10 || m==13 || m==16 || m==17 || m==18 || m==20 || m==22; }
__attribute__((target("ssse3"))) bool copyFiniteVectorsSimd(const uint8_t* source,EngineVector* output,unsigned count) {
    const auto endian=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    const auto absolute=_mm_set1_epi32(0x7FFFFFFF),maximum=_mm_set1_epi32(0x7F7FFFFF);
    for(unsigned i=0;i<count;++i) {
        const auto v=_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source+i*16)),endian);
        if(_mm_movemask_epi8(_mm_cmpgt_epi32(_mm_and_si128(v,absolute),maximum)))return false;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output[i].data()),v);
    }
    return true;
}
bool copyFiniteVectors(const uint8_t* source,EngineVector* output,unsigned count) {
    static const bool ssse3=[] {int cpu[4]{};__cpuid(cpu,1);return (cpu[2]&(1<<9))!=0;}();
    if(ssse3)return copyFiniteVectorsSimd(source,output,count);
    for(unsigned i=0;i<count;++i) {
        for(unsigned lane=0;lane<4;++lane)output[i][lane]=std::bit_cast<float>(word(source+i*16+lane*4));
        if(!finite(output[i]))return false;
    }
    return true;
}
// Reuse draw objects and their fragment-name capacity. Shared_ptr control
// blocks still use their normal allocator. Small per-thread reserves transfer
// empty objects in batches, avoiding a shared lock on every capture/release.
// Resource references and partial banks are cleared before recycling. The
// releasing thread prepares the next capture without extending engine work.
struct alignas(64) LocalWorldDrawPool {
    std::array<WorldDraw*,32> draws{};
    size_t size=0;
    WorldDraw* take();
    void put(WorldDraw* draw);
};
struct WorldDrawPool {
    std::mutex mutex;
    std::array<WorldDraw*,4096> draws{};
    size_t size=0;
    // A fixed process-wide bound, including after short-lived threads exit.
    // The first eight threads each own one reserve permanently; additional
    // threads use the shared pool directly. No TLS destructor owns draw data.
    std::array<LocalWorldDrawPool,8> reserves;
    std::atomic<unsigned> assigned{0};
};
WorldDrawPool& sharedWorldDrawPool() {
    // Process lifetime: queued shared_ptrs may retire during static teardown
    // in another translation unit. The shared pool must remain available.
    static auto* pool=new WorldDrawPool;
    return *pool;
}
WorldDraw* LocalWorldDrawPool::take() {
    if(!size) {
        auto& shared=sharedWorldDrawPool();
        std::lock_guard lock(shared.mutex);
        while(size<draws.size() && shared.size)draws[size++]=shared.draws[--shared.size];
    }
    return size?draws[--size]:nullptr;
}
void LocalWorldDrawPool::put(WorldDraw* draw) {
    if(size==draws.size()) {
        auto& shared=sharedWorldDrawPool();
        std::array<WorldDraw*,32> excess{};size_t excessSize=0;
        {
            std::lock_guard lock(shared.mutex);
            while(size) {
                auto* item=draws[--size];
                if(shared.size<shared.draws.size())shared.draws[shared.size++]=item;
                else excess[excessSize++]=item;
            }
        }
        // Destruction must not hold the shared pool lock.
        while(excessSize)delete excess[--excessSize];
    }
    draws[size++]=draw;
}
LocalWorldDrawPool* localWorldDrawPool() {
    thread_local auto* reserve=[]() -> LocalWorldDrawPool* {
        auto& shared=sharedWorldDrawPool();
        auto next=shared.assigned.load(std::memory_order_relaxed);
        while(next<shared.reserves.size()) {
            if(shared.assigned.compare_exchange_weak(next,next+1,std::memory_order_relaxed))
                return &shared.reserves[next];
        }
        return nullptr;
    }();
    return reserve;
}
void resetPartialWorldDraw(WorldDraw* raw) noexcept {
    // Fully overwritten fields need no clearing. Reset partial banks before
    // publishing an empty object to the pool, after its last owner retires.
    // Normal queued draws retire on the rendering thread; rejected captures
    // follow the same reset path on their producer.
    std::memset(raw->constants.vectors.data(), 0, sizeof(raw->constants.vectors));
    std::memset(raw->fragmentConstants.data(), 0, sizeof(raw->fragmentConstants));
    std::memset(raw->textureObjects.data(), 0, sizeof(raw->textureObjects));
    raw->textureIds.fill(0);
    raw->samplers.fill(WorldSampler{});
    raw->material = WorldMaterial::depth;
    raw->fragmentFlags = 0;
}
void recycleWorldDraw(WorldDraw* raw) noexcept {
    if(!raw) return;
    try {
        // Release shared ownership promptly so textures/geometry can evict.
        // String clear retains capacity for the next fragment name.
        raw->fragmentName.clear();
        raw->geometry.vertices.reset();
        raw->geometry.indices.reset();
        for(auto& texture : raw->textures) texture.reset();
        resetPartialWorldDraw(raw);
        if(auto* local=localWorldDrawPool()) {local->put(raw);return;}
        auto& shared=sharedWorldDrawPool();
        std::lock_guard lock(shared.mutex);
        if(shared.size<shared.draws.size()) {shared.draws[shared.size++]=raw;return;}
    } catch (...) {}
    delete raw;
}
std::shared_ptr<WorldDraw> acquireWorldDraw() {
    WorldDraw* raw = nullptr;
    if(auto* local=localWorldDrawPool())raw=local->take();
    else {
        auto& shared=sharedWorldDrawPool();
        std::lock_guard lock(shared.mutex);
        if(shared.size)raw=shared.draws[--shared.size];
    }
    if(!raw) {
        raw = new WorldDraw();
        resetPartialWorldDraw(raw);
    }
    return std::shared_ptr<WorldDraw>(raw, recycleWorldDraw);
}
// Task-59 private failure detail: mirror decodeWorldVertices' format walk to
// locate the first nonfinite lane without changing the decoder itself. Only
// raw-float lanes (formats 1..4) can hold nonfinite bits: halves and packed
// fields convert to finite floats by construction, and this decoder applies
// no scale/offset. Runs only on a diagnosed capture failure; never on normal
// paths. badLane 0xFF means no nonfinite lane located, 0xFE an invalid
// format/stride walk.
bool locateImmediateNonfinite(const StoredGeometry& g,ImmediateCaptureReason& out) noexcept {
    try {
        out.badVertex=UINT32_MAX;out.badSlot=0;out.badLane=0xFF;
        if(!g.vertexCount || g.vertexCount>65535 || g.vertices.size()!=uint64_t(g.vertexCount)*g.stride)
            {out.badLane=0xFE;return true;}
        std::array<uint32_t,16> offsets{};uint32_t stride=0;
        for(unsigned s=0;s<16;++s) {
            const auto f=g.formats[s];
            if(f>=sizes.size() || (f && !(f<=4 || (f>=9 && f<=12) || (f>=14 && f<=19))))
                {out.badSlot=s;out.badLane=0xFE;return true;}
            offsets[s]=stride;stride+=sizes[f];
        }
        if(stride!=g.stride || !g.formats[0]) {out.badLane=0xFE;return true;}
        for(uint32_t i=0;i<g.vertexCount;++i)
            for(unsigned s=0;s<16;++s) {
                const auto f=g.formats[s];
                if(!f || f>4)continue;
                const uint8_t* p=g.vertices.data()+size_t(i)*stride+offsets[s];
                for(unsigned n=0;n<f;++n) {
                    const uint32_t bits=word(p+n*4);
                    if(!std::isfinite(std::bit_cast<float>(bits))) {
                        out.badVertex=i;out.badSlot=s;out.badLane=n;
                        out.rawBE=bits;out.rawSize=4;return true;
                    }
                }
            }
        return false;
    } catch (...) { return false; }
}
// Task-62b recovery: NaN/Inf allocator leftovers in immediate vertices that no
// index references must not reject visible triangles (all-zero bytes decode
// finite in every format). Zeroes every unreferenced vertex — tail and
// interior holes alike — in a still-unpublished owned copy, preserving live
// bytes; indices are untouched, so order, winding and duplicates survive.
// Takes the SAME owned index snapshot that the caller validated OOB-free and
// will publish, so canonicalization and submission cannot disagree; OOB
// values are ignored for marking and must already have rejected before this
// runs. Optionally fills the task-59 membership fields from that same
// snapshot. Never touches cached or stored snapshots: sole ownership is
// verified, and the intern cache below compares full bytes on hit, so cache
// equality (not hash difference) is the actual stale-reuse protection.
// Returns false when the copy is shared. Failure path only; the successful
// hot path performs no scan, hash, allocation or extra guest read.
bool canonicalizeUnreferencedVertices(const std::shared_ptr<StoredGeometry>& owned,
    const uint16_t* snapIndices,uint32_t count,ImmediateCaptureReason* reason) noexcept {
    try {
        if(!owned || owned.use_count()!=1 || !snapIndices || !count || count%3 || count>49152 ||
            !owned->vertexCount || owned->vertexCount>65535 || !owned->stride ||
            uint64_t(owned->vertexCount)*owned->stride!=owned->vertices.size())return false;
        if(reason)reason->membershipKnown=true;
        std::vector<uint8_t> referenced(owned->vertexCount,0);
        for(uint32_t pos=0;pos<count;++pos) {
            const uint16_t v=snapIndices[pos];
            if(v<owned->vertexCount)referenced[v]=1;
            if(reason && reason->badVertex!=UINT32_MAX && v==reason->badVertex && !reason->referenced) {
                reason->referenced=true;reason->firstRefPos=pos;
            }
        }
        for(uint32_t v=0;v<owned->vertexCount;++v) {
            if(referenced[v])continue;
            std::memset(owned->vertices.data()+uint64_t(v)*owned->stride,0,owned->stride);
        }
        return true;
    } catch (...) { return false; }
}
// Immediate quads and index lists repeat across passes and frames. Intern only
// complete owned bytes, never a guest pointer whose contents can change. The
// direct-mapped cache is bounded to 512 * 16 KiB per producer thread.
std::shared_ptr<const StoredGeometry> retainImmediate(std::shared_ptr<StoredGeometry> value,ImmediateCaptureReason* reason=nullptr) {
    thread_local std::array<std::shared_ptr<const StoredGeometry>,512> cache;
    uint64_t hash=14695981039346656037ull;
    auto bytes=[&](const void* raw,size_t size) {
        const auto* p=static_cast<const uint8_t*>(raw);
        while(size>=8) {uint64_t word;std::memcpy(&word,p,8);hash=(hash^word)*1099511628211ull;p+=8;size-=8;}
        while(size--)hash=(hash^*p++)*1099511628211ull;
    };
    const bool small=value->bytes()<=16384;
    std::shared_ptr<const StoredGeometry>* slot=nullptr;
    if(small) {
        bytes(&value->vertexCount,sizeof(value->vertexCount));bytes(&value->stride,sizeof(value->stride));
        bytes(value->formats.data(),value->formats.size());
        bytes(value->vertices.data(),value->vertices.size());bytes(value->indices.data(),value->indices.size()*2);
        slot=&cache[hash%cache.size()];
        if(*slot && (*slot)->vertexCount==value->vertexCount && (*slot)->stride==value->stride &&
           (*slot)->formats==value->formats && (*slot)->vertices==value->vertices && (*slot)->indices==value->indices)
            return *slot;
    }
    if(value->vertexCount) {
        if(!validateWorldVertices(*value)) {
            if(reason) {
                reason->stage=ImmediateCaptureReason::Stage::vertexDecode;
                reason->stride=value->stride;reason->vertexCount=value->vertexCount;
                reason->formats=value->formats;
                locateImmediateNonfinite(*value,*reason);
                if(reason->badVertex!=UINT32_MAX && value->stride &&
                    value->stride<=reason->vertexBytes.size()) {
                    const uint64_t offset=uint64_t(reason->badVertex)*value->stride;
                    if(offset+value->stride<=value->vertices.size()) {
                        reason->vertexByteCount=value->stride;
                        std::memcpy(reason->vertexBytes.data(),value->vertices.data()+offset,value->stride);
                    }
                }
            }
            return {};
        }
    }
    if(slot)*slot=value;
    return value;
}
}

namespace {
// Shared by validation and expansion so supported formats, extents and the
// required position stream cannot diverge. Slot 11 still occupies storage and
// must be validated even though WorldVertex has no corresponding output.
bool worldVertexLayout(const StoredGeometry& g,std::array<uint32_t,16>& offsets) noexcept {
    if (!g.vertexCount || g.vertexCount>65535 || g.vertices.size()!=uint64_t(g.vertexCount)*g.stride) return false;
    uint32_t stride=0;
    for (unsigned s=0;s<16;++s) {
        const auto f=g.formats[s];
        if (f>=sizes.size() || (f && !(f<=4 || (f>=9 && f<=12) || (f>=14 && f<=19)))) return false;
        offsets[s]=stride; stride+=sizes[f];
    }
    return stride==g.stride && g.formats[0];
}
}

bool validateWorldVertices(const StoredGeometry& g) noexcept {
    std::array<uint32_t,16> offsets{};
    if(!worldVertexLayout(g,offsets))return false;
    // Integer, byte and normalized packed formats always decode to finite
    // values. Only float1..4 need a data scan. Merge adjacent float fields to
    // avoid walking all sixteen semantic slots for every vertex.
    struct FloatSpan {uint32_t offset=0,bytes=0;};
    std::array<FloatSpan,16> spans{};
    unsigned spanCount=0;
    for(unsigned s=0;s<16;++s)if(g.formats[s]>=1 && g.formats[s]<=4) {
        const uint32_t bytes=sizes[g.formats[s]];
        if(spanCount && spans[spanCount-1].offset+spans[spanCount-1].bytes==offsets[s])
            spans[spanCount-1].bytes+=bytes;
        else spans[spanCount++]={offsets[s],bytes};
    }
    if(!spanCount)return true;
    return parallelSceneRange(g.vertexCount,16384,[&](size_t first,size_t end) {
    for(size_t vertex=first;vertex<end;++vertex) {
        const auto* start=g.vertices.data()+size_t(vertex)*g.stride;
        for(unsigned s=0;s<spanCount;++s) {
            const auto* field=start+spans[s].offset;
            for(unsigned byte=0;byte<spans[s].bytes;byte+=4)
                if((word(field+byte)&0x7F800000u)==0x7F800000u)return false;
        }
    }
    return true;
    });
}

bool decodeWorldVertices(const StoredGeometry& g, std::vector<WorldVertex>& output) noexcept {
    try {
        std::array<uint32_t,16> offsets{};
        if(!worldVertexLayout(g,offsets))return false;
        const uint32_t stride=g.stride;
        std::vector<WorldVertex> result(g.vertexCount);
        if(!parallelSceneRange(g.vertexCount,2048,[&](size_t first,size_t end) {
        for (size_t i=first;i<end;++i) {
            std::array<EngineVector,16> slots{};
            for (unsigned s=0;s<16;++s) {
                auto& v=slots[s]; v={0,0,0,1};
                const auto f=g.formats[s]; const auto* p=g.vertices.data()+size_t(i)*stride+offsets[s];
                if(!f)continue; // The implicit (0,0,0,1) stream is already finite.
                if (f<=4) { for (unsigned n=0;n<f;++n) v[n]=std::bit_cast<float>(word(p+4*n)); }
                else if (f>=9 && f<=12) { for (unsigned n=0;n<f-8u;++n) v[n]=float(half(p+2*n)); }
                else if (f==19) {
                    const uint32_t packed=word(p);
                    for (unsigned n=0;n<3;++n) {
                        const unsigned bits=n==2?10:11, mask=(1u<<bits)-1;
                        v[n]=float((packed>>(n*11))&mask);
                    }
                } else if (f==14 || f==15) {
                    // Original engine declaration type table 82A40010:
                    // 14=0x2A2190 (signed normalized 11:11:10),
                    // 15=0x2A2090 (unsigned normalized 11:11:10).
                    const uint32_t packed=word(p);
                    for (unsigned n=0;n<3;++n) {
                        const unsigned bits=n==2?10:11, mask=(1u<<bits)-1;
                        const uint32_t raw=(packed>>(n*11))&mask;
                        if (f==15) v[n]=float(raw)/float(mask);
                        else {
                            const int32_t signedValue=int32_t(raw^(1u<<(bits-1)))-int32_t(1u<<(bits-1));
                            v[n]=(std::max)(-1.0f,float(signedValue)/float(mask>>1));
                        }
                    }
                } else {
                    const auto packed=word(p);
                    for (unsigned n=0;n<4;++n) {
                        const unsigned component=f==18 && (n==0 || n==2)?2-n:n;
                        v[n]=float((packed>>(component*8))&255u);
                        if (f!=17) v[n]/=255.0f;
                    }
                }
                if (!finite(v)) return false;
            }
            auto& v=result[i]; v.position=slots[0]; v.normal=slots[9]; v.color=slots[10];
            for (unsigned t=0;t<8;++t) v.tex[t]=slots[t+1];
            v.indices=slots[12]; v.weights=slots[13]; v.indices2=slots[14]; v.weights2=slots[15];
        }
        return true;
        }))return false;
        output=std::move(result); return true;
    } catch (...) { return false; }
}

bool snapshotImmediateWorldGeometry(uint8_t* base,uint32_t descriptor,uint32_t indexAddress,uint32_t count,
                                    std::shared_ptr<const StoredGeometry> stored,StoredDraw& output,
                                    ImmediateCaptureReason* reason) noexcept {
    try {
        // Reset supplied reason at entry so reuse cannot retain earlier fields.
        if(reason)*reason=ImmediateCaptureReason{};
        using Stage=ImmediateCaptureReason::Stage;
        auto fail=[&](Stage stage,uint32_t d0=0,uint32_t d1=0,uint32_t d2=0) {
            if(reason){reason->stage=stage;reason->detail0=d0;reason->detail1=d1;reason->detail2=d2;}
            return false;
        };
        if (!count || count%3 || count>49152) return fail(Stage::count,count);
        auto g=std::make_shared<StoredGeometry>();
        bool usedRecovery=false;
        std::vector<uint16_t> recoveryIndices;
        if (!stored) {
            std::array<uint8_t,68> header{};
            if (!copyRenderMemory(base,descriptor,header.data(),header.size())) return fail(Stage::headerRead,descriptor);
            g->vertexCount=half(header.data());
            uint32_t fieldCode=0;
            if(half(header.data()+2))fieldCode|=1;
            if(word(header.data()+56))fieldCode|=2;
            if(word(header.data()+60))fieldCode|=4;
            if(word(header.data()+64))fieldCode|=8;
            if(!g->vertexCount || g->vertexCount>16384)fieldCode|=16;
            if(fieldCode) return fail(Stage::headerBounds,g->vertexCount,fieldCode);
            std::array<std::vector<uint8_t>,16> streams;
            auto stream=[&](unsigned slot,uint32_t pointer,uint8_t format) {
                if (!pointer || !format) return fail(Stage::streamBounds,slot,pointer,format);
                g->formats[slot]=format;g->stride+=sizes[format];streams[slot].resize(size_t(g->vertexCount)*sizes[format]);
                if(!copyRenderMemory(base,pointer,streams[slot].data(),streams[slot].size()))
                    return fail(Stage::streamCopy,slot,pointer,format);
                return true;
            };
            if (!stream(0,word(header.data()+4),3)) return false;
            for(unsigned slot=0;slot<8;++slot) {
                const auto pointer=word(header.data()+8+slot*4);const auto components=header[40+slot];
                if(pointer && components>4) {
                    if(reason){reason->stage=Stage::headerBounds;reason->detail0=g->vertexCount;reason->detail1=32u|slot;}
                    return false;
                }
                if (pointer && !stream(slot+1,pointer,components)) return false;
            }
            if (word(header.data()+48) && !stream(9,word(header.data()+48),3)) return false;
            if (word(header.data()+52) && !stream(10,word(header.data()+52),18)) return false;
            g->vertices.resize(size_t(g->vertexCount)*g->stride);
            for(unsigned vertex=0;vertex<g->vertexCount;++vertex) {
                size_t offset=size_t(vertex)*g->stride;
                for(unsigned slot=0;slot<16;++slot) if(g->formats[slot]) {
                    const auto n=sizes[g->formats[slot]];
                    std::memcpy(g->vertices.data()+offset,streams[slot].data()+size_t(vertex)*n,n);offset+=n;
                }
            }
            // Pass a copy so g survives for task-62b recovery below; the hot path
            // pays one shared_ptr copy and performs no scan, hash or extra read.
            stored=retainImmediate(g,reason);
            // Task-62b: lazy single owned index snapshot. Only the failure
            // path reads guest indices, and that SAME snapshot drives OOB
            // refusal, canonicalization and the published draw.
            if(!stored) {
                if(reason && reason->stage==Stage::ok)reason->stage=Stage::vertexDecode;
                // Single owned read; unreadable indices cannot recover.
                std::vector<uint8_t> snapBytes(size_t(count)*2);
                if(!copyRenderMemory(base,indexAddress,snapBytes.data(),snapBytes.size())) {
                    ++immediateCanonicalRefused;
                    return false;
                }
                std::vector<uint16_t> snap(count);
                for(uint32_t i=0;i<count;++i)snap[i]=half(snapBytes.data()+size_t(i)*2);
                // Refuse OOB before any canonicalization or success accounting.
                for(uint32_t i=0;i<count;++i) {
                    if(snap[i]>=g->vertexCount) {
                        if(reason){reason->stage=Stage::indexOob;reason->detail0=i;reason->detail1=snap[i];reason->detail2=g->vertexCount;}
                        return false;
                    }
                }
                if(!canonicalizeUnreferencedVertices(g,snap.data(),count,reason)) {
                    ++immediateCanonicalRefused;
                    return false;
                }
                auto retry=retainImmediate(g,reason);
                if(!retry) {
                    // Retry can locate a different bad vertex than the first
                    // pass; membership must describe the final failure.
                    if(reason) {
                        reason->membershipKnown=false;reason->referenced=false;reason->firstRefPos=UINT32_MAX;
                        if(reason->badVertex!=UINT32_MAX) {
                            reason->membershipKnown=true;
                            for(uint32_t pos=0;pos<count;++pos) {
                                if(snap[pos]==reason->badVertex){reason->referenced=true;reason->firstRefPos=pos;break;}
                            }
                        }
                    }
                    ++immediateCanonicalRefused;
                    return false;
                }
                stored=std::move(retry);
                if(reason)*reason=ImmediateCaptureReason{};
                usedRecovery=true;
                recoveryIndices=std::move(snap);
            }
            g=std::make_shared<StoredGeometry>();
        }
        if(usedRecovery) {
            g->indices=std::move(recoveryIndices);
            for(uint32_t i=0;i<count;++i) {
                if(g->indices[i]>=stored->vertexCount)return fail(Stage::indexOob,i,g->indices[i],stored->vertexCount);
            }
        } else {
            // The unpublished owned vector can receive the big-endian bytes
            // directly. Convert each element only after its two bytes are read;
            // queued snapshots and caller output remain untouched on failure.
            g->indices.resize(count);
            auto* bytes=reinterpret_cast<uint8_t*>(g->indices.data());
            if (!copyRenderMemory(base,indexAddress,bytes,count*sizeof(uint16_t))) return fail(Stage::indexCopy,indexAddress,count);
            for (unsigned i=0;i<count;++i) {
                g->indices[i]=half(bytes+i*2);
                if (g->indices[i]>=stored->vertexCount) return fail(Stage::indexOob,i,g->indices[i],stored->vertexCount);
            }
        }
        auto retainedIndices=retainImmediate(std::move(g));
        if(!retainedIndices)return fail(Stage::retainIndices);
        // Commit only after retention succeeds; avoid a large StoredDraw temporary.
        output.vertices=std::move(stored);output.indices=std::move(retainedIndices);
        output.firstIndex=0;output.indexCount=count;
        output.transforms.reset();output.vertexBindings.reset();
        if(usedRecovery) {
            ++immediateCanonicalRecovered;
            // Bounded profile-only positive proof, per smoke texture
            // (identification only; acceptance never branches on tex id).
            if(profileEngineCpu) {
                uint8_t idBytes[2]{};
                if(copyRenderMemory(base,context+16896+8,idBytes,sizeof(idBytes))) {
                    const uint32_t tex0=(uint32_t(idBytes[0])<<8)|idBytes[1];
                    if(tex0==1371) {
                        ++immediateCanonicalRecoveredTex1371;
                        if(immediateCanonicalLog1371.fetch_add(1,std::memory_order_relaxed)<4)
                            std::fprintf(stderr,"[EngineImmediateCanonical] tex0=1371 recovered=1 vcount=%u indexCount=%u\n",
                                output.vertices->vertexCount,output.indexCount);
                    } else if(tex0==1477) {
                        ++immediateCanonicalRecoveredTex1477;
                        if(immediateCanonicalLog1477.fetch_add(1,std::memory_order_relaxed)<4)
                            std::fprintf(stderr,"[EngineImmediateCanonical] tex0=1477 recovered=1 vcount=%u indexCount=%u\n",
                                output.vertices->vertexCount,output.indexCount);
                    }
                }
            }
        }
        return true;
    } catch (...) {
        // Fully qualified: the using-alias above lives inside try and is out
        // of scope in the handler.
        if(reason)reason->stage=ImmediateCaptureReason::Stage::exception;
        return false;
    }
}

// Internal callers own a fresh, unpublished result, so fill it directly.
// The public wrapper below still leaves caller output unchanged on failure.
static bool prepareWorldVertexProgramInto(const EngineVertexBindingSnapshot& b, WorldVertexOptions& o,
                                          WorldVertexConstants& c) noexcept {
    const auto& d=b.descriptor; const auto bytes=encodeEngineVertexDescriptor(d);
    auto reject=[&](const char* reason,unsigned first,unsigned count,unsigned vector=UINT32_MAX,unsigned lane=0,uint32_t bits=0) {
        static std::atomic<unsigned> reports{};
        if(reports.fetch_add(1,std::memory_order_relaxed)<32)
            std::fprintf(stderr,"[EngineWorldVertexFailure] reason=%s first=%u count=%u vector=%u lane=%u bits=%08X flags=%08X palette=%u device=%08X\n",
                         reason,first,count,vector,lane,bits,d.flags,d.palette,b.deviceAddress);
        return false;
    };
    for (unsigned i=0;i<5;++i) if (word(bytes.data()+i*4)!=b.key[i+1]) return reject("descriptor-key",i,1);
    o.weights=(d.flags>>16)&15;
    if (o.weights>8) return reject("weight-count",o.weights,0);
    o.positionConversion=(d.flags&0x04000000)!=0; o.normal=(d.flags&0x00200000)!=0;
    o.tangents=(d.flags&0x00400000)!=0; o.normalizeNormal=(d.flags&0x00800000)!=0;
    o.vertexColor=(d.flags&0x01000000)==0;
    c.references[0]={d.palette,d.positionConversion,d.color,0};
    auto take=[&](unsigned first,unsigned count) {
        if (first>=256 || count>256-first) return reject("constant-range",first,count);
        if(copyFiniteVectors(b.constantBytes.data()+first*16,c.vectors.data()+first,count))return true;
        // Only inspect individual lanes after an existing validation failure.
        // Successful draws retain the same conversion and validation work.
        for(unsigned v=first;v<first+count;++v)for(unsigned lane=0;lane<4;++lane) {
            const auto bits=word(b.constantBytes.data()+v*16+lane*4);
            if((bits&0x7FFFFFFFu)>0x7F7FFFFFu)return reject("nonfinite-constant",first,count,v,lane,bits);
        }
        return reject("constant-copy",first,count);
    };
    if (!take(0,4) || !take(7,2) || !take(d.color,1) || (o.positionConversion && !take(d.positionConversion,2)) ||
        (o.weights && !take(d.palette,156))) return false;
    for (unsigned s=0;s<8;++s) {
        if (!supportedMode(d.modes[s])) {
            // Bounded opt-in evidence for unmapped original modes (observed:
            // stage 3 mode 16 in stages 0,4,8,16,16,9,1,1). Prints the full
            // stage context needed to pin the exact original template branch
            // (coordinates, conversion/matrix selectors, parameter slots).
            // Profile-gated only: zero overhead (not even a counter fetch)
            // when disabled. Fail-closed: the draw is still rejected below.
            if (profileEngineCpu) {
                static std::atomic<unsigned> unknownModes{};
                if (unknownModes.fetch_add(1,std::memory_order_relaxed)<4) {
                    const unsigned coord = unsigned((d.coordinateMapping>>(8+3*s))&7);
                    std::fprintf(stderr,"[EngineWorldVertexMode] stage=%u mode=%u modes=%u,%u,%u,%u,%u,%u,%u,%u coord=%u convertSel=%u matrixSel=%u params=%u,%u,%u,%u flags=%08X palette=%u\n",
                        s,d.modes[s],d.modes[0],d.modes[1],d.modes[2],d.modes[3],d.modes[4],d.modes[5],d.modes[6],d.modes[7],
                        coord,d.conversions[s],d.matrices[s],d.parameters[s][0],d.parameters[s][1],d.parameters[s][2],d.parameters[s][3],d.flags,d.palette);
                }
            }
            return reject("unsupported-mode",s,d.modes[s]);
        }
        // Stage 5 of original bumpcubeenv writes the basis into TEXCOORD5-7.
        // Only this cache-verified form is supported; preserve other failures.
        if (d.modes[s]==18 && ((s!=1 && s!=5) || d.modes[s+1]!=4 || d.modes[s+2]!=4 ||
                              (d.flags&(1u<<(8+s))))) return reject("basis-mode",s,d.modes[s]);
        if (((d.modes[s]==13 || d.modes[s]==22) && s!=0) || (d.modes[s]==17 && s!=5) ||
            (d.modes[s]==10 && s!=1) ||
            (d.modes[s]==9 && s!=3 && s!=4 && s!=5) ||
            (d.modes[s]==16 && s!=3 && s!=4 && s!=5)) return reject("mode-stage",s,d.modes[s]);
        o.modes[s]=d.modes[s]; o.coordinates[s]=uint8_t((d.coordinateMapping>>(8+3*s))&7);
        o.conversions[s]=(d.flags&(1u<<o.coordinates[s]))!=0; o.matrices[s]=(d.flags&(1u<<(8+s)))!=0;
        c.references[s+1]={d.conversions[s],d.matrices[s],d.parameters[s][0],0};
        const bool input=o.modes[s]==0 || o.modes[s]==13 || o.modes[s]==22 || (o.tangents && (s==2 || s==3));
        if ((input && o.conversions[s] && !take(d.conversions[s],2)) ||
            (o.modes[s]!=4 && o.matrices[s] && !take(d.matrices[s],4)) ||
            (o.modes[s]==1 && !take(d.parameters[s][0],4)) ||
            (o.modes[s]==10 && !take(d.parameters[s][0],3)) ||
            (o.modes[s]==18 && !take(d.parameters[s][0],s==1?8:3)) ||
            (o.modes[s]==17 && !take(d.parameters[s][0],6)) ||
            (o.modes[s]==22 && !take(d.parameters[s][0],2)) ||
            (o.modes[s]==16 && !take(d.parameters[s][0],2)) ||
            (o.modes[s]==13 && !take(4,2)) ||
            ((o.modes[s]==7 || o.modes[s]==9 || o.modes[s]==20) && !take(d.parameters[s][0],1))) return false;
    }
    return true;
}
bool prepareWorldVertexProgram(const EngineVertexBindingSnapshot& b, WorldVertexOptions& options,
                               WorldVertexConstants& constants) noexcept {
    WorldVertexOptions o;WorldVertexConstants c;
    if(!prepareWorldVertexProgramInto(b,o,c))return false;
    options=o;constants=c;return true;
}

// Geometry-aware palette fallback. Runs only after the strict full-palette
// copy fails. The original template fetches c[palette+floor(index*c8.w)+0..2]
// for every active influence before weighting (MUL/MAD: R3=MUL(_vMI,c[0+8].w),
// A0=(int)ARL(R3), ARL=floor, then MUL/MAD by the weight), with no
// zero-weight skip, so every influence in the actual draw index subset is
// validated here. D3D11 flushes denormals to signed zero and allows MUL
// latitude, so GPU addresses are bracketed conservatively below (never the
// CPU float product alone).
namespace {
constexpr unsigned kPaletteVectors = 156;
constexpr unsigned kPaletteRows = 3;
// Prove the palette rows one influence may read on the GPU. The exact double
// product of the two float operands brackets the GPU result with the nearest
// floats below/above it; the union of both floor addresses is proven, so
// truncation or nearest rounding cannot escape. Exact zero and exactly
// representable normal products keep their single exact address. Any
// subnormal operand/product, overflow, or possible out-of-range address fails.
bool gpuPaletteAddresses(float index, float scale, unsigned palette, bool* used) noexcept {
    if (std::fpclassify(index) == FP_SUBNORMAL || std::fpclassify(scale) == FP_SUBNORMAL) return false;
    const double exact = double(index) * double(scale);
    if (exact != 0.0 && std::fabs(exact) < double(FLT_MIN)) return false;
    const float nearest = float(exact);
    if (!std::isfinite(nearest)) return false;
    float low, high;
    if (double(nearest) == exact) low = high = nearest;
    else if (double(nearest) < exact) {
        low = nearest;
        high = std::nextafter(nearest, std::numeric_limits<float>::infinity());
        if (!std::isfinite(high)) return false;
    } else {
        high = nearest;
        low = std::nextafter(nearest, -std::numeric_limits<float>::infinity());
    }
    const float addresses[2] = {std::floor(low), std::floor(high)};
    for (unsigned k = 0; k < 2; ++k) {
        const float address = addresses[k];
        // Same bounds the submit-time shader bind enforces; either neighbor
        // out of range fails the whole proof.
        if (!std::isfinite(address) || address < 0 || address > float(kPaletteVectors - kPaletteRows) ||
            uint64_t(palette) + unsigned(address) + 2 >= 256) return false;
        const unsigned row = unsigned(address);
        used[row] = used[row + 1] = used[row + 2] = true;
    }
    return true;
}
bool proveAndFillPalette(const EngineVertexBindingSnapshot& b, const StoredDraw& geometry,
                         const WorldVertexOptions& o, WorldVertexConstants& c,
                         unsigned palette, const bool required[256], WorldPaletteProof& proof) noexcept {
    const float scale = c.vectors[8][3];
    if (!std::isfinite(scale) || !o.weights) return false;
    if (!geometry.vertices || !geometry.indices || !geometry.indexCount ||
        uint64_t(geometry.firstIndex) + geometry.indexCount > geometry.indices->indices.size()) return false;
    // Capture must agree with submit: skinned draws need blend streams.
    const auto& formats = geometry.vertices->formats;
    if (!formats[12] || !formats[13]) return false;
    if (o.weights > 4 && (!formats[14] || !formats[15])) return false;
    std::vector<WorldVertex> decoded;
    if (!decodeWorldVertices(*geometry.vertices, decoded) || decoded.empty()) return false;
    bool used[kPaletteVectors] = {};
    // Actual draw index subset only; a shared immutable buffer may hold other
    // draws' indices outside [firstIndex, firstIndex+indexCount). Decoded
    // CPU floats are the same bits uploaded to the GPU.
    for (uint32_t i = 0; i < geometry.indexCount; ++i) {
        const uint16_t vertex = geometry.indices->indices[geometry.firstIndex + i];
        if (vertex >= decoded.size()) return false;
        const auto& v = decoded[vertex];
        for (unsigned w = 0; w < o.weights; ++w) {
            const float index = w < 4 ? v.indices[w] : v.indices2[w - 4];
            if (!gpuPaletteAddresses(index, scale, palette, used)) return false;
        }
    }
    proof.usageProven = true;
    for (unsigned r = 0; r < kPaletteVectors; ++r)
        if (used[r] && palette + r == 135) proof.referencesVector135 = true;
    // Every actual read must be finite before anything is published.
    for (unsigned r = 0; r < kPaletteVectors; ++r) if (used[r]) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto bits = word(b.constantBytes.data() + (palette + r) * 16 + lane * 4);
            if ((bits & 0x7FFFFFFFu) > 0x7F7FFFFFu) return false;
        }
    }
    unsigned ignored = 0, usedCount = 0;
    for (unsigned r = 0; r < kPaletteVectors; ++r) {
        const unsigned row = palette + r;
        if (used[r] || required[row]) {
            for (unsigned lane = 0; lane < 4; ++lane)
                c.vectors[row][lane] = std::bit_cast<float>(word(b.constantBytes.data() + row * 16 + lane * 4));
            if (used[r]) ++usedCount;
        } else {
            if (!ignored) proof.firstIgnoredRow = row;
            if (proof.firstIgnoredNonfiniteRow == 256) {
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const auto bits = word(b.constantBytes.data() + row * 16 + lane * 4);
                    if ((bits & 0x7FFFFFFFu) > 0x7F7FFFFFu) { proof.firstIgnoredNonfiniteRow = row; break; }
                }
            }
            ++ignored;
            c.vectors[row] = {0, 0, 0, 0};
        }
    }
    proof.fallback = true;
    proof.usedRows = usedCount;
    proof.ignoredRows = ignored;
    static std::atomic<unsigned> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 4)
        std::fprintf(stderr, "[EngineWorldPaletteFallback] geometry=%08X weights=%u scale=%g used=%u ignored=%u firstIgnored=%u ignoredNaN=%u vector135=%u\n",
                     geometry.vertices->address, o.weights, double(scale), usedCount, ignored,
                     proof.firstIgnoredRow, proof.firstIgnoredNonfiniteRow,
                     proof.referencesVector135 ? 1u : 0u);
    return true;
}
}
bool prepareWorldVertexProgramWithGeometry(const EngineVertexBindingSnapshot& b, const StoredDraw& geometry,
                                           WorldVertexOptions& options, WorldVertexConstants& constants,
                                           WorldPaletteProof* proof) noexcept {
    try {
        WorldVertexOptions o;
        WorldVertexConstants c;
        WorldPaletteProof local;
        const auto& d = b.descriptor;
        const auto bytes = encodeEngineVertexDescriptor(d);
        for (unsigned i = 0; i < 5; ++i) if (word(bytes.data() + i * 4) != b.key[i + 1]) return false;
        o.weights = (d.flags >> 16) & 15;
        if (o.weights > 8) return false;
        o.positionConversion = (d.flags & 0x04000000) != 0; o.normal = (d.flags & 0x00200000) != 0;
        o.tangents = (d.flags & 0x00400000) != 0; o.normalizeNormal = (d.flags & 0x00800000) != 0;
        o.vertexColor = (d.flags & 0x01000000) == 0;
        c.references[0] = {d.palette, d.positionConversion, d.color, 0};
        // Rows independently required outside the palette; an overlapping
        // palette NaN consumed by another stage still fails through these.
        bool required[256] = {};
        auto takeStrict = [&](unsigned first, unsigned count) {
            if (first >= 256 || count > 256 - first) return false;
            if (!copyFiniteVectors(b.constantBytes.data() + first * 16, c.vectors.data() + first, count)) return false;
            for (unsigned v = first; v < first + count; ++v) required[v] = true;
            return true;
        };
        if (!takeStrict(0, 4) || !takeStrict(7, 2) || !takeStrict(d.color, 1) ||
            (o.positionConversion && !takeStrict(d.positionConversion, 2))) return false;
        for (unsigned s = 0; s < 8; ++s) {
            if (!supportedMode(d.modes[s])) return false;
            if (d.modes[s] == 18 && ((s != 1 && s != 5) || d.modes[s + 1] != 4 || d.modes[s + 2] != 4 ||
                                    (d.flags & (1u << (8 + s))))) return false;
            if (((d.modes[s] == 13 || d.modes[s] == 22) && s != 0) || (d.modes[s] == 17 && s != 5) ||
                (d.modes[s] == 10 && s != 1) ||
                (d.modes[s] == 9 && s != 3 && s != 4 && s != 5) ||
                (d.modes[s] == 16 && s != 3 && s != 4 && s != 5)) return false;
            o.modes[s] = d.modes[s]; o.coordinates[s] = uint8_t((d.coordinateMapping >> (8 + 3 * s)) & 7);
            o.conversions[s] = (d.flags & (1u << o.coordinates[s])) != 0; o.matrices[s] = (d.flags & (1u << (8 + s))) != 0;
            c.references[s + 1] = {d.conversions[s], d.matrices[s], d.parameters[s][0], 0};
            const bool input = o.modes[s] == 0 || o.modes[s] == 13 || o.modes[s] == 22 || (o.tangents && (s == 2 || s == 3));
            if ((input && o.conversions[s] && !takeStrict(d.conversions[s], 2)) ||
                (o.modes[s] != 4 && o.matrices[s] && !takeStrict(d.matrices[s], 4)) ||
                (o.modes[s] == 1 && !takeStrict(d.parameters[s][0], 4)) ||
                (o.modes[s] == 10 && !takeStrict(d.parameters[s][0], 3)) ||
                (o.modes[s] == 18 && !takeStrict(d.parameters[s][0], s == 1 ? 8 : 3)) ||
                (o.modes[s] == 17 && !takeStrict(d.parameters[s][0], 6)) ||
                (o.modes[s] == 22 && !takeStrict(d.parameters[s][0], 2)) ||
                (o.modes[s] == 16 && !takeStrict(d.parameters[s][0], 2)) ||
                (o.modes[s] == 13 && !takeStrict(4, 2)) ||
                ((o.modes[s] == 7 || o.modes[s] == 9 || o.modes[s] == 20) && !takeStrict(d.parameters[s][0], 1))) return false;
        }
        if (o.weights) {
            const unsigned palette = d.palette;
            if (palette >= 256 || kPaletteVectors > 256 - palette) return false;
            if (copyFiniteVectors(b.constantBytes.data() + palette * 16, c.vectors.data() + palette, kPaletteVectors)) {
                for (unsigned r = 0; r < kPaletteVectors; ++r) required[palette + r] = true;
            } else if (!proveAndFillPalette(b, geometry, o, c, palette, required, local)) {
                static std::atomic<unsigned> rejects{};
                if (rejects.fetch_add(1, std::memory_order_relaxed) < 16)
                    std::fprintf(stderr, "[EngineWorldPaletteFallbackRejected] geometry=%08X weights=%u usageProven=%u vector135=%u\n",
                                 geometry.vertices ? geometry.vertices->address : 0, o.weights,
                                 local.usageProven ? 1u : 0u, local.referencesVector135 ? 1u : 0u);
                if (proof) *proof = local;
                return false;
            }
        }
        options = o; constants = c;
        if (proof) *proof = local;
        return true;
    } catch (...) { return false; }
}

uint16_t worldFragmentTextureMask(std::string_view name,uint32_t flags) noexcept {
    // Passes repeat the same immutable program. This cache owns no guest
    // pointer; masks come from the hash-pinned original shader translator.
    thread_local const WorldFragmentBinding* previous=nullptr;
    if(previous && previous->flags==flags && name==previous->name)return uint16_t(previous->textures);
    for(const auto& binding:worldFragmentBindings)if(binding.flags==flags && name==binding.name) {
        previous=&binding;return uint16_t(binding.textures);
    }
    return 0xFFFF; // Preserve unknown-program diagnostics and future resources.
}

static bool snapshotWorldTargets(uint8_t* base,uint32_t device,WorldSurfaceTargets& output) noexcept {
    if(!device || uint64_t(device)+12452>0x100000000ull)return false;
    std::array<uint8_t,20> objects{},objectsAfter{};
    std::array<uint8_t,24> state{},stateAfter{};
    if(!copyRenderMemory(base,device+12432,objects.data(),objects.size()) ||
       !copyRenderMemory(base,device+10368,state.data(),state.size()) ||
       !copyRenderMemory(base,device+12432,objectsAfter.data(),objectsAfter.size()) ||
       !copyRenderMemory(base,device+10368,stateAfter.data(),stateAfter.size()) ||
       objects!=objectsAfter || state!=stateAfter)return false;
    // Completed registers: layout, color0, depth, color1, color2, color3.
    // Do not dereference the objects: even their allocation headers can already
    // differ from what the original binder selected for this command.
    constexpr unsigned registers[]{1,3,4,5,2};
    WorldSurfaceTargets result;
    for(unsigned slot=0;slot<5;++slot) {
        result.targets[slot]=word(objects.data()+slot*4);
        if(result.targets[slot])result.surfaceBindings[slot]={word(state.data()),word(state.data()+registers[slot]*4),true};
    }
    output=result;return true;
}
static bool snapshotWorldDrawInto(uint8_t* base,const StoredDraw& geometry,WorldDraw& result) noexcept {
    ++observed;
    auto fail=[&](unsigned step) {
        ++rejected;
        static std::array<std::atomic<unsigned>,13> reports{};
        if(step<reports.size() && reports[step]++<4)
            std::fprintf(stderr,"[EngineWorldSnapshotRejected] step=%u geometry=%08X\n",step,
                         geometry.vertices?geometry.vertices->address:0);
        return false;
    };
    try {
        if (!geometry || !geometry.vertexBindings || !geometry.vertexBindings->deviceAddress ||
            uint64_t(geometry.vertexBindings->deviceAddress)+12600>0x100000000ull) return fail(1);
        result.geometry={geometry.vertices,geometry.indices,geometry.firstIndex,geometry.indexCount};
        if (!prepareWorldVertexProgramInto(*geometry.vertexBindings,result.options,result.constants)) {
            // Strict full-palette validation failed. Retry once with a
            // geometry-aware proof that only proven-unread rows may be
            // sanitized; publish only when every actual read is finite.
            WorldVertexOptions fallbackOptions;
            WorldVertexConstants fallbackConstants;
            WorldPaletteProof paletteProof;
            if (!prepareWorldVertexProgramWithGeometry(*geometry.vertexBindings, geometry,
                                                       fallbackOptions, fallbackConstants, &paletteProof)) {
                static std::atomic<unsigned> reports{};
                if(reports++<16) {
                    const auto& d=geometry.vertexBindings->descriptor;
                    std::fprintf(stderr,"[EngineWorldVertexUnsupported] flags=%08X modes=%u,%u,%u,%u,%u,%u,%u,%u palette=%u geometry=%08X\n",
                        d.flags,d.modes[0],d.modes[1],d.modes[2],d.modes[3],d.modes[4],d.modes[5],d.modes[6],d.modes[7],d.palette,geometry.vertices->address);
                }
                return fail(2);
            }
            result.options=fallbackOptions;result.constants=fallbackConstants;
        }
        std::array<uint8_t,20> program{}; std::array<uint8_t,24> viewport{};
        if (!copyRenderMemory(base,context+16896,result.attributes.data(),160) ||
            !copyRenderMemory(base,context+17152,viewport.data(),24) ||
            !snapshotWorldTargets(base,geometry.vertexBindings->deviceAddress,result)) return fail(3);
        for (unsigned i=0;i<4;++i) result.viewport[i]=word(viewport.data()+4*i);
        result.depthRange={std::bit_cast<float>(word(viewport.data()+16)),std::bit_cast<float>(word(viewport.data()+20)),0,0};
        if(!finite(result.depthRange) || result.depthRange[0]<0 || result.depthRange[0]>1 || result.depthRange[1]<0 || result.depthRange[1]>1) return fail(4);
        if (!result.viewport[2] || !result.viewport[3] || result.viewport[2]>4096 || result.viewport[3]>4096) return fail(4);
        const auto* a=result.attributes.data(); const auto address=word(a);
        if (address) {
            std::array<char,96> name{};
            if (!copyRenderMemory(base,address,program.data(),20) || word(program.data())!=5 ||
                !copyRenderMemory(base,word(program.data()+4),name.data(),name.size()) ||
                std::find(name.begin(),name.end(),char(0))==name.end()) return fail(5);
            if (std::strcmp(name.data(),"XRShader_MotionMap")==0) result.material=WorldMaterial::motion;
            else if (std::strcmp(name.data(),"XRShader_FP20_NDSP")==0 ||
                     std::strcmp(name.data(),"XRShader_FP20_NDS")==0) result.material=WorldMaterial::ndsp;
            else result.material=WorldMaterial::post;
            result.fragmentName=name.data();result.fragmentFlags=word(program.data()+16)>>8;
            const auto count=word(program.data()+16)&255u;
            if (count>16 || (result.material==WorldMaterial::ndsp && count<4)) return fail(6);
            std::array<uint8_t,256> env{};
            // Original822478C0 uploads to device+6016. Read that completed
            // copy, not the GUI's mutable program parameter allocation.
            if (count && !copyRenderMemory(base,geometry.vertexBindings->deviceAddress+6016,env.data(),count*16)) return fail(7);
            for (unsigned v=0;v<count;++v) {
                for (unsigned l=0;l<4;++l) result.fragmentConstants[v][l]=std::bit_cast<float>(word(env.data()+v*16+l*4));
                // These are shader data, not host indices or extents. The
                // original GUIFadeToWhite suppliesFFFFFFFF in z/w while its
                // blur contribution is zero. Preserve those IEEE bits instead
                // of dropping an otherwise valid original draw on the CPU.
            }
        } else if ((word(a+92)&0x0FF00001u) || a[97]!=8) {
            // Original 822483BC keeps the fragment stage for alpha coverage
            // (flag 1) or a non-ALWAYS alpha comparison even with color writes
            // disabled. Vegetation's depth prepass still needs its texture;
            // treating it as solid depth exposes whole cards to later lighting.
            result.material=WorldMaterial::fixed;
            unsigned stages=0;
            for(unsigned s=0;s<16;++s)if(half(a+8+s*2)) {if(s!=stages) return fail(9);++stages;}
            // The Darkness effect finishes with a two-texture fixed-function
            // composite. It expands the intermediate atlas into the scene;
            // dropping it leaves those intermediate quadrants on screen.
            constexpr const char* programs[]{"MRenderXenon_Attrib_TexEnvMode00",
                "MRenderXenon_Attrib_TexEnvMode01","MRenderXenon_Attrib_TexEnvMode02"};
            if(stages>=std::size(programs)) return fail(9);
            result.fragmentName=programs[stages];
            std::array<uint8_t,16> env{};
            if(stages && !copyRenderMemory(base,geometry.vertexBindings->deviceAddress+6016,env.data(),16)) return fail(10);
            for(unsigned lane=0;lane<4;++lane)result.fragmentConstants[0][lane]=std::bit_cast<float>(word(env.data()+lane*4));
            if(!finite(result.fragmentConstants[0])) return fail(10);
        }
        result.textureMask=result.material==WorldMaterial::depth?0:worldFragmentTextureMask(result.fragmentName,result.fragmentFlags);
        // Depth/motion and texture-free fragments cannot consume a sampler or
        // image. Other passes retain every source-declared slot, even if D3D
        // subsequently optimizes an unused fetch out of this permutation.
        if(result.textureMask) {
            const auto device=geometry.vertexBindings->deviceAddress;
            // Original82864F20 stores the completed object at12536+slot*4
            // and its fetch/sampler descriptor at1152+slot*24. 822569E0 skips
            // rebinding unchanged IDs: a subsequently switched resource-table
            // wrapper is NOT necessarily the texture used by this draw.
            std::array<uint8_t,16*24> samplerBytes{};
            std::array<uint8_t,16*4> boundObjects{};
            if(!copyRenderMemory(base,device+1152,samplerBytes.data(),samplerBytes.size()) ||
               !copyRenderMemory(base,device+12536,boundObjects.data(),boundObjects.size()))return fail(11);
            std::array<uint8_t,4> tableBytes{};
            // The table supplies upload residency only when it still describes
            // the bound object. Direct device bindings need no resource ID.
            copyRenderMemory(base,context+17964,tableBytes.data(),4);
            const auto table=word(tableBytes.data());
            for (unsigned s=0;s<16;++s)if(result.textureMask&(1u<<s)) {
                const auto id=result.textureIds[s]=half(a+8+s*2);
                std::array<uint32_t,6> state{};
                for(unsigned w=0;w<6;++w)state[w]=word(samplerBytes.data()+s*24+w*4);
                auto& sampler=result.samplers[s];sampler=decodeWorldSampler(state);
                const auto object=word(boundObjects.data()+s*4);
                // Unbinding deliberately leaves the old fetch words in the
                // original device. A null object must never revive that image.
                if(!object)continue;
                WorldTexture texture;
                if(!snapshotWorldTexture(base,object,texture))continue;
                // Retain the object's virtual storage address for resolve keys;
                // the device fetch address is a translated physical alias.
                // Without matching upload metadata, the completed sampler's
                // minimum is a conservative readable tail, not a new binding.
                texture.firstMip=sampler.lodValid?sampler.minLevel:0;
                std::array<uint8_t,4> pointer{};std::array<uint8_t,184> resource{};
                if(id && table && uint64_t(table)+(id+1ull)*4+4<=0x100000000ull &&
                   copyRenderMemory(base,table+(id+1)*4,pointer.data(),4) &&
                   copyRenderMemory(base,word(pointer.data()),resource.data(),resource.size())) {
                    const auto flags=word(resource.data()+172);
                    const unsigned selected=(flags&0x02000000)?88:8;
                    const auto external=word(resource.data()+selected+76);
                    const auto requested=external?external:word(resource.data()+selected)?word(pointer.data())+selected+4:0;
                    if(requested==object && (flags&0x10000000)) {
                        const float fade=std::bit_cast<float>(word(resource.data()+176));
                        const int32_t pending=int32_t(word(resource.data()+180));
                        if(!std::isfinite(fade) || (fade>0 && (pending<0 || uint32_t(pending)>=texture.mipLevels)))continue;
                        // 82257450 publishes the skipped prefix. 82257010
                        // clears the integer when complete, even during fade.
                        texture.firstMip=fade>0?uint32_t(pending):0;
                        std::array<uint8_t,184> after{};
                        std::array<uint8_t,4> currentPointer{};
                        if(!copyRenderMemory(base,word(pointer.data()),after.data(),after.size()) ||
                           !copyRenderMemory(base,table+(id+1)*4,currentPointer.data(),4) || currentPointer!=pointer ||
                           word(after.data()+selected)!=word(resource.data()+selected) ||
                           word(after.data()+selected+76)!=external ||
                           std::memcmp(after.data()+172,resource.data()+172,12))continue;
                    } else if(requested!=object) {
                        // Keep evidence available during late gameplay without
                        // printing every material pass or exhausting a boot quota.
                        thread_local uint64_t mismatches=0;
                        thread_local std::chrono::steady_clock::time_point nextReport{};
                        const auto now=std::chrono::steady_clock::now();
                        if(++mismatches<=4 || now>=nextReport) {
                            nextReport=now+std::chrono::seconds(5);
                            std::fprintf(stderr,"[EngineWorldTextureBinding] mismatch=%llu program=%s slot=%u id=%u requested=%08X bound=%08X storage=%08X\n",
                                mismatches,result.fragmentName.c_str(),s,id,requested,object,texture.storage);
                        }
                    }
                }
                if(texture.firstMip>=texture.mipLevels)continue;
                std::array<uint8_t,4> currentObject{};std::array<uint8_t,24> currentSampler{};
                if(!copyRenderMemory(base,device+12536+s*4,currentObject.data(),currentObject.size()) ||
                   word(currentObject.data())!=object ||
                   !copyRenderMemory(base,device+1152+s*24,currentSampler.data(),currentSampler.size()) ||
                   std::memcmp(currentSampler.data(),samplerBytes.data()+s*24,currentSampler.size()))continue;
                result.textureObjects[s]=texture;
            }
        }
        ++captured;return true;
    } catch (...) {return fail(12);}
}
bool snapshotWorldDraw(uint8_t* base,const StoredDraw& geometry,WorldDraw& output) noexcept {
    WorldDraw result;
    if(!snapshotWorldDrawInto(base,geometry,result))return false;
    output=std::move(result);return true;
}
std::shared_ptr<WorldDraw> captureWorldDraw(uint8_t* base,const StoredDraw& geometry) noexcept {
    try {
        auto result=acquireWorldDraw();
        if(snapshotWorldDrawInto(base,geometry,*result))return result;
    }catch(...){}
    return {};
}
WorldSampler decodeWorldSampler(const std::array<uint32_t,6>& words) noexcept {
    WorldSampler result;
    const unsigned mag=(words[3]>>19)&3,min=(words[3]>>21)&3,mip=(words[3]>>23)&3,aniso=(words[3]>>25)&7;
    if((words[0]&3)!=2)return result;
    // Addressing/filter support is independent of the captured residency bounds.
    result.minLevel=(words[4]>>2)&15;result.maxLevel=(words[4]>>6)&15;
    result.lodValid=result.minLevel<=result.maxLevel;result.baseOnly=mip==2;
    const int bias=int((words[4]>>12)&1023);
    result.bias=float((bias^512)-512)/32.0f;
    if(!result.lodValid || mag>1 || min>1 || mip>2 || aniso>5)return result;
    for(unsigned a=0;a<3;++a) {
        result.address[a]=(words[0]>>(10+a*3))&7;
        if(result.address[a]==4 || result.address[a]==5 || result.address[a]==7)return result;
    }
    result.minLinear=min!=0;result.magLinear=mag!=0;result.mipLinear=mip==1;result.baseOnly=mip==2;
    result.anisotropy=aniso?uint8_t(1u<<(aniso-1)):1;
    result.border=words[5]&3;
    if(result.border>1)return result;
    result.valid=true;return result;
}
bool snapshotWorldTexture(uint8_t* base,uint32_t object,WorldTexture& output) noexcept {
    std::array<uint8_t,64> header{};
    if(!copyRenderMemory(base,object,header.data(),header.size())) return false;
    WorldTexture t;t.object=object;t.storage=word(header.data()+32)&0xFFFFF000u;
    t.format=word(header.data()+32)&63;t.width=(word(header.data()+36)&8191)+1;t.height=((word(header.data()+36)>>13)&8191)+1;
    const auto exp=(word(header.data()+40)>>13)&63;t.exponent=int(exp^32)-32;
    t.faces=((word(header.data()+48)>>9)&3)==3?6:1;
    t.mipLevels=(std::min)(((word(header.data()+44)>>6)&15)+1,
                         unsigned(std::bit_width((std::max)(t.width,t.height))));
    if(t.width>4096 || t.height>4096 || (t.faces==6 && t.width!=t.height)) return false;
    output=t;return true;
}
bool snapshotWorldResolve(uint8_t* base,uint32_t device,uint32_t flags,uint32_t rectangle,uint32_t destination,
                          uint32_t offset,uint32_t color,float depth,uint32_t stencil,uint32_t face,uint32_t mip,WorldResolve& output) noexcept {
    WorldResolve r;r.face=face;r.mip=mip;r.flags=flags;r.depth=depth;r.stencil=stencil;r.exponent=int((flags>>26)^32)-32;
    if((flags&7)>4 || !snapshotWorldTexture(base,destination,r.destination) || !std::isfinite(depth) ||
       uint64_t(device)+12452>0x100000000ull) return false;
    std::array<uint8_t,16> view{},rect{},rgba{};std::array<uint8_t,8> off{};
    if(!snapshotWorldTargets(base,device,r) || !copyRenderMemory(base,context+17152,view.data(),16) ||
       (rectangle && !copyRenderMemory(base,rectangle,rect.data(),16)) ||
       (offset && !copyRenderMemory(base,offset,off.data(),8)) || (color && !copyRenderMemory(base,color,rgba.data(),16))) return false;
    for(unsigned i=0;i<4;++i) {r.viewport[i]=word(view.data()+i*4);r.rectangle[i]=word(rect.data()+i*4);r.color[i]=std::bit_cast<float>(word(rgba.data()+i*4));}
    if(!rectangle)r.rectangle={0,0,r.destination.width,r.destination.height};
    for(unsigned i=0;i<2;++i)r.offset[i]=word(off.data()+i*4);
    if(!finite(r.color) || r.rectangle[2]<=r.rectangle[0] || r.rectangle[3]<=r.rectangle[1] ||
       r.rectangle[2]>4096 || r.rectangle[3]>4096 ||
       uint64_t(r.offset[0])+r.rectangle[2]-r.rectangle[0]>r.destination.width ||
       uint64_t(r.offset[1])+r.rectangle[3]-r.rectangle[1]>r.destination.height) return false;
    if(mip || face>=r.destination.faces)return false;
    output=r;return true;
}
void recordWorldSubmission(WorldMaterial m) { ++submitted[unsigned(m)]; }
bool snapshotWorldClear(uint8_t* base,uint32_t device,uint32_t flags,uint32_t color,
                        float depth,uint32_t stencil,WorldClear& output) noexcept {
    if (!device || uint64_t(device)+12452>0x100000000ull || !(flags&49) || !std::isfinite(depth)) return false;
    WorldClear result;result.flags=flags&49;result.depth=depth;result.stencil=stencil;
    std::array<uint8_t,16> viewport{},rgba{};
    if (!snapshotWorldTargets(base,device,result) ||
        !copyRenderMemory(base,context+17152,viewport.data(),16) ||
        ((flags&1) && !copyRenderMemory(base,color,rgba.data(),16))) return false;
    for (unsigned i=0;i<4;++i) {result.viewport[i]=word(viewport.data()+i*4);result.color[i]=std::bit_cast<float>(word(rgba.data()+i*4));}
    if (!result.viewport[2] || !result.viewport[3] || result.viewport[2]>4096 || result.viewport[3]>4096 || !finite(result.color)) return false;
    if(result.viewport[0]>4096-result.viewport[2] || result.viewport[1]>4096-result.viewport[3])return false;
    output=result;return true;
}
void printWorldCounters() {
    std::fprintf(stderr,"[EngineWorld] observed=%llu captured=%llu rejected=%llu submittedDepth=%llu submittedMotion=%llu submittedLit=%llu submittedFixed=%llu submittedPost=%llu\n",
        observed.load(),captured.load(),rejected.load(),submitted[0].load(),submitted[1].load(),submitted[2].load(),submitted[3].load(),submitted[4].load());
    std::fprintf(stderr,"[EngineImmediate] canonicalRecovered=%llu canonicalRefused=%llu recovered1371=%llu recovered1477=%llu\n",
        immediateCanonicalRecovered.load(),immediateCanonicalRefused.load(),
        immediateCanonicalRecoveredTex1371.load(),immediateCanonicalRecoveredTex1477.load());
}
}
