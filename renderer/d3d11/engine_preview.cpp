#include "engine_preview.h"
#include "renderer/engine/engine_performance.h"
#include "runtime/native/graphics_settings.h"
#include "renderer/engine/prompt_icons.h"
#include "renderer/engine/display_layout.h"
#include "renderer/engine/video_layout.h"
#include "runtime/native/input.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace DarkRecomp {
namespace {
void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char error[180];
    std::snprintf(error, sizeof(error), "Engine preview %s failed: 0x%08X", operation, unsigned(result));
    throw std::runtime_error(error);
}
// McDonald GDC12: UpdateSubresource costs more CPU than Map/Unmap on dynamic
// buffers. Preview constant uploads use DISCARD mapping.
void updateConstants(ID3D11DeviceContext* context,ID3D11Buffer* buffer,const void* data,size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"preview constant map");
    std::memcpy(mapped.pData,data,bytes);
    context->Unmap(buffer,0);
}
constexpr char shader[] = R"(
cbuffer Transform : register(b0) { row_major float4x4 projection; };
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct Fragment { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
Fragment vertexMain(Input v) {
    Fragment o; o.position = mul(float4(v.position, 1), projection);
    o.uv = v.uv; o.color = v.color; return o;
}
Texture2D<float> alphaImage : register(t0);
SamplerState imageSampler : register(s0);
float4 pixelMain(Fragment f) : SV_TARGET {
    return float4(f.color.rgb, f.color.a * alphaImage.Sample(imageSampler, f.uv));
}
Texture2D<float2> chromaImage : register(t1);
Texture2D<float4> colorImage : register(t2);
float4 colorMain(Fragment f) : SV_TARGET {
    return colorImage.Sample(imageSampler, f.uv) * f.color;
}
float4 videoMain(Fragment f) : SV_TARGET {
    // Original CMWnd_ModTexture_PaintVideo_YUV2RGB.fp: U in luminance,
    // V in alpha. BE A8L8 storage is V,U, uploaded unchanged as host R,G.
    float y = alphaImage.Sample(imageSampler, f.uv);
    float2 uv = chromaImage.Sample(imageSampler, f.uv).yx - 0.5;
    float3 rgb = y * 1.164 - 0.073035294117647058823529411764044;
    rgb += float3(1.596 * uv.y, -0.391 * uv.x - 0.813 * uv.y, 2.018 * uv.x);
    return float4(saturate(rgb * f.color.rgb), 0);
}
struct Presentation { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Presentation presentationVertex(uint id : SV_VertexID) {
    Presentation o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D<float4> presentationImage : register(t3);
cbuffer PresentationSettings : register(b1) { float4 displayTone; };
float4 applyDisplayTone(float4 color) {
    if (displayTone.x != 1.0) color.rgb = pow(saturate(color.rgb), displayTone.xxx);
    if (displayTone.y != 1.0) color.rgb = saturate(color.rgb * displayTone.y);
    return color;
}
float4 presentationPixel(Presentation f) : SV_TARGET {
    return applyDisplayTone(presentationImage.SampleLevel(imageSampler, f.uv, 0));
}
// Directional FXAA on the final, display-encoded UNORM image. Measure offsets
// in source texels so resizing and ultrawide output do not change the filter.
float4 antialiasingPixel(Presentation f) : SV_TARGET {
    uint width, height;
    presentationImage.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float4 center = presentationImage.SampleLevel(imageSampler, f.uv, 0);
    float3 nw = presentationImage.SampleLevel(imageSampler, f.uv + texel * float2(-1,-1), 0).rgb;
    float3 ne = presentationImage.SampleLevel(imageSampler, f.uv + texel * float2( 1,-1), 0).rgb;
    float3 sw = presentationImage.SampleLevel(imageSampler, f.uv + texel * float2(-1, 1), 0).rgb;
    float3 se = presentationImage.SampleLevel(imageSampler, f.uv + texel * float2( 1, 1), 0).rgb;
    const float3 luma = float3(0.299, 0.587, 0.114);
    float m = dot(center.rgb, luma);
    float a = dot(nw, luma), b = dot(ne, luma), c = dot(sw, luma), d = dot(se, luma);
    float low = min(m, min(min(a,b), min(c,d)));
    float high = max(m, max(max(a,b), max(c,d)));
    // Preserve flat areas and very low contrast detail exactly.
    if (high - low < max(1.0/32.0, high * (1.0/8.0))) return applyDisplayTone(center);
    float2 direction = float2((c+d)-(a+b), (a+c)-(b+d));
    float reduction = max((a+b+c+d) * (1.0/32.0), 1.0/128.0);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduction), -8.0, 8.0) * texel;
    float3 inner = 0.5 * (
        presentationImage.SampleLevel(imageSampler, f.uv - direction / 6.0, 0).rgb +
        presentationImage.SampleLevel(imageSampler, f.uv + direction / 6.0, 0).rgb);
    float3 outer = inner * 0.5 + 0.25 * (
        presentationImage.SampleLevel(imageSampler, f.uv - direction * 0.5, 0).rgb +
        presentationImage.SampleLevel(imageSampler, f.uv + direction * 0.5, 0).rgb);
    float filteredLuma = dot(outer, luma);
    return applyDisplayTone(float4(filteredLuma < low || filteredLuma > high ? inner : outer, center.a));
}
)";
}

EnginePreviewD3D11::EnginePreviewD3D11(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain,
                                     uint32_t renderWidth, uint32_t renderHeight, uint32_t scale)
    : device_(device), context_(context), swapChain_(swapChain) {
    if (!device || !context || !swapChain) throw std::invalid_argument("Engine preview requires initialized display");
    if (scale<1 || scale>3) throw std::invalid_argument("World render scale must be between one and three");
    Ptr<ID3D11Texture2D> back;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
    if (renderWidth && renderHeight) { desc.Width = renderWidth; desc.Height = renderHeight; }
    width_ = desc.Width; height_ = desc.Height;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; desc.MiscFlags = 0;
    check(device_->CreateTexture2D(&desc, nullptr, &target_), "create target");
    check(device_->CreateRenderTargetView(target_.Get(), nullptr, &rtv_), "create target view");
    check(device_->CreateShaderResourceView(target_.Get(), nullptr, &presentationSource_), "create presentation source");
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device_->CreateTexture2D(&desc, nullptr, &staging_), "create readback");
    Ptr<ID3DBlob> vertex, pixel, errors;
    HRESULT hr = D3DCompile(shader, sizeof(shader)-1, "engine_text_preview", nullptr, nullptr,
                           "vertexMain", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    check(hr, "compile vertex shader");
    check(D3DCompile(shader, sizeof(shader)-1, "engine_text_preview", nullptr, nullptr,
                    "pixelMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, &errors), "compile pixel shader");
    check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs_), "create vertex shader");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps_), "create pixel shader");
    pixel.Reset(); errors.Reset();
    check(D3DCompile(shader, sizeof(shader)-1, "engine_video_preview", nullptr, nullptr,
                    "videoMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, &errors), "compile video shader");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &videoPs_), "create video shader");
    pixel.Reset(); errors.Reset();
    check(D3DCompile(shader, sizeof(shader)-1, "engine_color_preview", nullptr, nullptr,
                    "colorMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, &errors), "compile color shader");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &colorPs_), "create color shader");
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    static_assert(sizeof(SimpleVertex) == 36);
    check(device_->CreateInputLayout(elements, 3, vertex->GetBufferPointer(), vertex->GetBufferSize(), &layout_), "create vertex layout");
    D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 64; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.Usage = D3D11_USAGE_DYNAMIC; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device_->CreateBuffer(&buffer, nullptr, &constants_), "create matrix buffer");
    buffer.ByteWidth = 16;
    check(device_->CreateBuffer(&buffer, nullptr, &presentationConstants_), "create presentation settings buffer");
    // Worst-case single immediate mesh: 16384 vertices (36 bytes each) and
    // 49152 indices. One DISCARD upload per mesh avoids per-frame kernel
    // allocations while keeping each draw's contents independent.
    D3D11_BUFFER_DESC dynamicDesc{};
    dynamicDesc.ByteWidth = 16384 * sizeof(SimpleVertex);
    dynamicDesc.Usage = D3D11_USAGE_DYNAMIC; dynamicDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    dynamicDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device_->CreateBuffer(&dynamicDesc, nullptr, &dynamicVB_), "create dynamic VB");
    dynamicDesc.ByteWidth = 49152 * sizeof(uint16_t);
    dynamicDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    check(device_->CreateBuffer(&dynamicDesc, nullptr, &dynamicIB_), "create dynamic IB");
    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0]; rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check(device_->CreateBlendState(&blend, &blend_), "create blending");
    D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    check(device_->CreateDepthStencilState(&depth, &depth_), "create depth state");
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.DepthClipEnable = TRUE;
    check(device_->CreateRasterizerState(&raster, &raster_), "create raster state");
    D3D11_SAMPLER_DESC sampler{}; sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX; sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    check(device_->CreateSamplerState(&sampler, &sampler_), "create sampler");
    vertex.Reset(); pixel.Reset();
    check(D3DCompile(shader, sizeof(shader)-1, "presentation", nullptr, nullptr,
                    "presentationVertex", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex, nullptr), "compile presentation vertex");
    check(D3DCompile(shader, sizeof(shader)-1, "presentation", nullptr, nullptr,
                    "presentationPixel", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, nullptr), "compile presentation pixel");
    check(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &presentationVs_), "create presentation vertex");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &presentationPs_), "create presentation pixel");
    pixel.Reset(); errors.Reset();
    hr = D3DCompile(shader, sizeof(shader)-1, "presentation", nullptr, nullptr,
                    "antialiasingPixel", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    check(hr, "compile FXAA pixel shader");
    check(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &antialiasingPs_), "create FXAA pixel shader");
    world_=std::make_unique<WorldRendererD3D11>(device,context,scale);
    render({});
}

void EnginePreviewD3D11::uploadVideoFrame(const VideoFrame& video) {
    if (videoTexY_ && videoTexUV_ && videoWidth_ == video.width && videoHeight_ == video.height) {
        context_->UpdateSubresource(videoTexY_.Get(), 0, nullptr, video.luma.data(), video.width, 0);
        context_->UpdateSubresource(videoTexUV_.Get(), 0, nullptr, video.chroma.data(), video.width, 0);
        return;
    }
    D3D11_TEXTURE2D_DESC yDesc{};
    yDesc.Width = video.width; yDesc.Height = video.height;
    yDesc.MipLevels = yDesc.ArraySize = yDesc.SampleDesc.Count = 1;
    yDesc.Format = DXGI_FORMAT_R8_UNORM;
    yDesc.Usage = D3D11_USAGE_DEFAULT; yDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA yData{video.luma.data(), video.width, 0};
    Ptr<ID3D11Texture2D> yTex;
    check(device_->CreateTexture2D(&yDesc, &yData, &yTex), "upload original video plane");
    D3D11_TEXTURE2D_DESC uvDesc{};
    uvDesc.Width = video.width / 2; uvDesc.Height = video.height / 2;
    uvDesc.MipLevels = uvDesc.ArraySize = uvDesc.SampleDesc.Count = 1;
    uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvDesc.Usage = D3D11_USAGE_DEFAULT; uvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA uvData{video.chroma.data(), video.width, 0};
    Ptr<ID3D11Texture2D> uvTex;
    check(device_->CreateTexture2D(&uvDesc, &uvData, &uvTex), "upload original video plane");
    Ptr<ID3D11ShaderResourceView> yView, uvView;
    check(device_->CreateShaderResourceView(yTex.Get(), nullptr, &yView), "create video plane view");
    check(device_->CreateShaderResourceView(uvTex.Get(), nullptr, &uvView), "create video plane view");
    videoTexY_ = std::move(yTex); videoTexUV_ = std::move(uvTex);
    videoY_ = std::move(yView); videoUV_ = std::move(uvView);
    videoWidth_ = video.width; videoHeight_ = video.height;
}

void EnginePreviewD3D11::render(const std::vector<SimpleMesh>& meshes,Native::PreviewFramePart part) {
    if(part.first==frameOpen_)throw std::logic_error("Preview frame parts are out of order");
    if(part.first) {
    world_->beginFrame();
    if(inspectionPending_) {world_->inspectNextFrame();inspectionPending_=false;}
    worldPresented_=false;
    // Prompt source is read once per frame: no per-mesh input locks, and all
    // prompt meshes in the frame agree. Labels are combined truthful icons, so
    // no mouse-capture context is claimed; the same source feeds the world.
    framePromptSource_ = Native::nativeInput().promptSource() == Native::PromptInputSource::Controller ?
        Native::PromptRenderSource::Controller : Native::PromptRenderSource::KeyboardMouse;
    world_->setPromptSource(framePromptSource_ == Native::PromptRenderSource::KeyboardMouse);
    if (framePromptSource_ == Native::PromptRenderSource::KeyboardMouse) {
        static std::atomic<unsigned> sourceLogs{0};
        if (sourceLogs.load() < 4) {
            sourceLogs++;
            std::fprintf(stderr, "[PromptSource] keyboard/mouse frame\n");
        }
    }
    world_->pollHistograms();
    const float black[] = {0,0,0,1};
    context_->ClearRenderTargetView(rtv_.Get(), black);
    }
    frameOpen_=!part.last;
    const auto promptSource=framePromptSource_;
    const auto promptContext=Native::PromptRenderContext::Menu;
    auto restoreSimpleState=[&] {
    world_->invalidateBindings();
    ID3D11RenderTargetView* target = rtv_.Get(); context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend_.Get(), nullptr, ~0u); context_->OMSetDepthStencilState(depth_.Get(), 0);
    context_->RSSetState(raster_.Get());
    D3D11_VIEWPORT vp{0,0,float(width_),float(height_),0,1}; context_->RSSetViewports(1, &vp);
    context_->IASetInputLayout(layout_.Get()); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0); context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0); context_->HSSetShader(nullptr, nullptr, 0); context_->DSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* constant = constants_.Get(); context_->VSSetConstantBuffers(0,1,&constant);
    ID3D11SamplerState* sampler = sampler_.Get(); context_->PSSetSamplers(0,1,&sampler);
    };
    bool simpleStateDirty = true;
    for (const auto& mesh : meshes) {
        Native::setRenderSamplePhase(Native::RenderSamplePhase::other);
        if(mesh.worldQuery) {world_->histogram(mesh.worldQuery,mesh.worldQueryBegin);continue;}
        // Partial clears replace the pipeline; surface growth also unbinds targets.
        if (mesh.worldClear) {world_->clear(*mesh.worldClear);simpleStateDirty=true;continue;}
        if (mesh.worldResolve) {
            if(!world_->resolve(*mesh.worldResolve)) {static unsigned failures=0;if(failures++<16)std::fprintf(stderr,"[EngineWorldResolveRejected] flags=%08X destination=%08X format=%u\n",mesh.worldResolve->flags,mesh.worldResolve->destination.object,mesh.worldResolve->destination.format);}
            simpleStateDirty = true;continue;
        }
        if (mesh.worldPresent) {worldPresented_=world_->present(*mesh.worldPresent,target_.Get());simpleStateDirty = true;continue;}
        if (mesh.world) {
            if(!world_->draw(*mesh.world)) {static unsigned failures=0;if(failures++<20)std::fprintf(stderr,"[EngineWorldDrawRejected] material=%u program=%s flags=%u\n",unsigned(mesh.world->material),mesh.world->fragmentName.c_str(),mesh.world->fragmentFlags);}
            simpleStateDirty = true;continue;
        }
        if (mesh.vertices.empty() || mesh.indices.empty() || int(bool(mesh.texture)) + int(bool(mesh.video)) + int(bool(mesh.colorTexture)) != 1 ||
            mesh.vertices.size() > 16384 || mesh.indices.size() > 49152)
            throw std::invalid_argument("Invalid preview mesh");
        for (auto index : mesh.indices) if (index >= mesh.vertices.size()) throw std::invalid_argument("Invalid preview index");
        if (simpleStateDirty) { restoreSimpleState(); simpleStateDirty = false; }
        if (isFullscreenVideo(mesh)) {
            const auto fit = fitDisplay(mesh.video->width, mesh.video->height, width_, height_);
            const D3D11_VIEWPORT viewport{fit.x, fit.y, fit.width, fit.height, 0, 1};
            context_->RSSetViewports(1, &viewport);
            simpleStateDirty = true;
        }
        ID3D11SamplerState* defaultSampler=sampler_.Get();context_->PSSetSamplers(0,1,&defaultSampler);
        if (mesh.video) {
            const auto& video = *mesh.video;
            if (!video.width || !video.height || video.width > 2048 || video.height > 2048 ||
                video.width % 2 || video.height % 2 || video.luma.size() != uint64_t(video.width) * video.height ||
                video.chroma.size() != uint64_t(video.width) * video.height / 2)
                throw std::invalid_argument("Invalid preview video frame");
            if (videoImage_ != mesh.video) {
                uploadVideoFrame(video);
                videoImage_ = mesh.video;
            }
            ID3D11ShaderResourceView* views[]{videoY_.Get(),videoUV_.Get(),nullptr};
            context_->PSSetShaderResources(0,3,views); context_->PSSetShader(videoPs_.Get(),nullptr,0);
            // Original blend factors ONE,ZERO; video shader deliberately outputs alpha zero.
            context_->OMSetBlendState(nullptr,nullptr,~0u);
        } else if (mesh.colorTexture) {
            // Verified prompt textures resolve to cached native keycap/mouse
            // icons under keyboard/mouse source; anything else (including the
            // controller source) keeps the original artwork and cache entry.
            // The draw's sampled UV bounds travel with the request so wide
            // canvases and atlas tiles keep exact sample behavior instead of
            // stretching or cropping the replacement (look tutorial 64x32).
            std::shared_ptr<const ColorImage> promptIcon;
            if (promptSource == Native::PromptRenderSource::KeyboardMouse && mesh.colorTexture &&
                DarkRecomp::Prompts::buttonFromOriginByte(mesh.colorTexture->promptOrigin) !=
                DarkRecomp::Prompts::Button::Unknown && !mesh.vertices.empty()) {
                float u0 = mesh.vertices[0].uv[0], v0 = mesh.vertices[0].uv[1];
                float u1 = u0, v1 = v0;
                for (const auto& vertex : mesh.vertices) {
                    if (!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1])) { u0 = 0; v0 = 0; u1 = 1; v1 = 1; break; }
                    u0 = (std::min)(u0, vertex.uv[0]); v0 = (std::min)(v0, vertex.uv[1]);
                    u1 = (std::max)(u1, vertex.uv[0]); v1 = (std::max)(v1, vertex.uv[1]);
                }
                promptIcon = Native::promptReplacementForUv(mesh.textureId, mesh.colorTexture,
                    promptSource, promptContext, u0, v0, u1, v1);
            } else {
                promptIcon = Native::promptReplacementFor(mesh.textureId, mesh.colorTexture, promptSource, promptContext);
            }
            const auto& effective = promptIcon ? promptIcon : mesh.colorTexture;
            const auto& image = *effective;
            if (!image.valid() || image.faces!=1)throw std::invalid_argument("Invalid preview color image");
            if (!colorTextures_.contains(effective.get())) {
                constexpr size_t budget=64*1024*1024;
                const unsigned levels=image.authoredMips?unsigned(image.mips.size()+1):1;
                size_t gpuBytes=0;
                for(unsigned mip=0;mip<levels;++mip)gpuBytes+=size_t((std::max)(1u,image.width>>mip))*(std::max)(1u,image.height>>mip)*4;
                while ((colorBytes_+gpuBytes>budget || colorTextures_.size()>=256) && !colorTextures_.empty()) {
                    auto oldest=std::min_element(colorTextures_.begin(),colorTextures_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
                    colorBytes_-=oldest->second.bytes;colorTextures_.erase(oldest);
                }
                D3D11_TEXTURE2D_DESC td{};td.Width=image.width;td.Height=image.height;
                td.MipLevels=levels;td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                Ptr<ID3D11Texture2D> texture;CachedColor cached{effective,{},0,gpuBytes,{}};
                check(device_->CreateTexture2D(&td,nullptr,&texture),"upload original color texture");
                for(unsigned mip=image.firstMip;mip<levels;++mip) {
                    const auto& pixels=mip?image.mips[mip-1]:image.pixels;
                    context_->UpdateSubresource(texture.Get(),mip,nullptr,pixels.data(),(std::max)(1u,image.width>>mip)*4,0);
                }
                check(device_->CreateShaderResourceView(texture.Get(),nullptr,&cached.view),"create color view");
                if(image.firstMip) {
                    D3D11_SAMPLER_DESC desc{};sampler_->GetDesc(&desc);desc.MinLOD=float(image.firstMip);
                    check(device_->CreateSamplerState(&desc,&cached.sampler),"create resident color sampler");
                }
                colorBytes_+=gpuBytes;colorTextures_.emplace(effective.get(),std::move(cached));
            }
            auto& cached = colorTextures_.at(effective.get()); cached.used = ++colorUse_;
            if(cached.sampler) {ID3D11SamplerState* sampler=cached.sampler.Get();context_->PSSetSamplers(0,1,&sampler);}
            ID3D11ShaderResourceView* views[]{nullptr,nullptr,cached.view.Get()};
            context_->PSSetShaderResources(0,3,views); context_->PSSetShader(colorPs_.Get(),nullptr,0);
            context_->OMSetBlendState(mesh.opaque ? nullptr : blend_.Get(),nullptr,~0u);
        } else {
          if (!mesh.texture->width || !mesh.texture->height || mesh.texture->width > 2048 || mesh.texture->height > 2048 ||
              mesh.texture->pixels.size() != uint64_t(mesh.texture->width) * mesh.texture->height)
              throw std::invalid_argument("Invalid preview alpha image");
          if (!textures_.contains(mesh.texture.get())) {
            if (textures_.size() >= 64) textures_.clear();
            D3D11_TEXTURE2D_DESC td{}; td.Width = mesh.texture->width; td.Height = mesh.texture->height;
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.Format = DXGI_FORMAT_R8_UNORM;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{mesh.texture->pixels.data(), mesh.texture->width, 0};
            Ptr<ID3D11Texture2D> texture; CachedImage cached{mesh.texture,{}};
            check(device_->CreateTexture2D(&td, &data, &texture), "upload original alpha pixels");
            check(device_->CreateShaderResourceView(texture.Get(), nullptr, &cached.view), "create alpha view");
            textures_.emplace(mesh.texture.get(), std::move(cached));
          }
          ID3D11ShaderResourceView* views[]{textures_.at(mesh.texture.get()).view.Get(),nullptr,nullptr};
          context_->PSSetShaderResources(0,3,views); context_->PSSetShader(ps_.Get(),nullptr,0);
          context_->OMSetBlendState(blend_.Get(),nullptr,~0u);
        }
        D3D11_MAPPED_SUBRESOURCE mappedVB{}, mappedIB{};
        check(context_->Map(dynamicVB_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mappedVB), "map dynamic VB");
        std::memcpy(mappedVB.pData,mesh.vertices.data(),mesh.vertices.size()*sizeof(SimpleVertex));
        context_->Unmap(dynamicVB_.Get(),0);
        check(context_->Map(dynamicIB_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mappedIB), "map dynamic IB");
        std::memcpy(mappedIB.pData,mesh.indices.data(),mesh.indices.size()*sizeof(uint16_t));
        context_->Unmap(dynamicIB_.Get(),0);
        ID3D11Buffer* vertexBuffer = dynamicVB_.Get(); UINT stride = sizeof(SimpleVertex), offset = 0;
        context_->IASetVertexBuffers(0,1,&vertexBuffer,&stride,&offset); context_->IASetIndexBuffer(dynamicIB_.Get(),DXGI_FORMAT_R16_UINT,0);
        updateConstants(context_.Get(),constants_.Get(),mesh.projection.data(),sizeof(mesh.projection));
        context_->DrawIndexed(UINT(mesh.indices.size()),0,0);
    }
}
void EnginePreviewD3D11::releaseDisplayTarget() {
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    presentationTarget_.Reset();
    world_->invalidateBindings();
}
void EnginePreviewD3D11::copyToDisplay() {
    if(frameOpen_)throw std::logic_error("Cannot display an unfinished preview frame");
    Ptr<ID3D11Texture2D> back; check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)), "get display target");
    D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
    const auto settings = Native::graphicsSettings();
    const bool antialiasing = settings.antialiasing;
    if (!antialiasing && settings.gammaPercent == 100 && settings.brightnessPercent == 100 &&
        desc.Width == width_ && desc.Height == height_) {
        context_->CopyResource(back.Get(), target_.Get());
        return;
    }
    if (!presentationTarget_)
        check(device_->CreateRenderTargetView(back.Get(), nullptr, &presentationTarget_), "create presentation target");
    context_->ClearState(); world_->invalidateBindings();
    const float black[]{0, 0, 0, 1};
    context_->ClearRenderTargetView(presentationTarget_.Get(), black);
    ID3D11RenderTargetView* target = presentationTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetDepthStencilState(depth_.Get(), 0);
    context_->RSSetState(raster_.Get());
    const auto fit = fitDisplay(width_, height_, desc.Width, desc.Height);
    const D3D11_VIEWPORT viewport{fit.x, fit.y, fit.width, fit.height, 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(presentationVs_.Get(), nullptr, 0);
    context_->PSSetShader(antialiasing ? antialiasingPs_.Get() : presentationPs_.Get(), nullptr, 0);
    const float tone[]{100.0f / settings.gammaPercent, settings.brightnessPercent / 100.0f, 0, 0};
    updateConstants(context_.Get(),presentationConstants_.Get(),tone,sizeof(tone));
    ID3D11Buffer* constants = presentationConstants_.Get(); context_->PSSetConstantBuffers(1, 1, &constants);
    ID3D11SamplerState* sampler = sampler_.Get(); context_->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* source = presentationSource_.Get(); context_->PSSetShaderResources(3, 1, &source);
    context_->Draw(3, 0);
    source = nullptr; context_->PSSetShaderResources(3, 1, &source);
}
uint32_t EnginePreviewD3D11::readPixel(uint32_t x, uint32_t y) {
    if(frameOpen_)throw std::logic_error("Cannot read an unfinished preview frame");
    if (x >= width_ || y >= height_) throw std::invalid_argument("Invalid preview pixel coordinate");
    context_->CopyResource(staging_.Get(), target_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&mapped), "read pixel");
    uint32_t value; std::memcpy(&value, static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4,4);
    context_->Unmap(staging_.Get(),0); return value;
}
void EnginePreviewD3D11::saveBmp(const std::filesystem::path& path) {
    if(frameOpen_)throw std::logic_error("Cannot capture an unfinished preview frame");
    std::vector<uint8_t> pixels(size_t(width_)*height_*4);
    context_->CopyResource(staging_.Get(), target_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&mapped), "capture frame");
    for (uint32_t y=0;y<height_;++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch;
        for (uint32_t x=0;x<width_;++x) {
            auto* output = pixels.data()+(size_t(y)*width_+x)*4;
            output[0]=row[x*4+2]; output[1]=row[x*4+1]; output[2]=row[x*4]; output[3]=255;
        }
    }
    context_->Unmap(staging_.Get(),0);
    BITMAPFILEHEADER file{}; file.bfType=0x4D42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+DWORD(pixels.size());
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=width_; info.biHeight=-LONG(height_);
    info.biPlanes=1; info.biBitCount=32; info.biSizeImage=DWORD(pixels.size());
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file)); output.write(reinterpret_cast<const char*>(&info),sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()); output.close();
    if (!output) throw std::runtime_error("Cannot save native preview frame");
}
}
