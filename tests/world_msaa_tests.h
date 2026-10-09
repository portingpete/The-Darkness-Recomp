#pragma once

static void worldMsaaContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    context->ClearState();WorldRendererD3D11 renderer(device,context);
    auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=3;geometry->stride=16;
    geometry->formats[0]=4;geometry->vertices.resize(48);geometry->indices={0,1,2};
    const EngineVector positions[]{{-.75f,-.75f,.5f,1},{0,.75f,.5f,1},{.75f,-.75f,.5f,1}};
    for(unsigned v=0;v<3;++v)for(unsigned lane=0;lane<4;++lane)
        put(geometry->vertices.data()+v*16+lane*4,std::bit_cast<uint32_t>(positions[v][lane]));
    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;binding.descriptor.modes.fill(4);
    constantTexgen(binding,0,{0,0,1,0});constantTexgen(binding,1,{0,0,1,0});
    const auto descriptor=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|descriptor[i*4+n];
    for(unsigned i=0;i<16;++i)binding.constantBytes[7*16+i]=0;
    WorldClear clear;clear.targets={8101,0,0,0,8102};clear.viewport={0,0,64,64};clear.flags=49;
    WorldDraw draw;draw.geometry.vertices=draw.geometry.indices=geometry;draw.geometry.indexCount=3;
    draw.targets=clear.targets;draw.viewport=clear.viewport;draw.material=WorldMaterial::motion;
    require(prepareWorldVertexProgram(binding,draw.options,draw.constants),"MSAA vertex binding");
    put(draw.attributes.data()+92,0x01104006);draw.attributes[96]=8;draw.attributes[97]=8;
    draw.attributes[120]=0x80;draw.attributes[121]=2;
    draw.attributes[124]=127;draw.attributes[125]=draw.attributes[126]=255;
    auto half=[](uint16_t h) {const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    std::vector<uint8_t> baseline,previousCoverage;
    unsigned previousSamples=0;
    for(unsigned requested:{1u,2u,4u,8u}) {
        renderer.setSampleCount(requested);
        require(renderer.sampleCount()<=requested && renderer.sampleCount()>=1,"MSAA unsupported-count fallback");
        renderer.clear(clear);require(renderer.draw(draw),"MSAA color/depth triangle");
        const auto pixels=renderer.readSurface(8101,false),depth=renderer.readSurface(8102,true);
        require(pixels.size()==64*64*8 && depth.size()==64*64*4,"MSAA readback dimensions");
        size_t partial=0,different=0;
        std::vector<uint16_t> edgeLevels;
        for(size_t at=0;at<pixels.size();at+=8) {uint16_t channels[4]{};std::memcpy(channels,pixels.data()+at,8);
            require(channels[0]==channels[1] && channels[2]==0,"MSAA introduced a color tint");
            const auto red=half(channels[0]);require(red>=0 && red<=.5,"MSAA color resolve overshoot");
            partial+=red>0 && red<.5;
            if(red>0 && red<.5 && std::find(edgeLevels.begin(),edgeLevels.end(),channels[0])==edgeLevels.end())
                edgeLevels.push_back(channels[0]);
            if(previousCoverage.size()==pixels.size())different+=bool(std::memcmp(pixels.data()+at,previousCoverage.data()+at,8));
        }
        if(requested==1) {baseline=pixels;require(partial==0,"Off contains synthetic coverage");}
        else if(renderer.sampleCount()>1)require(partial>16 && pixels!=baseline,"MSAA did not create subpixel geometry coverage");
        if(previousSamples>=2 && renderer.sampleCount()>previousSamples)
            require(different>16,"Higher MSAA sample counts collapsed to identical edge coverage");
        std::printf("[MSAAComparison] requested=%u active=%u partialPixels=%zu edgeLevels=%zu differentFrom%ux=%zu\n",
                    requested,renderer.sampleCount(),partial,edgeLevels.size(),previousSamples,different);
        previousCoverage=pixels;previousSamples=renderer.sampleCount();
        uint32_t center=0;std::memcpy(&center,depth.data()+(32*64+32)*4,4);
        require((center>>24)==127 && std::abs(double(center&0xffffff)-8388608)<=1,"MSAA lost depth/stencil");

        // Sample-count changes preserve retained color and packed depth/stencil.
        renderer.setSampleCount(1);
        require(renderer.readSurface(8101,false)==pixels && renderer.readSurface(8102,true)==depth,
                "Turning MSAA off discarded retained color or depth/stencil");
        renderer.setSampleCount(requested);
        require(renderer.readSurface(8101,false)==pixels && renderer.readSurface(8102,true)==depth,
                "Turning MSAA on discarded retained color or depth/stencil");

        renderer.setRenderScale(2);
        const auto scaled=renderer.readSurface(8101,false);
        require(scaled.size()==128*128*8,"MSAA live resolution extent");
        for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x)
            require(!std::memcmp(scaled.data()+(y*128+x)*8,pixels.data()+((y/2)*64+x/2)*8,8),
                    "MSAA live resize changed retained color");
        renderer.setRenderScale(1);
        require(renderer.readSurface(8102,true)==depth,"MSAA live resize changed packed depth/stencil");

        renderer.clear(clear);
        auto queryDraw=draw;put(queryDraw.attributes.data()+92,0x01100002);queryDraw.attributes[96]=8;
        auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
        renderer.histogram(query,true);require(renderer.draw(queryDraw),"MSAA histogram geometry");
        renderer.readSurface(8102,true); // Helper transfers must not count as geometry.
        renderer.histogram(query,false);context->Flush();const auto deadline=GetTickCount64()+5000;
        while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
        require(query->result->samples.load()==1152,"MSAA altered logical exposure samples");
        retainedDepthGrowth(renderer,8200+requested);
        retainedAttachmentPair(renderer,context,draw,8300+requested*10);
    }
    context->ClearState();
    std::puts("WorldMSAA passed: 2x/4x/8x coverage, depth/stencil, live mode/scale retention, atlas growth and normalized exposure queries.");
}
