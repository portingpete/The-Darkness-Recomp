#pragma once
#include "renderer/engine/simple_mesh.h"
#include "world_renderer.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>
#include <unordered_map>

namespace DarkRecomp {
// Window-thread-only diagnostic renderer for the verified engine text subset.
// Keeps an owned target so window presentation cannot discard its contents.
class EnginePreviewD3D11 {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<IDXGISwapChain> swapChain_;
    Ptr<ID3D11Texture2D> target_, staging_;
    Ptr<ID3D11RenderTargetView> rtv_;
    Ptr<ID3D11ShaderResourceView> presentationSource_;
    Ptr<ID3D11RenderTargetView> presentationTarget_;
    Ptr<ID3D11VertexShader> presentationVs_;
    Ptr<ID3D11PixelShader> presentationPs_, antialiasingPs_;
    Ptr<ID3D11Buffer> presentationConstants_;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> ps_, videoPs_, colorPs_;
    Ptr<ID3D11InputLayout> layout_;
    Ptr<ID3D11Buffer> constants_;
    // Reused dynamic uploads for immediate GUI meshes. Per-frame immutable
    // buffer creation stalled the render thread; DISCARD mapping reuses the
    // same driver allocation without kernel transitions or fragmentation.
    Ptr<ID3D11Buffer> dynamicVB_, dynamicIB_;
    Ptr<ID3D11BlendState> blend_;
    Ptr<ID3D11DepthStencilState> depth_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11SamplerState> sampler_;
    struct CachedImage { std::shared_ptr<const AlphaImage> image; Ptr<ID3D11ShaderResourceView> view; };
    std::unordered_map<const AlphaImage*, CachedImage> textures_;
    struct CachedColor { std::shared_ptr<const ColorImage> image; Ptr<ID3D11ShaderResourceView> view; uint64_t used = 0; size_t bytes = 0; Ptr<ID3D11SamplerState> sampler; };
    std::unordered_map<const ColorImage*, CachedColor> colorTextures_;
    size_t colorBytes_ = 0;
    uint64_t colorUse_ = 0;
    std::shared_ptr<const VideoFrame> videoImage_;
    Ptr<ID3D11ShaderResourceView> videoY_, videoUV_;
    Ptr<ID3D11Texture2D> videoTexY_, videoTexUV_;
    uint32_t videoWidth_ = 0, videoHeight_ = 0;
    void uploadVideoFrame(const VideoFrame& video);
    uint32_t width_ = 0, height_ = 0;
    bool worldPresented_=false,frameOpen_=false,inspectionPending_=false;
    Native::PromptRenderSource framePromptSource_=Native::PromptRenderSource::KeyboardMouse;
    std::unique_ptr<WorldRendererD3D11> world_;
public:
    // Output dimensions are physical pixels; scale applies to guest world commands.
    EnginePreviewD3D11(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*,
                       uint32_t renderWidth = 0, uint32_t renderHeight = 0, uint32_t scale = 1);
    void releaseDisplayTarget();
    void render(const std::vector<SimpleMesh>& meshes,Native::PreviewFramePart part={});
    void copyToDisplay();
    bool frameInProgress() const {return frameOpen_;}
    bool worldPresented() const {return !frameOpen_ && worldPresented_;}
    void inspectNextWorldFrame() {inspectionPending_=true;}
    void setDiagnostics(bool enabled) {if(world_)world_->setDiagnostics(enabled);}
    void printPerformance() const {if(world_)world_->printPerformance();}
    size_t textureBudget() const {return world_->textureBudget();}
    uint32_t readPixel(uint32_t x, uint32_t y);
    void saveBmp(const std::filesystem::path& path);
};
}
