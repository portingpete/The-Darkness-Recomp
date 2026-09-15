#include "renderer/d3d11/display_context_d3d11.h"
#include <dxgi1_5.h>
#include <cstdio>
#include <stdexcept>

using Display=DarkRecomp::CDisplayContextD3D11;
static void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
struct Window {
    HWND handle=CreateWindowExW(0,L"STATIC",L"DarkRecomp presentation contract",
        WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Window(){require(handle!=nullptr,"Cannot create presentation test window");}
    ~Window(){DestroyWindow(handle);}
};
static bool supportedByChain(IDXGISwapChain* chain) {
    ComPtr<IDXGIFactory5> factory;
    BOOL supported=FALSE;
    return SUCCEEDED(chain->GetParent(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported))) && supported;
}
static void descriptor(Display& display,UINT flags,unsigned width,unsigned height) {
    DXGI_SWAP_CHAIN_DESC desc{};
    require(SUCCEEDED(display.GetSwapChain()->GetDesc(&desc)),"Cannot read live swap chain description");
    require(desc.Flags==flags,"Swap chain creation/resize lost presentation flags");
    require(desc.BufferDesc.Width==width && desc.BufferDesc.Height==height,"Wrong resized dimensions");
    require(desc.Windowed && desc.SwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.BufferCount==2,
            "Presentation changed fullscreen mode, flip policy or buffer count");
}
static void presentModes(Display& display,unsigned& accepted,unsigned& occluded) {
    // Real DXGI validates flags. A sync interval >0 combined with the tearing
    // present flag is illegal, so exercise both modes on the SAME live chain.
    for(unsigned interval:{0u,1u,2u,0u,1u,0u}) {
        display.Clear(Display::ClearColor,0,1,0,1,0);
        uint32_t color=0;
        require(display.ReadbackCenterPixel(color) && color==0xFF00FF00,"Presentation lost rendered contents");
        const auto status=display.Present(interval);
        require(SUCCEEDED(status),"Present rejected a sync/tearing flag combination");
        if(status==S_OK)++accepted;
        else if(status==DXGI_STATUS_OCCLUDED)++occluded;
        require(display.GetDevice()->GetDeviceRemovedReason()==S_OK,"Presentation removed the graphics device");
    }
}
static void exercise(bool enableTearing) {
    Window first,second;
    Display display(enableTearing);
    require(FAILED(display.Present(0)),"Uninitialized presentation reported success");
    display.Init(first.handle,64,64);
    const bool available=supportedByChain(display.GetSwapChain());
    const UINT flags=enableTearing && available?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    unsigned accepted=0,occluded=0;
    descriptor(display,flags,64,64);presentModes(display,accepted,occluded);
    for(unsigned size:{96u,80u,80u,64u}) {
        display.Resize(size,size);descriptor(display,flags,size,size);
        presentModes(display,accepted,occluded);
    }
    auto* original=display.GetDevice();
    bool rejected=false;
    try{display.Init(second.handle,0,64);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected && display.GetDevice()==original,"Failed initialization replaced live display resources");
    descriptor(display,flags,64,64);presentModes(display,accepted,occluded);
    // Initialization builds a new chain before destroying the old one. Flags
    // must be published after DestroyLocked clears the previous chain's state.
    display.Init(second.handle,72,72);descriptor(display,flags,72,72);
    presentModes(display,accepted,occluded);
    display.Destroy();require(FAILED(display.Present(0)),"Destroyed presentation reported success");
    display.Init(first.handle,64,64);descriptor(display,flags,64,64);
    presentModes(display,accepted,occluded);
    std::printf("PresentationContract requested=%u supported=%u swapFlags=0x%X accepted=%u occluded=%u; resize, reinit, pixels and VSync toggles passed.\n",
        unsigned(enableTearing),unsigned(available),flags,accepted,occluded);
}
int main() {
    try {
        exercise(true);exercise(false);
        std::puts("Hidden-window GPU contracts validate API behavior, not monitor VRR activation or visible smoothness.");
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"Presentation contract: %s\n",error.what());return 1;}
}
