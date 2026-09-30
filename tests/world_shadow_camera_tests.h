// Reconstruct actual rasterized scene depth while the camera moves around a
// fixed caster. Constant UV/depth fixtures cannot expose camera-dependent
// reconstruction, homogeneous-divide or view-to-shadow matrix mistakes.
static void shadowCameraPass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    constexpr unsigned logicalSide=64;
    constexpr float nearPlane=1,farPlane=100,lightExtent=20,projectionX=1.2f,projectionY=1.1f;
    constexpr float casterLeft=-1.25f,casterRight=3.125f,casterBottom=-2.5f,casterTop=.625f;
    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;
    binding.descriptor.modes.fill(4);binding.descriptor.modes[0]=1;
    binding.descriptor.parameters[0][0]=20;
    const auto descriptor=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|descriptor[i*4+n];
    // The fixture's unused constants may contain unrelated transforms. Supply
    // all consumed mode-1 rows before preparing the owned vertex program.
    for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)
        put(binding.constantBytes.data()+(20+row)*16+lane*4,0);
    auto base=seed;
    require(prepareWorldVertexProgram(binding,base.options,base.constants),"Moving-camera vertex binding rejected");
    base.viewport={0,0,logicalSide,logicalSide};base.depthRange={1,0,0,0};
    base.constants.vectors[7]={0,0,0,0};base.attributes.fill(0);base.attributes[97]=8;
    base.geometry.firstIndex=0;base.geometry.indexCount=6;
    base.textures={};base.textureObjects={};base.samplers={};
    auto quad=[](const std::array<EngineVector,4>& positions) {
        auto geometry=std::make_shared<StoredGeometry>();
        geometry->vertexCount=4;geometry->stride=12;geometry->formats[0]=3;
        geometry->vertices.resize(48);geometry->indices={0,1,2,0,2,3};
        for(unsigned i=0;i<4;++i)for(unsigned lane=0;lane<3;++lane)
            put(geometry->vertices.data()+i*12+lane*4,std::bit_cast<uint32_t>(positions[i][lane]));
        return geometry;
    };
    // Rasterize the fixed light's asymmetric caster; both camera reconstruction
    // and shadow-atlas orientation must preserve its footprint.
    auto caster=quad({EngineVector{casterLeft,casterBottom,5,1},EngineVector{casterRight,casterBottom,5,1},
                      EngineVector{casterRight,casterTop,5,1},EngineVector{casterLeft,casterTop,5,1}});
    auto shadowDraw=base;shadowDraw.material=WorldMaterial::depth;shadowDraw.fragmentName.clear();
    shadowDraw.fragmentFlags=0;shadowDraw.targets={0,0,0,0,3121};
    shadowDraw.geometry.vertices=shadowDraw.geometry.indices=caster;
    shadowDraw.constants.vectors[0]={2/lightExtent,0,0,0};
    shadowDraw.constants.vectors[1]={0,2/lightExtent,0,0};
    shadowDraw.constants.vectors[2]={0,0,1/lightExtent,0};
    shadowDraw.constants.vectors[3]={0,0,0,1};
    put(shadowDraw.attributes.data()+92,6);shadowDraw.attributes[96]=4;
    WorldClear shadowClear;shadowClear.targets=shadowDraw.targets;shadowClear.viewport=base.viewport;shadowClear.flags=48;
    renderer.clear(shadowClear);require(renderer.draw(shadowDraw),"Moving-camera caster depth raster rejected");
    WorldResolve shadowResolve;shadowResolve.targets=shadowDraw.targets;shadowResolve.flags=4;
    shadowResolve.rectangle={0,0,logicalSide,logicalSide};shadowResolve.destination={3124,0x3124000,logicalSide,logicalSide,23,0,1};
    require(renderer.resolve(shadowResolve),"Moving-camera shadow depth resolve rejected");
    struct Camera {float x,y,z,yaw;};
    constexpr Camera cameras[]{{0,0,0,0},{-3,1,0,.2f},{3,-1,0,-.2f},
                               {-4,-2,3,.3f},{4,2,3,-.3f},{1,-1,-2,.12f}};
    auto halfFloat=[](uint16_t h) {
        const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);
    };
    // Keep distinct immutable geometry identities alive throughout the cases.
    std::vector<std::shared_ptr<StoredGeometry>> receiverGeometry;
    unsigned poses=0,shadowChecks=0,litChecks=0,depthChecks=0;
    for(float receiverZ:{10.0f,16.0f})for(const auto& camera:cameras) {
        const float cosine=std::cos(camera.yaw),sine=std::sin(camera.yaw);
        const EngineVector right{cosine,0,-sine,0},up{0,1,0,0},forward{sine,0,cosine,0};
        const EngineVector cameraPosition{camera.x,camera.y,camera.z,0};
        const float depthA=farPlane/(farPlane-nearPlane),depthB=farPlane*nearPlane/(farPlane-nearPlane);
        // Find a receiver quad beyond all four screen corners. Every vertex
        // lies on the same world plane, even when that plane slopes in view Z.
        std::array<EngineVector,4> positions{};
        constexpr float corners[4][2]{{-1.1f,-1.1f},{1.1f,-1.1f},{1.1f,1.1f},{-1.1f,1.1f}};
        for(unsigned i=0;i<4;++i) {
            EngineVector ray{};
            for(unsigned lane=0;lane<3;++lane)
                ray[lane]=right[lane]*corners[i][0]/projectionX+up[lane]*corners[i][1]/projectionY+forward[lane];
            const float distance=(receiverZ-camera.z)/ray[2];
            for(unsigned lane=0;lane<3;++lane)positions[i][lane]=cameraPosition[lane]+ray[lane]*distance;
            positions[i][3]=1;
        }
        auto receiver=quad(positions);receiverGeometry.push_back(receiver);
        auto sceneDraw=base;sceneDraw.geometry.vertices=sceneDraw.geometry.indices=receiver;
        sceneDraw.material=WorldMaterial::depth;sceneDraw.fragmentName.clear();sceneDraw.fragmentFlags=0;
        sceneDraw.targets={0,0,0,0,3122};sceneDraw.constants.vectors[7]={-camera.x,-camera.y,-camera.z,0};
        for(unsigned lane=0;lane<4;++lane) {
            sceneDraw.constants.vectors[0][lane]=right[lane]*projectionX;
            sceneDraw.constants.vectors[1][lane]=up[lane]*projectionY;
            sceneDraw.constants.vectors[2][lane]=forward[lane]*depthA;
            sceneDraw.constants.vectors[3][lane]=forward[lane];
        }
        sceneDraw.constants.vectors[2][3]=-depthB;
        // Mode 1 uses the world position before c7 is added. Include camera
        // translation in each texgen plane to match homogeneous screen UV.
        std::array<EngineVector,4> screen{};
        for(unsigned row=0;row<4;++row) {
            screen[row]=sceneDraw.constants.vectors[row];
            for(unsigned lane=0;lane<3;++lane)screen[row][3]-=screen[row][lane]*cameraPosition[lane];
        }
        for(unsigned lane=0;lane<4;++lane) {
            sceneDraw.constants.vectors[20][lane]=.5f*(screen[0][lane]+screen[3][lane]);
            sceneDraw.constants.vectors[21][lane]=.5f*(screen[3][lane]-screen[1][lane]);
            sceneDraw.constants.vectors[22][lane]=0;
            sceneDraw.constants.vectors[23][lane]=screen[3][lane];
        }
        put(sceneDraw.attributes.data()+92,6);sceneDraw.attributes[96]=4;
        WorldClear sceneClear;sceneClear.targets=sceneDraw.targets;sceneClear.viewport=base.viewport;sceneClear.flags=48;
        renderer.clear(sceneClear);require(renderer.draw(sceneDraw),"Moving-camera receiver depth raster rejected");
        const auto sceneDepth=renderer.readSurface(3122,true);
        WorldResolve sceneResolve=shadowResolve;sceneResolve.targets=sceneDraw.targets;
        sceneResolve.destination={3125,0x3125000,logicalSide,logicalSide,23,0,1};
        require(renderer.resolve(sceneResolve),"Moving-camera scene depth resolve rejected");
        auto projector=sceneDraw;projector.material=WorldMaterial::post;projector.fragmentName="XREngine_ShadowProj";
        projector.fragmentFlags=8;projector.targets={3123,0,0,0,0};projector.fragmentConstants={};
        projector.fragmentConstants[0]={nearPlane,farPlane,1/farPlane,0};
        projector.fragmentConstants[1]={2*farPlane,farPlane+nearPlane,2*farPlane*nearPlane,2*(farPlane-nearPlane)};
        projector.fragmentConstants[2]={0,0,1/projectionX,1/projectionY};
        // The original Xenon reconstruction has view Y pointing down. Convert
        // it back to world space before the fixed orthographic light projection.
        for(unsigned axis=0;axis<3;++axis)
            projector.fragmentConstants[3+axis]={right[axis]/lightExtent,-up[axis]/lightExtent,
                                                  forward[axis]/lightExtent,cameraPosition[axis]/lightExtent};
        projector.fragmentConstants[6]={0,0,0,1};
        projector.fragmentConstants[9]={1.0f/logicalSide,1.0f/logicalSide,0,0};
        projector.textureObjects[0]=shadowResolve.destination;projector.textureObjects[1]=sceneResolve.destination;
        for(unsigned slot=0;slot<2;++slot) {
            auto& sampler=projector.samplers[slot];sampler.valid=sampler.lodValid=true;
            sampler.minLevel=sampler.maxLevel=0;sampler.address.fill(2);
        }
        put(projector.attributes.data()+92,0x01100000);projector.attributes[97]=8;
        WorldClear targetClear;targetClear.targets=projector.targets;targetClear.viewport=base.viewport;targetClear.flags=1;
        renderer.clear(targetClear);require(renderer.draw(projector),"Moving-camera shadow projector rejected");
        const auto pixels=renderer.readSurface(3123,false);const unsigned side=logicalSide*scale;
        require(pixels.size()==size_t(side)*side*8 && sceneDepth.size()==size_t(side)*side*4,
                "Moving-camera target dimensions differ");
        unsigned poseShadow=0,poseLit=0;
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            // Independent ray/plane intersection supplies the world-space
            // oracle. Avoid the PCF transition band instead of reproducing its
            // tap weights in the expected result.
            const double nx=(2*(double(x)+.5)/side-1)/projectionX;
            const double ny=(1-2*(double(y)+.5)/side)/projectionY;
            const double rayX=cosine*nx+sine,rayY=ny,rayZ=-sine*nx+cosine;
            const double distance=(receiverZ-camera.z)/rayZ;
            const double worldX=camera.x+rayX*distance,worldY=camera.y+rayY*distance;
            const size_t index=size_t(y)*side+x;
            uint32_t packedDepth=0;std::memcpy(&packedDepth,sceneDepth.data()+index*4,4);
            const double expectedDepth=1-(depthA-depthB/distance);
            require(std::abs(double(packedDepth&0xFFFFFF)/0xFFFFFF-expectedDepth)<3e-6,
                    "Moving-camera receiver depth differs from ray/plane projection");
            ++depthChecks;
            constexpr double margin=1.0;
            const bool shadowed=worldX>casterLeft+margin && worldX<casterRight-margin &&
                                worldY>casterBottom+margin && worldY<casterTop-margin;
            const bool lit=worldX<casterLeft-margin || worldX>casterRight+margin ||
                           worldY<casterBottom-margin || worldY>casterTop+margin;
            if(!shadowed && !lit)continue;
            uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+index*8,8);
            const double expected=shadowed?1:0;
            if(std::abs(halfFloat(rgba[0])-expected)>.01 || halfFloat(rgba[1])!=0 || halfFloat(rgba[2])!=0 ||
               std::abs(halfFloat(rgba[3])-(1-expected))>.01) {
                std::fprintf(stderr,"ShadowCamera%u pose=%u receiverZ=%g pixel=%u,%u world=%g,%g actual=%g,%g expected=%g,%g\n",
                    scale,poses,double(receiverZ),x,y,worldX,worldY,halfFloat(rgba[0]),halfFloat(rgba[3]),expected,1-expected);
                throw std::runtime_error("Shadow footprint changed with camera position/angle");
            }
            if(shadowed)++poseShadow;else ++poseLit;
        }
        require(poseShadow>0 && poseLit>500,"Moving-camera fixture lost its shadow or lit samples");
        shadowChecks+=poseShadow;litChecks+=poseLit;++poses;
    }
    std::printf("ShadowCamera%u: %u translated/yawed camera/depth poses, %u depth, %u shadow and %u lit pixels passed.\n",
        scale,poses,depthChecks,shadowChecks,litChecks);
}
