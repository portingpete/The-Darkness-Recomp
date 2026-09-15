#pragma once
#include "renderer/engine/world_mesh.h"
#include "transient_geometry_buffers.h"
#include "descriptor_cache.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace DarkRecomp {
class WorldVertexShaderD3D11 {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Native::WorldVertexOptions options_;
    Ptr<ID3DBlob> code_;
    Ptr<ID3D11VertexShader> shader_;
    Ptr<ID3D11InputLayout> layout_;
    Ptr<ID3D11Buffer> constants_, references_;
    std::optional<Native::WorldVertexConstants> uploadedConstants_;
public:
    WorldVertexShaderD3D11(ID3D11Device*,const Native::WorldVertexOptions&,bool rasterize=false);
    bool bind(ID3D11DeviceContext*,const Native::WorldVertexConstants&,const std::vector<Native::WorldVertex>&,
              const std::array<std::array<float,2>,8>* indexBounds=nullptr,bool alreadyBound=false);
    ID3DBlob* bytecode() const {return code_.Get();}
};
// Compatible views of completed engine storage share retained contents, even
// when their transient surface objects differ. Resolved textures remain owned
// by the engine's destination textures for later composition/presentation.
class WorldRendererD3D11 {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct StringHash {
        using is_transparent=void;
        size_t operator()(std::string_view key) const noexcept {return std::hash<std::string_view>{}(key);}
    };
    struct StringEqual {
        using is_transparent=void;
        bool operator()(std::string_view left,std::string_view right) const noexcept {return left==right;}
    };
    template<class T> using StringCache=std::unordered_map<std::string,T,StringHash,StringEqual>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    const uint32_t scale_;
    TransientGeometryBuffers transientBuffers_;
    struct Geometry {std::shared_ptr<const Native::StoredGeometry> source; std::vector<Native::WorldVertex> vertices; Ptr<ID3D11Buffer> buffer; uint64_t used=0;UINT transientCapacity=0;
        std::array<std::array<float,2>,8> indexBounds{};
        size_t bytes() const {return transientCapacity?transientCapacity:vertices.size()*sizeof(Native::WorldVertex);}};
    struct Indices {std::shared_ptr<const Native::StoredGeometry> source; Ptr<ID3D11Buffer> buffer; uint64_t used=0;uint16_t maximum=0;UINT transientCapacity=0;
        size_t bytes() const {return transientCapacity?transientCapacity:source->indices.size()*sizeof(uint16_t);}};
    // Guest dimensions and ownership stay logical; only GPU storage is scaled.
    struct Surface {uint32_t width=0,height=0; Ptr<ID3D11Texture2D> texture; std::array<Ptr<ID3D11RenderTargetView>,6> faces; Ptr<ID3D11RenderTargetView> color; Ptr<ID3D11DepthStencilView> depth; Ptr<ID3D11ShaderResourceView> view;};
    std::unordered_map<const Native::StoredGeometry*,Geometry> geometry_;
    std::unordered_map<const Native::StoredGeometry*,Indices> indices_;
    size_t geometryBytes_=0,indexBytes_=0;
    uint64_t resourceUse_=0,vertexUploads_=0,indexUploads_=0;
    // Page-in pacing: cap fresh STORED geometry/index decode+upload bytes per
    // frame so a dense first-sight area spreads across frames instead of
    // freezing one for tens of ms (an 81ms single render was measured on a
    // ~25MB page-in). Deferred draws are rejected for this frame only; the
    // engine resubmits them in later snapshots. Transient immediate snapshots
    // are exempt: they are small, steady per-frame churn, and deferring them
    // cannot amortize (they are re-created every frame, so a starved tail
    // could never catch up). Stored uploads amortize across frames, and the
    // first upload of a frame always proceeds, so a single huge mesh can
    // never starve. Small scenes (every GPU contract) never reach the budget
    // and render pixel-identically. Render thread only: no locking needed.
    static constexpr size_t kFrameUploadBudgetBytes = 4 * 1024 * 1024;
    size_t frameUploadBytes_ = 0;
    uint64_t budgetDeferred_ = 0;
    // Worst-single-upload tripwire (per 5s report window; reset each print).
    // Clock reads happen only on cache-miss paths, so steady-state cost is
    // zero. Decode tracks CPU conversion; upload tracks GPU creation/transfer
    // with the slowest site named (vertex/index/image/surface).
    double worstDecodeMs_ = 0; size_t worstDecodeBytes_ = 0;
    double worstUploadMs_ = 0; size_t worstUploadBytes_ = 0;
    const char* worstUploadKind_ = "";
    void noteWorstUpload(double ms, size_t bytes, const char* kind);
    DescriptorCache<D3D11_DEPTH_STENCIL_DESC,Ptr<ID3D11DepthStencilState>> depthStates_;
    DescriptorCache<D3D11_BLEND_DESC,Ptr<ID3D11BlendState>> blendStates_;
    DescriptorCache<D3D11_RASTERIZER_DESC,Ptr<ID3D11RasterizerState>> rasterStates_;
    DescriptorCache<D3D11_SAMPLER_DESC,Ptr<ID3D11SamplerState>> samplerStates_;
    StringCache<std::unique_ptr<WorldVertexShaderD3D11>> shaders_;
    std::unordered_map<uint64_t,Surface> surfaces_;
    struct Image {std::shared_ptr<const ColorImage> source;Ptr<ID3D11ShaderResourceView> view;uint64_t used=0;size_t bytes=0;};
    std::unordered_map<const ColorImage*,Image> images_;
    size_t imageBytes_=0;
    size_t imageBudget_=512*1024*1024;
    uint64_t imageUploads_=0,profileDraws_=0;
    double profileTextures_=0,profileGeometry_=0,profileBinding_=0,profileStates_=0,profileSubmit_=0;
    struct FragmentProgram {Ptr<ID3D11PixelShader> shader;unsigned textures=0,cubes=0;};
    StringCache<FragmentProgram> fragments_;
    // Last-draw lookup cache (render thread only, like every other member).
    // Consecutive draws usually repeat the previous material/shader; a
    // component comparison then skips the snprintf + string-hash lookups.
    // Safe: fragments_ is fully populated in the constructor and never
    // mutated later (a cached miss can never go stale), and unordered_map
    // rehash preserves element references (a cached program pointer never
    // dangles). shaders_ DOES insert on miss, so its entry refreshes on
    // every miss path below. No invalidation is needed in beginFrame,
    // invalidateBindings, clear or resolve.
    std::string lastFragmentName_;
    std::string lastFragmentKey_;
    const FragmentProgram* lastFragment_ = nullptr;
    bool lastFragmentFound_ = false;
    unsigned lastFragmentFlags_ = 0;
    bool lastFragmentValid_ = false;
    Native::WorldVertexOptions lastShaderOptions_{};
    WorldVertexShaderD3D11* lastShader_ = nullptr;
    bool lastShaderValid_ = false;
    std::unordered_map<uint64_t,Surface> resolved_;
    uint64_t presentCount_=0;
    bool inspectFrame_=false;
    bool diagnostics_=false;
    unsigned inspection_=0;
    std::unordered_set<std::string> inspectedPrograms_;
    // Bounded opt-in smoke evidence (task 54). Untouched on normal runs:
    // every use is gated by inspectFrame_, set only by an explicit
    // inspectNextFrame() call. Caps: inspections 1..16 emit (matching the
    // render-trace requestedInspection<16 bound), at most 128 blended-draw
    // records per inspection, then a single truncation marker.
    unsigned smokeRecords_=0;
    bool smokeTruncated_=false;
    unsigned smokeDrawOrdinal_=0;
    void smokeEvidence(const Native::WorldDraw&,uint32_t,const std::vector<Native::WorldVertex>&,unsigned);
    Ptr<ID3D11Buffer> fragmentConstants_,textureScales_,transferConstants_,viewportConstants_;
    Native::EngineVector uploadedViewport_{};
    std::array<Native::EngineVector,256> uploadedFragment_{};
    std::array<Native::EngineVector,16> uploadedScales_{};
    std::array<uint8_t,16> uploadedAlpha_{};
    bool viewportUploaded_=false,pixelConstantsUploaded_=false;
    bool bindingsValid_=false;
    bool pixelBuffersBound_=false;
    WorldVertexShaderD3D11* boundVertex_=nullptr;
    ID3D11Buffer* boundVB_=nullptr;
    ID3D11Buffer* boundIB_=nullptr;
    ID3D11RenderTargetView* boundColor_=nullptr;
    ID3D11DepthStencilView* boundDepth_=nullptr;
    ID3D11DepthStencilState* boundDepthState_=nullptr;
    ID3D11BlendState* boundBlend_=nullptr;
    ID3D11RasterizerState* boundRaster_=nullptr;
    ID3D11PixelShader* boundPixel_=nullptr;
    unsigned boundStencil_=0;
    D3D11_RECT boundScissor_{};
    D3D11_VIEWPORT boundViewport_{};
    std::array<ID3D11ShaderResourceView*,16> boundViews_{};
    std::array<ID3D11SamplerState*,16> boundSamplers_{};
    Ptr<ID3D11VertexShader> transferVertex_;
    Ptr<ID3D11PixelShader> transferPixel_;
    Ptr<ID3D11DepthStencilState> transferDepth_;
    Ptr<ID3D11RasterizerState> transferRaster_;
    Ptr<ID3D11SamplerState> wrapSampler_,cubeSampler_;
    Ptr<ID3D11VertexShader> clearVertex_;
    Ptr<ID3D11PixelShader> clearPixel_;
    Ptr<ID3D11Buffer> clearConstants_;
    Ptr<ID3D11RasterizerState> clearRaster_;
    std::array<Ptr<ID3D11DepthStencilState>,4> clearDepthStates_;
    bool clearReady_=false;
    uint64_t fullClearRegions_=0,partialClearRegions_=0,emptyClearRegions_=0;
    void initializePartialClear();
    std::array<Ptr<ID3D11Query>,5> queries_;
    std::array<bool,5> queryPending_{};
    std::array<uint64_t,5> drawOrdinals_{},queryOrdinals_{};
    struct HistogramQuery {std::shared_ptr<Native::WorldQuery> source;Ptr<ID3D11Query> query;std::vector<Ptr<ID3D11Query>> segments;};
    HistogramQuery activeHistogram_;
    std::vector<HistogramQuery> pendingHistograms_;
    bool pauseHistogram();
    void resumeHistogram();
    Ptr<ID3D11Buffer> alphaTestConstants_;
    Surface& surface(uint64_t storageKey,uint32_t width,uint32_t height,bool depth);
    std::vector<uint8_t> readSurfaceKey(uint64_t storageKey,bool depth);
    ID3D11ShaderResourceView* image(const std::shared_ptr<const ColorImage>&);
    ID3D11SamplerState* sampler(const Native::WorldSampler&);
    void transfer(ID3D11ShaderResourceView*,ID3D11RenderTargetView*,const std::array<uint32_t,4>& source,
                  const std::array<uint32_t,2>& offset,float scale);
    bool promptKeyboardMouse_ = true;
    bool bloom_ = true;
    bool motionBlur_ = false;
public:
    WorldRendererD3D11(ID3D11Device*,ID3D11DeviceContext*,uint32_t scale=1);
    void inspectNextFrame() {inspectFrame_=true;++inspection_;inspectedPrograms_.clear();smokeRecords_=0;smokeTruncated_=false;smokeDrawOrdinal_=0;}
    void setDiagnostics(bool enabled) {diagnostics_=enabled;}
    void setPromptSource(bool keyboardMouse) {promptKeyboardMouse_ = keyboardMouse;}
    void printPerformance();
    void beginFrame();
    // Required after external code changes the shared immediate context.
    void invalidateBindings() {bindingsValid_=false;}
    size_t textureBudget() const {return imageBudget_;}
    bool draw(const Native::WorldDraw&);
    void clear(const Native::WorldClear&);
    bool resolve(const Native::WorldResolve&);
    bool present(const Native::WorldTexture&,ID3D11Texture2D*);
    void histogram(const std::shared_ptr<Native::WorldQuery>&,bool begin);
    void pollHistograms();
    // Explicit diagnostic readbacks. Identity-only calls address synthetic
    // commands; guest commands must use their owned completed binding.
    // Returns physical GPU pixels (logical width/height multiplied by scale).
    std::vector<uint8_t> readSurface(uint32_t identity,bool depth);
    std::vector<uint8_t> readSurface(const Native::WorldSurfaceTargets&,unsigned slot);
};
}
