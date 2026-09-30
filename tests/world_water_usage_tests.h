#pragma once

// Original cache2/438/439 stage layouts. The normal/tangents export prepared
// vertex inputs, so stale TEXPARAM references must never become required rows.
static void worldWaterUsageContract() {
    constexpr std::array<std::array<uint8_t,8>,3> layouts{{
        {{0,20,1,11,23,24,1,1}}, {{0,0,8,11,23,24,1,1}}, {{0,0,8,11,23,24,4,4}}
    }};
    auto binding = [&](unsigned variant,unsigned weights,bool matrix) {
        auto b=paletteBinding(weights,1);
        auto& d=b.descriptor;
        d.flags=0x41600000 | (weights<<16) | 0xC;
        d.coordinateMapping=0xFAC68800;
        d.modes=layouts[variant];
        d.conversions.fill(78);
        d.conversions[2]=80;d.conversions[3]=82;
        poisonConstant(b,78);
        for(unsigned s=0;s<8;++s) {
            d.parameters[s][0]=uint8_t(12+4*s);
            if(d.modes[s]==1)for(unsigned r=0;r<4;++r)
                setConstantRow(b,12+4*s+r,{float(r+1),float(s+2),float(r+s+3),float(r+4)});
            if(d.modes[s]==20)setConstantRow(b,12+4*s,{3,5,7,1});
        }
        for(unsigned s=3;s<=5;++s) {
            d.parameters[s][0]=uint8_t(200+s);
            poisonConstant(b,200+s);
            d.matrices[s]=uint8_t(44+4*(s-3));
            if(matrix) {
                d.flags|=1u<<(8+s);
                for(unsigned r=0;r<4;++r)
                    setConstantRow(b,d.matrices[s]+r,{float(s+r),float(int(r)-1),float(s+2),float(r+3)});
            }
        }
        setConstantRow(b,80,{2,-3,4,1});setConstantRow(b,81,{.5f,1,-2,0});
        setConstantRow(b,82,{-1,2,.25f,1});setConstantRow(b,83,{3,-1,2,0});
        rekeyBinding(b);
        return b;
    };
    StoredDraw draw;
    draw.vertices=paletteVertices({{{1,2,3}}},{{{0,0,0,0}}},{{{1,0,0,0}}},
                                 {{{0,0,0,0}}},{{{0,0,0,0}}});
    draw.indices=paletteIndices({0});draw.indexCount=1;
    auto rejected=[&](const EngineVertexBindingSnapshot& b) {
        WorldVertexOptions o;o.weights=123;
        WorldVertexConstants c;c.vectors[32]={1,2,3,4};
        const auto beforeO=o;const auto beforeC=c;
        require(!prepareWorldVertexProgram(b,o,c),"Invalid water basis accepted by strict preparation");
        require(o==beforeO && c.vectors==beforeC.vectors && c.references==beforeC.references,
                "Rejected water basis published partial strict output");
        require(!prepareWorldVertexProgramWithGeometry(b,draw,o,c),"Invalid water basis accepted with geometry");
        require(o==beforeO && c.vectors==beforeC.vectors && c.references==beforeC.references,
                "Rejected water basis published partial geometry output");
    };
    for(unsigned variant=0;variant<layouts.size();++variant)for(bool matrix:{false,true}) {
        const auto b=binding(variant,0,matrix);
        WorldVertexOptions o,geometryO;WorldVertexConstants c,geometryC;
        require(prepareWorldVertexProgram(b,o,c),"Original Water/CubeWater vertex layout rejected");
        require(prepareWorldVertexProgramWithGeometry(b,draw,geometryO,geometryC),"Water geometry preparation rejected");
        require(o==geometryO && c.vectors==geometryC.vectors && c.references==geometryC.references,
                "Strict and geometry water preparation disagree");
        require(o.normal && o.tangents && o.modes==layouts[variant],"Water basis or authored modes lost");
        checkUsedRowsExact(b,c,80,4);
        for(unsigned s=3;s<=5;++s) {
            require(c.vectors[200+s]==EngineVector{},"Unused water TEXPARAM poison escaped sanitization");
            if(matrix)checkUsedRowsExact(b,c,b.descriptor.matrices[s],4);
        }
        for(unsigned row=80;row<84;++row)for(unsigned lane=0;lane<4;++lane) {
            auto bad=b;poisonConstant(bad,row,lane);rejected(bad);
        }
        if(matrix)for(unsigned s=3;s<=5;++s) {
            auto bad=b;poisonConstant(bad,b.descriptor.matrices[s]+3,3);rejected(bad);
            bad=b;bad.descriptor.matrices[s]=253;rekeyBinding(bad);rejected(bad);
        }
    }
    for(unsigned mode:{11u,23u,24u}) {
        const unsigned valid=mode==11?3:mode==23?4:5;
        for(unsigned stage=0;stage<8;++stage)if(stage!=valid) {
            auto bad=binding(2,0,false);bad.descriptor.modes.fill(4);
            bad.descriptor.modes[stage]=uint8_t(mode);rekeyBinding(bad);rejected(bad);
        }
        auto bad=binding(2,0,false);
        bad.descriptor.flags &= mode==11?~0x00200000u:~0x00400000u;
        rekeyBinding(bad);rejected(bad);
    }
    for(unsigned weights:{4u,8u}) {
        auto b=binding(2,weights,true);
        WorldVertexOptions o;WorldVertexConstants c;WorldPaletteProof proof;
        require(!prepareWorldVertexProgram(b,o,c),"Strict water accepted poisoned palette tail");
        require(prepareWorldVertexProgramWithGeometry(b,draw,o,c,&proof) && proof.fallback,
                "Water basis blocked proven geometry-aware palette recovery");
        checkUsedRowsExact(b,c,80,4);checkUsedRowsExact(b,c,44,12);checkUsedRowsExact(b,c,96,3);
        require(c.vectors[203]==EngineVector{},"Unused water parameter remained in recovered palette");
        poisonConstant(b,81,1);rejected(b);
    }
    std::puts("WorldWaterUsage passed: authored Water/CubeWater layouts, basis flags/stages, dead parameters, tangent conversions, matrices and palette recovery.");
}
