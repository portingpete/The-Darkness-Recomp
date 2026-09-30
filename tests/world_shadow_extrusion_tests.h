// Mode 7 is the position-changing shadowvolume2 branch (VP.xrg 709-738;
// original Xenon cache records 97/275/276/408), not constant TEXCOORD output.
// The paired captures contain the same positions and blend bytes in both
// halves of each volume, with only TEXCOORD0.x changing from zero to one.
// In particular meshes 5471/5472 become volumes 5473/5474 (8/4 weights).
// This compact fixture keeps that encoding without embedding game geometry.
// Include after captureWorldPositions(), which captures the real GPU shader.
static void shadowExtrusionGpuContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    constexpr std::array<std::array<unsigned,3>,3> packedPositions{{
        {256,512,128},{1024,768,512},{2047,2047,1023}
    }};
    constexpr std::array<unsigned,4> fourWeights{128,64,32,31};
    constexpr std::array<unsigned,8> eightWeights{64,48,32,32,24,20,18,17};
    constexpr std::array<EngineVector,4> projection{{
        {1.25f,.1f,-.2f,.4f},{-.05f,.8f,.3f,-.2f},
        {.2f,-.15f,1.1f,.6f},{.01f,.02f,.03f,1}
    }};
    constexpr EngineVector camera{.75f,-1.25f,2.5f,0};
    constexpr EngineVector light{-.5f,1.5f,-2,20};
    unsigned compared=0,separated=0,clamped=0;
    for(unsigned weights:{0u,4u,8u})for(unsigned variant=0;variant<3;++variant) {
        const bool convertPosition=variant!=2,convertSelector=variant!=0;
        const unsigned coordinate=variant==1?2u:0u;
        StoredGeometry geometry;
        geometry.vertexCount=6;geometry.formats[0]=15;
        geometry.formats[1]=geometry.formats[3]=1;
        geometry.stride=12+(weights==8?16:weights==4?8:0);
        if(weights)geometry.formats[12]=geometry.formats[13]=16;
        if(weights==8)geometry.formats[14]=geometry.formats[15]=16;
        geometry.vertices.resize(size_t(geometry.vertexCount)*geometry.stride);
        for(unsigned vertex=0;vertex<geometry.vertexCount;++vertex) {
            auto* bytes=geometry.vertices.data()+size_t(vertex)*geometry.stride;
            const auto& position=packedPositions[vertex%3];
            put(bytes,position[0]|(position[1]<<11)|(position[2]<<22));
            const float selector=vertex<3?0.0f:1.0f;
            // The other coordinate deliberately disagrees, so binding the
            // selector to a hard-coded TEXCOORD0 cannot satisfy the oracle.
            put(bytes+4,std::bit_cast<uint32_t>(coordinate==0?selector:1-selector));
            put(bytes+8,std::bit_cast<uint32_t>(coordinate==2?selector:1-selector));
            for(unsigned group=0;group<(weights+3)/4;++group) {
                uint32_t indices=0,blend=0;
                for(unsigned lane=0;lane<4;++lane) {
                    const unsigned bone=group*4+lane;
                    indices|=bone<<(lane*8);
                    blend|=(weights==8?eightWeights[bone]:fourWeights[lane])<<(lane*8);
                }
                put(bytes+12+group*8,indices);put(bytes+16+group*8,blend);
            }
        }
        std::vector<WorldVertex> vertices;
        require(decodeWorldVertices(geometry,vertices),"Shadow extrusion packed fixture rejected");
        for(unsigned n=0;n<3;++n) {
            require(vertices[n].position==vertices[n+3].position &&
                    vertices[n].indices==vertices[n+3].indices && vertices[n].weights==vertices[n+3].weights &&
                    vertices[n].indices2==vertices[n+3].indices2 && vertices[n].weights2==vertices[n+3].weights2 &&
                    vertices[n].tex[coordinate][0]==0 && vertices[n+3].tex[coordinate][0]==1,
                    "Shadow extrusion fixture lost its coincident near/far vertex pairs");
        }
        for(float radius:{20.0f,1.0f}) {
            auto binding=fixture(weights,false);
            auto& descriptor=binding.descriptor;
            descriptor.flags=0x01000000|(weights<<16)|(convertPosition?0x04000000:0)|
                (convertSelector?(1u<<coordinate):0);
            descriptor.coordinateMapping=coordinate<<8;
            descriptor.modes.fill(4);descriptor.modes[0]=7;
            descriptor.conversions.fill(0);descriptor.conversions[0]=78;
            descriptor.parameters={};descriptor.parameters[0][0]=12;descriptor.matrices.fill(0);
            const auto encoded=encodeEngineVertexDescriptor(descriptor);binding.key={};
            for(unsigned i=0;i<5;++i)for(unsigned lane=0;lane<4;++lane)
                binding.key[i+1]=(binding.key[i+1]<<8)|encoded[i*4+lane];
            auto set=[&](unsigned reg,EngineVector value) {
                for(unsigned lane=0;lane<4;++lane)
                    put(binding.constantBytes.data()+reg*16+lane*4,std::bit_cast<uint32_t>(value[lane]));
            };
            for(unsigned row=0;row<4;++row)set(row,projection[row]);
            set(7,camera);set(12,{light[0],light[1],light[2],radius});
            set(78,{.5f,7,11,13});set(79,{.25f,17,19,23});
            WorldVertexOptions options;WorldVertexConstants constants;
            require(prepareWorldVertexProgram(binding,options,constants),"Shadow extrusion binding rejected");
            require(options.modes[0]==7 && options.coordinates[0]==coordinate &&
                    options.conversions[0]==convertSelector && options.positionConversion==convertPosition,
                    "Shadow extrusion descriptor selected the wrong input variant");
            context->ClearState();
            const auto actual=captureWorldPositions(device,context,vertices,options,constants);
            require(actual.size()==vertices.size(),"Shadow extrusion GPU capture lost vertices");
            for(unsigned n=0;n<vertices.size();++n) {
                const auto& vertex=vertices[n];
                // Independent geometric oracle: blend the affine bone
                // transforms, then move toward the light's radius along the
                // outward ray. Camera translation happens after extrusion.
                std::array<double,4> point{vertex.position[0],vertex.position[1],vertex.position[2],1};
                if(convertPosition) {
                    point[0]=point[0]*2+1;point[1]=point[1]*3-1;point[2]=point[2]*4+.5;
                }
                if(weights) {
                    std::array<double,3> blended{};
                    for(unsigned bone=0;bone<weights;++bone) {
                        const double weight=bone<4?vertex.weights[bone]:vertex.weights2[bone-4];
                        for(unsigned lane=0;lane<3;++lane)
                            blended[lane]+=weight*(point[lane]*(lane+2)+double(bone+lane)/8);
                    }
                    for(unsigned lane=0;lane<3;++lane)point[lane]=blended[lane];
                }
                std::array<double,3> ray{};double distanceSquared=0;
                for(unsigned lane=0;lane<3;++lane) {
                    ray[lane]=point[lane]-light[lane];distanceSquared+=ray[lane]*ray[lane];
                }
                const double distance=std::sqrt(distanceSquared);
                const double extension=(std::max)(0.0,double(radius)-distance);
                double selector=vertex.tex[coordinate][0];
                if(convertSelector)selector=selector*.5+.25;
                for(unsigned lane=0;lane<3;++lane)
                    point[lane]+=ray[lane]/distance*extension*selector+camera[lane];
                for(unsigned row=0;row<4;++row) {
                    double expected=0;
                    for(unsigned lane=0;lane<4;++lane)expected+=projection[row][lane]*point[lane];
                    if(!std::isfinite(actual[n][row]) || std::abs(actual[n][row]-expected)>4e-5*(1+std::abs(expected))) {
                        std::fprintf(stderr,"ShadowExtrusion weights=%u variant=%u radius=%g vertex=%u lane=%u actual=%.9g expected=%.12g\n",
                            weights,variant,radius,n,row,actual[n][row],expected);
                        require(false,"Shadow extrusion GPU position differs from the original geometric operation");
                    }
                    ++compared;
                }
                // The original shadow-volume cache exports position only;
                // mode 7 must no longer leak the light as constant TEXCOORD0.
                for(unsigned lane=4;lane<8;++lane)
                    require(actual[n][lane]==0,"Shadow extrusion still executes constant TEXCOORD0 mode");
            }
            double pairDistance=0;
            for(unsigned lane=0;lane<4;++lane)pairDistance+=std::abs(actual[0][lane]-actual[3][lane]);
            if(radius==20) {
                require(pairDistance>.1,"Shadow volume near/far halves still collapse");++separated;
            } else {
                for(unsigned n=0;n<3;++n)for(unsigned lane=0;lane<4;++lane)
                    require(std::abs(actual[n][lane]-actual[n+3][lane])<1e-5,
                            "Shadow extrusion moves vertices that are outside the light radius");
                ++clamped;
            }
        }
    }
    std::printf("ShadowExtrusionGPU passed: %u position comparisons, %u separated volume pairs, %u radius-clamped variants; static/4/8 weights, packed positions and remapped/converted selectors.\n",
                compared,separated,clamped);
}

// Exercise renderer input validation and the real depth/stencil state as well
// as shader stream output. CPU-extruded FLOAT3 geometry provides an independent
// reference for the recovered position-changing mode on a closed volume.
static void shadowExtrusionRasterPass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    constexpr std::array<std::array<float,3>,3> nearPoints{{
        {-.2f,-.2f,0},{.25f,-.2f,0},{.02f,.25f,0}
    }};
    constexpr std::array<uint16_t,24> indices{{
        0,2,1,3,4,5,0,1,3,1,4,3,1,2,4,2,5,4,2,0,5,0,3,5
    }};
    constexpr EngineVector light{0,0,-.75f,2};
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=6;geometry->stride=16;geometry->formats[0]=3;geometry->formats[1]=1;
    geometry->vertices.resize(6*16);geometry->indices.assign(indices.begin(),indices.end());
    auto reference=std::make_shared<StoredGeometry>();
    reference->vertexCount=6;reference->stride=12;reference->formats[0]=3;
    reference->vertices.resize(6*12);reference->indices=geometry->indices;
    for(unsigned vertex=0;vertex<6;++vertex) {
        const auto& nearVertex=nearPoints[vertex%3];
        const double distance=std::sqrt(double(nearVertex[0])*nearVertex[0]+double(nearVertex[1])*nearVertex[1]+.75*.75);
        for(unsigned lane=0;lane<3;++lane) {
            put(geometry->vertices.data()+vertex*16+lane*4,std::bit_cast<uint32_t>(nearVertex[lane]));
            const float extruded=vertex<3?nearVertex[lane]:float(light[lane]+(nearVertex[lane]-light[lane])*light[3]/distance);
            put(reference->vertices.data()+vertex*12+lane*4,std::bit_cast<uint32_t>(extruded));
        }
        put(geometry->vertices.data()+vertex*16+12,std::bit_cast<uint32_t>(vertex<3?0.0f:1.0f));
    }
    auto binding=fixture(0,false);auto& descriptor=binding.descriptor;
    descriptor.flags=0x01000000;descriptor.coordinateMapping=0;
    descriptor.modes.fill(4);descriptor.modes[0]=7;
    descriptor.parameters={};descriptor.parameters[0][0]=12;
    descriptor.conversions.fill(0);descriptor.matrices.fill(0);
    const auto encoded=encodeEngineVertexDescriptor(descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned lane=0;lane<4;++lane)
        binding.key[i+1]=(binding.key[i+1]<<8)|encoded[i*4+lane];
    auto set=[&](unsigned reg,EngineVector value) {
        for(unsigned lane=0;lane<4;++lane)
            put(binding.constantBytes.data()+reg*16+lane*4,std::bit_cast<uint32_t>(value[lane]));
    };
    set(0,{.7f,0,0,0});set(1,{0,.7f,0,0});set(2,{0,0,.2f,.25f});set(3,{0,0,0,1});
    set(7,{0,0,0,0});set(12,light);
    auto draw=seed;draw.targets={0,0,0,0,3512};draw.surfaceBindings={};draw.viewport={0,0,64,64};
    draw.material=WorldMaterial::depth;draw.fragmentName.clear();draw.fragmentFlags=0;
    draw.depthRange={0,1,0,0};draw.geometry.vertices=draw.geometry.indices=geometry;
    draw.geometry.firstIndex=0;draw.geometry.indexCount=unsigned(indices.size());
    require(prepareWorldVertexProgram(binding,draw.options,draw.constants),"Shadow extrusion raster binding rejected");
    draw.attributes.fill(0);put(draw.attributes.data()+92,0x42ba);draw.attributes[96]=4;draw.attributes[97]=8;
    put(draw.attributes.data()+116,(64u<<16)|64u);
    draw.attributes[120]=draw.attributes[122]=0x80;
    draw.attributes[121]=6;draw.attributes[123]=7;
    draw.attributes[124]=128;draw.attributes[125]=draw.attributes[126]=255;
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;
    clear.flags=48;clear.depth=.35f;clear.stencil=128;
    renderer.clear(clear);const auto empty=renderer.readSurface(3512,true);
    // A mode-7 draw without its selector must fail, rather than accepting the
    // default (0,0,0,1) coordinate and silently collapsing the volume again.
    auto missingSelector=draw;missingSelector.geometry.vertices=missingSelector.geometry.indices=reference;
    require(!renderer.draw(missingSelector),"Shadow extrusion accepted a missing selector stream");
    require(renderer.readSurface(3512,true)==empty,"Rejected shadow extrusion changed the stencil target");
    auto ordinary=draw;ordinary.options.modes.fill(4);
    ordinary.geometry.vertices=ordinary.geometry.indices=reference;
    require(renderer.draw(ordinary),"CPU-extruded reference volume rejected");
    const auto expected=renderer.readSurface(3512,true);
    renderer.clear(clear);require(renderer.draw(draw),"Mode-7 extrusion volume rejected");
    const auto actual=renderer.readSurface(3512,true);
    require(actual.size()==size_t(64*scale)*(64*scale)*4 && actual.size()==expected.size() && actual.size()==empty.size(),
            "Shadow extrusion raster dimensions differ");
    unsigned stencilPixels=0;
    for(size_t byte=0;byte<actual.size();byte+=4) {
        uint32_t got=0,want=0,initial=0;
        std::memcpy(&got,actual.data()+byte,4);std::memcpy(&want,expected.data()+byte,4);
        std::memcpy(&initial,empty.data()+byte,4);
        require((got&0xffffff)==(initial&0xffffff) && (want&0xffffff)==(initial&0xffffff),
                "Shadow extrusion modified receiver depth");
        if(got!=want) {
            std::fprintf(stderr,"ShadowExtrusionRaster scale=%u pixel=%zu actual=%08X expected=%08X\n",scale,byte/4,got,want);
            require(false,"Mode-7 stencil footprint differs from independently extruded reference geometry");
        }
        stencilPixels+=(got>>24)!=128;
    }
    require(stencilPixels>20*scale*scale,"Shadow extrusion raster fixture produced no meaningful stencil footprint");
    std::printf("ShadowExtrusionRaster%u passed: %u stencil pixels match CPU-extruded volume; depth preserved and missing selector rejected.\n",
                scale,stencilPixels);
}
