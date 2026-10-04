#include "renderer/d3d11/engine_preview.h"
#include "runtime/native/graphics_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "display_gamma_guest_tests.h"

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
using Microsoft::WRL::ComPtr;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static void check(HRESULT value,const char* message) {require(SUCCEEDED(value),message);}

struct Display {
    HWND window=CreateWindowExW(0,L"STATIC",L"Original display gamma contract",WS_OVERLAPPEDWINDOW,
                               0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    explicit Display(bool warp) {
        require(window!=nullptr,"Cannot create gamma test window");
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;desc.BufferDesc.Width=1024;desc.BufferDesc.Height=4;
        desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        D3D_FEATURE_LEVEL level;
        check(D3D11CreateDeviceAndSwapChain(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,
              nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&device,&level,&context),"Gamma test display creation failed");
    }
    ~Display() {if(context)context->ClearState();if(window)DestroyWindow(window);}
    std::vector<uint32_t> pixels() {
        ComPtr<ID3D11Texture2D> back,staging;
        check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"Gamma output buffer missing");
        D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc,nullptr,&staging),"Gamma readback allocation failed");
        context->CopyResource(staging.Get(),back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Gamma output map failed");
        std::vector<uint32_t> result(size_t(desc.Width)*desc.Height);
        for(unsigned y=0;y<desc.Height;++y)std::memcpy(result.data()+size_t(y)*desc.Width,
            static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,desc.Width*4);
        context->Unmap(staging.Get(),0);return result;
    }
    void resize(EnginePreviewD3D11& preview,unsigned width,unsigned height) {
        preview.releaseDisplayTarget();context->ClearState();
        check(chain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,0),"Gamma output resize failed");
    }
};
static SimpleMesh stripes() {
    auto white=std::make_shared<AlphaImage>();white->width=white->height=1;white->pixels={255};
    SimpleMesh mesh;mesh.texture=white;mesh.opaque=true;
    mesh.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(unsigned x=0;x<1024;++x) {
        const float left=float(x)/512-1,right=float(x+1)/512-1;
        const unsigned first=unsigned(mesh.vertices.size());
        const float r=float(x)/1023,g=float(1023-x)/1023,b=float((x*73)%1024)/1023;
        for(const auto point:{std::array<float,2>{left,-1},{right,-1},{right,1},{left,1}})
            mesh.vertices.push_back({{point[0],point[1],.5f},{0,0},{r,g,b,1}});
        for(unsigned index:{0u,1u,2u,0u,2u,3u})mesh.indices.push_back(uint16_t(first+index));
    }
    return mesh;
}
static unsigned input(unsigned x,unsigned c) {return c==0?x:c==1?1023-x:(x*73)%1024;}
static unsigned calibratedCode(const DisplayGamma& gamma,unsigned q,unsigned c) {
    if(!gamma.piecewise) {
        const unsigned index=(q*255*2+1023)/(1023*2);
        return gamma.entries[index*3+c][0]/64;
    }
    const auto& pair=gamma.entries[(q/8)*3+c];
    // Independent integer reference: keep delta fractions until UNORM10 storage.
    return std::min(1023u,(pair[0]*8+(q%8)*pair[1]+256)/512);
}
static void gpu(bool warp) {
    require(setGraphicsSettings({}),"Cannot reset display settings");
    Display display(warp);
    EnginePreviewD3D11 preview(display.device.Get(),display.context.Get(),display.chain.Get(),1024,4,1,true);
    auto mesh=stripes();
    unsigned comparisons=0;
    const auto retailTransfer=DisplayGammaGuestTestDetail::independentTransfer();
    for(unsigned mode=0;mode<3;++mode) {
        const bool piecewise=mode!=2;
        auto gamma=std::make_shared<DisplayGamma>();gamma->piecewise=piecewise;
        for(unsigned i=0;i<(piecewise?128u:256u);++i)for(unsigned c=0;c<3;++c) {
            unsigned base,delta;
            if(mode==0) {
                // Original 8286F970's neutral ramp, converted by the independent
                // retail HDTV oracle. Default calibration is not a PC pow curve.
                const unsigned start=(i*65535/127)>>6;
                const unsigned end=(std::min)(1023u,start+(((i+1)*65535/127-i*65535/127)>>6));
                base=retailTransfer[start];delta=retailTransfer[end]-base;
            } else if(piecewise) {
                base=c==0?i*4:c==1?i*8:128;
                delta=c==0?4:c==1?(i==127?0:8):(i%2?128:64);
            } else {base=c==0?i*2:c==1?i*4:(i*29)%1024;delta=0;}
            gamma->entries[i*3+c]={base*64,delta*64};
        }
        SimpleMesh state;state.displayGamma=gamma;
        display.resize(preview,1024,4);preview.render({state,mesh});preview.copyToDisplay();
        const auto first=display.pixels();
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<1024;++x) {
            const auto pixel=first[y*1024+x];
            for(unsigned c=0;c<3;++c) {
                const auto code=calibratedCode(*gamma,input(x,c),c);
                const auto expected=(code*255+511)/1023;
                if(std::abs(int((pixel>>(c*8))&255)-int(expected))>1) {
                    std::fprintf(stderr,"Gamma mode=%u x=%u c=%u actual=%u expected=%u\n",piecewise,x,c,(pixel>>(c*8))&255,expected);
                    require(false,"Display gamma differs from the independent Xbox LUT oracle");
                }
                ++comparisons;
            }
            require(pixel>>24==255,"Calibrated display alpha differs");
        }
        const auto raw=preview.readPixel(400,2);
        preview.copyToDisplay();require(display.pixels()==first,"Repeated calibration compounded the LUT");
        require(preview.readPixel(400,2)==raw,"Calibration changed the owned raw frontbuffer");
        // Nonlinear lookup must precede bilinear output scaling.
        display.resize(preview,512,2);preview.copyToDisplay();const auto half=display.pixels();
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<512;++x)for(unsigned c=0;c<3;++c) {
            const auto a=calibratedCode(*gamma,input(x*2,c),c),b=calibratedCode(*gamma,input(x*2+1,c),c);
            const double expected=(a+b)*255.0/(2*1023);
            require(std::abs(double((half[y*512+x]>>(c*8))&255)-expected)<=1.1,"Gamma was applied after resampling");
            ++comparisons;
        }
        // Black borders are added after calibration, including ramps with a lift.
        display.resize(preview,1024,8);preview.copyToDisplay();const auto bordered=display.pixels();
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<1024;++x)
            require(bordered[y*1024+x]==(y<2 || y>=6?0xff000000:first[(y-2)*1024+x]),"Gamma affected letterbox borders or scaled source texels");
        display.resize(preview,1024,4);
        require(preview.resizeRenderTarget(2048,8,2),"High precision gamma target could not resize");
        preview.render({mesh});preview.copyToDisplay();require(display.pixels()==first,"Gamma changed after a live render-scale increase");
        require(preview.resizeRenderTarget(1024,4,1),"High precision gamma target could not return to native scale");
    }
    std::printf("OriginalDisplayGamma %s passed: %u RGB checks, all1024 inputs, PWL/table, channel order, scaling order, borders, repaint and live resize.\n",warp?"WARP":"hardware",comparisons);
}
int main(int argc,char** argv) {
    try {
        bool warp=false;
        for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"--warp")==0)warp=true;
            else testGuestDisplayGamma(std::filesystem::path(argv[i]));
        gpu(warp);return 0;
    }catch(const std::exception& error) {std::fprintf(stderr,"OriginalDisplayGamma failed: %s\n",error.what());return 1;}
}
