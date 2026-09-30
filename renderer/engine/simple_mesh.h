#pragma once
#include <array>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

namespace DarkRecomp {
namespace Native {struct WorldDraw;struct WorldClear;struct WorldResolve;struct WorldTexture;struct StoredDraw;struct WorldQuery;}
struct SimpleVertex { float position[3]; float uv[2]; float color[4]; };
struct AlphaImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
};
struct ColorImage {
    uint32_t width = 0, height = 0;
    uint32_t faces = 1;
    std::vector<uint8_t> pixels; // Owned, tightly packed RGBA8.
    uint32_t sourceCodec = 0; // Decoder provenance only; all owned levels contain RGBA8 pixels.
    // Verified original prompt origin, owned with this immutable generation.
    // Unknown (0) renders the original pixels; a known origin selects a cached
    // native icon at the render boundary. Set once at publish; never mutated
    // by queued draws. Encodes Prompts::Origin without a header cycle.
    uint8_t promptOrigin = 0;
    // Each authored lower level owns tightly packed RGBA8 for all faces.
    // A decoded resource with maxLOD=0 has one authored level. Images supplied
    // only as a base by CPU callers retain the generated-mip fallback.
    std::vector<std::vector<uint8_t>> mips;
    bool authoredMips = false;
    // Original resource-relative index of the first uploaded level. Missing
    // leading levels stay empty; dimensions and mip indices are never rebased.
    unsigned firstMip = 0;
    bool valid() const {
        if(!width || !height || width>2048 || height>2048 || (faces!=1 && faces!=6) ||
           (faces==6 && width!=height) || mips.size()>=unsigned(std::bit_width((std::max)(width,height))) ||
           firstMip>mips.size() || (!authoredMips && (firstMip || !mips.empty())))return false;
        for(unsigned mip=0;mip<=mips.size();++mip) {
            const auto& data=mip?mips[mip-1]:pixels;
            const std::size_t expected=mip<firstMip?0:std::size_t((std::max)(1u,width>>mip))*
                (std::max)(1u,height>>mip)*faces*4;
            if(data.size()!=expected)return false;
        }
        return true;
    }
    std::size_t bytes() const {
        std::size_t total = pixels.size();
        for (const auto& mip : mips) total += mip.size();
        return total;
    }
};
struct VideoFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> luma, chroma; // Full-size Y; half-size BE A8L8 bytes V,U.
    double timestampSeconds = 0;
};
struct SimpleMesh {
    std::shared_ptr<Native::WorldQuery> worldQuery;
    bool worldQueryBegin=false;
    std::shared_ptr<const Native::WorldDraw> world;
    std::shared_ptr<const Native::WorldClear> worldClear;
    std::shared_ptr<const Native::WorldResolve> worldResolve;
    std::shared_ptr<const Native::WorldTexture> worldPresent;
    std::vector<SimpleVertex> vertices;
    std::vector<uint16_t> indices;
    std::array<float, 16> projection{};
    std::shared_ptr<const AlphaImage> texture;
    std::shared_ptr<const VideoFrame> video;
    std::shared_ptr<const ColorImage> colorTexture;
    uint32_t textureId = 0;
    bool opaque = false;
};

namespace Native {
bool copyRenderMemory(uint8_t* base, uint64_t address, void* output, size_t size);
// Compare an owned snapshot with the current readable bytes. A mismatch or
// inaccessible range fails; no second temporary copy of the bank is needed.
bool equalRenderMemory(uint8_t* base, uint64_t address, const void* expected, size_t size);
// Exact 68-byte CPU vertex descriptor consumed by original 8225CDD8.
// This bounded subset accepts position/UV0/color; unsupported streams reject.
const char* decodeSimpleMesh(uint8_t* base, uint32_t descriptor, uint32_t indices,
                             uint32_t triangles, SimpleMesh& result, bool allowWhiteColor = false);
const char* decodeAlphaImage(uint8_t* base, uint32_t image, AlphaImage& result);
const char* decodeColorImage(uint8_t* base, uint32_t image, ColorImage& result);
const char* decodeUploadImage(uint8_t* base,uint32_t image,uint32_t pixels,ColorImage& result);
uint32_t worldTextureTiledOffset(uint32_t x,uint32_t y,uint32_t width,uint32_t bytesPerBlock);
const char* decodeWorldTextureImage(uint8_t* base,uint32_t object,ColorImage& result,unsigned firstMip=0);
const char* decodeVideoFrame(uint8_t* base, uint32_t frame, VideoFrame& result);
void enableEnginePreview();
// Configure before the game starts; tests retain the conservative default.
void setPreviewTextureBudget(size_t bytes);
bool enginePreviewEnabled();
void previewPrepareTexture(uint32_t textureId);
// Completed upload snapshots; resource identity prevents incremental updates
// from borrowing another generation or a different texture subobject.
std::shared_ptr<const ColorImage> previewCapturedTexture(uint32_t id,uint32_t resource);
void previewPublishTexture(uint32_t id,uint32_t resource,std::shared_ptr<const ColorImage> image);
void previewObserveImage(uint8_t* base, uint32_t image, uint32_t textureId, uint32_t mip);
void previewObserveVideo(uint8_t* base, uint32_t container, uint32_t localId);
// Adaptive prompt icon substitution, resolved at the render boundary.
// Returns nullptr to keep the original controller artwork.
enum class PromptRenderSource : uint8_t { KeyboardMouse = 0, Controller = 1 };
enum class PromptRenderContext : uint8_t { Menu = 0, Gameplay = 1 };
std::shared_ptr<const ColorImage> promptReplacementFor(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& image, PromptRenderSource source, PromptRenderContext context);
// Immediate-path variant with the draw's sampled UV bounds so atlas tiles and
// wide canvases keep exact sample behavior (aspect-preserved icon fit).
std::shared_ptr<const ColorImage> promptReplacementForUv(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& image, PromptRenderSource source, PromptRenderContext context,
    float u0, float v0, float u1, float v1);
// Bounded scoped capture for verified original names. Hooks open a scope for
// the fetching image address, commit an origin (including Unknown/0, which
// invalidates), and close the scope so nesting restores. Commits are accepted
// only inside an active scope; direct unscoped commits are ignored (the
// upload-origin collector still receives direct notes independently).
// previewObserveImage consumes and clears even on early returns; a later
// unknown image reusing an address cannot inherit. No permanent map.
void previewBeginPromptCapture(uint32_t imageAddress);
bool previewCommitPromptOrigin(uint32_t imageAddress, uint8_t origin);
void previewEndPromptCapture();
// Consumes the pending origin for an image address (fail-closed unknown
// while overflowed, without disturbing the saved depth-8 state). Shared by
// previewObserveImage so tests assert the real consume path, not only commits.
uint8_t takePromptOriginFor(uint32_t imageAddress);
void previewSetPendingPromptOrigin(uint32_t imageAddress, uint8_t origin);
// Prompt-origin diagnostics live in this unit so EngineMesh and WorldRenderer
// test targets link without render_trace.cpp (which needs PPC imports).
void countPromptOriginAttempt(bool classified);
void printPromptOriginCounters();
uint64_t promptOriginAttempts();
uint64_t promptOriginClassified();
void previewObserveTriangles(uint8_t* base, uint32_t indices, uint32_t triangles);
void previewObserveWorld(uint8_t* base,const StoredDraw&);
void previewObserveImmediateWorld(uint8_t* base,uint32_t device,uint32_t indices,uint32_t indexCount);
void previewObserveClear(uint8_t* base,uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,const std::array<int32_t,4>& rectangle);
// Output of original 8225F320 at the triangle draw call site in 8225DE78.
// Counts are indices, not packet words or triangles.
void previewObserveDecodedTriangles(uint8_t* base, uint32_t indices, uint32_t capacity, uint32_t produced);
void previewEndFrame();
// The live renderer must consume every frame: resolves can feed later frames.
// Disable before stopping the consumer to release a waiting producer.
// Streaming bounds the live handoff without dropping the tail of a large
// frame. Its consumer must request PreviewFramePart and present only last.
void setPreviewFrameBackpressure(bool enabled,bool streamLargeFrames=false);
bool previewWorldActive();
void previewObserveResolve(uint8_t*,uint32_t device,uint32_t flags,uint32_t rectangle,uint32_t destination,
                           uint32_t offset,uint32_t color,float depth,uint32_t stencil,uint32_t face,uint32_t mip);
void previewObservePresent(uint8_t*,uint32_t frontbuffer);
// Original histogram callbacks bracket draws and consume prior completed bins.
void previewBeginHistogram(unsigned bin);
void previewEndHistogram();
bool previewReadHistogram(unsigned bin,uint64_t& samples);
// A bounded wait lets the window consume a late frame immediately instead of
// adding another display-period delay. Zero retains nonblocking behavior.
struct PreviewFramePart {bool first=true,last=true;};
bool takePreviewFrame(std::vector<SimpleMesh>& frame,unsigned waitMilliseconds=0,PreviewFramePart* part=nullptr);
void printPreviewCounters();
void recordPreviewRender(size_t meshCount);
}
}
