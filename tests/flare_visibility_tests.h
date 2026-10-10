// The original flare callback divides real visible logical pixels by the
// query patch area. Colorless, read-only depth quads must therefore retain
// visible, partly hidden and hidden counts at every render resolution.
static void flareVisibilityContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        auto geometry=colorGradeQuad(10,10,32,32);
        for(unsigned v=0;v<4;++v) {
            // Vertex bytes are big endian, just like the real patch capture.
            uint32_t xb,yb;std::memcpy(&xb,geometry->vertices.data()+v*20,4);
            std::memcpy(&yb,geometry->vertices.data()+v*20+4,4);
            const float x=std::bit_cast<float>(_byteswap_ulong(xb)),y=std::bit_cast<float>(_byteswap_ulong(yb));
            put(geometry->vertices.data()+v*20,std::bit_cast<uint32_t>(x+.75f));
            put(geometry->vertices.data()+v*20+4,std::bit_cast<uint32_t>(y-.75f));
        }
        auto patch=colorGradeDraw(geometry,6121,32,32);
        patch.targets[4]=6122;patch.material=WorldMaterial::depth;
        patch.fragmentName.clear();patch.attributes.fill(0);
        put(patch.attributes.data()+92,2);patch.attributes[96]=5;patch.attributes[97]=8;
        WorldClear clear;clear.targets=patch.targets;clear.viewport=patch.viewport;
        clear.flags=49;clear.color={.25f,.5f,.75f,1};clear.depth=1;
        auto result=std::make_shared<WorldQueryResult>();
        auto await=[&](const std::shared_ptr<WorldQueryResult>& completed,uint64_t expected) {
            context->Flush();const auto deadline=GetTickCount64()+5000;
            while(completed->samples.load()!=expected && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
            require(completed->samples.load()==expected,"Flare query lost logical visibility or counted helper pixels");
        };
        for(unsigned hidden:{0u,5u,10u,0u}) {
            renderer.clear(clear);
            if(hidden) {
                auto wall=clear;wall.flags=16;wall.depth=0;
                wall.rectangle=std::array<int32_t,4>{12,12,int32_t(12+hidden),22};
                renderer.clear(wall);
            }
            const auto color=renderer.readSurface(6121,false),depth=renderer.readSurface(6122,true);
            const auto prior=result->samples.load();
            auto query=std::make_shared<WorldQuery>();query->result=result;renderer.histogram(query,true);
            require(renderer.draw(patch),"Original colorless flare patch rejected");
            // A helper rasterized on another target must not add its pixels.
            auto helper=clear;helper.targets={6123,0,0,0,0};helper.flags=1;
            helper.rectangle=std::array<int32_t,4>{2,2,7,7};renderer.clear(helper);
            renderer.histogram(query,false);
            require(result->samples.load()==prior,"Beginning a pending flare query erased the prior completed count");
            await(result,(10-hidden)*10);
            require(renderer.readSurface(6121,false)==color && renderer.readSurface(6122,true)==depth,
                    "Flare visibility patch changed scene color or depth");
        }
        // Two in-flight generations of one flare ID share its completion
        // owner. An older hidden measurement must not overwrite a newer
        // visible one; a final independent query witnesses both completions.
        for(unsigned hidden:{10u,0u}) {
            renderer.clear(clear);
            if(hidden) {
                auto wall=clear;wall.flags=16;wall.depth=0;
                wall.rectangle=std::array<int32_t,4>{12,12,22,22};renderer.clear(wall);
            }
            auto generation=std::make_shared<WorldQuery>();generation->result=result;
            renderer.histogram(generation,true);require(renderer.draw(patch),"Flare history patch rejected");
            renderer.histogram(generation,false);
        }
        require(result->samples.load()==100,"Pending flare generations discarded completed visibility");
        auto witness=std::make_shared<WorldQuery>();witness->result=std::make_shared<WorldQueryResult>();
        renderer.histogram(witness,true);renderer.histogram(witness,false);await(witness->result,0);
        require(result->samples.load()==100,"An older flare completion replaced the newer visible count");
        std::printf("FlareVisibility: scale=%u visible=100 partial=50 hidden=0 restored=100; ordered history and unchanged scene.\n",scale);
    }
    context->ClearState();
}
