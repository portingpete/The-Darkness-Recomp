#pragma once

// CPU-only consumption checks for the original mode-7 volume extrusion.
// Included after world_palette_usage_tests.h for its binding/geometry helpers.
static void worldShadowInputContract() {
    auto binding=[](unsigned weights,bool converted) {
        auto b=paletteBinding(weights,1);
        auto& d=b.descriptor;
        d.flags|=0x07000000 | (converted?4u:0u) | 0x100;
        d.positionConversion=76;
        d.coordinateMapping=(d.coordinateMapping&~0x700u)|0x200u;
        d.modes[0]=7;d.parameters[0][0]=12;d.conversions[0]=78;
        // The original shadow branch skips texture-output matrices entirely.
        d.matrices[0]=255;
        setConstantRow(b,8,{0,1,.5f,1});
        setConstantRow(b,12,{968,692,-140,192});
        setConstantRow(b,76,{2,3,4,1});setConstantRow(b,77,{-2,1,.5f,0});
        setConstantRow(b,78,{2,3,4,5});setConstantRow(b,79,{-.5f,.25f,.75f,1});
        if(!converted)poisonConstant(b,78);
        rekeyBinding(b);return b;
    };
    StoredDraw draw;
    draw.vertices=paletteVertices({{{1,2,3}}},{{{0,0,0,0}}},{{{1,0,0,0}}},
                                  {{{0,0,0,0}}},{{{0,0,0,0}}});
    draw.indices=paletteIndices({0});draw.indexCount=1;
    auto rejected=[&](const EngineVertexBindingSnapshot& b) {
        WorldVertexOptions o;o.weights=123;
        WorldVertexConstants c;c.vectors[32]={1,2,3,4};
        const auto oldO=o;const auto oldC=c;
        require(!prepareWorldVertexProgram(b,o,c),"Invalid shadow-volume constants accepted by strict preparation");
        require(o==oldO && c.vectors==oldC.vectors && c.references==oldC.references,
                "Rejected shadow-volume constants changed strict output");
        require(!prepareWorldVertexProgramWithGeometry(b,draw,o,c),"Invalid shadow-volume constants accepted with geometry");
        require(o==oldO && c.vectors==oldC.vectors && c.references==oldC.references,
                "Rejected shadow-volume constants changed geometry output");
    };
    for(unsigned weights:{0u,4u,8u})for(bool converted:{false,true}) {
        const auto b=binding(weights,converted);
        WorldVertexOptions o,geometryO;WorldVertexConstants c,geometryC;
        require(prepareWorldVertexProgram(b,o,c),"Original mode-7 binding rejected");
        require(prepareWorldVertexProgramWithGeometry(b,draw,geometryO,geometryC),"Geometry mode-7 binding rejected");
        require(o==geometryO && c.vectors==geometryC.vectors && c.references==geometryC.references,
                "Shadow-volume constant preparation differs between paths");
        require(o.modes[0]==7 && o.coordinates[0]==2 && o.conversions[0]==converted &&
                o.weights==weights && c.references[1][2]==12,"Shadow-volume selector binding was lost");
        checkUsedRowsExact(b,c,12,1);checkUsedRowsExact(b,c,76,2);
        if(converted)checkUsedRowsExact(b,c,78,2);
        else require(c.vectors[78]==EngineVector{} && c.vectors[79]==EngineVector{},
                     "Unused shadow-volume conversion rows were copied");
        for(unsigned lane=0;lane<4;++lane) {
            auto bad=b;poisonConstant(bad,12,lane);rejected(bad);
        }
        if(converted)for(unsigned row:{78u,79u})for(unsigned lane=0;lane<4;++lane) {
            auto bad=b;poisonConstant(bad,row,lane);rejected(bad);
        }
    }
    for(unsigned stage=1;stage<8;++stage) {
        auto bad=binding(0,false);bad.descriptor.modes[0]=4;
        bad.descriptor.modes[stage]=7;bad.descriptor.parameters[stage][0]=12;
        rekeyBinding(bad);rejected(bad);
    }
    {
        auto edge=binding(0,true);edge.descriptor.conversions[0]=254;
        setConstantRow(edge,254,{2,3,4,5});setConstantRow(edge,255,{1,2,3,4});rekeyBinding(edge);
        WorldVertexOptions o;WorldVertexConstants c;
        require(prepareWorldVertexProgram(edge,o,c) && prepareWorldVertexProgramWithGeometry(edge,draw,o,c),
                "Last valid shadow selector conversion range rejected");
        checkUsedRowsExact(edge,c,254,2);
        edge.descriptor.conversions[0]=255;rekeyBinding(edge);rejected(edge);
    }
    for(unsigned weights:{4u,8u}) {
        auto b=binding(weights,true);
        b.descriptor.conversions[0]=140;b.descriptor.parameters[0][0]=142;
        setConstantRow(b,140,{2,3,4,5});setConstantRow(b,141,{-.5f,.25f,.75f,1});
        setConstantRow(b,142,{968,692,-140,192});poisonConstant(b,135);rekeyBinding(b);
        WorldVertexOptions o;WorldVertexConstants c;WorldPaletteProof proof;
        require(!prepareWorldVertexProgram(b,o,c),"Strict shadow binding accepted an unproven palette tail");
        require(prepareWorldVertexProgramWithGeometry(b,draw,o,c,&proof) && proof.fallback,
                "Valid shadow-volume palette fallback rejected");
        checkUsedRowsExact(b,c,140,3);
        require(c.vectors[135]==EngineVector{},"Unused shadow palette poison escaped sanitization");
        // These overlap the palette but remain consumed by the extrusion even
        // when none of the geometry's skinning indices references those rows.
        for(unsigned row:{140u,141u,142u}) {
            auto bad=b;poisonConstant(bad,row);rejected(bad);
        }
    }
    std::puts("WorldShadowInput passed: mode-7 stage, selector conversions, finite rows and palette recovery.");
}
