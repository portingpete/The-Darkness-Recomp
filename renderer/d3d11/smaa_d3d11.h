#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace DarkRecomp {
// SMAA 1x, using the upstream high-quality preset and unmodified lookup data.
// Window-thread-only. All three passes run at the owned image's resolution;
// the returned image is then scaled and toned by the presentation renderer.
class SmaaD3D11 {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    struct Pass {
        Ptr<ID3D11VertexShader> vertex;
        Ptr<ID3D11PixelShader> pixel;
        UINT linearSampler = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        UINT pointSampler = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
    };
    std::array<Pass,3> passes_;
    struct Image {
        Ptr<ID3D11Texture2D> texture;
        Ptr<ID3D11RenderTargetView> target;
        Ptr<ID3D11ShaderResourceView> source;
    };
    Image edges_, weights_, output_;
    Ptr<ID3D11ShaderResourceView> area_, search_;
    Ptr<ID3D11SamplerState> linear_, point_;
    Ptr<ID3D11Buffer> metrics_;
    Ptr<ID3D11RasterizerState> raster_;
    Ptr<ID3D11DepthStencilState> depth_;
    UINT width_ = 0, height_ = 0;
    Image image(UINT width, UINT height, DXGI_FORMAT format);
    void ensureTargets(UINT width, UINT height);
public:
    SmaaD3D11(ID3D11Device*, ID3D11DeviceContext*);
    ID3D11ShaderResourceView* filter(ID3D11ShaderResourceView* source);
};
}
