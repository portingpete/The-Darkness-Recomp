#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace DarkRecomp {

// Native host display backend. This is not a guest CDisplayContext object:
// its C++ methods and vtable do not match the original engine's guest ABI.
// See build_native/run/director-renderer-boundary-notes.md before adding hooks.
class CDisplayContextD3D11 {
public:
    // Disabling tearing keeps the compatibility path available to diagnostics.
    explicit CDisplayContextD3D11(bool enableTearing = true);
    virtual ~CDisplayContextD3D11();

    // Host-only mask; these values are not the original engine's clear flags.
    static constexpr uint32_t ClearColor = 1;
    static constexpr uint32_t ClearDepth = 2;
    static constexpr uint32_t ClearStencil = 4;
    static constexpr uint32_t ClearAll = ClearColor | ClearDepth | ClearStencil;

    // Host API only. Guest calls require separately verified argument mappings.
    virtual void Destroy();
    virtual void Init(HWND hWnd, uint32_t width, uint32_t height);
    // Window thread only, with direct renderer operations stopped. Keeps the
    // device and all game resources alive; callers must invalidate bound state.
    void Resize(uint32_t width, uint32_t height);
    virtual void SetViewport(float x, float y, float w, float h);
    virtual void Clear(uint32_t flags, float r, float g, float b, float a,
                       float depth, uint8_t stencil = 0);
    // Returns the actual DXGI status, including occlusion or device failure.
    virtual HRESULT Present(unsigned syncInterval = 1);

    // Unimplemented placeholders; these do not submit game rendering.
    virtual void BindTexture(uint32_t slot, void* textureResource);
    virtual void SetRenderState(uint32_t state, uint32_t value);
    virtual void BeginScene();
    virtual void EndScene();

    // Read before Present: the discard swapchain does not retain its contents.
    // RGBA bytes are returned as 0xAABBGGRR on the native little-endian host.
    bool ReadbackCenterPixel(uint32_t& rgba);
    bool IsInitialized() const;

    // Borrowed host handles. Callers must exclude concurrent Init/Destroy and
    // serialize any direct context use with their other rendering operations.
    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    IDXGISwapChain* GetSwapChain() const { return m_swapChain.Get(); }

private:
    void DestroyLocked();
    mutable std::mutex m_mutex;
    HWND m_hWnd;
    uint32_t m_width;
    uint32_t m_height;
    const bool m_enableTearing;
    UINT m_swapChainFlags = 0;
    UINT m_lastSyncInterval = ~0u, m_lastPresentFlags = ~0u;
    // Cached exclusive-fullscreen state for the tearing fast path. The chain
    // is created windowed with DXGI_MWA_NO_ALT_ENTER, so exclusive transitions
    // are rare; re-query at most every kFullscreenPollPresents or on failure.
    unsigned m_presentCalls = 0;
    BOOL m_cachedExclusive = FALSE;
    bool m_cachedExclusiveValid = false;
    static constexpr unsigned kFullscreenPollPresents = 60;

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGISwapChain>         m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backBufferRTV;
    ComPtr<ID3D11Texture2D>        m_depthStencilBuffer;
    ComPtr<ID3D11DepthStencilView> m_depthStencilView;
    ComPtr<ID3D11Texture2D>        m_readback;
};

} // namespace DarkRecomp
