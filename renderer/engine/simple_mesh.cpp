#include "simple_mesh.h"
#include "prompt_icons.h"
#include "texture_mip_layout.h"
#include "engine_performance.h"
#include "stored_geometry.h"
#include "engine_texture_constants.h"
#include "engine_vertex_descriptor.h"
#include "engine_palette.h"
#include "engine_vertex_program.h"
#include "world_mesh.h"
#include "frame_resource_set.h"
#include "scene_work.h"
#include "render_trace.h"
#include "runtime/native/display_mode.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace DarkRecomp::Native {
namespace {
bool TextureUploadTraceEnabled() {
    // Per-new-texture success logs cost unbuffered writes during area loads.
    // Failures stay on their bounded paths; gate success spam behind opt-in.
    static const bool enabled = std::getenv("DARK_TEXTURE_TRACE") != nullptr &&
        std::getenv("DARK_TEXTURE_TRACE")[0] == '1';
    return enabled;
}
constexpr uint32_t context = 0x82A69B00;
uint32_t u32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}
uint16_t u16(const uint8_t* bytes) { return uint16_t(bytes[0]) << 8 | bytes[1]; }
float f32(const uint8_t* bytes) { return std::bit_cast<float>(u32(bytes)); }
bool readWord(uint8_t* base, uint32_t address, uint32_t& value) {
    uint8_t bytes[4];
    if (!copyRenderMemory(base, address, bytes, 4)) return false;
    value = u32(bytes); return true;
}
std::atomic<bool> active{false},worldActive{false};
std::atomic<uint64_t> observed{}, accepted{}, unsupported{}, frames{}, images{}, rejectedImages{};
std::atomic<uint64_t> renderedFrames{}, renderedMeshes{};
std::atomic<uint64_t> videoImages{}, videoMeshes{};
std::atomic<uint64_t> colorImages{}, colorMeshes{};
std::atomic<uint64_t> decodedBatches{}, decodedIndices{}, decodedAccepted{}, decodedInvalid{};
std::array<std::atomic<uint64_t>, 10> rejectionReasons{};
std::mutex queueMutex;
std::condition_variable frameConsumed,frameAvailable;
bool frameBackpressure=false;
bool streamLargeFrames=false,pendingFirst=true;
PreviewFramePart readyPart;
// A live frame may contain many lighting passes. Bound the in-flight batch,
// not the logical frame: producer, ready slot and consumer own at most one
// batch each. Constants/commands are bounded by 512 entries; immutable world
// buffers and images have independent byte limits and are charged once.
constexpr size_t livePartCommands=512,worldGeometryByteLimit=64*1024*1024;
constexpr size_t worldImageByteLimit=256*1024*1024;
std::unordered_map<uint32_t, std::shared_ptr<const AlphaImage>> textures;
std::unordered_map<uint32_t, std::shared_ptr<const ColorImage>> alphaColors;
struct CachedColor { std::shared_ptr<const ColorImage> image; uint64_t used = 0; uint32_t resource = 0; };
std::unordered_map<uint32_t, CachedColor> colorTextures;
// The original menu upload working set exceeds64MiB before its model is first
// drawn. Keep that working set resident; dropping an uploaded map cannot cause
// the guest to repeat its already-completed upload.
size_t colorBudget = 256 * 1024 * 1024;
size_t colorBytes = 0, pendingColorBytes = 0;
uint64_t colorUse = 0;
struct CachedVideo {
    uint32_t owner = 0, frame = 0;
    std::array<uint8_t, 8> timestamp{};
    std::shared_ptr<const VideoFrame> image;
};
std::unordered_map<uint32_t, CachedVideo> videos;
std::vector<SimpleMesh> pending, ready, recycledFrame;
std::atomic<uint64_t> frameBufferReuses{};
bool frameReady = false;
size_t pendingVertices = 0, pendingWorldVertices=0;
FrameResourceSet<StoredGeometry> pendingGeometry;
size_t pendingGeometryBytes=0;
FrameResourceSet<ColorImage> pendingWorldImages;
size_t pendingWorldImageBytes=0,pendingGuiDraws=0;
std::atomic<uint64_t> worldQueued{},worldVertexBudgetDrops{},worldCommandBudgetDrops{};
std::atomic<uint64_t> worldImageBudgetDrops{},streamedParts{};
struct QueueFrameCounts {
    size_t worldDraws=0, commandDrops=0, vertexDrops=0, clearDrops=0, resolveDrops=0, guiDrops=0;
    size_t commands=0,parts=0,imageDrops=0;
    std::array<size_t,5> passes{};
};
QueueFrameCounts pendingCounts;
std::atomic<size_t> queueHighWater{0}, oversizedFrames{0};
unsigned queueFrameReports=0;
void publishPreviewPart(std::unique_lock<std::mutex>& lock,bool last);
bool liveStreaming() {return frameBackpressure && streamLargeFrames;}
bool reservePreviewCommand(std::unique_lock<std::mutex>& lock) {
    if(liveStreaming() && pending.size()>=livePartCommands)publishPreviewPart(lock,false);
    return pending.size()<4096;
}
std::array<std::shared_ptr<WorldQueryResult>,8> histogramResults;
thread_local std::shared_ptr<WorldQuery> activeHistogram;
// Task-59 bounded pre-capture failure diagnostics: profile-only (--profile-engine),
// failures-only. Categories mirror previewObserveImmediateWorld bail stages:
// 0 attr, 1 state, 2 stream, 3 geometry, 4 bindings, 5 device, 6 exception.
// Smoke-identified failures cap 32 per category; unidentified (tex0 probe
// failed) failures cap 4 total; first 2 smoke successes log a reference.
// Normal runs never touch these and perform no extra guest reads.
std::atomic<unsigned> immediateFailLogged[7]{};
std::atomic<unsigned> immediateGenericLogged{0};
std::atomic<unsigned> immediateSmokeSuccessLogged{0};
void reportImmediateCapture(unsigned category,uint32_t smokeTex0,uint32_t device,uint32_t indicesAddr,uint32_t count,
    uint32_t beFlags,uint32_t boundId,uint32_t boundVal,uint32_t descriptor,const ImmediateCaptureReason* reason,bool haveAttr) {
    try {
        const bool smoke=smokeTex0==1477 || smokeTex0==1371;
        if(smoke) {
            if(category>=7 || immediateFailLogged[category].fetch_add(1,std::memory_order_relaxed)>=32)return;
        } else if(immediateGenericLogged.fetch_add(1,std::memory_order_relaxed)>=4)return;
        constexpr const char* stages[]{"attr","state","stream","geometry","bindings","device","exception"};
        constexpr const char* substages[]{"ok","count","headerRead","headerBounds","streamBounds","streamCopy",
            "indexCopy","indexOob","vertexDecode","retainIndices","exception"};
        const size_t sub=reason?size_t(reason->stage):0;
        std::fprintf(stderr,
            "[ImmediateCaptureFail] frame=%llu ms=%llu smoke=%u tex0=%u stage=%s device=%08X indices=%08X count=%u "
            "beFlags=%08X boundId=%u boundVal=%u descriptor=%08X sub=%s d0=%u d1=%u d2=%u badV=%u badS=%u badL=%u rawBE=%08X rawN=%u "
            "refKnown=%u ref=%u refPos=%u stride=%u vc=%u\n",
            frames.load(std::memory_order_relaxed),GetTickCount64(),unsigned(smoke),smokeTex0,
            category<7?stages[category]:"?",device,indicesAddr,count,
            haveAttr?beFlags:0u,boundId,boundVal,descriptor,
            sub<11?substages[sub]:"?",
            reason?reason->detail0:0u,reason?reason->detail1:0u,reason?reason->detail2:0u,
            reason?reason->badVertex:0u,reason?reason->badSlot:0u,reason?reason->badLane:0u,
            reason?reason->rawBE:0u,reason?reason->rawSize:0u,
            reason?unsigned(reason->membershipKnown):0u,reason?unsigned(reason->referenced):0u,reason?reason->firstRefPos:0u,
            reason?reason->stride:0u,reason?reason->vertexCount:0u);
        bool anyFormat=false;
        if(reason)for(auto f:reason->formats)if(f){anyFormat=true;break;}
        if(reason && (reason->vertexByteCount || anyFormat)) {
            char fhex[16*2+1]{},vhex[256*2+1]{};
            auto nibble=[](unsigned v){return char(v<10?'0'+v:'a'+v-10);};
            for(unsigned i=0;i<16;++i){fhex[i*2]=nibble(reason->formats[i]>>4);fhex[i*2+1]=nibble(reason->formats[i]&15);}
            const uint32_t bytes=reason->vertexByteCount<256?reason->vertexByteCount:256;
            for(uint32_t i=0;i<bytes;++i){vhex[i*2]=nibble(reason->vertexBytes[i]>>4);vhex[i*2+1]=nibble(reason->vertexBytes[i]&15);}
            std::fprintf(stderr,"[ImmediateCaptureBytes] frame=%llu tex0=%u formats=%s vertex=%s\n",
                frames.load(std::memory_order_relaxed),smokeTex0,fhex,vhex);
        }
    } catch (...) {}
}
size_t textureBytes = 0;
size_t pendingVideoBytes = 0;
// Keep the faulting instructions inside a call covered by the SEH scope.
// Clang may lower an intrinsic memcpy in __try to unprotected inline loads.
__declspec(noinline) void copyEngineBytes(void* output,const void* input,size_t size) {
    std::memcpy(output,input,size);
}
__declspec(noinline) bool equalEngineBytes(const void* expected,const void* input,size_t size) {
    return std::memcmp(expected,input,size)==0;
}
}

bool copyRenderMemory(uint8_t* base, uint64_t address, void* output, size_t size) {
    if (!base || !output || !address || address >= 0x100000000ull || size > 0x100000000ull-address) return false;
    // The AOT engine shares this process. Avoid a kernel transition for each
    // small draw-state read while still rejecting inaccessible/decommitted
    // ranges, including a decommit that races the copy. Callers publish only
    // complete snapshots and keep their existing source-stability checks.
    __try {
        copyEngineBytes(output,base+address,size);
        return true;
    } __except (GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
                ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        if(renderMemoryFaults.fetch_add(1,std::memory_order_relaxed)<4)
            std::fprintf(stderr,"[RenderMemoryFault] source=%08llX bytes=%zu\n",address,size);
        return false;
    }
}
bool equalRenderMemory(uint8_t* base,uint64_t address,const void* expected,size_t size) {
    if(!base || !expected || !address || address>=0x100000000ull || size>0x100000000ull-address)return false;
    __try {
        return equalEngineBytes(expected,base+address,size);
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        renderMemoryFaults.fetch_add(1,std::memory_order_relaxed);return false;
    }
}
void setPreviewTextureBudget(size_t bytes) {
    std::lock_guard lock(queueMutex);
    colorBudget=std::clamp(bytes,size_t(256*1024*1024),size_t(2ull*1024*1024*1024));
    std::fprintf(stderr,"[Performance] Native decoded texture cache budgetMiB=%zu\n",colorBudget/(1024*1024));
}

uint32_t worldTextureTiledOffset(uint32_t x,uint32_t y,uint32_t width,uint32_t bytesPerBlock) {
    // Native inverse of the original CPU texture upload's 32x32 block layout.
    // The original AOT tile routine828AFB00 is the independent test oracle.
    const uint32_t log=std::countr_zero(bytesPerBlock),aligned=(width+31)&~31u;
    const uint32_t macro=((x>>5)+(y>>5)*(aligned>>5))<<(log+7);
    const uint32_t micro=((x&7)+((y&6)<<2))<<log;
    const uint32_t offset=macro+((micro&~15u)<<1)+(micro&15)+((y&8)<<(log+3))+((y&1)<<4);
    return ((offset&~511u)<<3)+((offset&448)<<2)+(offset&63)+((y&16)<<7)+((((y&8)>>2)+(x>>3)&3)<<6);
}

const char* decodeSimpleMesh(uint8_t* base, uint32_t descriptor, uint32_t indexAddress,
                             uint32_t triangles, SimpleMesh& result, bool allowWhiteColor) {
    if (!triangles || triangles >= 65535) return "empty or batched triangle request";
    uint8_t header[68];
    if (!copyRenderMemory(base, descriptor, header, sizeof(header))) return "unreadable vertex descriptor";
    const uint32_t count = u16(header);
    if (!count || count > 16384 || triangles > 16384) return "preview geometry budget exceeded";
    if (u16(header + 2) || u32(header + 48) || u32(header + 56) || u32(header + 60) || u32(header + 64))
        return "additional vertex streams are unsupported";
    for (unsigned stage = 1; stage < 8; ++stage)
        if (u32(header + 8 + stage * 4) || header[40 + stage]) return "multiple UV streams are unsupported";
    const bool colored = u32(header + 52) != 0;
    if (!u32(header + 4) || !u32(header + 8) || header[40] != 2 || (!colored && !allowWhiteColor))
        return "preview requires float3 position, float2 UV0 and vertex color";
    std::vector<uint8_t> positions(count * 12), uv(count * 8), colors(count * 4), indices(triangles * 6);
    if (!copyRenderMemory(base, u32(header + 4), positions.data(), positions.size()) ||
        !copyRenderMemory(base, u32(header + 8), uv.data(), uv.size()) ||
        (colored && !copyRenderMemory(base, u32(header + 52), colors.data(), colors.size())) ||
        !copyRenderMemory(base, indexAddress, indices.data(), indices.size())) return "unreadable geometry stream";
    SimpleMesh decoded;
    decoded.vertices.resize(count);
    decoded.indices.resize(triangles * 3);
    for (size_t i = 0; i < decoded.indices.size(); ++i) {
        decoded.indices[i] = u16(indices.data() + i * 2);
        if (decoded.indices[i] >= count) return "triangle index outside vertex stream";
    }
    for (uint32_t i = 0; i < count; ++i) {
        auto& vertex = decoded.vertices[i];
        for (unsigned j = 0; j < 3; ++j) {
            vertex.position[j] = f32(positions.data() + i * 12 + j * 4);
            if (!std::isfinite(vertex.position[j])) return "nonfinite vertex position";
        }
        for (unsigned j = 0; j < 2; ++j) {
            vertex.uv[j] = f32(uv.data() + i * 8 + j * 4);
            if (!std::isfinite(vertex.uv[j])) return "nonfinite UV";
        }
        // Guest D3DCOLOR bytes are A,R,G,B. Vertex colors use UNORM, unlike
        // the engine's separate clear-color conversion, which uses 1/256.
        if (!colored) {
            std::fill(std::begin(vertex.color), std::end(vertex.color), 1.0f);
            continue;
        }
        vertex.color[0] = colors[i * 4 + 1] / 255.0f;
        vertex.color[1] = colors[i * 4 + 2] / 255.0f;
        vertex.color[2] = colors[i * 4 + 3] / 255.0f;
        vertex.color[3] = colors[i * 4] / 255.0f;
    }
    result = std::move(decoded);
    return nullptr;
}

static const char* decodePlane(uint8_t* base, uint32_t image, uint32_t expectedFormat,
                               uint32_t bytesPerPixel, AlphaImage& result) {
    uint8_t header[48];
    if (!copyRenderMemory(base, image, header, sizeof(header))) return "unreadable CImage";
    const uint32_t width = u32(header + 16), height = u32(header + 20), pitch = u32(header + 24);
    const uint32_t flags = u32(header + 40), format = u32(header + 32);
    if (u32(header) != 0x82097610 || format != expectedFormat || u32(header + 28) != bytesPerPixel ||
        !(flags & 0x800) || (flags & 0x1200)) return "image is not the required linear CPU plane";
    if (!width || !height || width > 2048 || height > 2048 || pitch < uint64_t(width) * bytesPerPixel || pitch > 8192 ||
        uint64_t(pitch) * height > u32(header + 12)) return "invalid image extent or pitch";
    // Same alignment rules as original CImage lock 82788B60.
    const uint64_t allocationAddress = u32(header + 8);
    uint64_t address = allocationAddress;
    if (flags & 0x200000) address = (address + 31) & ~31ull;
    else if (flags & 0x100000) address = (address + 15) & ~15ull;
    const uint64_t size = uint64_t(pitch) * height;
    if (address - allocationAddress + size > u32(header + 12)) return "aligned image exceeds its storage";
    if (!address || address + size > 0x100000000ull) return "image pointer overflow";
    std::vector<uint8_t> source(size);
    if (!copyRenderMemory(base, uint32_t(address), source.data(), source.size())) return "unreadable image pixels";
    AlphaImage decoded;
    decoded.width = width; decoded.height = height;
    const uint32_t rowBytes = width * bytesPerPixel;
    decoded.pixels.resize(size_t(rowBytes) * height);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(decoded.pixels.data() + y * rowBytes, source.data() + y * pitch, rowBytes);
    result = std::move(decoded);
    return nullptr;
}

const char* decodeAlphaImage(uint8_t* base, uint32_t image, AlphaImage& result) {
    return decodePlane(base, image, 0x40000, 1, result);
}

static const char* decodeColorBlocks(const std::vector<uint8_t>& blocks,uint32_t width,uint32_t height,uint32_t codec,ColorImage& result);
const char* decodeColorImage(uint8_t* base, uint32_t image, ColorImage& result) {
    uint8_t header[48], stream[16];
    if (!copyRenderMemory(base, image, header, sizeof(header))) return "unreadable color CImage";
    const uint32_t width = u32(header + 16), height = u32(header + 20), flags = u32(header + 40);
    // Original 82257450 selects the 16-byte block header with 0x4000.
    // Compressed images do NOT require the linear-image 0x800 flag.
    // Other streams and linear color layouts require separate reconstruction.
    if (u32(header) != 0x82097610 || u32(header + 28) != 4 || u32(header + 32) != 0x800 ||
        (flags & 0x15200) != 0x5000) return "unsupported color image layout";
    if (!width || !height || width > 2048 || height > 2048) return "invalid color image dimensions";
    const uint32_t allocation = u32(header + 8), storage = u32(header + 12);
    if (storage < 16 || !copyRenderMemory(base, allocation, stream, sizeof(stream))) return "unreadable block header";
    auto le32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                                           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
    const uint32_t codec = le32(stream), length = le32(stream + 4), offset = le32(stream + 12);
    // Only observed non-premultiplied codecs are accepted. Original switch
    // maps 0 to DXT1 (8-byte blocks), 1/2 to DXT3, and 3/4 to DXT5.
    if (codec != 0 && codec != 4) return "unsupported block codec";
    const uint32_t blockSize = codec == 0 ? 8 : 16;
    const uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
    const uint64_t expected = uint64_t(blocksX) * blocksY * blockSize;
    if (offset != 16 || le32(stream + 8) || length != expected || uint64_t(offset) + length > storage ||
        uint64_t(allocation) + offset + length > 0x100000000ull) return "invalid compressed image extent";
    std::vector<uint8_t> blocks(length);
    if (!copyRenderMemory(base, allocation + offset, blocks.data(), blocks.size())) return "unreadable compressed pixels";
    return decodeColorBlocks(blocks,width,height,codec,result);
}
static const char* decodeColorBlocks(const std::vector<uint8_t>& blocks,uint32_t width,uint32_t height,uint32_t codec,ColorImage& result) {
    const uint32_t blockSize=codec==0?8:16,blocksX=(width+3)/4,blocksY=(height+3)/4;
    if((codec!=0 && codec!=4) || blocks.size()!=uint64_t(blocksX)*blocksY*blockSize)return "invalid block image size";
    auto le32=[](const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;};
    ColorImage decoded;
    decoded.width = width; decoded.height = height; decoded.sourceCodec = codec;
    decoded.pixels.resize(size_t(width) * height * 4);
    // BC1/BC3 bit layout and interpolation follow the Direct3D block-compression contract:
    // https://learn.microsoft.com/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression
    // Decode before the original uploader's temporary byte swap. Cropping only
    // unused edge texels preserves the original non-multiple-of-four dimensions.
    if(!parallelSceneRange(size_t(blocksX)*blocksY,1024,[&](size_t first,size_t end) {
    for(size_t blockIndex=first;blockIndex<end;++blockIndex) {
        const auto by=uint32_t(blockIndex/blocksX),bx=uint32_t(blockIndex%blocksX);
        const uint8_t* block = blocks.data() + (size_t(by) * blocksX + bx) * blockSize;
        const uint8_t* color = block + (codec == 4 ? 8 : 0);
        const uint16_t c0 = uint16_t(color[0]) | uint16_t(color[1]) << 8;
        const uint16_t c1 = uint16_t(color[2]) | uint16_t(color[3]) << 8;
        uint8_t palette[4][4]{};
        for (unsigned i = 0; i < 2; ++i) {
            const uint16_t c = i ? c1 : c0;
            const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            palette[i][0] = uint8_t((r << 3) | (r >> 2));
            palette[i][1] = uint8_t((g << 2) | (g >> 4));
            palette[i][2] = uint8_t((b << 3) | (b >> 2));
            palette[i][3] = 255;
        }
        if (codec == 4 || c0 > c1) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                palette[2][channel] = uint8_t((2 * palette[0][channel] + palette[1][channel]) / 3);
                palette[3][channel] = uint8_t((palette[0][channel] + 2 * palette[1][channel]) / 3);
            }
            palette[2][3] = palette[3][3] = 255;
        } else {
            for (unsigned channel = 0; channel < 3; ++channel)
                palette[2][channel] = uint8_t((palette[0][channel] + palette[1][channel]) / 2);
            palette[2][3] = 255; // BC1 index 3 stays transparent black.
        }
        uint8_t alpha[8]{};
        uint64_t alphaIndices = 0;
        if (codec == 4) {
            alpha[0] = block[0]; alpha[1] = block[1];
            if (alpha[0] > alpha[1]) {
                for (unsigned i = 1; i <= 6; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
            } else {
                for (unsigned i = 1; i <= 4; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
                alpha[6] = 0; alpha[7] = 255;
            }
            for (unsigned i = 0; i < 6; ++i) alphaIndices |= uint64_t(block[i + 2]) << (i * 8);
        }
        const uint32_t colorIndices = le32(color + 4);
        for (unsigned y = 0; y < 4 && by * 4 + y < height; ++y)
            for (unsigned x = 0; x < 4 && bx * 4 + x < width; ++x) {
                const unsigned index = y * 4 + x;
                uint8_t* out = decoded.pixels.data() + (size_t(by * 4 + y) * width + bx * 4 + x) * 4;
                std::memcpy(out, palette[(colorIndices >> (index * 2)) & 3], 4);
                if (codec == 4) out[3] = alpha[(alphaIndices >> (index * 3)) & 7];
            }
    }
    return true;
    }))return "block image decode failed";
    result = std::move(decoded);
    return nullptr;
}

const char* decodeWorldTextureImage(uint8_t* base,uint32_t object,ColorImage& result,unsigned firstMip) {
    uint8_t header[64]{};
    if(!copyRenderMemory(base,object,header,sizeof(header)))return "unreadable texture resource";
    uint32_t fetch[6]{};for(unsigned i=0;i<6;++i)fetch[i]=u32(header+28+i*4);
    const uint32_t baseWidth=(fetch[2]&8191)+1,baseHeight=((fetch[2]>>13)&8191)+1;
    if(baseWidth>2048 || baseHeight>2048)return "unsupported streamed texture dimensions";
    const uint32_t format=fetch[1]&63,endian=(fetch[1]>>6)&3,swizzle=fetch[3];
    const bool compressed=format==18 || format==20 || format==49,tiled=(fetch[0]&0x80000000u)!=0;
    const unsigned lastMip=(std::min)((fetch[4]>>6)&15,unsigned(std::bit_width((std::max)(baseWidth,baseHeight)))-1u);
    if(firstMip>lastMip)return "resident mip range exceeds resource";
    ColorImage image;image.width=baseWidth;image.height=baseHeight;image.authoredMips=true;
    image.firstMip=firstMip;image.mips.resize(lastMip);
    for(unsigned mip=firstMip;mip<=lastMip;++mip) {
        TextureMipLayout layout;
        if(const auto error=getTextureMipLayout(fetch,0,mip,layout))return error;
        const unsigned width=layout.width,height=layout.height,faces=layout.faces;
        const unsigned bw=layout.blocksWide,bh=layout.blocksHigh,bytes=layout.bytesPerBlock;
        const uint64_t sourceBytes=uint64_t(layout.faceStrideBytes)*faces;
        if(sourceBytes>32*1024*1024)return "streamed texture extent overflow";
        std::vector<uint8_t> linear(size_t(bw)*bh*bytes);
        std::vector<uint8_t> combined;combined.reserve(size_t(width)*height*4*faces);
        for(unsigned face=0;face<faces;++face) {
            // Fast path: copy the whole face once (single SEH guard) and swizzle
            // from that snapshot without further kernel transitions. Padding gaps
            // can be uncommitted in sparse suballocations; on failure fall back
            // to per-block reads that touch only addressed blocks.
            const uint64_t faceBase=uint64_t(layout.allocationAddress)+layout.surfaceOffsetBytes+
                uint64_t(face)*layout.faceStrideBytes;
            std::vector<uint8_t> faceSnapshot;
            bool haveSnapshot=false;
            if(layout.faceStrideBytes && layout.faceStrideBytes<=8*1024*1024 &&
               faceBase+layout.faceStrideBytes<=0x100000000ull) {
                faceSnapshot.resize(layout.faceStrideBytes);
                if(copyRenderMemory(base,uint32_t(faceBase),faceSnapshot.data(),faceSnapshot.size()))
                    haveSnapshot=true;
                else
                    faceSnapshot.clear();
            }
            auto readSpan=[&](uint32_t alignedOffset,unsigned span,uint8_t* output)->bool {
                if(uint64_t(alignedOffset)+span>layout.faceStrideBytes) return false;
                if(haveSnapshot) {
                    std::memcpy(output,faceSnapshot.data()+alignedOffset,span);
                    return true;
                }
                return copyRenderMemory(base,uint32_t(faceBase+alignedOffset),output,span);
            };
            for(unsigned y=0;y<bh;++y)for(unsigned x=0;x<bw;++x) {
                const unsigned sx=x+layout.originBlockX,sy=y+layout.originBlockY;
                const uint32_t offset=tiled?worldTextureTiledOffset(sx,sy,layout.pitchBlocks,bytes):
                    sy*layout.rowPitchBytes+sx*bytes;
                if(uint64_t(offset)+bytes>layout.faceStrideBytes)return "texture block escaped face allocation";
                // Read only addressed blocks. Original suballocation can leave
                // gaps in padded faces; accessibility of a neighboring page is
                // not evidence that its padding belongs to this texture.
                const unsigned swap=endian==1?1:endian==2?3:endian==3?2:0;
                const unsigned span=(std::max)(bytes,1u<<std::bit_width(swap));
                const unsigned alignedOffset=offset&~(span-1);
                uint8_t input[16]{};
                if(!readSpan(alignedOffset,span,input))
                    return "unreadable streamed texture pixels";
                for(unsigned lane=0;lane<bytes;++lane) {
                    const unsigned swapped=(offset+lane)^swap;
                    if(swapped>=layout.faceStrideBytes)return "endian-swapped byte escaped texture extent";
                    linear[(size_t(y)*bw+x)*bytes+lane]=input[swapped-alignedOffset];
                }
            }
            ColorImage decoded;
            if(format==2) {
                decoded.width=width;decoded.height=height;decoded.sourceCodec=2;decoded.pixels.resize(size_t(width)*height*4);
                for(size_t i=0;i<linear.size();++i) {decoded.pixels[i*4]=linear[i];decoded.pixels[i*4+3]=255;}
            } else if(format==49) {
                decoded.width=width;decoded.height=height;decoded.sourceCodec=49;decoded.pixels.resize(size_t(width)*height*4);
                auto channel=[](const uint8_t* p,std::array<uint8_t,16>& out) {
                    uint8_t table[8]{p[0],p[1]};
                    if(p[0]>p[1])for(unsigned k=1;k<=6;++k)table[k+1]=uint8_t(((7-k)*p[0]+k*p[1])/7);
                    else {for(unsigned k=1;k<=4;++k)table[k+1]=uint8_t(((5-k)*p[0]+k*p[1])/5);table[6]=0;table[7]=255;}
                    uint64_t indices=0;for(unsigned k=0;k<6;++k)indices|=uint64_t(p[k+2])<<(8*k);
                    for(unsigned k=0;k<16;++k)out[k]=table[(indices>>(3*k))&7];
                };
                for(unsigned by=0;by<bh;++by)for(unsigned bx=0;bx<bw;++bx) {
                    const auto* p=linear.data()+(size_t(by)*bw+bx)*16;std::array<uint8_t,16> red{},green{};
                    channel(p,red);channel(p+8,green);
                    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(bx*4+x<width && by*4+y<height) {
                        auto* pixel=decoded.pixels.data()+(size_t(by*4+y)*width+bx*4+x)*4;
                        pixel[0]=red[y*4+x];pixel[1]=green[y*4+x];pixel[2]=0;pixel[3]=255;
                    }
                }
            } else if(compressed) {
                if(const auto error=decodeColorBlocks(linear,width,height,format==18?0:4,decoded))return error;
            } else {decoded.width=width;decoded.height=height;decoded.sourceCodec=format;decoded.pixels=linear;}
            // The original resource's component selectors include constants0/1.
            for(size_t i=0;i<decoded.pixels.size();i+=4) {
                const std::array<uint8_t,6> channels{decoded.pixels[i],decoded.pixels[i+1],decoded.pixels[i+2],decoded.pixels[i+3],0,255};
                for(unsigned c=0;c<4;++c) {
                    const auto select=(swizzle>>(1+c*3))&7;
                    if(select>=channels.size())return "unsupported texture component selector";
                    decoded.pixels[i+c]=channels[select];
                }
            }
            image.sourceCodec=decoded.sourceCodec;
            combined.insert(combined.end(),decoded.pixels.begin(),decoded.pixels.end());
        }
        image.faces=faces;
        if(!mip)image.pixels=std::move(combined);else image.mips[mip-1]=std::move(combined);
    }
    result=std::move(image);return nullptr;
}

const char* decodeVideoFrame(uint8_t* base, uint32_t frame, VideoFrame& result) {
    if (!frame || uint64_t(frame) + 108 > 0x100000000ull) return "video frame pointer overflow";
    uint8_t time[8];
    if (!copyRenderMemory(base, frame, time, sizeof(time))) return "unreadable video timestamp";
    const double timestamp = std::bit_cast<double>((uint64_t(u32(time)) << 32) | u32(time + 4));
    if (!std::isfinite(timestamp) || timestamp < 0) return "invalid video timestamp";
    AlphaImage y, uv;
    if (const char* error = decodePlane(base, frame + 12, 0x2000, 1, y)) return error;
    if (const char* error = decodePlane(base, frame + 60, 0x20000, 2, uv)) return error;
    if (uv.width * 2 != y.width || uv.height * 2 != y.height) return "video chroma plane size mismatch";
    VideoFrame decoded{y.width, y.height, std::move(y.pixels), std::move(uv.pixels)};
    decoded.timestampSeconds = timestamp;
    result = std::move(decoded);
    return nullptr;
}

void previewObserveVideo(uint8_t* base, uint32_t container, uint32_t localId) {
    if (!active || localId % 3 != 2) return;
    uint8_t header[64], array[28], video[136];
    if (!copyRenderMemory(base, container, header, sizeof(header)) || u32(header) != 0x82097C50 ||
        !copyRenderMemory(base, u32(header + 56), array, sizeof(array)) || localId / 3 >= u32(array + 4)) return;
    const uint64_t entry = uint64_t(u32(array + 24)) + (localId / 3) * 4ull;
    uint32_t owner = 0;
    if (entry + 4 > 0x100000000ull || !readWord(base, uint32_t(entry), owner) ||
        !copyRenderMemory(base, owner, video, sizeof(video)) || u32(video) != 0x82097CF8) return;
    const uint32_t frame = u32(video + 80), key = u32(video + 126);
    std::array<uint8_t, 8> timestamp;
    if (!frame || !u16(video + 126) || !u16(video + 128) ||
        !copyRenderMemory(base, frame, timestamp.data(), timestamp.size())) return;
    {
        std::lock_guard lock(queueMutex);
        if (auto it = videos.find(key); it != videos.end() && it->second.owner == owner &&
            it->second.frame == frame && it->second.timestamp == timestamp) return;
    }
    auto image = std::make_shared<VideoFrame>();
    if (decodeVideoFrame(base, frame, *image)) return;
    // The original container retains this selected frame until its next
    // update on the engine thread. Only owned plane copies cross threads.
    std::lock_guard lock(queueMutex);
    if (!videos.contains(key) && videos.size() >= 4) videos.erase(videos.begin());
    videos[key] = {owner, frame, timestamp, image};
    if (++videoImages == 1) std::printf("[EnginePreview] Copied original YUV video frame %ux%u, texture pair=%08X\n",
                                     image->width, image->height, key);
}

void enableEnginePreview() { active = true; }
bool enginePreviewEnabled() { return active.load(std::memory_order_relaxed); }
void previewPrepareTexture(uint32_t id) {
    if (!active || !id) return;
    std::lock_guard lock(queueMutex);
    alphaColors.erase(id);
    if (auto it = textures.find(id); it != textures.end()) {
        textureBytes -= it->second->pixels.size(); textures.erase(it);
    }
    if (auto it = colorTextures.find(id); it != colorTextures.end()) {
        colorBytes -= it->second.image->bytes(); colorTextures.erase(it);
    }
}
std::shared_ptr<const ColorImage> previewCapturedTexture(uint32_t id,uint32_t resource) {
    std::lock_guard lock(queueMutex);
    const auto it=colorTextures.find(id);
    return it!=colorTextures.end() && it->second.resource==resource?it->second.image:nullptr;
}
void previewPublishTexture(uint32_t id,uint32_t resource,std::shared_ptr<const ColorImage> image) {
    if(!active || !id || !resource || !image || !image->valid())return;
    std::lock_guard lock(queueMutex);
    if(image->bytes()>colorBudget)return;
    if(auto it=colorTextures.find(id);it!=colorTextures.end()) {colorBytes-=it->second.image->bytes();colorTextures.erase(it);}
    while(colorBytes+image->bytes()>colorBudget && !colorTextures.empty()) {
        auto oldest=std::min_element(colorTextures.begin(),colorTextures.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
        colorBytes-=oldest->second.image->bytes();colorTextures.erase(oldest);
    }
    colorBytes+=image->bytes();colorTextures[id]={image,++colorUse,resource};++colorImages;
    if (TextureUploadTraceEnabled())
        std::fprintf(stderr,"[EngineImageUpload] authored id=%u resource=%08X size=%ux%u faces=%u first=%u levels=%zu bytes=%zu\n",
            id,resource,image->width,image->height,image->faces,image->firstMip,image->mips.size()+1,image->bytes());
}
std::shared_ptr<const ColorImage> promptReplacementFor(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& image, PromptRenderSource source, PromptRenderContext context) {
    const auto nativeSource = source == PromptRenderSource::Controller ?
        Prompts::Source::Controller : Prompts::Source::KeyboardMouse;
    const auto nativeContext = context == PromptRenderContext::Gameplay ?
        Prompts::Context::Gameplay : Prompts::Context::Menu;
    return Prompts::replacementFor(textureId, image, nativeSource, nativeContext);
}
std::shared_ptr<const ColorImage> promptReplacementForUv(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& image, PromptRenderSource source, PromptRenderContext context,
    float u0, float v0, float u1, float v1) {
    const auto nativeSource = source == PromptRenderSource::Controller ?
        Prompts::Source::Controller : Prompts::Source::KeyboardMouse;
    const auto nativeContext = context == PromptRenderContext::Gameplay ?
        Prompts::Context::Gameplay : Prompts::Context::Menu;
    return Prompts::replacementForUv(textureId, image, nativeSource, nativeContext, u0, v0, u1, v1);
}
std::atomic<uint64_t> gPromptAttempts{0}, gPromptClassified{0};
void countPromptOriginAttempt(bool classified) {
    const uint64_t attempts = ++gPromptAttempts;
    if (classified) ++gPromptClassified;
    if ((attempts & 255) == 0)
        std::fprintf(stderr, "[PromptOrigin] attempts=%llu classified=%llu\n",
            (unsigned long long)attempts, (unsigned long long)gPromptClassified.load());
}
void printPromptOriginCounters() {
    std::fprintf(stderr, "[PromptOrigin] attempts=%llu classified=%llu\n",
        (unsigned long long)gPromptAttempts.load(), (unsigned long long)gPromptClassified.load());
}
uint64_t promptOriginAttempts() { return gPromptAttempts.load(); }
uint64_t promptOriginClassified() { return gPromptClassified.load(); }
namespace {
struct PromptPending { uint32_t image = 0; uint8_t origin = 0; bool valid = false; };
thread_local PromptPending pendingPrompt;
thread_local PromptPending pendingStack[8];
thread_local unsigned pendingDepth = 0;
thread_local unsigned pendingOverflow = 0;
}
void previewBeginPromptCapture(uint32_t imageAddress) {
    if (pendingDepth < 8) {
        pendingStack[pendingDepth++] = pendingPrompt;
        pendingPrompt.image = imageAddress;
        pendingPrompt.origin = 0;
        pendingPrompt.valid = false;
    } else {
        // Fail closed: leave the saved depth-8 state untouched. Commits and
        // consumes while overflowed yield unknown without storing or clearing.
        ++pendingOverflow;
    }
}
bool previewCommitPromptOrigin(uint32_t imageAddress, uint8_t origin) {
    if (!imageAddress) return false;
    if (!pendingDepth && !pendingOverflow) return false;
    if (pendingOverflow) return false;
    if (!pendingPrompt.valid || pendingPrompt.image == imageAddress || pendingPrompt.image == 0) {
        pendingPrompt.image = imageAddress;
        pendingPrompt.origin = origin;
        pendingPrompt.valid = true;
        return true;
    }
    return false;
}
uint8_t takePromptOriginFor(uint32_t imageAddress) {
    if (pendingOverflow) return 0;
    uint8_t origin = 0;
    if (pendingPrompt.valid && pendingPrompt.image == imageAddress) origin = pendingPrompt.origin;
    pendingPrompt = {};
    return origin;
}
void previewEndPromptCapture() {
    if (pendingOverflow) { --pendingOverflow; return; }
    if (pendingDepth) pendingPrompt = pendingStack[--pendingDepth];
    else pendingPrompt = {};
}
void previewSetPendingPromptOrigin(uint32_t imageAddress, uint8_t origin) {
    previewCommitPromptOrigin(imageAddress, origin);
}
void previewObserveImage(uint8_t* base, uint32_t address, uint32_t id, uint32_t mip) {
    const uint8_t scopedOrigin = takePromptOriginFor(address);
    if (!active || !id || mip) return;
    auto image = std::make_shared<AlphaImage>();
    if (decodeAlphaImage(base, address, *image)) {
        auto color = std::make_shared<ColorImage>();
        if (decodeColorImage(base, address, *color)) { ++rejectedImages; return; }
        color->promptOrigin = scopedOrigin;
        std::lock_guard lock(queueMutex);
        if (auto it = colorTextures.find(id); it != colorTextures.end()) {
            colorBytes -= it->second.image->bytes(); colorTextures.erase(it);
        }
        while (colorBytes + color->bytes() > colorBudget && !colorTextures.empty()) {
            auto oldest = std::min_element(colorTextures.begin(), colorTextures.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            colorBytes -= oldest->second.image->bytes(); colorTextures.erase(oldest);
        }
        colorBytes += color->bytes(); colorTextures[id] = {color, ++colorUse}; ++colorImages;
        if (TextureUploadTraceEnabled())
            std::printf("[EnginePreview] Copied original color texture id=%u size=%ux%u codec=%u\n",
                        id, color->width, color->height, color->sourceCodec);
        return;
    }
    std::lock_guard lock(queueMutex);
    if (textureBytes + image->pixels.size() > 32 * 1024 * 1024) { ++rejectedImages; return; }
    if (auto it = textures.find(id); it != textures.end()) textureBytes -= it->second->pixels.size();
    textureBytes += image->pixels.size(); textures[id] = image; alphaColors.erase(id); ++images;
    if (TextureUploadTraceEnabled())
        std::printf("[EnginePreview] Copied original alpha texture id=%u size=%ux%u\n", id, image->width, image->height);
}
const char* decodeUploadImage(uint8_t* base,uint32_t address,uint32_t pixels,ColorImage& result) {
    uint8_t header[48]{};
    if (!copyRenderMemory(base,address,header,sizeof(header))) return "unreadable upload CImage";
    const auto width=u32(header+16),height=u32(header+20),format=u32(header+32),flags=u32(header+40);
    if(flags&0x200)return "upload CImage is metadata only";
    const bool alpha=format==0x40000 && u32(header+28)==1 && (flags&0x15800)==0x800;
    const bool luminance=format==0x2000 && u32(header+28)==1 && (flags&0x15800)==0x800;
    const bool dxn=format==0x20000 && u32(header+28)==2 && (flags&0x15000)==0x11000;
    const bool rgb=(format==0x40 || format==0x800) && u32(header+28)==4 && (flags&0x15800)==0x800;
    if (!alpha && !luminance && !dxn && !rgb) {
        if(uint64_t(u32(header+8))+16!=pixels)return "upload pixels do not identify owned BC stream";
        return decodeColorImage(base,address,result);
    }
    if (u32(header)!=0x82097610 || !width || !height || width>2048 || height>2048) return "invalid upload image layout";
    const uint32_t allocation=u32(header+8),storage=u32(header+12);
    const uint32_t pitch=u32(header+24);
    if (!dxn && (pitch<uint64_t(width)*(rgb?4:1) || pitch>8192)) return "invalid upload pitch";
    const size_t bytes=dxn?size_t((width+3)/4)*((height+3)/4)*16:size_t(pitch)*height;
    if (pixels<allocation || uint64_t(pixels)+bytes>uint64_t(allocation)+storage || uint64_t(pixels)+bytes>0x100000000ull)
        return "upload pixels escape owned CImage allocation";
    std::vector<uint8_t> input(bytes);
    if (!copyRenderMemory(base,pixels,input.data(),bytes)) return "unreadable upload pixels";
    ColorImage image;image.width=width;image.height=height;image.sourceCodec=alpha?0x40000:dxn?49:rgb?format:2;image.pixels.resize(size_t(width)*height*4);
    if (rgb) {
        // 8226977C/B8 map CImage0x40/0x800 to XDK X8R8G8B8/A8R8G8B8.
        // This observer runs before the temporary 82257E8C word byte-swap;
        // original CImage storage is B,G,R,X/A, with row pitch retained.
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto* in=input.data()+size_t(y)*pitch+x*4;auto* out=image.pixels.data()+(size_t(y)*width+x)*4;
            out[0]=in[2];out[1]=in[1];out[2]=in[0];out[3]=format==0x800?in[3]:255;
        }
    } else if (alpha) {
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const size_t i=size_t(y)*width+x;image.pixels[i*4]=image.pixels[i*4+1]=image.pixels[i*4+2]=255;
            image.pixels[i*4+3]=input[size_t(y)*pitch+x];
        }
    } else if (luminance) {
        // 82269700 maps CImage format0x2000 to XDK L8=0x28000102.
        for (unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const size_t i=size_t(y)*width+x;image.pixels[i*4]=image.pixels[i*4+1]=image.pixels[i*4+2]=input[size_t(y)*pitch+x];image.pixels[i*4+3]=255;
        }
    } else {
        auto block=[](const uint8_t* p,std::array<uint8_t,16>& values) {
            uint8_t table[8]{p[0],p[1]};
            if (p[0]>p[1]) for (unsigned k=1;k<=6;++k) table[k+1]=uint8_t(((7-k)*p[0]+k*p[1])/7);
            else {for (unsigned k=1;k<=4;++k)table[k+1]=uint8_t(((5-k)*p[0]+k*p[1])/5);table[6]=0;table[7]=255;}
            uint64_t indices=0;for(unsigned k=0;k<6;++k)indices|=uint64_t(p[k+2])<<(8*k);
            for(unsigned k=0;k<16;++k)values[k]=table[(indices>>(3*k))&7];
        };
        const unsigned blocksX=(width+3)/4;
        for (unsigned by=0;by<(height+3)/4;++by) for(unsigned bx=0;bx<blocksX;++bx) {
            const auto* p=input.data()+(size_t(by)*blocksX+bx)*16;
            std::array<uint8_t,16> luma{},alpha{};block(p,luma);block(p+8,alpha);
            // Original82257450 chooses0x08000171: DXN/BC5 with component
            // selectors0,0,0,1 (bits18,21,24,27). Match its L,L,L,A view.
            for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(bx*4+x<width && by*4+y<height) {
                auto* out=image.pixels.data()+(size_t(by*4+y)*width+bx*4+x)*4;const auto k=y*4+x;
                out[0]=out[1]=out[2]=luma[k];out[3]=alpha[k];
            }
        }
    }
    result=std::move(image);return nullptr;
}
static bool submitTriangles(uint8_t* base, uint32_t indices, uint32_t triangles) {
    if (!active || worldActive) return false;
    ++observed;
    // 65535 describes a list of child requests. Original recursion reaches
    // these wrappers again; the parent must not duplicate its child meshes.
    if (triangles == 65535 || !triangles || !indices) return false;
    uint8_t attributes[160], projection[64], model[64], viewport[16];
    uint32_t matrixBase = 0, matrixIndex = 0, descriptor = 0, vbId = 0;
    auto reject = [&](unsigned reason) { ++unsupported; ++rejectionReasons[reason]; return false; };
    if (!copyRenderMemory(base, context + 16896, attributes, sizeof(attributes)) ||
        !copyRenderMemory(base, context + 17088, projection, sizeof(projection)) ||
        !copyRenderMemory(base, context + 17152, viewport, sizeof(viewport)) ||
        !readWord(base, context + 8224, matrixBase) || !readWord(base, context + 8232, matrixIndex) ||
        !readWord(base, context + 16512, descriptor) || !readWord(base, context + 12564, vbId) ||
        matrixIndex >= 256 || uint64_t(matrixBase) + matrixIndex * 656ull + 80 > 0x100000000ull ||
        !copyRenderMemory(base, matrixBase + matrixIndex * 656 + 16, model, 64)) return reject(0);
    // Original 8225D780 selects the explicit CPU descriptor before the
    // stored-buffer ID. The ID may remain set while immediate data is active.
    // Inline data is a fallback only when both selectors are absent.
    if (!descriptor && vbId) return reject(1);
    // Initial bring-up supports the captured fixed-function text material:
    // identity model-view, one alpha texture, vertex tint and alpha blending.
    // Other material/pass contracts stay explicitly counted as unsupported.
    const bool textMaterial = !u32(attributes) && !u32(attributes + 4) && u32(attributes + 92) == 0x01100218 &&
                             attributes[144] == 5 && attributes[145] == 6;
    const bool opaqueMaterial = !u32(attributes) && !u32(attributes + 4) && u32(attributes + 92) == 0x01100212 &&
                               attributes[144] == 2 && attributes[145] == 1;
    bool videoMaterial = false;
    if (u32(attributes) && !u32(attributes + 4) && u32(attributes + 92) == 0x01100212 &&
        attributes[144] == 2 && attributes[145] == 1 && u32(attributes + 108) == 0xFFFFFFFF &&
        u32(attributes + 116) == 0xFFFFFFFF) {
        uint8_t program[20];
        constexpr char name[] = "CMWnd_ModTexture_PaintVideo_YUV2RGB";
        char actual[sizeof(name)];
        videoMaterial = copyRenderMemory(base, u32(attributes), program, sizeof(program)) && u32(program) == 5 &&
            !u32(program + 8) && !u32(program + 12) && !u32(program + 16) &&
            copyRenderMemory(base, u32(program + 4), actual, sizeof(actual)) && !std::memcmp(actual, name, sizeof(name));
    }
    if (!textMaterial && !opaqueMaterial && !videoMaterial) return reject(2);
    const auto displayMode = nativeVideoMode();
    const bool originalViewport = u32(viewport + 8) == 1280 && u32(viewport + 12) == 720;
    const bool nativeViewport = u32(viewport + 8) == displayMode.width && u32(viewport + 12) == displayMode.height;
    if (u32(viewport) || u32(viewport + 4) || (!originalViewport && !nativeViewport))
        return reject(3);
    for (unsigned stage = videoMaterial ? 2 : 1; stage < 16; ++stage)
        if (u16(attributes + 8 + stage * 2)) return reject(4);
    for (unsigned i = 0; i < 16; ++i)
        if (f32(model + i * 4) != (i % 5 == 0 ? 1.0f : 0.0f)) return reject(5);
    std::shared_ptr<const AlphaImage> texture;
    std::shared_ptr<const VideoFrame> video;
    std::shared_ptr<const ColorImage> colorTexture;
    {
        std::lock_guard lock(queueMutex);
        if (videoMaterial) {
            auto it = videos.find(u32(attributes + 8));
            if (it != videos.end()) video = it->second.image;
        } else {
            auto it = textures.find(u16(attributes + 8));
            if (textMaterial && it != textures.end()) texture = it->second;
            auto color = colorTextures.find(u16(attributes + 8));
            if (!texture && color != colorTextures.end() && color->second.image->faces==1) {
                colorTexture = color->second.image; color->second.used = ++colorUse;
            }
        }
    }
    if (!texture && !video && !colorTexture) {
        if(profileEngineCpu) {
            static unsigned reports=0;
            if(reports++<64)std::fprintf(stderr,
                "[GuiMissingTexture] id=%u pair=%08X material=%s descriptor=%08X triangles=%u flags=%08X\n",
                unsigned(u16(attributes+8)),u32(attributes+8),videoMaterial?"video":textMaterial?"text":"opaque",
                descriptor,triangles,u32(attributes+92));
        }
        return reject(6);
    }
    SimpleMesh mesh;
    if (!descriptor) descriptor = context + 12480;
    const bool whiteColor = videoMaterial || (colorTexture && u32(attributes + 108) == 0xFFFFFFFF &&
                                             u32(attributes + 116) == 0xFFFFFFFF);
    if (decodeSimpleMesh(base, descriptor, indices, triangles, mesh, whiteColor)) return reject(7);
    for (unsigned i = 0; i < 16; ++i) {
        mesh.projection[i] = f32(projection + i * 4);
        if (!std::isfinite(mesh.projection[i])) return reject(8);
    }
    mesh.texture = texture;
    mesh.video = video;
    mesh.colorTexture = colorTexture;
    mesh.textureId = u16(attributes + 8);
    mesh.opaque = opaqueMaterial;
    std::unique_lock lock(queueMutex);
    const size_t videoBytes = video ? video->luma.size() + video->chroma.size() : 0;
    auto newColorBytes = colorTexture && std::none_of(pending.begin(), pending.end(),
        [&](const auto& queued) { return queued.colorTexture == colorTexture; }) ? colorTexture->bytes() : 0;
    // World commands must not exhaust the separate UI draw allowance.
    if(liveStreaming() && !pending.empty() && (pending.size()>=livePartCommands ||
       pendingVertices+mesh.vertices.size()>262144 || pendingVideoBytes+videoBytes>32*1024*1024 ||
       pendingColorBytes+newColorBytes>64*1024*1024)) {
        publishPreviewPart(lock,false);newColorBytes=colorTexture?colorTexture->bytes():0;
    }
    if ((!liveStreaming() && pendingGuiDraws >= 512) || !reservePreviewCommand(lock) || pendingVertices + mesh.vertices.size() > 262144 ||
        pendingVideoBytes + videoBytes > 32 * 1024 * 1024 || pendingColorBytes + newColorBytes > 64 * 1024 * 1024) {
        ++pendingCounts.guiDrops;return reject(9);
    }
    pendingVideoBytes += videoBytes;
    pendingColorBytes += newColorBytes;
    if (video) ++videoMeshes;
    if (colorTexture) ++colorMeshes;
    ++pendingGuiDraws;pendingVertices += mesh.vertices.size();pending.push_back(std::move(mesh)); ++accepted;
    return true;
}
void previewObserveTriangles(uint8_t* base, uint32_t indices, uint32_t triangles) {
    submitTriangles(base, indices, triangles);
}
void previewObserveWorld(uint8_t* base,const StoredDraw& geometry) {
    EngineCpuScope profile(EnginePhase::snapshot);
    if (!active) return;
    worldActive=true;
    try {
        auto draw=captureWorldDraw(base,geometry);
        if(!draw)return;
        std::unique_lock lock(queueMutex);
        ++pendingCounts.worldDraws;
        // Draws retain shared immutable buffers. Count each retained resource
        // once, not once per depth/light/fog/material pass. UI vertices are
        // copied per mesh and use a separate budget.
        size_t additionalVertices=0,additionalBytes=0;
        // The caller and captured draw already own these resources through
        // publication. Accounting needs identities, not extra shared_ptr copies
        // that contend with resource releases on the render thread.
        const std::array resources{geometry.vertices.get(),geometry.indices.get()};
        auto measureGeometry=[&] {
            additionalVertices=additionalBytes=0;
            for(size_t i=0;i<resources.size();++i) {
                const auto* resource=resources[i];
                if(i && resource==resources[0])continue;
                if(!pendingGeometry.contains(resource)) {
                    additionalVertices+=resource->vertexCount;additionalBytes+=resource->bytes();
                }
            }
        };
        measureGeometry();
        if(liveStreaming() && !pending.empty() && (pending.size()>=livePartCommands ||
           pendingWorldVertices+additionalVertices>4*1024*1024 ||
           pendingGeometryBytes+additionalBytes>worldGeometryByteLimit)) {
            publishPreviewPart(lock,false);measureGeometry();
        }
        if (!reservePreviewCommand(lock) || pendingWorldVertices+additionalVertices>4*1024*1024 ||
            pendingGeometryBytes+additionalBytes>worldGeometryByteLimit) {
            if(pending.size()>=4096)++pendingCounts.commandDrops;else ++pendingCounts.vertexDrops;
            const auto dropped=pending.size()>=4096?++worldCommandBudgetDrops:++worldVertexBudgetDrops;
            if(dropped<=8)std::fprintf(stderr,"[EngineWorldQueueDrop] frame=%llu program=%s commands=%zu uniqueVertices=%zu uniqueGeometry=%zu uniqueBytes=%zu additionalVertices=%zu\n",
                frames.load()+1,draw->fragmentName.c_str(),pending.size(),pendingWorldVertices,pendingGeometry.size(),pendingGeometryBytes,additionalVertices);
            return;
        }
        for (uint32_t slots=draw->textureMask;slots;slots&=slots-1) {
            const unsigned s=std::countr_zero(slots);
            if(!draw->textureObjects[s].object || !draw->textureIds[s])continue;
            const auto it=colorTextures.find(draw->textureIds[s]);
            if(it==colorTextures.end())continue;
            const auto& texture=draw->textureObjects[s];const auto& sampler=draw->samplers[s];
            if(it->second.resource && (it->second.resource!=texture.object ||
               it->second.image->width!=texture.width || it->second.image->height!=texture.height ||
               it->second.image->faces!=texture.faces))continue;
            const auto required=(std::max)(texture.firstMip,sampler.lodValid?unsigned(sampler.minLevel):0u);
            if(it->second.image->firstMip>required)continue;
            draw->textures[s]=it->second.image;it->second.used=++colorUse;
        }
        for (uint32_t slots=draw->textureMask;slots;slots&=slots-1) {
            const unsigned s=std::countr_zero(slots);
            if(draw->textures[s])continue;
            if(!draw->textureObjects[s].object)continue;
            if(auto it=textures.find(draw->textureIds[s]);draw->textureIds[s] && it!=textures.end()) {
                auto& cached=alphaColors[draw->textureIds[s]];
                if(cached) {draw->textures[s]=cached;continue;}
                auto color=std::make_shared<ColorImage>();color->width=it->second->width;color->height=it->second->height;
                color->sourceCodec=2;color->pixels.resize(it->second->pixels.size()*4);
                for(size_t i=0;i<it->second->pixels.size();++i) {
                    color->pixels[i*4]=color->pixels[i*4+1]=color->pixels[i*4+2]=255;color->pixels[i*4+3]=it->second->pixels[i];
                }
                cached=color;draw->textures[s]=std::move(color);
            }
            if(!draw->textures[s] && draw->textureObjects[s].object) {
                traceMissingWorldTexture(base,draw->textureIds[s],draw->textureObjects[s].object,draw->textureObjects[s].storage);
                if(draw->material==WorldMaterial::ndsp || draw->material==WorldMaterial::fixed ||
                   draw->fragmentName.starts_with("XRShader_") || draw->fragmentName=="XRUtil_RenderSurface" ||
                   draw->fragmentName=="VBOp_GenEnv2" || draw->fragmentName=="VBOp_Fresnel" ||
                   // CCFuser texture1 is its original 324x18 RGB lookup map.
                   // Reload the source pixels if its CPU cache entry expired.
                   (draw->fragmentName=="XREngine_CCFuser" && s==1)) {
                    auto decoded=std::make_shared<ColorImage>();
                    if(!decodeWorldTextureImage(base,draw->textureObjects[s].object,*decoded,draw->textureObjects[s].firstMip)) {
                        if(auto old=colorTextures.find(draw->textureIds[s]);old!=colorTextures.end()) {
                            colorBytes-=old->second.image->bytes();colorTextures.erase(old);
                        }
                        while(colorBytes+decoded->bytes()>colorBudget && !colorTextures.empty()) {
                            auto oldest=std::min_element(colorTextures.begin(),colorTextures.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
                            colorBytes-=oldest->second.image->bytes();colorTextures.erase(oldest);
                        }
                        colorBytes+=decoded->bytes();colorTextures[draw->textureIds[s]]={decoded,++colorUse,draw->textureObjects[s].object};
                        draw->textures[s]=decoded;++colorImages;
                        if (TextureUploadTraceEnabled())
                            std::fprintf(stderr,"[EngineStreamedTexture] owned id=%u size=%ux%u format=%u\n",draw->textureIds[s],decoded->width,decoded->height,draw->textureObjects[s].format);
                    }
                }
            }
        }
        // Capture only distinct images used by this draw. Reuse this short
        // list if a budget boundary publishes the pending batch; all image
        // ownership stays in draw while the non-owning list is in use.
        std::array<const ColorImage*,16> images{};
        size_t imageCount=0;
        for(uint32_t slots=draw->textureMask;slots;slots&=slots-1) {
            const auto* image=draw->textures[std::countr_zero(slots)].get();
            if(image && std::find(images.begin(),images.begin()+imageCount,image)==images.begin()+imageCount)
                images[imageCount++]=image;
        }
        size_t additionalImageBytes=0;
        auto measureImages=[&] {
            additionalImageBytes=0;
            for(size_t i=0;i<imageCount;++i)
                if(!pendingWorldImages.contains(images[i]))additionalImageBytes+=images[i]->bytes();
        };
        measureImages();
        if(liveStreaming() && !pending.empty() && pendingWorldImageBytes+additionalImageBytes>worldImageByteLimit) {
            publishPreviewPart(lock,false);measureGeometry();measureImages();
        }
        if(pendingWorldVertices+additionalVertices>4*1024*1024 ||
           pendingGeometryBytes+additionalBytes>worldGeometryByteLimit) {
            ++pendingCounts.vertexDrops;++worldVertexBudgetDrops;return;
        }
        if(pendingWorldImageBytes+additionalImageBytes>worldImageByteLimit) {
            ++pendingCounts.imageDrops;++worldImageBudgetDrops;return;
        }
        for(const auto* resource:resources)if(pendingGeometry.insert(resource))pendingGeometryBytes+=resource->bytes();
        pendingWorldVertices+=additionalVertices;
        for(size_t i=0;i<imageCount;++i)pendingWorldImages.insert(images[i]);
        pendingWorldImageBytes+=additionalImageBytes;
        SimpleMesh command;command.world=std::move(draw);pending.push_back(std::move(command));
        ++worldQueued;
    } catch (...) {}
}
void previewObserveImmediateWorld(uint8_t* base,uint32_t device,uint32_t indices,uint32_t count) {
    if (!active || !worldActive || !device || uint64_t(device)+12456>0x100000000ull) return;
    // Task-59 bounded pre-capture failure diagnostics. diagnose is read once;
    // when false the original path below runs with no extra guest reads,
    // logging, hashing or allocations. Failure collection never stops after
    // successes; only the first 2 smoke successes log a reference line.
    const bool diagnose=profileEngineCpu;
    uint32_t smokeTex0=0;
    if(diagnose) {
        uint8_t idBytes[2]{};
        // File-scope context below (same 0x82A69B00 as the local below).
        if(copyRenderMemory(base,context+16896+8,idBytes,sizeof(idBytes)))smokeTex0=u16(idBytes);
    }
    const bool smoke=smokeTex0==1477 || smokeTex0==1371;
    uint32_t beFlags=0,id=0,boundVal=0,descriptor=0;
    bool haveAttr=false;
    auto fail=[&](unsigned category,const ImmediateCaptureReason* reason=nullptr) {
        if(diagnose)reportImmediateCapture(category,smokeTex0,device,indices,count,
            beFlags,id,boundVal,descriptor,reason,haveAttr);
    };
    try {
        constexpr uint32_t context=0x82A69B00;
        std::array<uint8_t,160> attributes{};std::array<uint8_t,24> state{};
        if (!copyRenderMemory(base,context+16896,attributes.data(),attributes.size())) {fail(0);return;}
        haveAttr=true;beFlags=u32(attributes.data()+92);
        if(!copyRenderMemory(base,context+16512,state.data(),state.size())) {fail(1);return;}
        descriptor=u32(state.data());id=u32(state.data()+20);
        std::shared_ptr<const StoredGeometry> stored;
        if (!descriptor && id && id!=0xFFFFFFFF) {
            std::array<uint8_t,4> bound{};
            if (!copyRenderMemory(base,device+12452,bound.data(),4)) {fail(2);return;}
            boundVal=u32(bound.data());
            if(!(stored=storedGeometryCache().vertexStream(base,id,boundVal))) {fail(2);return;}
        } else if (!descriptor) descriptor=context+12480;
        StoredDraw geometry;
        ImmediateCaptureReason reason;
        if (!snapshotImmediateWorldGeometry(base,descriptor,indices,count,std::move(stored),geometry,
            diagnose?&reason:nullptr)) {fail(3,diagnose?&reason:nullptr);return;}
        if(!captureEngineVertexBindings(base,geometry.vertexBindings)) {fail(4);return;}
        if(geometry.vertexBindings->deviceAddress!=device) {fail(5);return;}
        if(diagnose && smoke && immediateSmokeSuccessLogged.fetch_add(1,std::memory_order_relaxed)<2)
            std::fprintf(stderr,"[ImmediateCaptureOk] frame=%llu ms=%llu tex0=%u device=%08X indices=%08X count=%u "
                "descriptor=%08X boundId=%u vcount=%u indexCount=%u beFlags=%08X\n",
                frames.load(std::memory_order_relaxed),GetTickCount64(),smokeTex0,
                device,indices,count,descriptor,id,
                geometry.vertices?geometry.vertices->vertexCount:0u,geometry.indexCount,beFlags);
        previewObserveWorld(base,geometry);
    } catch (...) { fail(6); }
}
void previewObserveClear(uint8_t* base,uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,const std::array<int32_t,4>& rectangle) {
    if (!active) return;
    try {
        auto clear=std::make_shared<WorldClear>();
        if (!snapshotWorldClear(base,device,flags,color,depth,stencil,*clear)) return;
        if(rectangle[2]<rectangle[0] || rectangle[3]<rectangle[1])return;
        clear->rectangle=rectangle;
        std::unique_lock lock(queueMutex);
        if (!reservePreviewCommand(lock)) {++pendingCounts.clearDrops;return;}
        SimpleMesh command;command.worldClear=std::move(clear);pending.push_back(std::move(command));
    } catch (...) {}
}
void previewObserveDecodedTriangles(uint8_t* base, uint32_t indices, uint32_t capacity, uint32_t produced) {
    if (!active) return;
    ++decodedBatches;
    // Original 8225DE78 allocates at most 8192 BE16 indices per chunk and
    // passes the produced count directly to triangle-list DrawIndexed.
    if (!capacity || capacity > 8192 || produced > capacity || produced % 3) { ++decodedInvalid; return; }
    if (!produced) return;
    decodedIndices += produced;
    if (submitTriangles(base, indices, produced / 3)) ++decodedAccepted;
}
bool previewWorldActive() {return worldActive;}
void previewObserveResolve(uint8_t* base,uint32_t device,uint32_t flags,uint32_t rectangle,uint32_t destination,
                           uint32_t offset,uint32_t color,float depth,uint32_t stencil,uint32_t face,uint32_t mip) {
    if(!active || !worldActive) return;
    try {
        auto resolve=std::make_shared<WorldResolve>();
        if(!snapshotWorldResolve(base,device,flags,rectangle,destination,offset,color,depth,stencil,face,mip,*resolve)) return;
        std::unique_lock lock(queueMutex);if(!reservePreviewCommand(lock)) {++pendingCounts.resolveDrops;return;}
        SimpleMesh command;command.worldResolve=std::move(resolve);pending.push_back(std::move(command));
    }catch(...){}
}
void previewObservePresent(uint8_t* base,uint32_t frontbuffer) {
    if(!active || !worldActive) return;
    try {
        auto texture=std::make_shared<WorldTexture>();
        if(!snapshotWorldTexture(base,frontbuffer,*texture)) return;
        std::unique_lock lock(queueMutex);
        if(liveStreaming())reservePreviewCommand(lock);
        SimpleMesh command;command.worldPresent=std::move(texture);pending.push_back(std::move(command));
    }catch(...){}
}
void setPreviewFrameBackpressure(bool enabled,bool streaming) {
    {std::lock_guard lock(queueMutex);frameBackpressure=enabled;streamLargeFrames=enabled && streaming;}
    if(!enabled)frameConsumed.notify_all();
}
void previewBeginHistogram(unsigned bin) {
    if(!active || !worldActive || bin>=histogramResults.size() || activeHistogram)return;
    std::unique_lock lock(queueMutex);
    if(liveStreaming())reservePreviewCommand(lock);
    auto& result=histogramResults[bin];if(!result)result=std::make_shared<WorldQueryResult>();
    activeHistogram=std::make_shared<WorldQuery>();activeHistogram->result=result;
    SimpleMesh command;command.worldQuery=activeHistogram;command.worldQueryBegin=true;
    pending.push_back(std::move(command));
}
void previewEndHistogram() {
    if(!activeHistogram)return;
    std::unique_lock lock(queueMutex);
    if(liveStreaming())reservePreviewCommand(lock);
    SimpleMesh command;command.worldQuery=std::move(activeHistogram);pending.push_back(std::move(command));
}
bool previewReadHistogram(unsigned bin,uint64_t& samples) {
    std::lock_guard lock(queueMutex);
    if(bin>=histogramResults.size() || !histogramResults[bin])return false;
    const auto value=histogramResults[bin]->samples.load(std::memory_order_acquire);
    if(value==UINT64_MAX)return false;
    samples=value;return true;
}
namespace {
void publishPreviewPart(std::unique_lock<std::mutex>& lock,bool last) {
    frameConsumed.wait(lock,[]{return !frameBackpressure || !frameReady;});
    pendingCounts.commands+=pending.size();++pendingCounts.parts;
    for(const auto& command:pending)if(command.world)++pendingCounts.passes[size_t(command.world->material)];
    if(!last)++streamedParts;
    if(last) {
    queueHighWater.store((std::max)(queueHighWater.load(std::memory_order_relaxed),pendingCounts.commands),std::memory_order_relaxed);
    if(pendingCounts.commands>4096 || pendingCounts.commandDrops || pendingCounts.vertexDrops ||
       pendingCounts.clearDrops || pendingCounts.resolveDrops || pendingCounts.guiDrops || pendingCounts.imageDrops) {
        ++oversizedFrames;
        if(queueFrameReports<32) {
            ++queueFrameReports;
            const auto& passes=pendingCounts.passes;
            std::fprintf(stderr,"[EngineQueueFrame] frame=%llu commands=%zu worldAttempts=%zu commandDrops=%zu vertexDrops=%zu clearDrops=%zu resolveDrops=%zu guiDrops=%zu lastPartVertices=%zu lastPartGeometry=%zu lastPartBytes=%zu depth=%zu motion=%zu ndsp=%zu fixed=%zu post=%zu parts=%zu imageDrops=%zu\n",
                frames.load()+1,pendingCounts.commands,pendingCounts.worldDraws,pendingCounts.commandDrops,pendingCounts.vertexDrops,
                pendingCounts.clearDrops,pendingCounts.resolveDrops,pendingCounts.guiDrops,pendingWorldVertices,
                pendingGeometry.size(),pendingGeometryBytes,passes[0],passes[1],passes[2],passes[3],passes[4],pendingCounts.parts,pendingCounts.imageDrops);
        }
    }
    pendingCounts={};pendingGuiDraws=0;++frames;
    }
    readyPart={pendingFirst,last};pendingFirst=last;
    ready = std::move(pending); pending.clear();
    // The consumer returns only empty storage. Reuse capacity without moving
    // thousands of commands through the vector's growth stages every frame.
    pending.swap(recycledFrame);
    if(pending.capacity())frameBufferReuses.fetch_add(1,std::memory_order_relaxed);
    pendingVertices = pendingVideoBytes = pendingColorBytes = 0;
    pendingGeometry.clear();pendingGeometryBytes=pendingWorldVertices=0;
    pendingWorldImages.clear();pendingWorldImageBytes=0;frameReady = true;
    lock.unlock();frameAvailable.notify_one();
    lock.lock();
}
}
void previewEndFrame() {
    EngineCpuScope profile(EnginePhase::handoff);
    recordEngineFrameProcessor();
    if (!active) return;
    std::unique_lock lock(queueMutex);
    publishPreviewPart(lock,true);
}
bool takePreviewFrame(std::vector<SimpleMesh>& frame,unsigned waitMilliseconds,PreviewFramePart* part) {
    std::unique_lock lock(queueMutex);
    if(waitMilliseconds && !frameReady)
        frameAvailable.wait_for(lock,std::chrono::milliseconds(waitMilliseconds),[]{return frameReady;});
    if (!frameReady) return false;
    if(part)*part=readyPart;
    auto completed=std::move(ready);ready.clear();frameReady=false;
    lock.unlock();frameConsumed.notify_one();
    // Retiring thousands of draw snapshots must not hold the producer's
    // queue lock. Their immutable shared resources outlive either frame.
    frame.swap(completed);
    completed.clear();
    // Never destroy retained resources under queueMutex. Publish only the
    // cleared allocation, retaining at most one spare command buffer.
    {std::lock_guard recycleLock(queueMutex);
        if(completed.capacity()>recycledFrame.capacity())completed.swap(recycledFrame);
    }
    return true;
}
void printPreviewCounters() {
    if (!active) return;
    printPromptOriginCounters();
    printStoredGeometryCounters();
    printWorldCounters();
    std::fprintf(stderr,"[EngineWorldQueue] queued=%llu vertexBudgetDrops=%llu commandBudgetDrops=%llu\n",
        worldQueued.load(),worldVertexBudgetDrops.load(),worldCommandBudgetDrops.load());
    std::fprintf(stderr,"[EngineQueueFrames] highWater=%zu oversizedFrames=%zu\n",
        queueHighWater.load(std::memory_order_relaxed),oversizedFrames.load(std::memory_order_relaxed));
    std::fprintf(stderr,"[EngineQueueStreaming] intermediateParts=%llu imageBudgetDrops=%llu partCommandLimit=%zu geometryMiB=%zu imageMiB=%zu\n",
        streamedParts.load(),worldImageBudgetDrops.load(),livePartCommands,worldGeometryByteLimit/(1024*1024),worldImageByteLimit/(1024*1024));
    std::fprintf(stderr,"[EngineFrameBuffers] reused=%llu\n",frameBufferReuses.load(std::memory_order_relaxed));
    printEngineTextureCounters();
    printEngineVertexDescriptorCounters();
    printEnginePaletteCounters();
    printEngineVertexProgramCounters();
    std::fprintf(stderr, "[EnginePreview] observed=%llu accepted=%llu unsupported=%llu frames=%llu alphaImages=%llu rejectedImages=%llu\n",
        observed.load(), accepted.load(), unsupported.load(), frames.load(), images.load(), rejectedImages.load());
    std::fprintf(stderr, "[EnginePreview] nativeRenderedFrames=%llu nativeSubmittedMeshes=%llu\n",
                 renderedFrames.load(), renderedMeshes.load());
    std::fprintf(stderr, "[EnginePreview] videoImages=%llu acceptedVideoMeshes=%llu\n", videoImages.load(), videoMeshes.load());
    std::fprintf(stderr, "[EnginePreview] colorImages=%llu acceptedColorMeshes=%llu\n", colorImages.load(), colorMeshes.load());
    std::fprintf(stderr, "[EnginePreview] decodedBatches=%llu decodedIndices=%llu acceptedDecodedMeshes=%llu invalidDecodedBatches=%llu\n",
        decodedBatches.load(), decodedIndices.load(), decodedAccepted.load(), decodedInvalid.load());
    std::fprintf(stderr, "[EnginePreview] rejectedState=%llu rejectedStoredVB=%llu rejectedMaterial=%llu rejectedViewport=%llu rejectedTextureStages=%llu rejectedModel=%llu rejectedMissingTexture=%llu rejectedStreams=%llu rejectedProjection=%llu rejectedBudget=%llu\n",
        rejectionReasons[0].load(), rejectionReasons[1].load(), rejectionReasons[2].load(), rejectionReasons[3].load(),
        rejectionReasons[4].load(), rejectionReasons[5].load(), rejectionReasons[6].load(), rejectionReasons[7].load(),
        rejectionReasons[8].load(), rejectionReasons[9].load());
}
void recordPreviewRender(size_t meshCount) { ++renderedFrames; renderedMeshes += meshCount; }
}
