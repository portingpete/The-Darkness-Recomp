#include "renderer/engine/render_trace.h"
#include <filesystem>
#include <fstream>
#include <iterator>

// Explicit inspection is observational: it must retain the resources consumed
// by this projector and leave subsequent rendering identical to an ordinary
// renderer, including the transition into the next frame.
static void shadowCapturePass(ID3D11Device* device,ID3D11DeviceContext* context,
                              const WorldDraw& seed,unsigned scale=2) {
    const auto directory=std::filesystem::temp_directory_path()/
        ("DarkRecomp-shadow-capture-"+std::to_string(GetCurrentProcessId())+"-"+
         std::to_string(GetTickCount64())+"-"+std::to_string(scale));
    require(!std::filesystem::exists(directory),"Shadow capture fixture directory already exists");
    Native::configureShadowCapture(directory);
    struct DisableCapture {~DisableCapture(){Native::configureShadowCapture({});}} disable;
    context->ClearState();
    WorldRendererD3D11 inspected(device,context,scale),ordinary(device,context,scale);

    WorldClear atlas;atlas.targets[4]=3401;atlas.viewport={0,0,64,64};atlas.flags=48;
    atlas.surfaceBindings[4]={0x00400040,0x000105c0,true};
    WorldClear scene=atlas;scene.targets[4]=3402;scene.depth=.5f;scene.stencil=11;
    scene.surfaceBindings[4].info=0x00010000;
    WorldClear target;target.targets={3403,0,0,0,3402};target.viewport=atlas.viewport;
    target.surfaceBindings[0]={0x00400040,0x000002e0,true};
    target.surfaceBindings[4]=scene.surfaceBindings[4];target.flags=1;
    WorldResolve atlasCopy;atlasCopy.targets=atlas.targets;atlasCopy.surfaceBindings=atlas.surfaceBindings;
    atlasCopy.viewport=atlas.viewport;atlasCopy.flags=4;atlasCopy.rectangle={0,0,64,64};
    atlasCopy.destination={3404,0x3404000,64,64,23,0,1};
    WorldResolve sceneCopy=atlasCopy;sceneCopy.targets=scene.targets;sceneCopy.surfaceBindings=scene.surfaceBindings;
    sceneCopy.destination={3405,0x3405000,64,64,23,0,1};
    WorldResolve colorCopy=atlasCopy;colorCopy.targets=target.targets;colorCopy.surfaceBindings=target.surfaceBindings;
    colorCopy.flags=0;colorCopy.destination={3406,0x3406000,64,64,6,0,1};

    auto caster=seed;caster.targets=atlas.targets;caster.surfaceBindings=atlas.surfaceBindings;
    caster.material=WorldMaterial::depth;caster.fragmentName.clear();caster.fragmentFlags=0;
    caster.viewport=atlas.viewport;caster.attributes.fill(0);caster.attributes[96]=8;caster.attributes[97]=8;
    put(caster.attributes.data()+92,6);
    auto projector=seed;projector.targets=target.targets;projector.surfaceBindings=target.surfaceBindings;
    projector.viewport=target.viewport;projector.material=WorldMaterial::post;
    projector.fragmentName="XREngine_ShadowProj";projector.fragmentFlags=8;projector.fragmentConstants={};
    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;
    binding.descriptor.modes.fill(4);binding.descriptor.modes[0]=1;binding.descriptor.parameters[0][0]=20;
    const auto descriptor=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|descriptor[i*4+n];
    const std::array<EngineVector,4> texgen{{{0,0,0,.5f},{0,0,0,.5f},{0,0,0,0},{0,0,0,1}}};
    for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)
        put(binding.constantBytes.data()+(20+row)*16+lane*4,std::bit_cast<uint32_t>(texgen[row][lane]));
    for(unsigned lane=0;lane<4;++lane)put(binding.constantBytes.data()+7*16+lane*4,0);
    require(prepareWorldVertexProgram(binding,projector.options,projector.constants),
            "Shadow capture projector binding failed");
    projector.fragmentConstants[1]={0,0,1,-1};projector.fragmentConstants[5]={0,0,0,.5f};
    projector.fragmentConstants[6]={0,0,0,1};projector.fragmentConstants[9]={1.0f/64,1.0f/64,0,0};
    projector.textures={};projector.textureObjects={};projector.samplers={};
    projector.textureObjects[0]=atlasCopy.destination;projector.textureObjects[1]=sceneCopy.destination;
    for(unsigned slot=0;slot<2;++slot) {
        auto& sampler=projector.samplers[slot];sampler.valid=sampler.lodValid=true;
        sampler.minLevel=sampler.maxLevel=0;sampler.address.fill(2);
    }
    projector.attributes.fill(0);put(projector.attributes.data()+92,0x01100000);projector.attributes[97]=8;
    auto rejected=projector;rejected.geometry={};
    D3D11_TEXTURE2D_DESC outputDesc{};outputDesc.Width=outputDesc.Height=64*scale;
    outputDesc.MipLevels=outputDesc.ArraySize=outputDesc.SampleDesc.Count=1;
    outputDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> output;
    check(device->CreateTexture2D(&outputDesc,nullptr,&output),"Shadow capture presentation target");
    struct Pixels {std::vector<uint8_t> color,atlas,scene;};
    auto render=[&](WorldRendererD3D11& renderer,bool inspect,float casterDepth) {
        context->ClearState();renderer.beginFrame();
        if(inspect)renderer.inspectNextFrame();
        renderer.clear(atlas);
        caster.constants.vectors[2]={0,0,0,casterDepth};
        require(renderer.draw(caster),"Shadow capture caster rejected");
        require(renderer.resolve(atlasCopy),"Shadow capture atlas resolve rejected");
        renderer.clear(scene);require(renderer.resolve(sceneCopy),"Shadow capture scene resolve rejected");
        renderer.clear(target);
        require(renderer.draw(projector),"Shadow capture projector rejected");
        // The repeat must work using the cached bindings after the diagnostic
        // readbacks. Intentionally rejected input must be recorded as such.
        require(renderer.draw(projector),"Shadow capture changed cached projector bindings");
        require(!renderer.draw(rejected),"Shadow capture invalid draw was accepted");
        require(renderer.resolve(colorCopy),"Shadow capture output resolve rejected");
        require(renderer.present(colorCopy.destination,output.Get()),"Shadow capture presentation failed");
        renderer.endFrame();
        return Pixels{renderer.readSurface(target,0),renderer.readSurface(atlas,4),renderer.readSurface(scene,4)};
    };
    auto same=[](const Pixels& first,const Pixels& second) {
        return first.color==second.color && first.atlas==second.atlas && first.scene==second.scene;
    };
    const auto uncaptured=render(inspected,false,.75f);
    require(!std::filesystem::exists(directory) || std::filesystem::is_empty(directory),
            "Shadow capture wrote files before an explicit inspection");
    require(same(uncaptured,render(ordinary,false,.75f)),"Ordinary shadow capture fixture differs");
    for(float depth:{.75f,.25f}) {
        const auto expected=render(ordinary,false,depth);
        const auto actual=render(inspected,true,depth);
        require(same(expected,actual),"Shadow inspection changed rendered color or depth/stencil");
        require(actual.color.size()==size_t(64*scale)*(64*scale)*8 &&
                actual.atlas.size()==size_t(64*scale)*(64*scale)*4 && actual.scene.size()==actual.atlas.size(),
                "Shadow capture fixture dimensions differ");
    }

    // Keep this reader limited to scalar fields emitted by the diagnostic
    // schema. Full JSON and raw resources remain available for offline tools.
    auto scalar=[](const std::string& line,const char* name) {
        const auto marker=std::string("\"")+name+"\":";
        auto first=line.find(marker);
        require(first!=std::string::npos,"Shadow capture metadata field missing");
        first+=marker.size();first=line.find_first_not_of(" \t",first);
        require(first!=std::string::npos,"Shadow capture metadata field empty");
        if(line[first]=='\"') {
            const auto last=line.find('\"',first+1);
            require(last!=std::string::npos,"Shadow capture metadata string unterminated");
            return line.substr(first+1,last-first-1);
        }
        return line.substr(first,line.find_first_of(",]} \t\r\n",first)-first);
    };
    auto number=[&](const std::string& line,const char* name) {
        return std::stoull(scalar(line,name));
    };
    auto lines=[](const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary);
        require(bool(input),"Shadow capture metadata file missing");
        std::vector<std::string> result;std::string line;
        while(std::getline(input,line)) {
            if(!line.empty() && line.back()=='\r')line.pop_back();
            require(line.size()>=2 && line.front()=='{' && line.back()=='}',
                    "Shadow capture metadata line is incomplete");
            result.push_back(std::move(line));
        }
        require(input.eof(),"Shadow capture metadata read failed");
        return result;
    };
    auto readBytes=[](const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary);
        require(bool(input),"Shadow capture binary file missing");
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    };
    constexpr const char* commands[]{"clear","draw","resolve","clear","resolve","clear",
                                    "draw","draw","draw","resolve","present"};
    constexpr unsigned drawCommands[]{1,6,7,8};
    for(unsigned inspection=1;inspection<=2;++inspection) {
        const auto stem="inspection-"+std::to_string(inspection);
        const auto records=lines(directory/(stem+"-commands.jsonl"));
        const auto gpu=readBytes(directory/(stem+"-gpu.bin"));
        const auto owned=lines(directory/("owned-inspection-"+std::to_string(inspection)+".jsonl"));
        require(!readBytes(directory/("owned-inspection-"+std::to_string(inspection)+".bin")).empty(),
                "Shadow capture omitted owned geometry bytes");
        require(owned.size()==4,"Shadow capture lost accepted or rejected owned draws");
        for(unsigned ordinal=0;ordinal<owned.size();++ordinal) {
            require(number(owned[ordinal],"inspection")==inspection && number(owned[ordinal],"draw")==ordinal,
                    "Shadow capture owned draw ordering differs");
            const uint64_t key=ordinal?target.surfaceKey(0):atlas.surfaceKey(4);
            char expected[48]{};std::snprintf(expected,sizeof(expected),"\"keyHex\":\"%llx\"",
                static_cast<unsigned long long>(key));
            require(owned[ordinal].find(expected)!=std::string::npos,
                    "Shadow capture owned descriptor lost completed storage identity");
        }
        require(!records.empty() && scalar(records.front(),"event")=="begin" &&
                scalar(records.back(),"event")=="end","Shadow capture frame boundaries missing");
        unsigned command=0,draws=0,gpuRecords=0;
        std::array<unsigned,4> roles{};
        size_t nextOffset=0;
        const auto side=64*scale;
        for(const auto& record:records) {
            require(number(record,"inspection")==inspection,"Shadow capture mixed inspection frames");
            const auto event=scalar(record,"event");
            if(event=="begin" || event=="end")continue;
            if(event!="gpu") {
                require(command<std::size(commands) && event==commands[command] &&
                        number(record,"command")==command,"Shadow capture command sequence differs");
                if(event=="draw") {
                    require(draws<std::size(drawCommands) && drawCommands[draws]==command &&
                            number(record,"draw")==draws && number(record,"reason")==unsigned(draws==3?4:0),
                            "Shadow capture draw outcome does not match its owned descriptor");
                    ++draws;
                }
                if(event=="resolve" || event=="present")
                    require(number(record,"reason")==0,"Shadow capture reports a successful transfer as rejected");
                ++command;continue;
            }
            ++gpuRecords;
            require(record.find("\"omitted\"")==std::string::npos,
                    "Small shadow capture fixture unexpectedly omitted GPU evidence");
            const auto gpuCommand=number(record,"command"),gpuDraw=number(record,"draw");
            require((gpuCommand==6 && gpuDraw==1) || (gpuCommand==7 && gpuDraw==2),
                    "Shadow capture GPU evidence belongs to another command/draw");
            require(number(record,"width")==side && number(record,"height")==side &&
                    number(record,"subresource")==0,"Shadow capture GPU dimensions/subresource differ");
            const auto role=scalar(record,"role"),stage=scalar(record,"stage"),encoding=scalar(record,"encoding");
            require(stage=="before" || stage=="after","Shadow capture GPU stage missing");
            const uint64_t key=std::stoull(scalar(record,"keyHex"),nullptr,16);
            const auto rowBytes=number(record,"rowBytes"),offset=number(record,"offset"),size=number(record,"bytes");
            require(offset==nextOffset && size==rowBytes*side && offset<=gpu.size() && size<=gpu.size()-offset,
                    "Shadow capture GPU blob rows/offsets/length differ");
            nextOffset+=size;
            const auto* center=gpu.data()+offset+size_t(side/2)*rowBytes+(side/2)*(role=="target-alpha"?2:4);
            if(role=="texture") {
                const auto slot=number(record,"slot");
                require(slot<2 && stage=="before" && encoding=="raw" &&
                        number(record,"sourceFormat")==DXGI_FORMAT_R32_FLOAT && rowBytes==side*4u &&
                        key==(slot?sceneCopy.destination.key():atlasCopy.destination.key()),
                        "Shadow capture lost the projector's resolved texture binding");
                ++roles[slot];
                float value=0;std::memcpy(&value,center,4);
                const float expected=slot?.5f:inspection==1?.75f:.25f;
                require(std::abs(value-expected)<1e-6f,"Shadow capture sampled stale atlas/scene-depth contents");
            } else if(role=="depth-stencil") {
                ++roles[2];
                require(encoding=="D24S8-LE" && rowBytes==side*4u && key==scene.surfaceKey(4),
                        "Shadow capture lost depth/stencil surface identity");
                uint32_t value=0;std::memcpy(&value,center,4);
                require((value>>24)==11 && std::abs(double(value&0xFFFFFF)/0xFFFFFF-.5)<1e-6,
                        "Shadow capture changed or misdecoded scene depth/stencil");
            } else {
                ++roles[3];
                require(role=="target-alpha" && encoding=="R16_FLOAT-alpha" && rowBytes==side*2u &&
                        key==target.surfaceKey(0),"Shadow capture target-alpha encoding/key differs");
                uint16_t value=0;std::memcpy(&value,center,2);
                const auto expected=stage=="before" && gpuCommand==6?0u:inspection==1?0u:0x3c00u;
                require(value==expected,"Shadow capture alpha does not reflect the before/after projector state");
            }
        }
        require(command==std::size(commands) && draws==4 && gpuRecords==12 &&
                roles==std::array<unsigned,4>{2,2,4,4} && nextOffset==gpu.size(),
                "Shadow capture lost commands, GPU phases or binary bytes");
        require(number(records.back(),"commands")==command && number(records.back(),"draws")==draws,
                "Shadow capture frame totals differ from recorded commands");
    }
    auto snapshotFiles=[&] {
        std::vector<std::pair<std::string,std::vector<uint8_t>>> files;
        for(const auto& entry:std::filesystem::directory_iterator(directory))if(entry.is_regular_file())
            files.emplace_back(entry.path().filename().string(),readBytes(entry.path()));
        std::sort(files.begin(),files.end(),[](const auto& first,const auto& second){return first.first<second.first;});
        return files;
    };
    const auto capturedFiles=snapshotFiles();
    require(same(render(ordinary,false,.75f),render(inspected,true,.75f)),
            "Bounded third inspection changed rendering");
    require(snapshotFiles()==capturedFiles,"A third inspection wrote or modified bounded capture files");
    require(!std::filesystem::exists(directory/"inspection-3-commands.jsonl") &&
            !std::filesystem::exists(directory/"owned-inspection-3.jsonl"),
            "Shadow capture exceeded its two-inspection limit");
    std::printf("ShadowCapture%u: explicit-only capture, two ordered frames, GPU atlas/scene-depth/stencil/alpha bytes, observational rendering and third-capture bound passed; artifacts=%s\n",
                scale,directory.string().c_str());
}
