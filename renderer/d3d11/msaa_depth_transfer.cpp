#include "msaa_depth_transfer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace DarkRecomp {
namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;

void check(HRESULT status, const char* message) {
    if (FAILED(status)) throw std::runtime_error(message);
}

constexpr char transferShader[] = R"hlsl(
float4 transferVertex(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

#if COLOR_TRANSFER
Texture2D<float4> colorSource : register(t0);
float4 transferColorPixel(float4 position : SV_Position) : SV_Target {
    return colorSource.Load(int3(int2(position.xy), 0));
}
#else
#if MULTISAMPLED
Texture2DMS<float> depthSource : register(t0);
#if COPY_STENCIL
#if STENCIL_GREEN
Texture2DMS<uint2> stencilSource : register(t1);
#else
Texture2DMS<uint> stencilSource : register(t1);
#endif
#endif
#else
Texture2D<float> depthSource : register(t0);
#if COPY_STENCIL
#if STENCIL_GREEN
Texture2D<uint2> stencilSource : register(t1);
#else
Texture2D<uint> stencilSource : register(t1);
#endif
#endif
#endif

#if COPY_STENCIL
cbuffer StencilSelection : register(b0) {
    uint stencilReference;
    uint3 padding;
};
#endif

float transferPixel(float4 position : SV_Position) : SV_Depth {
    int2 pixel = int2(position.xy);
#if COPY_STENCIL
#if MULTISAMPLED
    uint sourceStencil = stencilSource.Load(pixel, 0)
#else
    uint sourceStencil = stencilSource.Load(int3(pixel, 0))
#endif
#if STENCIL_GREEN
        .g
#endif
        ;
    if (sourceStencil != stencilReference) discard;
#endif
#if MULTISAMPLED
    // Native depth resolves have no unique depth average. Keep stencil and
    // depth from the same deterministic source sample during mode changes.
    return depthSource.Load(pixel, 0);
#else
    return depthSource.Load(int3(pixel, 0));
#endif
}
#endif
)hlsl";

Ptr<ID3DBlob> compile(const char* entry, const char* target, bool multisampled = false,
                     bool stencil = false, bool green = false, bool color = false) {
    const D3D_SHADER_MACRO macros[] = {
        {"MULTISAMPLED", multisampled ? "1" : "0"},
        {"COPY_STENCIL", stencil ? "1" : "0"},
        {"STENCIL_GREEN", green ? "1" : "0"},
        {"COLOR_TRANSFER", color ? "1" : "0"},
        {nullptr, nullptr},
    };
    Ptr<ID3DBlob> bytecode, errors;
    const HRESULT status = D3DCompile(transferShader, sizeof(transferShader) - 1,
        "msaa_depth_transfer", macros, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
    if (FAILED(status)) {
        std::string message = "Cannot compile MSAA depth transfer shader";
        if (errors) message += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                                 errors->GetBufferSize());
        throw std::runtime_error(message);
    }
    return bytecode;
}

class ScopedContextState {
    ID3D11DeviceContext1* context_;
    Ptr<ID3DDeviceContextState> previous_;
public:
    ScopedContextState(ID3D11DeviceContext1* context, ID3DDeviceContextState* isolated)
        : context_(context) {
        context_->SwapDeviceContextState(isolated, &previous_);
        context_->ClearState();
    }
    ~ScopedContextState() {
        // Release helper view bindings before returning to the saved state.
        // ClearState acts on the isolated state, never on the caller's state.
        context_->ClearState();
        context_->SwapDeviceContextState(previous_.Get(), nullptr);
    }
    ScopedContextState(const ScopedContextState&) = delete;
    ScopedContextState& operator=(const ScopedContextState&) = delete;
};

struct TextureView {
    Ptr<ID3D11Resource> resource;
    D3D11_TEXTURE2D_DESC texture{};
    uint32_t width = 0, height = 0;
};

TextureView textureView(ID3D11View* view, uint32_t mip, ID3D11Device* device) {
    TextureView result;
    view->GetResource(&result.resource);
    Ptr<ID3D11Texture2D> texture;
    check(result.resource.As(&texture), "MSAA depth transfer requires a 2D texture");
    Ptr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.Get() != device) throw std::invalid_argument("MSAA depth transfer views belong to another device");
    texture->GetDesc(&result.texture);
    if (mip >= result.texture.MipLevels || mip >= 32)
        throw std::invalid_argument("MSAA depth transfer mip is outside the source texture");
    result.width = (std::max)(1u, result.texture.Width >> mip);
    result.height = (std::max)(1u, result.texture.Height >> mip);
    return result;
}

TextureView shaderView(ID3D11ShaderResourceView* view, bool multisampled, ID3D11Device* device,
                       D3D11_SHADER_RESOURCE_VIEW_DESC& desc) {
    view->GetDesc(&desc);
    const auto expected = multisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    if (desc.ViewDimension != expected)
        throw std::invalid_argument("MSAA depth transfer source view has the wrong sample dimension");
    return textureView(view, multisampled ? 0 : desc.Texture2D.MostDetailedMip, device);
}

bool hasStencil(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}
}

MsaaDepthTransferD3D11::MsaaDepthTransferD3D11(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device) {
    if (!device || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        throw std::invalid_argument("MSAA depth transfer requires a device and its immediate context");
    Ptr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != device)
        throw std::invalid_argument("MSAA depth transfer context belongs to another device");
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
        throw std::runtime_error("MSAA depth transfer requires Direct3D feature level 11");
    Ptr<ID3D11Device1> device1;
    check(device->QueryInterface(IID_PPV_ARGS(&device1)), "MSAA depth transfer requires Direct3D 11.1 context states");
    check(context->QueryInterface(IID_PPV_ARGS(&context_)), "MSAA depth transfer requires a Direct3D 11.1 context");
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1
        ? D3D_FEATURE_LEVEL_11_1 : D3D_FEATURE_LEVEL_11_0;
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
        __uuidof(ID3D11Device1), nullptr, &isolatedState_), "Cannot create the MSAA depth transfer context state");

    const auto vertex = compile("transferVertex", "vs_5_0");
    check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertex_),
          "Cannot create MSAA depth transfer vertex shader");
    const auto color = compile("transferColorPixel", "ps_5_0", false, false, false, true);
    check(device->CreatePixelShader(color->GetBufferPointer(), color->GetBufferSize(), nullptr, &colorPixel_),
          "Cannot create MSAA retained-color transfer pixel shader");
    for (unsigned samples = 0; samples < 2; ++samples) {
        const auto depth = compile("transferPixel", "ps_5_0", samples != 0);
        check(device->CreatePixelShader(depth->GetBufferPointer(), depth->GetBufferSize(), nullptr, &depthPixels_[samples]),
              "Cannot create MSAA depth transfer pixel shader");
        for (unsigned green = 0; green < 2; ++green) {
            const auto stencil = compile("transferPixel", "ps_5_0", samples != 0, true, green != 0);
            check(device->CreatePixelShader(stencil->GetBufferPointer(), stencil->GetBufferSize(), nullptr,
                &stencilPixels_[samples][green]), "Cannot create MSAA stencil transfer pixel shader");
        }
    }
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 16;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&constants, nullptr, &stencilReference_), "Cannot create MSAA stencil transfer constants");

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.StencilReadMask = depth.StencilWriteMask = 0xff;
    depth.FrontFace.StencilFailOp = depth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    depth.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    depth.BackFace = depth.FrontFace;
    check(device->CreateDepthStencilState(&depth, &depthDisabled_), "Cannot create MSAA color transfer depth state");
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    check(device->CreateDepthStencilState(&depth, &depthOnly_), "Cannot create MSAA depth transfer state");
    depth.StencilEnable = TRUE;
    check(device->CreateDepthStencilState(&depth, &depthAndStencil_), "Cannot create MSAA depth/stencil transfer state");

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable = TRUE;
    check(device->CreateRasterizerState(&raster, &rasterizer_), "Cannot create MSAA depth transfer rasterizer");
}

void MsaaDepthTransferD3D11::copy(ID3D11ShaderResourceView* depthSource, ID3D11ShaderResourceView* stencilSource,
                               bool sourceMultisampled, ID3D11DepthStencilView* destination,
                               uint32_t width, uint32_t height, bool preserveStencil) {
    if (!depthSource || !destination || !width || !height || (preserveStencil && !stencilSource))
        throw std::invalid_argument("MSAA depth transfer requires source/destination views and nonzero dimensions");
    D3D11_SHADER_RESOURCE_VIEW_DESC depthDesc{};
    const auto depth = shaderView(depthSource, sourceMultisampled, device_.Get(), depthDesc);
    if (depthDesc.Format != DXGI_FORMAT_R16_UNORM && depthDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
        depthDesc.Format != DXGI_FORMAT_R32_FLOAT && depthDesc.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
        throw std::invalid_argument("MSAA depth transfer source does not expose a depth component");
    D3D11_DEPTH_STENCIL_VIEW_DESC destinationDesc{};
    destination->GetDesc(&destinationDesc);
    if (destinationDesc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D &&
        destinationDesc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2DMS)
        throw std::invalid_argument("MSAA depth transfer destination must be a 2D depth view");
    if ((destinationDesc.Flags & D3D11_DSV_READ_ONLY_DEPTH) ||
        (preserveStencil && ((destinationDesc.Flags & D3D11_DSV_READ_ONLY_STENCIL) || !hasStencil(destinationDesc.Format))))
        throw std::invalid_argument("MSAA depth transfer destination must allow the requested depth/stencil writes");
    const auto target = textureView(destination, destinationDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D
        ? destinationDesc.Texture2D.MipSlice : 0, device_.Get());
    if (depth.resource.Get() == target.resource.Get())
        throw std::invalid_argument("MSAA depth transfer source and destination must be distinct resources");
    if (width > depth.width || height > depth.height || width > target.width || height > target.height)
        throw std::invalid_argument("MSAA depth transfer rectangle exceeds its source or destination");
    bool green = false;
    if (preserveStencil) {
        D3D11_SHADER_RESOURCE_VIEW_DESC stencilDesc{};
        const auto stencil = shaderView(stencilSource, sourceMultisampled, device_.Get(), stencilDesc);
        green = stencilDesc.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ||
                stencilDesc.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        if (!green && stencilDesc.Format != DXGI_FORMAT_R8_UINT && stencilDesc.Format != DXGI_FORMAT_R16_UINT &&
            stencilDesc.Format != DXGI_FORMAT_R32_UINT)
            throw std::invalid_argument("MSAA depth transfer source does not expose a stencil component");
        if (stencil.resource.Get() == target.resource.Get() || width > stencil.width || height > stencil.height ||
            stencil.texture.SampleDesc.Count != depth.texture.SampleDesc.Count)
            throw std::invalid_argument("MSAA depth transfer stencil view does not match the source rectangle/samples");
    }

    const ScopedContextState restore(context_.Get(), isolatedState_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(preserveStencil ? stencilPixels_[unsigned(sourceMultisampled)][unsigned(green)].Get()
                                        : depthPixels_[unsigned(sourceMultisampled)].Get(), nullptr, 0);
    context_->RSSetState(rasterizer_.Get());
    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context_->OMSetRenderTargets(0, nullptr, destination);
    ID3D11ShaderResourceView* sources[] = {depthSource, preserveStencil ? stencilSource : nullptr};
    context_->PSSetShaderResources(0, 2, sources);
    if (!preserveStencil) {
        context_->OMSetDepthStencilState(depthOnly_.Get(), 0);
        context_->Draw(3, 0);
        return;
    }
    context_->PSSetConstantBuffers(0, 1, stencilReference_.GetAddressOf());
    for (uint32_t reference = 0; reference < 256; ++reference) {
        const uint32_t constants[4] = {reference, 0, 0, 0};
        context_->UpdateSubresource(stencilReference_.Get(), 0, nullptr, constants, 0, 0);
        context_->OMSetDepthStencilState(depthAndStencil_.Get(), reference);
        context_->Draw(3, 0);
    }
}

void MsaaDepthTransferD3D11::copyColor(ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination,
                                    uint32_t width, uint32_t height) {
    if (!source || !destination || !width || !height)
        throw std::invalid_argument("MSAA color transfer requires source/destination views and nonzero dimensions");
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc{};
    const auto color = shaderView(source, false, device_.Get(), sourceDesc);
    D3D11_RENDER_TARGET_VIEW_DESC destinationDesc{};
    destination->GetDesc(&destinationDesc);
    if (destinationDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
        destinationDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS)
        throw std::invalid_argument("MSAA color transfer destination must be a 2D color view");
    const auto target = textureView(destination, destinationDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D
        ? destinationDesc.Texture2D.MipSlice : 0, device_.Get());
    if (color.resource.Get() == target.resource.Get())
        throw std::invalid_argument("MSAA color transfer source and destination must be distinct resources");
    if (width > color.width || height > color.height || width > target.width || height > target.height)
        throw std::invalid_argument("MSAA color transfer rectangle exceeds its source or destination");

    const ScopedContextState restore(context_.Get(), isolatedState_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(colorPixel_.Get(), nullptr, 0);
    context_->RSSetState(rasterizer_.Get());
    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context_->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    context_->OMSetRenderTargets(1, &destination, nullptr);
    context_->PSSetShaderResources(0, 1, &source);
    context_->Draw(3, 0);
}
}
