#pragma once
#include <d3d11sdklayers.h>

// Interleaved world commands share the immediate context with UI rendering.
// Compare actual UI pixels, including a translated, translucent second draw.
static void testPreviewClearState(HWND window) {
    using Microsoft::WRL::ComPtr;
    unsigned failures=0;
    for(auto driver:{D3D_DRIVER_TYPE_HARDWARE,D3D_DRIVER_TYPE_WARP}) {
        const char* adapter=driver==D3D_DRIVER_TYPE_WARP?"WARP":"hardware";
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width=desc.BufferDesc.Height=64;
        desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferCount=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow=window;desc.Windowed=TRUE;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> swapChain;
        const UINT flags=GetEnvironmentVariableW(L"DARK_D3D_DEBUG",nullptr,0)?D3D11_CREATE_DEVICE_DEBUG:0;
        require(SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,flags,nullptr,0,
            D3D11_SDK_VERSION,&desc,&swapChain,&device,nullptr,&context)),"Clear-state test device creation failed");
        ComPtr<ID3D11InfoQueue> messages;
        if(flags)require(SUCCEEDED(device.As(&messages)),"Clear-state test requires requested D3D11 debug queue");
        {
            EnginePreviewD3D11 renderer(device.Get(),context.Get(),swapChain.Get());
            auto white=std::make_shared<AlphaImage>();white->width=white->height=1;white->pixels={255};
            SimpleMesh first;first.texture=white;
            first.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            first.vertices={
                {{-.75f,.75f,.5f},{0,0},{1,0,0,1}},{{-.125f,.75f,.5f},{1,0},{1,0,0,1}},
                {{-.125f,-.75f,.5f},{1,1},{1,0,0,1}},{{-.75f,-.75f,.5f},{0,1},{1,0,0,1}}
            };
            first.indices={0,1,2,0,2,3};
            auto second=first;second.projection[12]=1;
            for(auto& vertex:second.vertices) {
                vertex.color[0]=0;vertex.color[1]=1;vertex.color[3]=.5f;
            }
            struct Case {const char* name;uint32_t flags;bool partial,grow;};
            const Case cases[]{
                {"partial-color",1,true,false},{"partial-depth",16,true,false},
                {"partial-stencil",32,true,false},{"partial-depth-stencil",48,true,false},
                {"growing-color",1,false,true},{"growing-depth-stencil",48,false,true},
                {"full-retained-color",1,false,false}
            };
            unsigned identity=0xF3D00000;
            for(const auto& test:cases) {
                auto seed=std::make_shared<WorldClear>();
                seed->targets[0]=identity++;seed->targets[4]=identity++;
                seed->viewport={0,0,test.grow?64u:96u,test.grow?64u:80u};
                seed->flags=49;seed->color={.25f,.5f,.75f,1};seed->depth=.75f;seed->stencil=0x5A;
                auto clear=std::make_shared<WorldClear>(*seed);
                clear->viewport={0,0,96,80};clear->flags=test.flags;
                clear->color={0,0,1,1};clear->depth=.25f;clear->stencil=0xA5;
                if(test.partial)clear->rectangle=std::array<int32_t,4>{4,3,15,19};
                SimpleMesh initialize,command;initialize.worldClear=seed;command.worldClear=clear;
                if(messages)messages->ClearStoredMessages();
                renderer.render({initialize,first,command,second});
                const auto left=renderer.readPixel(16,32),right=renderer.readPixel(48,32);
                try {
                    require(left==0xFF0000FF,"World clear or following UI draw changed preceding UI pixels");
                    if(right!=0xFF008000 && right!=0xFF007F00) {
                        std::fprintf(stderr,"PreviewClearState[%s/%s] right=%08X expected=FF008000\n",adapter,test.name,right);
                        throw std::runtime_error("UI draw after world clear lost its target, transform, or blend state");
                    }
                    require(renderer.readPixel(32,2)==0xFF000000,"Restored UI draw covered outside geometry");
                    if(test.flags&1) {
                        auto resolve=std::make_shared<WorldResolve>();resolve->targets=clear->targets;
                        resolve->rectangle={0,0,64,64};resolve->destination={identity++,0,64,64,6};
                        SimpleMesh copy,present;copy.worldResolve=resolve;
                        present.worldPresent=std::make_shared<WorldTexture>(resolve->destination);
                        renderer.render({copy,present});require(renderer.worldPresented(),"Cannot inspect interleaved clear target");
                        require(renderer.readPixel(8,8)==0xFFFF0000,"Following UI draw corrupted the world clear region");
                        const auto outside=renderer.readPixel(32,32);
                        nearByte(outside,0,test.partial?64:0);nearByte(outside,1,test.partial?128:0);
                        nearByte(outside,2,test.partial?191:255);nearByte(outside,3,255);
                    }
                    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
                        SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
                        auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                        require(SUCCEEDED(messages->GetMessage(i,message,&size)),"Cannot read clear-state debug message");
                        if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR) {
                            std::fprintf(stderr,"D3D11 clear-state: %s\n",message->pDescription);
                            throw std::runtime_error("D3D11 error while interleaving UI draws and clears");
                        }
                    }
                    std::printf("PreviewClearState[%s/%s] passed\n",adapter,test.name);
                } catch(const std::exception& error) {
                    ++failures;std::fprintf(stderr,"PreviewClearState[%s/%s] failed: %s\n",adapter,test.name,error.what());
                }
            }
            if(messages)messages->ClearStoredMessages();
            testPreviewAntialiasing(renderer,device.Get(),context.Get(),swapChain.Get());
            if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
                SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
                auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                require(SUCCEEDED(messages->GetMessage(i,message,&size)),"Cannot read AA debug message");
                if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
                    std::fprintf(stderr,"D3D11 antialiasing: %s\n",message->pDescription);
                    throw std::runtime_error("D3D11 warning/error during antialiasing");
                }
            }
        }
        context->ClearState();context->Flush();
    }
    require(!failures,"Interleaved preview clear-state regressions failed");
}
