#pragma once
#include "stored_geometry.h"
#include "simple_mesh.h"
#include <atomic>
#include <string>
#include <string_view>

namespace DarkRecomp::Native {
// A completed native GPU measurement, shared across producer/consumer threads.
// UINT64_MAX denotes unavailable; zero is a real measured result.
struct WorldQueryResult {std::atomic<uint64_t> samples{UINT64_MAX};};
struct WorldQuery {std::shared_ptr<WorldQueryResult> result;};
struct WorldVertex {
    EngineVector position{}, normal{};
    std::array<EngineVector,8> tex{};
    EngineVector color{}, indices{}, weights{}, indices2{}, weights2{};
};
struct WorldVertexOptions {
    uint32_t weights = 0;
    bool positionConversion = false, normal = false, tangents = false, normalizeNormal = false, vertexColor = false;
    std::array<uint8_t,8> modes{}, coordinates{};
    std::array<bool,8> conversions{}, matrices{};
    bool operator==(const WorldVertexOptions&) const = default;
};
struct WorldVertexConstants {
    std::array<EngineVector,256> vectors{};
    std::array<std::array<uint32_t,4>,9> references{};
};
enum class WorldMaterial { depth, motion, ndsp, fixed, post };
struct WorldTexture {
    uint32_t object=0, storage=0, width=0, height=0, format=0;
    int exponent=0;
    uint32_t faces=1;
    uint32_t mipLevels=1,firstMip=0; // Declared levels and draw-time outstanding upload prefix.
    uint64_t key() const {return uint64_t(object)<<32|storage;}
};
struct WorldSampler {
    bool valid=false,minLinear=false,magLinear=false,mipLinear=false,baseOnly=false;
    bool lodValid=false; // LOD bounds remain usable when other sampler fields are unsupported.
    std::array<uint8_t,3> address{};
    uint8_t anisotropy=1,minLevel=0,maxLevel=0,border=0;
    float bias=0;
};
WorldSampler decodeWorldSampler(const std::array<uint32_t,6>& state) noexcept;
// The GPU needs immutable geometry and prepared constants, not a second copy
// of the 4 KiB engine constant bank used to validate those constants.
struct WorldGeometry {
    std::shared_ptr<const StoredGeometry> vertices, indices;
    uint32_t firstIndex=0,indexCount=0;
    explicit operator bool() const {return vertices && indices;}
};
// Original82861698/8285E378 latch these words into the device. Surface objects
// are temporary descriptors, not storage owners, and may be edited or recycled
// before the render consumer executes the command.
struct WorldSurfaceBinding {
    uint32_t layout=0,info=0;
    bool completed=false;
    static uint64_t syntheticKey(uint32_t identity,bool depth) noexcept {
        return identity?uint64_t(identity)*2+unsigned(depth):0;
    }
    uint64_t key(uint32_t identity,bool depth) const noexcept {
        if(!identity)return 0;
        // Hand-authored renderer commands have an explicit, disjoint namespace.
        // A failed guest capture never falls back to a pointer identity.
        if(!completed)return syntheticKey(identity,depth);
        const unsigned format=(info>>16)&15;
        const bool wide=!depth && (format==5 || format==7 || format==15);
        // Keep the completed pitch/sample layout and EDRAM tile base. Color
        // view precision/exponent switches retain the native float contents;
        // a different storage width, layout, tile base or plane is distinct.
        return (uint64_t(1)<<63)|(uint64_t(layout)<<14)|(uint64_t(wide)<<13)|
            (uint64_t(info&0xfff)<<1)|unsigned(depth);
    }
};
struct WorldSurfaceTargets {
    std::array<uint32_t,5> targets{}; // Original objects, for diagnostics only.
    std::array<WorldSurfaceBinding,5> surfaceBindings{}; // Color 0..3, depth.
    uint64_t surfaceKey(unsigned slot) const noexcept {
        return slot<targets.size()?surfaceBindings[slot].key(targets[slot],slot==4):0;
    }
};
struct WorldDraw : WorldSurfaceTargets {
    WorldGeometry geometry;
    WorldVertexOptions options;
    WorldVertexConstants constants;
    std::array<uint8_t,160> attributes{};
    std::array<uint32_t,4> viewport{};
    WorldMaterial material = WorldMaterial::depth;
    std::string fragmentName;
    uint32_t fragmentFlags=0;
    EngineVector depthRange{0,1,0,0};
    std::array<EngineVector,16> fragmentConstants{};
    std::array<uint16_t,16> textureIds{};
    uint16_t textureMask=0xFFFF; // Conservative original-source usage; manual draws retain all slots.
    std::array<WorldTexture,16> textureObjects{};
    std::array<WorldSampler,16> samplers{};
    std::array<std::shared_ptr<const ColorImage>,16> textures{};
};
uint16_t worldFragmentTextureMask(std::string_view name,uint32_t flags) noexcept;
struct WorldResolve : WorldSurfaceTargets {
    WorldTexture destination;
    std::array<uint32_t,4> viewport{}, rectangle{};
    std::array<uint32_t,2> offset{};
    uint32_t flags=0, stencil=0, face=0, mip=0;
    float depth=0;
    EngineVector color{};
    int exponent=0;
};
bool snapshotWorldTexture(uint8_t*,uint32_t object,WorldTexture&) noexcept;
bool snapshotWorldResolve(uint8_t*,uint32_t device,uint32_t flags,uint32_t rectangle,uint32_t destination,
                          uint32_t offset,uint32_t color,float depth,uint32_t stencil,uint32_t face,uint32_t mip,WorldResolve&) noexcept;
struct WorldClear : WorldSurfaceTargets {
    std::array<uint32_t,4> viewport{};
    uint32_t flags=0, stencil=0;
    float depth=0;
    EngineVector color{};
    std::optional<std::array<int32_t,4>> rectangle; // Completed native-engine clip, in surface pixels.
};
// Convert engine formats to vertex-fetch values, before the original shader's
// scale/offset. In particular signed 11:11:10 differs from the CPU converter.
bool decodeWorldVertices(const StoredGeometry&, std::vector<WorldVertex>&) noexcept;
// Validate the same layout and finite fetch values without allocating or
// expanding vertices. Immediate capture uses this before publishing its copy.
bool validateWorldVertices(const StoredGeometry&) noexcept;
// CPU float3 positions for immediate depth/stencil draws, or a verified stored
// vertex stream combined with the original engine's immediate triangle indices.
// Optional task-59 pre-capture failure diagnostics: when reason is provided,
// every silent failure records its stage (acceptance behavior unchanged).
// Per-stage field meanings: count->detail0=count; headerRead->detail0=
// descriptor; headerBounds->detail0=vertexCount,detail1=field-code bitmask
// (1=half+2,2=word56,4=word60,8=word64,16=vertexCount-range,
// 32=uv-components-range with low 3 bits holding the slot); streamBounds/
// streamCopy->detail0=slot,detail1=pointer,detail2=format; indexCopy->
// detail0=indexAddress,detail1=count; indexOob->detail0=position,
// detail1=value,detail2=vertexCount; vertexDecode fills the decode detail
// below (badVertex=UINT32_MAX with badLane=0xFE marks an invalid
// format/stride walk rather than a nonfinite lane).
struct ImmediateCaptureReason {
    enum class Stage : uint8_t {
        ok=0, count, headerRead, headerBounds, streamBounds, streamCopy,
        indexCopy, indexOob, vertexDecode, retainIndices, exception
    };
    Stage stage = Stage::ok;
    uint32_t detail0 = 0, detail1 = 0, detail2 = 0;
    uint32_t badVertex = UINT32_MAX, badSlot = 0, badLane = 0;
    uint32_t rawBE = 0;
    uint8_t rawSize = 0;
    // Tri-state membership: membershipKnown is set only after a successful
    // index-range read. referenced=false with membershipKnown=false means
    // "unproven", never "proven unreferenced".
    bool membershipKnown = false;
    bool referenced = false;
    uint32_t firstRefPos = UINT32_MAX;
    uint32_t stride = 0, vertexCount = 0;
    std::array<uint8_t,16> formats{};
    std::array<uint8_t,256> vertexBytes{};
    uint32_t vertexByteCount = 0;
};
bool snapshotImmediateWorldGeometry(uint8_t* base,uint32_t descriptor,uint32_t indices,uint32_t indexCount,
                                    std::shared_ptr<const StoredGeometry> stored,StoredDraw&,
                                    ImmediateCaptureReason* reason=nullptr) noexcept;
bool prepareWorldVertexProgram(const EngineVertexBindingSnapshot&, WorldVertexOptions&, WorldVertexConstants&) noexcept;
// Failure-only geometry proof for a nonfinite palette tail. When the strict
// full-palette copy above fails, this validates only the palette rows the
// actual draw subset reads: address=floor(index*c8.w) for every active
// influence, including zero weights (the template fetches before weighting,
// so 0*NaN is still NaN). GPU MUL latitude and denormal flushing are handled
// conservatively: subnormal operands/products fail, and the exact double
// product is bracketed with its float neighbors, proving the union of both
// floor addresses. Unread rows stay zero; rows also required by other
// constant ranges keep their strict copies. On success options/constants are
// published and proof describes the fallback; on failure they are unchanged
// while proof may still report whether absolute vector 135 is referenced.
struct WorldPaletteProof {
    bool fallback = false;
    unsigned usedRows = 0;
    unsigned ignoredRows = 0;
    unsigned firstIgnoredRow = 0;
    unsigned firstIgnoredNonfiniteRow = 256;
    bool referencesVector135 = false;
    bool usageProven = false;
};
bool prepareWorldVertexProgramWithGeometry(const EngineVertexBindingSnapshot&, const StoredDraw&,
                                           WorldVertexOptions&, WorldVertexConstants&,
                                           WorldPaletteProof* = nullptr) noexcept;
bool snapshotWorldDraw(uint8_t* base, const StoredDraw&, WorldDraw&) noexcept;
// Populate the final owned allocation directly and publish only after success.
std::shared_ptr<WorldDraw> captureWorldDraw(uint8_t* base,const StoredDraw&) noexcept;
bool snapshotWorldClear(uint8_t* base,uint32_t device,uint32_t flags,uint32_t color,
                        float depth,uint32_t stencil,WorldClear&) noexcept;
void printWorldCounters();
void recordWorldSubmission(WorldMaterial material);
}
