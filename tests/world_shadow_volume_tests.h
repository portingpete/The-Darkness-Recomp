// Exercise the actual closed shadow-volume mask, projector and cleanup states
// from owned-inspection-1 draws60/61/62 (boot-20260911-003849-818418). The camera
// reconstruction test uses a flat quad without stencil and cannot cover these.
static void shadowVolumePass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    constexpr unsigned logicalSide=64;
    constexpr float nearPlane=2.1f,farPlane=2048,projectionX=1.8f,projectionY=2;
    using Vector=std::array<double,3>;
    auto subtract=[](const Vector& a,const Vector& b) {return Vector{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
    auto dot=[](const Vector& a,const Vector& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    auto cross=[](const Vector& a,const Vector& b) {
        return Vector{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    auto normalized=[&](Vector a) {const double length=std::sqrt(dot(a,a));for(auto& x:a)x/=length;return a;};
    // Original immediate geometry, including the finite extruded cap. No cube
    // substitute: camera movement must exercise the captured silhouette sides.
    constexpr std::array<Vector,12> positions{{
        {-5.751537322998047,31.035829544067383,58.81462097167969},
        {-5.751537322998047,31.035829544067383,-1.661214828491211},
        {-5.751537322998047,-1.2144379615783691,-1.661214828491211},
        {-5.751537322998047,-1.2144379615783691,58.81462097167969},
        {-28.046863555908203,-1.2144379615783691,58.81462097167969},
        {-28.046863555908203,31.035829544067383,58.81462097167969},
        {-396.4079284667969,143.4004364013672,-755.3074951171875},
        {-261.7735595703125,104.67550659179688,-812.9241943359375},
        {-261.7475891113281,-75.66676330566406,-812.8418579101562},
        {-396.3316955566406,-114.80839538574219,-755.1486206054688},
        {-525.9705810546875,-104.65937805175781,-682.4254150390625},
        {-526.0541381835938,133.35845947265625,-682.5498046875}
    }};
    constexpr std::array<uint16_t,60> indices{{
        0,1,2,0,2,3,0,3,4,0,4,5,6,8,7,6,9,8,6,10,9,6,11,10,
        0,6,1,1,6,7,1,7,2,2,7,8,2,8,3,3,8,9,
        3,9,4,4,9,10,4,10,5,5,10,11,5,11,0,0,11,6
    }};
    auto makeGeometry=[](const auto& vertices,const auto& elements) {
        auto geometry=std::make_shared<StoredGeometry>();
        geometry->vertexCount=uint32_t(vertices.size());geometry->stride=12;geometry->formats[0]=3;
        geometry->vertices.resize(vertices.size()*12);geometry->indices.assign(elements.begin(),elements.end());
        for(size_t i=0;i<vertices.size();++i)for(unsigned lane=0;lane<3;++lane)
            put(geometry->vertices.data()+i*12+lane*4,std::bit_cast<uint32_t>(float(vertices[i][lane])));
        return geometry;
    };
    const auto volumeGeometry=makeGeometry(positions,indices);
    struct Triangle {Vector origin,edge1,edge2,normal;double squaredNormal;};
    std::array<Triangle,20> triangles{};
    for(unsigned i=0;i<triangles.size();++i) {
        auto& t=triangles[i];t.origin=positions[indices[i*3]];
        t.edge1=subtract(positions[indices[i*3+1]],t.origin);
        t.edge2=subtract(positions[indices[i*3+2]],t.origin);
        t.normal=cross(t.edge1,t.edge2);t.squaredNormal=dot(t.normal,t.normal);
    }
    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;
    binding.descriptor.modes.fill(4);binding.descriptor.modes[0]=1;
    binding.descriptor.parameters[0][0]=20;
    const auto descriptor=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned lane=0;lane<4;++lane)
        binding.key[i+1]=(binding.key[i+1]<<8)|descriptor[i*4+lane];
    // Constant interior shadow-atlas UV. All surviving projector pixels must
    // shade, isolating the stencil/volume contract from reconstruction math.
    const std::array<EngineVector,4> texgen{{{0,0,0,.5f},{0,0,0,.5f},{0,0,0,0},{0,0,0,1}}};
    for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)
        put(binding.constantBytes.data()+(20+row)*16+lane*4,std::bit_cast<uint32_t>(texgen[row][lane]));
    auto base=seed;
    require(prepareWorldVertexProgram(binding,base.options,base.constants),"Shadow-volume binding rejected");
    base.viewport={0,0,logicalSide,logicalSide};base.depthRange={1,0,0,0};
    base.attributes.fill(0);base.attributes[97]=8;base.textures={};base.textureObjects={};base.samplers={};
    base.fragmentConstants={};base.geometry.firstIndex=0;
    WorldClear atlas;atlas.targets[4]=3133;atlas.viewport=base.viewport;atlas.flags=48;atlas.depth=.75f;
    renderer.clear(atlas);
    WorldResolve atlasCopy;atlasCopy.targets=atlas.targets;atlasCopy.flags=4;
    atlasCopy.rectangle={0,0,logicalSide,logicalSide};atlasCopy.destination={3134,0x3134000,logicalSide,logicalSide,23,0,1};
    require(renderer.resolve(atlasCopy),"Shadow-volume atlas resolve rejected");

    struct Pose {Vector camera;double receiverZ;};
    const std::array<Pose,9> poses{{
        {{35,10,45},0},{{-90,10,50},0},{{-20,75,55},0},{{-20,-70,55},0},
        {{-12,15,20},0},       // Camera inside the captured volume.
        {{-4.75,14,20},0},     // Entry surface crosses the camera near plane.
        {{-12,15,20},-200},{{-12,15,20},-500},{{-12,15,20},-900}
    }};
    // Process overlapping characters/lights and then the original again. A
    // stale129 from one sequence must not suppress the next shadow.
    constexpr std::array<Vector,3> offsets{{{0,0,0},{-24,8,0},{0,0,0}}};
    const unsigned side=logicalSide*scale;
    std::vector<std::shared_ptr<StoredGeometry>> receivers;
    unsigned projections=0,marks=0,maskedOut=0,nearCrossings=0,verified=0;
    for(unsigned poseIndex=0;poseIndex<poses.size();++poseIndex) {
        const auto& pose=poses[poseIndex];
        const Vector forward=normalized(subtract(Vector{-20,10,0},pose.camera));
        const Vector right=normalized(cross(forward,Vector{0,0,1})),up=cross(right,forward);
        auto rayAt=[&](double x,double y) {
            Vector ray{};
            for(unsigned lane=0;lane<3;++lane)
                ray[lane]=forward[lane]+(2*x/side-1)*right[lane]/projectionX+(1-2*y/side)*up[lane]/projectionY;
            return ray;
        };
        std::array<Vector,4> corners{};
        constexpr double screenCorners[4][2]{{-.02,1.02},{1.02,1.02},{1.02,-.02},{-.02,-.02}};
        for(unsigned i=0;i<corners.size();++i) {
            const auto ray=rayAt(screenCorners[i][0]*side,screenCorners[i][1]*side);
            const double distance=(pose.receiverZ-pose.camera[2])/ray[2];
            require(distance>nearPlane && distance<farPlane,"Shadow-volume receiver leaves the camera depth range");
            for(unsigned lane=0;lane<3;++lane)corners[i][lane]=pose.camera[lane]+ray[lane]*distance;
        }
        constexpr std::array<uint16_t,6> receiverIndices{{0,1,2,0,2,3}};
        auto receiver=makeGeometry(corners,receiverIndices);receivers.push_back(receiver);
        auto scene=base;scene.targets={3131,0,0,0,3132};scene.material=WorldMaterial::depth;
        scene.fragmentName.clear();scene.fragmentFlags=0;
        scene.geometry.vertices=scene.geometry.indices=receiver;scene.geometry.indexCount=6;
        const float depthA=farPlane/(farPlane-nearPlane),depthB=farPlane*nearPlane/(farPlane-nearPlane);
        for(unsigned lane=0;lane<3;++lane) {
            scene.constants.vectors[0][lane]=float(right[lane])*projectionX;
            scene.constants.vectors[1][lane]=float(up[lane])*projectionY;
            scene.constants.vectors[2][lane]=float(forward[lane])*depthA;
            scene.constants.vectors[3][lane]=float(forward[lane]);
        }
        scene.constants.vectors[0][3]=scene.constants.vectors[1][3]=scene.constants.vectors[3][3]=0;
        scene.constants.vectors[2][3]=-depthB;
        scene.constants.vectors[7]={float(-pose.camera[0]),float(-pose.camera[1]),float(-pose.camera[2]),0};
        put(scene.attributes.data()+92,6);scene.attributes[96]=4;
        WorldClear sceneClear;sceneClear.targets=scene.targets;sceneClear.viewport=base.viewport;
        // Preserve stencil between camera poses: only the first frame clears
        // it to128. Each later sequence is responsible for its own cleanup.
        sceneClear.flags=poseIndex?16:48;sceneClear.stencil=128;renderer.clear(sceneClear);
        if(!poseIndex) {
            auto excluded=sceneClear;excluded.flags=32;excluded.stencil=0;
            excluded.rectangle=std::array<int32_t,4>{0,0,8,int32_t(logicalSide)};renderer.clear(excluded);
        }
        require(renderer.draw(scene),"Shadow-volume receiver draw rejected");
        const auto originalDepth=renderer.readSurface(3132,true);
        require(originalDepth.size()==size_t(side)*side*4,"Shadow-volume depth dimensions differ");
        WorldResolve sceneCopy=atlasCopy;sceneCopy.targets=scene.targets;
        sceneCopy.destination={3135,0x3135000,logicalSide,logicalSide,23,0,1};
        require(renderer.resolve(sceneCopy),"Shadow-volume scene resolve rejected");
        WorldClear colorClear;colorClear.targets=scene.targets;colorClear.viewport=base.viewport;
        colorClear.flags=1;colorClear.color={.125f,.25f,.5f,1};renderer.clear(colorClear);
        std::vector<bool> shaded(size_t(side)*side,false),stable(size_t(side)*side,true);
        for(const auto& offset:offsets) {
            auto mask=scene;mask.geometry.vertices=mask.geometry.indices=volumeGeometry;mask.geometry.indexCount=60;
            for(unsigned lane=0;lane<3;++lane)mask.constants.vectors[7][lane]=float(offset[lane]-pose.camera[lane]);
            put(mask.attributes.data()+92,0x5a12);mask.attributes[96]=7;
            mask.attributes[120]=0x40;mask.attributes[121]=0x30;mask.attributes[122]=0x80;mask.attributes[123]=2;
            mask.attributes[124]=128;mask.attributes[125]=mask.attributes[126]=255;
            require(renderer.draw(mask),"Captured shadow stencil-mask draw rejected");
            const auto maskedDepth=renderer.readSurface(3132,true);
            auto projector=mask;projector.material=WorldMaterial::post;
            projector.fragmentName="XREngine_ShadowProj";projector.fragmentFlags=8;
            projector.fragmentConstants={};projector.fragmentConstants[1]={0,0,1,-1};
            projector.fragmentConstants[5]={0,0,0,.5f};projector.fragmentConstants[6]={0,0,0,1};
            projector.fragmentConstants[9]={1.0f/logicalSide,1.0f/logicalSide,0,0};
            projector.textureObjects[0]=atlasCopy.destination;projector.textureObjects[1]=sceneCopy.destination;
            for(unsigned slot=0;slot<2;++slot) {
                auto& sampler=projector.samplers[slot];sampler.valid=sampler.lodValid=true;
                sampler.minLevel=sampler.maxLevel=0;sampler.address.fill(2);
            }
            // Original alpha-only destination-color multiply, EQUAL128, no
            // stencil writes. RGB must survive every overlapping projector.
            put(projector.attributes.data()+92,0x01005a1a);
            projector.attributes[120]=0x30;projector.attributes[121]=0;projector.attributes[126]=0;
            projector.attributes[144]=9;projector.attributes[145]=1;
            require(renderer.draw(projector),"Captured shadow projection-volume draw rejected");
            const auto color=renderer.readSurface(3131,false);
            auto cleanup=mask;cleanup.attributes[120]=0x20;cleanup.attributes[121]=0x22;
            require(renderer.draw(cleanup),"Captured shadow stencil cleanup rejected");
            const auto cleanedDepth=renderer.readSurface(3132,true);
            require(maskedDepth.size()==originalDepth.size() && cleanedDepth.size()==originalDepth.size() &&
                    color.size()==size_t(side)*side*8,"Shadow-volume readback dimensions differ");
            // Independently intersect a world-space camera ray with each
            // inward-wound face. Exit faces face the camera from inside and
            // are exactly the CCW surfaces used by this volume algorithm.
            auto classify=[&](double x,double y,bool countNear) {
                const auto ray=rayAt(x,y),camera=subtract(pose.camera,offset);
                const double receiverDistance=(pose.receiverZ-pose.camera[2])/ray[2];
                bool beforeReceiver=false,afterReceiver=false;
                for(const auto& t:triangles) {
                    const double orientation=dot(t.normal,ray);
                    if(std::abs(orientation)<1e-10)continue;
                    const double distance=dot(t.normal,subtract(t.origin,camera))/orientation;
                    if(distance<=0 || distance>farPlane)continue;
                    Vector point{};for(unsigned lane=0;lane<3;++lane)point[lane]=camera[lane]+distance*ray[lane];
                    const auto relative=subtract(point,t.origin);
                    const double u=dot(cross(relative,t.edge2),t.normal)/t.squaredNormal;
                    const double v=dot(cross(t.edge1,relative),t.normal)/t.squaredNormal;
                    if(u<0 || v<0 || u+v>1)continue;
                    if(orientation>0) {if(countNear && distance<nearPlane)++nearCrossings;continue;}
                    if(distance<nearPlane)continue;
                    if(distance<receiverDistance)beforeReceiver=true;else afterReceiver=true;
                }
                return beforeReceiver?2u:afterReceiver?1u:0u;
            };
            for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
                const size_t index=size_t(y)*side+x;const unsigned initial=x<8*scale?0:128;
                uint32_t original=0,marked=0,cleaned=0;
                std::memcpy(&original,originalDepth.data()+index*4,4);
                std::memcpy(&marked,maskedDepth.data()+index*4,4);
                std::memcpy(&cleaned,cleanedDepth.data()+index*4,4);
                require((original>>24)==initial,"Earlier shadow sequence leaked stencil into the next camera pose");
                require((marked&0xFFFFFF)==(original&0xFFFFFF) && cleaned==original,
                        "Shadow-volume sequence changed depth or failed to restore its original stencil");
                uint16_t rgba[4]{};std::memcpy(rgba,color.data()+index*8,8);
                require(rgba[0]==0x3000 && rgba[1]==0x3400 && rgba[2]==0x3800,
                        "Alpha-only shadow-volume projection modified scene RGB");
                const unsigned classification=classify(x+.5,y+.5,true);
                bool interior=true;
                for(const auto& delta:std::array<std::array<double,2>,4>{{{-.05,0},{.05,0},{0,-.05},{0,.05}}})
                    interior&=classify(x+.5+delta[0],y+.5+delta[1],false)==classification;
                stable[index]=stable[index] && interior;
                if(initial && classification==1)shaded[index]=true;
                if(!initial) {
                    require((marked>>24)==0 && rgba[3]==0x3c00,"Shadow volume ignored the scene stencil-zero mask");
                    ++maskedOut;
                } else if(stable[index]) {
                    const unsigned expectedStencil=classification==2?129:128;
                    if((marked>>24)!=expectedStencil || rgba[3]!=(shaded[index]?0:0x3c00)) {
                        std::fprintf(stderr,"ShadowVolume%u pose=%u receiverZ=%g pixel=%u,%u offset=%g,%g classification=%u stencil=%u expected=%u alpha=%04X shadowed=%u\n",
                            scale,poseIndex,pose.receiverZ,x,y,offset[0],offset[1],classification,
                            marked>>24,expectedStencil,unsigned(rgba[3]),unsigned(shaded[index]));
                        throw std::runtime_error("Captured shadow-volume mask changed with camera position or prior volume history");
                    }
                    projections+=classification==1;marks+=classification==2;++verified;
                }
            }
        }
    }
    require(projections>100 && marks>100 && maskedOut>100 && nearCrossings>100,
            "Shadow-volume fixture failed to exercise projection, cleanup, stencil-zero and near-plane crossing");
    std::printf("ShadowVolume%u: %u stable mask pixels, %u projections, %u stencil marks, %u excluded pixels and %u near-plane entry crossings across9 camera/depth poses and27 overlapping volume sequences.\n",
        scale,verified,projections,marks,maskedOut,nearCrossings);
}
