#include "renderer/d3d11/display_context_d3d11.h"
#include "renderer/d3d11/engine_preview.h"
#include <bit>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>
#include "renderer/engine/display_layout.h"
#include "app/windows/display_options.h"

using Display = DarkRecomp::CDisplayContextD3D11;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Action>
static void rejects(Action action, const char* message) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error(message);
}

static void pixelEquals(Display& display, uint32_t expected, const char* operation) {
    uint32_t actual = 0x12345678;
    require(display.ReadbackCenterPixel(actual), "Native GPU readback failed");
    if (actual != expected) {
        std::fprintf(stderr, "%s: pixel 0x%08X, expected 0x%08X\n", operation, actual, expected);
        throw std::runtime_error("Native GPU clear contract failed");
    }
}

struct TestWindow {
    HWND handle = nullptr;
    TestWindow() {
        // A real renderable host window; no keyboard or desktop automation.
        handle = CreateWindowExW(0, L"STATIC", L"DarkRecomp renderer contract",
                                 WS_OVERLAPPEDWINDOW, 0, 0, 128, 128,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(handle != nullptr, "Cannot create renderer test window");
    }
    ~TestWindow() { if (handle) DestroyWindow(handle); }
};

static void scaledRendererContract(Display& display) {
    using namespace DarkRecomp;
    using namespace DarkRecomp::Native;
    auto* device=display.GetDevice();auto* context=display.GetContext();
    auto put=[](uint8_t* p,uint32_t value) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(24-i*8));};
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=20;geometry->formats[0]=3;geometry->formats[1]=2;
    geometry->vertices.resize(80);geometry->indices={0,1,2,0,2,3};
    const float vertices[4][5]{{-1,-1,.5f,0,1},{-1,1,.5f,0,0},{1,1,.5f,1,0},{1,-1,.5f,1,1}};
    for(unsigned v=0;v<4;++v)for(unsigned c=0;c<5;++c)
        put(geometry->vertices.data()+v*20+c*4,std::bit_cast<uint32_t>(vertices[v][c]));
    auto asset=std::make_shared<ColorImage>();asset->width=asset->height=2;
    asset->pixels={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};draw.viewport={4,3,24,16};draw.targets={401,0,0,0,404};
    draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    draw.options.modes.fill(4);draw.options.modes[0]=0;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    draw.textures[0]=asset;draw.samplers[0].valid=true;draw.samplers[0].address.fill(2);
    put(draw.attributes.data()+92,0x01100026);draw.attributes[96]=draw.attributes[97]=8;
    put(draw.attributes.data()+112,9|(6u<<16));put(draw.attributes.data()+116,23|(15u<<16));
    rejects<std::invalid_argument>([&]{WorldRendererD3D11 bad(device,context,0);},"Accepted zero world scale");
    rejects<std::invalid_argument>([&]{WorldRendererD3D11 bad(device,context,4);},"Accepted unsupported world scale");
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        WorldClear clear;clear.targets=draw.targets;clear.viewport={0,0,32,24};clear.flags=49;
        clear.color={0,0,0,1};clear.depth=0;clear.stencil=17;renderer.clear(clear);
        require(renderer.draw(draw),"Scaled viewport/scissor draw rejected");
        // CPU assets retain their actual upload dimensions even though the
        // fragment shader samples them while rasterizing a larger framebuffer.
        ComPtr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(0,1,&view);
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
        require(SUCCEEDED(resource.As(&texture)),"Scaled draw lost the asset texture");
        D3D11_TEXTURE2D_DESC assetDesc{};texture->GetDesc(&assetDesc);
        require(assetDesc.Width==2 && assetDesc.Height==2,"GPU scaling resized a CPU texture asset");
        const unsigned width=32*scale,height=24*scale;
        const auto color=renderer.readSurface(401,false),depth=renderer.readSurface(404,true);
        require(color.size()==size_t(width)*height*8 && depth.size()==size_t(width)*height*4,"Scaled raster target/readback extent differs");
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const bool inside=x>=9*scale && x<23*scale && y>=6*scale && y<15*scale;
            const unsigned quadrant=(y>=11*scale?2:0)+(x>=16*scale?1:0);
            uint16_t actual[4]{};std::memcpy(actual,color.data()+(size_t(y)*width+x)*8,8);
            for(unsigned c=0;c<4;++c) {
                const bool lit=c==3 || (inside && asset->pixels[quadrant*4+c]!=0);
                require(actual[c]==(lit?0x3c00:0),"Scaled viewport/scissor or normalized texture sampling differs");
            }
            uint32_t actualDepth=0;std::memcpy(&actualDepth,depth.data()+(size_t(y)*width+x)*4,4);
            require((actualDepth>>24)==17,"Scaled draw changed retained stencil");
            const auto z=actualDepth&0xffffff;
            require(inside?(z>=0x7fffff && z<=0x800000):z==0,"Scaled depth coverage differs from color");
        }
        // Grow both attachments without clearing. Every physical pixel of the
        // original rectangle must survive, including packed depth/stencil bits.
        auto growth=clear;growth.viewport={0,0,40,32};growth.rectangle=std::array<int32_t,4>{0,0,0,0};renderer.clear(growth);
        const unsigned grownWidth=40*scale,grownHeight=32*scale;
        const auto grownColor=renderer.readSurface(401,false),grownDepth=renderer.readSurface(404,true);
        require(grownColor.size()==size_t(grownWidth)*grownHeight*8 && grownDepth.size()==size_t(grownWidth)*grownHeight*4,"Scaled retained growth extent differs");
        for(unsigned y=0;y<grownHeight;++y)for(unsigned x=0;x<grownWidth;++x)for(bool isDepth:{false,true}) {
            const size_t bytes=isDepth?4:8;const auto& original=isDepth?depth:color;const auto& grown=isDepth?grownDepth:grownColor;
            const uint8_t zero[8]{};const auto* expected=x<width && y<height?original.data()+(size_t(y)*width+x)*bytes:zero;
            require(std::memcmp(grown.data()+(size_t(y)*grownWidth+x)*bytes,expected,bytes)==0,"Scaled attachment growth lost retained pixels");
        }
        WorldResolve copy;copy.targets=clear.targets;copy.destination={501,4096,40,32,6,0};
        copy.rectangle={5,4,21,14};copy.offset={7,9};copy.flags=0x300;
        copy.color={1,0,1,1};copy.depth=.25f;copy.stencil=42;
        require(renderer.resolve(copy),"Scaled partial resolve rejected");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=grownWidth;desc.Height=grownHeight;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D11Texture2D> output,staging;
        require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&output)),"Scaled present fixture failed");
        require(renderer.present(copy.destination,output.Get()),"Logical resolved descriptor failed physical presentation");
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"Scaled resolve readback fixture failed");
        context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Scaled resolve readback failed");
        std::vector<uint8_t> resolved(size_t(grownWidth)*grownHeight*4);
        for(unsigned y=0;y<grownHeight;++y)std::memcpy(resolved.data()+size_t(y)*grownWidth*4,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,grownWidth*4);
        context->Unmap(staging.Get(),0);
        const auto resetColor=renderer.readSurface(401,false),resetDepth=renderer.readSurface(404,true);
        for(unsigned y=0;y<grownHeight;++y)for(unsigned x=0;x<grownWidth;++x) {
            // Guest 21/14 rounds to 24/16 BEFORE multiplying by scale.
            const bool copied=x>=7*scale && x<26*scale && y>=9*scale && y<21*scale;
            uint16_t expected[4]{};
            if(copied)std::memcpy(expected,grownColor.data()+(size_t(y-5*scale)*grownWidth+x-2*scale)*8,8);
            for(unsigned c=0;c<4;++c)require(resolved[(size_t(y)*grownWidth+x)*4+c]==(expected[c]?255:0),"Scaled resolve source, destination offset or eight-pixel fringe differs");
            const bool cleared=x>=5*scale && x<24*scale && y>=4*scale && y<16*scale;
            const uint16_t reset[4]{0x3c00,0,0x3c00,0x3c00};
            require(std::memcmp(resetColor.data()+(size_t(y)*grownWidth+x)*8,cleared?static_cast<const void*>(reset):grownColor.data()+(size_t(y)*grownWidth+x)*8,8)==0,"Scaled resolve cleared the wrong source color region");
            uint32_t actual=0;std::memcpy(&actual,resetDepth.data()+(size_t(y)*grownWidth+x)*4,4);
            if(cleared)require((actual>>24)==42 && (actual&0xffffff)>=0x3fffff && (actual&0xffffff)<=0x400000,"Scaled partial depth/stencil clear differs");
            else require(std::memcmp(&actual,grownDepth.data()+(size_t(y)*grownWidth+x)*4,4)==0,"Scaled partial clear changed neighboring depth/stencil");
        }
        // Sample the resolved GPU resource using the original guest descriptor
        // and normalized UVs, with no CPU fallback texture available.
        auto sampled=draw;sampled.targets={601,0,0,0,0};sampled.viewport={0,0,40,32};
        sampled.textures[0].reset();sampled.textureObjects[0]=copy.destination;put(sampled.attributes.data()+92,0x01100000);
        require(renderer.draw(sampled),"Logical descriptor rejected a scaled resolved resource");
        const auto sampledPixels=renderer.readSurface(601,false);
        for(size_t i=0;i<resolved.size();++i) {
            uint16_t actual=0;std::memcpy(&actual,sampledPixels.data()+i*2,2);
            require(actual==(resolved[i]?0x3c00:0),"Scaled resolved texture sampling lost physical pixel detail");
        }
        auto measured=draw;measured.material=WorldMaterial::post;measured.fragmentName="XREngine_Histogram";
        measured.fragmentConstants[0]={0,1,0,0};measured.fragmentConstants[1]={1,0,0,0};
        auto white=std::make_shared<ColorImage>();white->width=white->height=1;white->pixels={255,255,255,255};measured.textures[0]=white;
        put(measured.attributes.data()+92,0x01100020);
        auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
        renderer.histogram(query,true);require(renderer.draw(measured),"Scaled histogram first draw rejected");
        auto helper=clear;helper.targets={701,0,0,0,704};helper.rectangle=std::array<int32_t,4>{1,1,5,6};renderer.clear(helper);
        copy.flags=0;require(renderer.resolve(copy),"Scaled histogram resolve helper rejected");
        require(renderer.draw(measured),"Scaled histogram resumed draw rejected");renderer.histogram(query,false);
        context->Flush();const auto deadline=GetTickCount64()+5000;
        while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
        require(query->result->samples.load()==2*14*9,"Scaled histogram did not preserve logical counts or included helper geometry");
        std::printf("RendererScale%u passed: physical raster, asset dimensions, scissor, retained color/depth, resolve, sample and histogram.\n",scale);
    }
    context->ClearState();
    EnginePreviewD3D11 preview(device,context,display.GetSwapChain(),64,48,2);
    auto clear=std::make_shared<WorldClear>();clear->targets={801,0,0,0,0};clear->viewport={0,0,32,24};clear->flags=1;clear->color={0,1,0,1};
    auto resolve=std::make_shared<WorldResolve>();resolve->targets=clear->targets;resolve->rectangle={0,0,32,24};resolve->destination={901,8192,32,24,6,0};
    std::vector<SimpleMesh> frame(3);frame[0].worldClear=clear;frame[1].worldResolve=resolve;frame[2].worldPresent=std::make_shared<WorldTexture>(resolve->destination);
    preview.render(frame);
    require(preview.worldPresented() && preview.readPixel(63,47)==0xff00ff00,"EnginePreview did not forward scale to its physical output target");
    context->ClearState();
}

int main(int argc,char** argv) {
    try {
        TestWindow window;
        if(argc>1 && std::strcmp(argv[1],"--scaled-only")==0) {
            Display display;display.Init(window.handle,64,64);scaledRendererContract(display);return 0;
        }
        uint32_t dimension = 7;
        require(parseDisplayDimension(L"3440", dimension) && dimension == 3440, "Display width parse failed");
        for (const auto text : {L"", L"0", L"-1", L"12x", L"16385", L"99999999999999999999"})
            require(!parseDisplayDimension(text, dimension), "Invalid display dimension accepted");
        const auto wideRender = renderSizeForDisplay({3440,1440});
        const auto superwideRender = renderSizeForDisplay({5120,1440});
        require(wideRender.width == 1720 && wideRender.height == 720, "21:9 render aspect wrong");
        require(superwideRender.width == 2560 && superwideRender.height == 720, "32:9 render aspect wrong");
        const auto boundedRender = renderSizeForDisplay({5120,1440}, 2160);
        require(boundedRender.width == 4096 && boundedRender.height == 1152, "Unsupported internal width was not capped");
        const auto nativeRender = renderSizeForDisplay({3440,1440},1440);
        require(nativeRender.width == 3440 && nativeRender.height == 1440, "1440p native rendering was downscaled");
        const auto wide = DarkRecomp::fitDisplay(1280, 720, 2560, 1080);
        require(wide.x == 320 && wide.y == 0 && wide.width == 1920 && wide.height == 1080,
                "16:9 content stretched on ultrawide output");
        const auto tall = DarkRecomp::fitDisplay(2560, 720, 1280, 720);
        require(tall.x == 0 && tall.y == 180 && tall.width == 1280 && tall.height == 360,
                "32:9 content cropped on narrower output");
        Display display;
        uint32_t untouched = 0x12345678;
        require(!display.IsInitialized(), "New display reported ready");
        require(FAILED(display.Present()), "Uninitialized Present reported success");
        require(!display.ReadbackCenterPixel(untouched) && untouched == 0x12345678,
                "Failed readback changed its output");
        rejects<std::invalid_argument>([&] { display.Init(nullptr, 64, 64); }, "Accepted null window");
        rejects<std::invalid_argument>([&] { display.Init(window.handle, 0, 64); }, "Accepted zero width");
        rejects<std::invalid_argument>([&] { display.Init(window.handle, 64, 0); }, "Accepted zero height");
        require(!display.IsInitialized(), "Failed init left a ready display");
        rejects<std::logic_error>([&] { display.Clear(Display::ClearColor, 1, 0, 0, 1, 1); },
                                  "Uninitialized clear reported success");

        display.Init(window.handle, 64, 64);
        require(display.IsInitialized(), "Missing native render resources");
        scaledRendererContract(display);
        display.SetViewport(0, 0, 64, 64);
        display.Clear(Display::ClearAll, 1, 0, 1, 1, 0.75f, 23);
        pixelEquals(display, 0xFFFF00FF, "color clear");
        display.Clear(0, 0, 1, 0, 0, 0);
        pixelEquals(display, 0xFFFF00FF, "zero flags preserve color");
        display.Clear(Display::ClearDepth, 0, 1, 0, 0, 0.25f);
        pixelEquals(display, 0xFFFF00FF, "depth-only preserves color");
        display.Clear(Display::ClearStencil, 0, 1, 0, 0, 0, 7);
        pixelEquals(display, 0xFFFF00FF, "stencil-only preserves color");
        display.Clear(Display::ClearDepth | Display::ClearStencil, 0, 1, 0, 0, 1, 255);
        pixelEquals(display, 0xFFFF00FF, "depth/stencil preserve color");

        rejects<std::invalid_argument>([&] { display.Clear(8, 0, 0, 0, 0, 0); }, "Accepted unknown clear flag");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        rejects<std::invalid_argument>([&] { display.Clear(Display::ClearColor, nan, 0, 0, 0, 0); },
                                       "Accepted nonfinite clear color");
        rejects<std::invalid_argument>([&] { display.Clear(Display::ClearDepth, 0, 0, 0, 0, nan); },
                                       "Accepted nonfinite clear depth");
        pixelEquals(display, 0xFFFF00FF, "rejected clears preserve color");
        rejects<std::invalid_argument>([&] { display.SetViewport(0, 0, 0, 64); }, "Accepted empty viewport");
        rejects<std::invalid_argument>([&] { display.SetViewport(nan, 0, 64, 64); }, "Accepted nonfinite viewport");

        // A failed replacement must leave the existing resources usable.
        auto* priorDevice = display.GetDevice();
        rejects<std::invalid_argument>([&] { display.Init(window.handle, 0xFFFFFFFFu, 64); },
                                       "Accepted impossible texture size");
        require(display.GetDevice() == priorDevice && display.IsInitialized(), "Failed replacement lost resources");
        pixelEquals(display, 0xFFFF00FF, "failed replacement preserves color");
        display.Clear(Display::ClearColor, 0, 1, 0, 1, 0);
        pixelEquals(display, 0xFF00FF00, "second color clear");
        require(SUCCEEDED(display.Present()), "Native Present failed");

        // Wide, tall and restored window sizes must keep the same device and
        // produce valid fresh color/depth targets and a matching readback.
        for (const auto& size : {std::pair{252u, 108u}, std::pair{320u, 90u},
                                 std::pair{80u, 120u}, std::pair{64u, 64u}}) {
            display.Resize(size.first, size.second);
            require(display.GetDevice() == priorDevice && display.IsInitialized(), "Resize replaced the device");
            ComPtr<ID3D11Texture2D> back;
            require(SUCCEEDED(display.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(&back))), "Resized buffer missing");
            D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
            require(desc.Width == size.first && desc.Height == size.second, "Resize kept stale dimensions");
            display.Clear(Display::ClearAll, 0, 1, 1, 1, 1);
            pixelEquals(display, 0xFFFFFF00, "resized clear");
        }
        rejects<std::invalid_argument>([&] { display.Resize(0, 1080); }, "Resize accepted zero size");
        rejects<std::invalid_argument>([&] { display.Resize(20000, 1080); }, "Resize accepted excessive size");
        pixelEquals(display, 0xFFFFFF00, "invalid resize preserves display");

        rejects<std::logic_error>([&] { display.BindTexture(0, nullptr); }, "Texture placeholder reported success");
        rejects<std::logic_error>([&] { display.SetRenderState(0, 0); }, "State placeholder reported success");
        rejects<std::logic_error>([&] { display.BeginScene(); }, "Frame-begin placeholder reported success");
        rejects<std::logic_error>([&] { display.EndScene(); }, "Frame-end placeholder reported success");

        display.Destroy();
        display.Destroy();
        require(!display.IsInitialized() && !display.GetDevice() && !display.GetContext() && !display.GetSwapChain(),
                "Destroy retained native resources");
        require(FAILED(display.Present()), "Destroyed display reported success");
        display.Init(window.handle, 32, 48);
        display.Clear(Display::ClearColor, 0, 0, 1, 1, 0);
        pixelEquals(display, 0xFFFF0000, "reinitialize after destroy");
        std::puts("RendererContract passed: real GPU color readback, clear selection, validation, lifecycle and explicit unsupported operations.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "RendererContract failed: %s\n", error.what());
        return 1;
    }
}
