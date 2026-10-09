#include "renderer/d3d11/engine_preview.h"
#include "runtime/native/graphics_settings.h"

template<class Exception,class Function>
static void resolutionMustReject(Function&& function,const char* message) {
    bool rejected=false;
    try {function();} catch(const Exception&) {rejected=true;}
    require(rejected,message);
}

// Read the resolved resource in its actual storage format. This deliberately
// preserves every stored bit, including exponent-scaled UNORM and all faces;
// a color conversion would conceal a lossy migration.
static std::vector<uint8_t> resolutionResolvedBytes(WorldRendererD3D11& renderer,
    ID3D11Device* device,ID3D11DeviceContext* context,const WorldTexture& texture,unsigned scale) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=texture.width*scale;desc.Height=texture.height*scale;
    desc.MipLevels=desc.SampleDesc.Count=1;desc.ArraySize=texture.faces;
    const unsigned bytes=texture.format==26?8:4;
    desc.Format=texture.format==26?DXGI_FORMAT_R16G16B16A16_UNORM:
        texture.format==23?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&desc,nullptr,&staging),"Resolution resolved readback allocation");
    require(renderer.present(texture,staging.Get()),"Resolution retained resolve presentation rejected");
    const size_t row=size_t(desc.Width)*bytes,faceBytes=row*desc.Height;
    std::vector<uint8_t> result(faceBytes*desc.ArraySize);
    for(unsigned face=0;face<desc.ArraySize;++face) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(),face,D3D11_MAP_READ,0,&mapped),"Resolution resolved face readback");
        for(unsigned y=0;y<desc.Height;++y)
            std::memcpy(result.data()+face*faceBytes+y*row,
                static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,row);
        context->Unmap(staging.Get(),face);
    }
    return result;
}

static void resolutionRetainedBytes(const char* name,const std::vector<uint8_t>& logical,
    const std::vector<uint8_t>& physical,unsigned width,unsigned height,unsigned faces,
    unsigned bytes,unsigned scale) {
    require(logical.size()==size_t(width)*height*faces*bytes,"Resolution logical fixture extent differs");
    require(physical.size()==logical.size()*scale*scale,"Resolution migrated fixture extent differs");
    const unsigned physicalWidth=width*scale,physicalHeight=height*scale;
    for(unsigned face=0;face<faces;++face)for(unsigned y=0;y<physicalHeight;++y)
        for(unsigned x=0;x<physicalWidth;++x) {
            const auto* expected=logical.data()+((size_t(face)*height+y/scale)*width+x/scale)*bytes;
            const auto* actual=physical.data()+((size_t(face)*physicalHeight+y)*physicalWidth+x)*bytes;
            if(std::memcmp(actual,expected,bytes)) {
                std::fprintf(stderr,"ResolutionRetained: case=%s scale=%u face=%u pixel=%u,%u\n",
                    name,scale,face,x,y);
                throw std::runtime_error("Resolution switch changed retained resource bytes");
            }
        }
    std::printf("ResolutionRetained: case=%s scale=%u faces=%u bytes=%zu exact=1\n",
        name,scale,faces,physical.size());
}

static void worldResolutionSwitchContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    context->ClearState();WorldRendererD3D11 renderer(device,context,1);
    WorldClear color;color.targets[0]=4101;color.viewport={0,0,16,16};color.flags=1;
    color.color={.125f,.25f,.5f,.75f};renderer.clear(color);
    auto patch=color;patch.rectangle=std::array<int32_t,4>{3,5,11,13};patch.color={2,-.5f,.75f,1};renderer.clear(patch);
    WorldClear depth;depth.targets[4]=4102;depth.viewport=color.viewport;depth.flags=48;
    depth.depth=.25f;depth.stencil=0x35;renderer.clear(depth);
    patch=depth;patch.rectangle=std::array<int32_t,4>{4,2,12,10};patch.depth=.75f;patch.stencil=0xCA;renderer.clear(patch);
    const auto oldColor=renderer.readSurface(4101,false),oldDepth=renderer.readSurface(4102,true);

    WorldResolve resolve;resolve.targets=color.targets;resolve.viewport=color.viewport;resolve.rectangle={0,0,16,16};
    resolve.destination={4111,0x4111000,16,16,6,0};
    require(renderer.resolve(resolve),"Resolution RGBA8 fixture resolve rejected");
    const auto rgba8=resolve.destination;
    resolve.destination={4112,0x4112000,16,16,26,4};resolve.exponent=-4;
    require(renderer.resolve(resolve),"Resolution RGBA16 fixture resolve rejected");
    const auto rgba16=resolve.destination;
    resolve.targets=depth.targets;resolve.flags=4;resolve.exponent=0;
    resolve.destination={4113,0x4113000,16,16,23,0};
    require(renderer.resolve(resolve),"Resolution float-depth fixture resolve rejected");
    const auto floatDepth=resolve.destination;
    WorldClear cubeClear=color;cubeClear.targets[0]=4103;
    resolve.targets=cubeClear.targets;resolve.flags=0;
    resolve.destination={4114,0x4114000,16,16,6,0,6};
    for(unsigned face=0;face<6;++face) {
        cubeClear.color={float(face+1)/8,float(6-face)/8,float(face%3+1)/4,1};renderer.clear(cubeClear);
        auto facePatch=cubeClear;facePatch.rectangle=std::array<int32_t,4>{2,4,9,12};
        facePatch.color={float(6-face)/8,float(face+1)/8,.875f,1};renderer.clear(facePatch);
        resolve.face=face;require(renderer.resolve(resolve),"Resolution cubemap face fixture resolve rejected");
    }
    const auto cube=resolve.destination;
    struct Retained {const char* name;WorldTexture texture;unsigned bytes;std::vector<uint8_t> pixels;};
    std::array<Retained,4> retained{{{"RGBA8",rgba8,4,{}},{"RGBA16-exponent4",rgba16,8,{}},
        {"R32-depth",floatDepth,4,{}},{"six-cube-faces",cube,4,{}}}};
    for(auto& resource:retained)
        resource.pixels=resolutionResolvedBytes(renderer,device,context,resource.texture,1);

    // Seed the color cube exactly once. Later Final5 draws must consume its
    // migrated storage, rather than rebuilding it after every scale change.
    const auto cubeGeometry=colorGradeQuad(324,18,324,18);
    auto copy=colorGradeDraw(cubeGeometry,4121,324,18);
    copy.fragmentName="XREngine_CCFuser";copy.fragmentFlags=0;
    copy.fragmentConstants[0]={1,1,1,1};copy.textures[1]=colorGradeCube(true);
    copy.samplers[1]=colorGradeSampler(true,0);
    WorldClear clear;clear.targets=copy.targets;clear.viewport=copy.viewport;clear.flags=1;
    renderer.clear(clear);require(renderer.draw(copy),"Resolution color-cube seed draw rejected");
    const auto lookup=colorGradeResolve(renderer,copy,4122,324,18,26,4);
    const auto lookupRaw=resolutionResolvedBytes(renderer,device,context,lookup,1);
    const auto lookupLogical=colorGradeRead(renderer,device,context,lookup,1);
    constexpr std::array<std::array<uint8_t,3>,4> swatches{{{0,0,0},{3,5,12},{23,47,79},{231,17,89}}};
    auto scene=std::make_shared<ColorImage>();scene->width=16;scene->height=8;scene->authoredMips=true;
    scene->pixels.resize(16*8*4);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x) {
        auto* p=scene->pixels.data()+(y*16+x)*4;std::copy(swatches[x/4].begin(),swatches[x/4].end(),p);p[3]=255;
    }
    auto black=std::make_shared<ColorImage>();black->width=black->height=1;black->pixels={0,0,0,255};
    const auto screenGeometry=colorGradeQuad(16,8,16,8);
    std::array<std::vector<uint8_t>,2> gradeBaseline;
    unsigned gradeComparisons=0;
    auto verify=[&](unsigned scale) {
        require(renderer.renderScale()==scale,"Resolution world scale did not commit");
        resolutionRetainedBytes("FP16-color",oldColor,renderer.readSurface(4101,false),16,16,1,8,scale);
        resolutionRetainedBytes("packed-D24S8",oldDepth,renderer.readSurface(4102,true),16,16,1,4,scale);
        for(const auto& resource:retained)
            resolutionRetainedBytes(resource.name,resource.pixels,
                resolutionResolvedBytes(renderer,device,context,resource.texture,scale),
                16,16,resource.texture.faces,resource.bytes,scale);
        resolutionRetainedBytes("retail-color-LUT",lookupRaw,
            resolutionResolvedBytes(renderer,device,context,lookup,scale),324,18,1,8,scale);
        for(unsigned flags:{4u,14u}) {
            const unsigned row=flags==14;
            auto final=colorGradeDraw(screenGeometry,4123,16,8);
            final.fragmentName="XREngine_Final5";final.fragmentFlags=flags;
            final.textures[0]=scene;final.textures[1]=black;final.textureObjects[2]=lookup;
            final.samplers[0]=final.samplers[1]=colorGradeSampler(false,2);
            final.samplers[2]=colorGradeSampler(true,2);
            final.fragmentConstants[0]={1,1,1,1};final.fragmentConstants[1]={0,0,1,1};
            clear.targets=final.targets;clear.viewport=final.viewport;renderer.clear(clear);
            require(renderer.draw(final),"Resolution retained color-grade draw rejected");
            colorGradeLookupState(device,context,"resolution-retained-Final5",scale,scale>1?4:0);
            const auto output=colorGradeResolve(renderer,final,4124+row,16,8);
            const auto pixels=colorGradeRead(renderer,device,context,output,scale);
            if(gradeBaseline[row].empty())gradeBaseline[row]=pixels;
            unsigned maximum=0;
            for(unsigned n=0;n<swatches.size();++n) {
                const auto expected=colorGradeOracle(lookupLogical,swatches[n],flags);
                const auto* native=gradeBaseline[row].data()+(3*16+n*4+1)*4;
                for(unsigned y=2*scale;y<6*scale;++y)for(unsigned x=(n*4+1)*scale;x<(n*4+3)*scale;++x) {
                    const auto* p=pixels.data()+(size_t(y)*16*scale+x)*4;
                    require(p[3]==255,"Resolution color grading changed opaque alpha");
                    for(unsigned c=0;c<3;++c) {
                        const unsigned delta=unsigned(std::abs(int(p[c])-int(expected[c])));
                        maximum=(std::max)(maximum,delta);++gradeComparisons;
                        require(delta<=2 && std::abs(int(p[c])-int(native[c]))<=2,
                            "Resolution migrated color grading differs from native output or independent oracle");
                    }
                }
            }
            std::printf("ResolutionGrade: scale=%u Final5=%u maxDelta=%u retainedLUT=1\n",scale,flags,maximum);
        }
    };
    verify(1);
    require(!renderer.setRenderScale(1),"Unchanged world resolution reported a mutation");
    for(unsigned invalid:{0u,4u})
        resolutionMustReject<std::invalid_argument>([&]{renderer.setRenderScale(invalid);},"Invalid world resolution scale was accepted");
    for(unsigned next:{2u,1u}) {
        // A completed query remains pending in the owner until after the
        // switch. Its result must use the scale of the queried raster.
        auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
        auto queryDraw=colorGradeDraw(colorGradeQuad(16,16,16,16),4131,16,16);
        queryDraw.fragmentName="XREngine_CCFuser";queryDraw.fragmentFlags=0;
        queryDraw.fragmentConstants[0]={1,1,1,1};queryDraw.textures[1]=black;
        queryDraw.samplers[1]=colorGradeSampler(false,2);
        clear.targets=queryDraw.targets;clear.viewport=queryDraw.viewport;renderer.clear(clear);
        const unsigned old=renderer.renderScale();renderer.histogram(query,true);
        require(renderer.draw(queryDraw),"Resolution histogram coverage draw rejected");
        resolutionMustReject<std::logic_error>([&]{renderer.setRenderScale(next);},"Resolution changed during an active histogram");
        require(renderer.renderScale()==old,"Rejected active-histogram switch changed its scale");
        renderer.histogram(query,false);
        require(query->result->samples.load()==UINT64_MAX,"Completed query was consumed before migration fixture");
        require(renderer.setRenderScale(next),"Changed world resolution did not report a mutation");
        context->Flush();const auto deadline=GetTickCount64()+5000;
        while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
        const auto samples=query->result->samples.load();
        std::printf("ResolutionHistogram: oldScale=%u newScale=%u logicalSamples=%llu expected=256\n",
            old,next,static_cast<unsigned long long>(samples));
        require(samples==256,"Resolution switch normalized a completed query using the new scale");
        verify(next);
    }
    context->ClearState();
    std::printf("WorldResolutionSwitch passed: same-instance1-2-1, exact retained color/depth/stencil/resolve/cube/LUT bytes, %u grade comparisons, active/completed queries and invalid/no-op scales.\n",gradeComparisons);
}

struct ResolutionTestWindow {
    HWND handle=nullptr;
    ResolutionTestWindow() {
        WNDCLASSW description{};description.lpfnWndProc=DefWindowProcW;
        description.hInstance=GetModuleHandleW(nullptr);description.lpszClassName=L"DarkRecompResolutionSwitchContract";
        require(RegisterClassW(&description)!=0,"Resolution test window class registration failed");
        handle=CreateWindowExW(0,description.lpszClassName,L"Resolution switch contract",WS_OVERLAPPEDWINDOW,
            0,0,128,128,nullptr,nullptr,description.hInstance,nullptr);
        require(handle!=nullptr,"Resolution test window creation failed");
    }
    ~ResolutionTestWindow() {
        if(handle)DestroyWindow(handle);
        UnregisterClassW(L"DarkRecompResolutionSwitchContract",GetModuleHandleW(nullptr));
    }
};

static SimpleMesh resolutionFlatMesh(std::array<uint8_t,4> color) {
    auto image=std::make_shared<ColorImage>();image->width=image->height=1;
    image->pixels.assign(color.begin(),color.end());
    SimpleMesh mesh;mesh.colorTexture=image;mesh.opaque=true;
    mesh.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    mesh.vertices={{{-1,-1,.5f},{0,0},{1,1,1,1}},{{3,-1,.5f},{0,0},{1,1,1,1}},{{-1,3,.5f},{0,0},{1,1,1,1}}};
    mesh.indices={0,1,2};return mesh;
}

static void previewResolutionSwitchContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    context->ClearState();ResolutionTestWindow window;
    ComPtr<IDXGIDevice> dxgiDevice;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;
    check(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())),"Resolution DXGI device");
    check(dxgiDevice->GetAdapter(&adapter),"Resolution DXGI adapter");
    check(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())),"Resolution DXGI factory");
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=64;desc.BufferDesc.Height=36;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;
    desc.OutputWindow=window.handle;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> chain;
    check(factory->CreateSwapChain(device,&desc,&chain),"Resolution test swapchain");
    EnginePreviewD3D11 preview(device,context,chain.Get(),64,36,1);
    const auto saved=graphicsSettings();
    struct Restore {GraphicsSettings value;~Restore(){setGraphicsSettings(value);}} restore{saved};
    auto neutral=saved;neutral.antialiasing=AntialiasingMode::Off;neutral.brightnessPercent=100;
    require(setGraphicsSettings(neutral),"Resolution neutral presentation settings rejected");
    preview.render({resolutionFlatMesh({64,128,192,255})});
    const uint32_t original=preview.readPixel(63,35);
    require(original==0xffc08040,"Resolution preview seed pixels differ");
    auto verify=[&](unsigned width,unsigned height,unsigned scale,uint32_t expected) {
        require(preview.renderWidth()==width && preview.renderHeight()==height && preview.renderScale()==scale,
            "Resolution preview target dimensions or world scale differ");
        for(const auto point:{std::pair{0u,0u},std::pair{width/2,height/2},std::pair{width-1,height-1}})
            require(preview.readPixel(point.first,point.second)==expected,"Resolution preview migration changed retained pixels");
        resolutionMustReject<std::invalid_argument>([&]{preview.readPixel(width,height-1);},"Resolution preview accepted a pixel beyond its target");
        preview.copyToDisplay();
    };
    verify(64,36,1,original);
    require(!preview.resizeRenderTarget(64,36,1),"Unchanged preview target reported a mutation");
    for(const auto bad:std::array<std::array<unsigned,3>,5>{{{0,36,1},{64,0,1},{128,72,0},{128,72,4},{128,70,2}}})
        resolutionMustReject<std::invalid_argument>([&]{preview.resizeRenderTarget(bad[0],bad[1],bad[2]);},
            "Invalid preview target or changed logical extent was accepted");
    verify(64,36,1,original);
    preview.render({resolutionFlatMesh({0,0,255,255})},{true,false});
    resolutionMustReject<std::logic_error>([&]{preview.resizeRenderTarget(128,72,2);},"Open preview frame allowed a resolution switch");
    require(preview.frameInProgress() && preview.renderWidth()==64 && preview.renderHeight()==36 && preview.renderScale()==1,
        "Rejected open-frame resolution switch changed the target or frame state");
    preview.render({},{false,true});verify(64,36,1,0xffff0000);
    require(preview.resizeRenderTarget(128,72,2),"Preview upscale did not report a mutation");
    verify(128,72,2,0xffff0000);
    require(!preview.resizeRenderTarget(128,72,2),"Repeated preview upscale reported a mutation");
    require(preview.resizeRenderTarget(64,36,1),"Preview downscale did not report a mutation");
    verify(64,36,1,0xffff0000);
    preview.render({resolutionFlatMesh({192,64,128,255})});verify(64,36,1,0xff8040c0);
    require(device->GetDeviceRemovedReason()==S_OK,"Resolution switch removed the D3D device");
    context->ClearState();
    std::puts("PreviewResolutionSwitch passed: same-instance1-2-1, retained target/corner pixels and display copy, busy-frame rejection, invalid dimensions and no-op preservation.");
}

static void resolutionSwitchContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    worldResolutionSwitchContract(device,context);
    previewResolutionSwitchContract(device,context);
}
