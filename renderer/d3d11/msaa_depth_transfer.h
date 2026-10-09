#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace DarkRecomp {
// Window-thread-only depth migration. MSAA reads select sample zero and write
// that value to every destination sample. The caller owns query boundaries.
class MsaaDepthTransferD3D11 {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext1> context_;
    Ptr<ID3DDeviceContextState> isolatedState_;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> colorPixel_;
    std::array<Ptr<ID3D11PixelShader>, 2> depthPixels_;
    // [single/MSAA source][scalar/G-channel stencil SRV].
    std::array<std::array<Ptr<ID3D11PixelShader>, 2>, 2> stencilPixels_;
    Ptr<ID3D11Buffer> stencilReference_;
    Ptr<ID3D11DepthStencilState> depthDisabled_, depthOnly_, depthAndStencil_;
    Ptr<ID3D11RasterizerState> rasterizer_;
public:
    MsaaDepthTransferD3D11(ID3D11Device*, ID3D11DeviceContext*);
    // Copy the top-left width x height region between distinct 2D resources.
    // Stencil may be null when preserveStencil is false; existing destination
    // stencil is left intact. All immediate-context bindings are restored.
    void copy(ID3D11ShaderResourceView* depthSource, ID3D11ShaderResourceView* stencilSource,
              bool sourceMultisampled, ID3D11DepthStencilView* destination,
              uint32_t width, uint32_t height, bool preserveStencil);
    // Seed every sample of a color target from the retained single-sample image.
    // The top-left rectangle and all caller bindings have the same semantics.
    void copyColor(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination,
                   uint32_t width, uint32_t height);
};
}
