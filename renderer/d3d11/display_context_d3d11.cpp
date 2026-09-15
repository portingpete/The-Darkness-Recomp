#include "display_context_d3d11.h"
#include <dxgi1_5.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace DarkRecomp {

namespace {
bool SupportsTearing() {
    // Query the optional interface instead of requiring a particular Windows
    // version. Older systems/tools retain the ordinary flip-model path.
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIFactory5> factory5;
    BOOL supported = FALSE;
    return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                &supported, sizeof(supported))) && supported;
}

void RequireSuccess(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char message[192]{};
    std::snprintf(message, sizeof(message), "D3D11 %s failed (HRESULT 0x%08X)",
                  operation, static_cast<unsigned>(result));
    throw std::runtime_error(message);
}
}

CDisplayContextD3D11::CDisplayContextD3D11(bool enableTearing)
    : m_hWnd(nullptr), m_width(0), m_height(0), m_enableTearing(enableTearing) {}

CDisplayContextD3D11::~CDisplayContextD3D11() {
    Destroy();
}

void CDisplayContextD3D11::Destroy() {
    std::lock_guard lock(m_mutex);
    DestroyLocked();
}

void CDisplayContextD3D11::DestroyLocked() {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_backBufferRTV.Reset();
    m_depthStencilView.Reset();
    m_depthStencilBuffer.Reset();
    m_readback.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_hWnd = nullptr;
    m_width = m_height = 0;
    m_swapChainFlags = 0;
    m_lastSyncInterval = m_lastPresentFlags = ~0u;
    m_presentCalls = 0;
    m_cachedExclusiveValid = false;
}

void CDisplayContextD3D11::Init(HWND hWnd, uint32_t width, uint32_t height) {
    if (!IsWindow(hWnd) || !width || !height ||
        width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("D3D11 Init requires a valid window and texture dimensions");
    std::lock_guard lock(m_mutex);
    // Build a complete resource set before replacing the current display.
    // Any failure releases these local COM references and preserves the old set.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> backBufferRTV;
    ComPtr<ID3D11Texture2D> depthStencilBuffer;
    ComPtr<ID3D11DepthStencilView> depthStencilView;
    ComPtr<ID3D11Texture2D> readback;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = m_enableTearing && SupportsTearing() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 1, D3D11_SDK_VERSION,
        &scd, &swapChain, &device, &featureLevel, &context
    );
    RequireSuccess(hr, "CreateDeviceAndSwapChain");
    ComPtr<IDXGIFactory> factory;
    RequireSuccess(swapChain->GetParent(IID_PPV_ARGS(&factory)), "get display factory");
    RequireSuccess(factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER), "disable automatic exclusive fullscreen");
    ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description)))
            std::fprintf(stderr, "[Performance] GPU=%ls dedicatedMiB=%llu\n", description.Description,
                         uint64_t(description.DedicatedVideoMemory / (1024 * 1024)));
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    RequireSuccess(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "GetBuffer");
    RequireSuccess(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRTV),
                   "CreateRenderTargetView");

    // Setup Depth Stencil
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    RequireSuccess(device->CreateTexture2D(&depthDesc, nullptr, &depthStencilBuffer),
                   "CreateTexture2D(depth)");
    RequireSuccess(device->CreateDepthStencilView(depthStencilBuffer.Get(), nullptr, &depthStencilView),
                   "CreateDepthStencilView");

    D3D11_TEXTURE2D_DESC readbackDesc{};
    readbackDesc.Width = width;
    readbackDesc.Height = height;
    readbackDesc.MipLevels = 1;
    readbackDesc.ArraySize = 1;
    readbackDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    RequireSuccess(device->CreateTexture2D(&readbackDesc, nullptr, &readback),
                   "CreateTexture2D(readback)");

    // Bind Default Render Target & Depth
    ID3D11RenderTargetView* rtvs[] = { backBufferRTV.Get() };
    context->OMSetRenderTargets(1, rtvs, depthStencilView.Get());
    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);

    DestroyLocked();
    m_device = std::move(device);
    m_context = std::move(context);
    m_swapChain = std::move(swapChain);
    m_swapChainFlags = scd.Flags;
    m_backBufferRTV = std::move(backBufferRTV);
    m_depthStencilBuffer = std::move(depthStencilBuffer);
    m_depthStencilView = std::move(depthStencilView);
    m_readback = std::move(readback);
    m_hWnd = hWnd;
    m_width = width;
    m_height = height;
    m_presentCalls = 0;
    m_cachedExclusiveValid = false;
    std::fprintf(stderr, "[Presentation] flipDiscard=1 swapChainFlags=0x%X tearingEligible=%u (monitor/driver VRR activation is separate)\n",
                 m_swapChainFlags, unsigned((m_swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0));
    std::cout << "DarkRecomp: Native CDisplayContextD3D11 initialized successfully (" << m_width << "x" << m_height << ")" << std::endl;
}

void CDisplayContextD3D11::Resize(uint32_t width, uint32_t height) {
    if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("Invalid native display dimensions");
    std::lock_guard lock(m_mutex);
    if (!m_swapChain) throw std::logic_error("Resize requires an initialized display");
    if (width == m_width && height == m_height) return;

    // Allocate independent attachments before touching the swap chain.
    ComPtr<ID3D11Texture2D> depthBuffer, readback;
    ComPtr<ID3D11DepthStencilView> depthView;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    RequireSuccess(m_device->CreateTexture2D(&desc, nullptr, &depthBuffer), "resize depth");
    RequireSuccess(m_device->CreateDepthStencilView(depthBuffer.Get(), nullptr, &depthView), "resize depth view");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    RequireSuccess(m_device->CreateTexture2D(&desc, nullptr, &readback), "resize readback");

    m_context->ClearState();
    m_backBufferRTV.Reset();
    // ALLOW_TEARING cannot be added/removed by resizing an existing chain.
    RequireSuccess(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_swapChainFlags), "ResizeBuffers");
    ComPtr<ID3D11Texture2D> back;
    RequireSuccess(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)), "resized back buffer");
    RequireSuccess(m_device->CreateRenderTargetView(back.Get(), nullptr, &m_backBufferRTV), "resized target view");
    m_depthStencilBuffer = std::move(depthBuffer);
    m_depthStencilView = std::move(depthView);
    m_readback = std::move(readback);
    m_width = width; m_height = height;
    m_presentCalls = 0;
    m_cachedExclusiveValid = false;
    ID3D11RenderTargetView* target = m_backBufferRTV.Get();
    m_context->OMSetRenderTargets(1, &target, m_depthStencilView.Get());
    const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    m_context->RSSetViewports(1, &viewport);
}

void CDisplayContextD3D11::SetViewport(float x, float y, float w, float h) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) ||
        w <= 0 || h <= 0 || x < D3D11_VIEWPORT_BOUNDS_MIN || y < D3D11_VIEWPORT_BOUNDS_MIN ||
        x + w > D3D11_VIEWPORT_BOUNDS_MAX || y + h > D3D11_VIEWPORT_BOUNDS_MAX)
        throw std::invalid_argument("Invalid native D3D11 viewport");
    std::lock_guard lock(m_mutex);
    if (!m_context) throw std::logic_error("D3D11 viewport requires initialized resources");
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void CDisplayContextD3D11::Clear(uint32_t flags, float r, float g, float b, float a,
                               float depth, uint8_t stencil) {
    if (flags & ~ClearAll) throw std::invalid_argument("Unknown native D3D11 clear flags");
    if ((flags & ClearColor) &&
        (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a)))
        throw std::invalid_argument("Nonfinite native D3D11 clear color");
    if ((flags & ClearDepth) && !std::isfinite(depth))
        throw std::invalid_argument("Nonfinite native D3D11 clear depth");
    std::lock_guard lock(m_mutex);
    if (!m_context) throw std::logic_error("D3D11 clear requires initialized resources");
    const float color[4] = { r, g, b, a };
    if (flags & ClearColor) {
        m_context->ClearRenderTargetView(m_backBufferRTV.Get(), color);
    }
    UINT depthFlags = 0;
    if (flags & ClearDepth) depthFlags |= D3D11_CLEAR_DEPTH;
    if (flags & ClearStencil) depthFlags |= D3D11_CLEAR_STENCIL;
    if (depthFlags)
        m_context->ClearDepthStencilView(m_depthStencilView.Get(), depthFlags,
                                        (flags & ClearDepth) ? depth : 0.0f, stencil);
}

HRESULT CDisplayContextD3D11::Present(unsigned syncInterval) {
    std::lock_guard lock(m_mutex);
    if (!m_swapChain) return E_UNEXPECTED;
    UINT flags = 0;
    if (!syncInterval && (m_swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        // Borderless fullscreen is windowed to DXGI. If another host caller
        // enters exclusive mode, the tearing present flag is not legal there.
        // Exclusive transitions are rare (automatic exclusive is disabled via
        // DXGI_MWA_NO_ALT_ENTER), so cache the query and re-poll periodically.
        // On INVALID_CALL with tearing, refresh immediately and retry once.
        if (!m_cachedExclusiveValid || (m_presentCalls % kFullscreenPollPresents) == 0) {
            BOOL exclusive = TRUE;
            if (SUCCEEDED(m_swapChain->GetFullscreenState(&exclusive, nullptr))) {
                m_cachedExclusive = exclusive;
                m_cachedExclusiveValid = true;
            } else {
                // Query failed: fail safe to no tearing without caching.
                m_cachedExclusiveValid = false;
                m_cachedExclusive = TRUE;
            }
        }
        if (!m_cachedExclusive)
            flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    if (m_lastSyncInterval != syncInterval || m_lastPresentFlags != flags) {
        std::fprintf(stderr, "[Presentation] syncInterval=%u presentFlags=0x%X\n", syncInterval, flags);
        m_lastSyncInterval = syncInterval; m_lastPresentFlags = flags;
    }
    HRESULT status = m_swapChain->Present(syncInterval, flags);
    ++m_presentCalls;
    if (flags != 0 && status == DXGI_ERROR_INVALID_CALL) {
        // Possibly entered exclusive mode since the cached query. Refresh and
        // retry once without tearing if now exclusive.
        BOOL exclusive = TRUE;
        if (SUCCEEDED(m_swapChain->GetFullscreenState(&exclusive, nullptr))) {
            m_cachedExclusive = exclusive;
            m_cachedExclusiveValid = true;
            if (exclusive) {
                if (m_lastPresentFlags != 0) {
                    std::fprintf(stderr, "[Presentation] syncInterval=%u presentFlags=0x0 (exclusive fallback)\n", syncInterval);
                    m_lastSyncInterval = syncInterval; m_lastPresentFlags = 0;
                }
                status = m_swapChain->Present(syncInterval, 0);
                ++m_presentCalls;
            }
        } else {
            m_cachedExclusiveValid = false;
        }
    }
    return status;
}

void CDisplayContextD3D11::BindTexture(uint32_t slot, void* textureResource) {
    throw std::logic_error("Native engine texture binding is not implemented");
}

void CDisplayContextD3D11::SetRenderState(uint32_t state, uint32_t value) {
    throw std::logic_error("Native engine render states are not implemented");
}

void CDisplayContextD3D11::BeginScene() {
    throw std::logic_error("Native engine frame begin is not implemented");
}

void CDisplayContextD3D11::EndScene() {
    throw std::logic_error("Native engine frame end is not implemented");
}

bool CDisplayContextD3D11::ReadbackCenterPixel(uint32_t& rgba) {
    std::lock_guard lock(m_mutex);
    if (!m_swapChain || !m_context || !m_readback) return false;
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    m_context->CopyResource(m_readback.Get(), backBuffer.Get());
    m_context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + (m_height / 2) * mapped.RowPitch + (m_width / 2) * 4;
    std::memcpy(&rgba, pixel, sizeof(rgba));
    m_context->Unmap(m_readback.Get(), 0);
    return true;
}

bool CDisplayContextD3D11::IsInitialized() const {
    std::lock_guard lock(m_mutex);
    return m_device && m_context && m_swapChain && m_backBufferRTV &&
           m_depthStencilBuffer && m_depthStencilView && m_readback;
}

} // namespace DarkRecomp
