#pragma once

// CPU-only transport checks for the original stage1 wspos program. Uses the
// endian/geometry helpers from world_palette_usage_tests.h; never creates D3D.
static void worldPositionUsageContract() {
    const EngineVector rows[]{{2, -3, .5f, 7}, {-1, 4, 2, -5}, {3, .25f, -2, 11}};
    auto binding = [&](unsigned weights, bool matrix) {
        auto b = paletteBinding(weights, 1);
        auto& d = b.descriptor;
        d.flags |= 0x07000001 | (matrix ? 0x200 : 0);
        d.coordinateMapping = 0xFAC68800;
        d.positionConversion = 76;
        d.conversions.fill(78);
        d.modes[1] = 10;
        d.parameters[1][0] = matrix ? 16 : 12;
        d.matrices[1] = 12;
        setConstantRow(b, 8, {0, 1, .5f, 1});
        setConstantRow(b, 76, {2, -3, 4, 1});
        setConstantRow(b, 77, {1, .5f, -2, 0});
        // wspos consumes the converted/skinned position, never a UV input.
        poisonConstant(b, 78);
        if (matrix) for (unsigned r = 0; r < 4; ++r)
            setConstantRow(b, 12 + r, {float(r + 1), float(r + 2), float(r + 3), float(r + 4)});
        for (unsigned r = 0; r < 3; ++r) setConstantRow(b, d.parameters[1][0] + r, rows[r]);
        rekeyBinding(b);
        return b;
    };
    StoredDraw draw;
    draw.vertices = paletteVertices({{{1, 2, 3}}}, {{{0, 0, 0, 0}}}, {{{1, 0, 0, 0}}},
                                   {{{0, 0, 0, 0}}}, {{{0, 0, 0, 0}}});
    draw.indices = paletteIndices({0});
    draw.indexCount = 1;
    auto rejected = [&](const EngineVertexBindingSnapshot& b) {
        WorldVertexOptions o;
        o.weights = 123;
        WorldVertexConstants c;
        c.vectors[32] = {1, 2, 3, 4};
        const auto oldO = o;
        const auto oldC = c;
        require(!prepareWorldVertexProgram(b, o, c), "Invalid wspos binding accepted by strict preparation");
        require(o == oldO && c.vectors == oldC.vectors && c.references == oldC.references,
                "Rejected wspos binding published partial strict constants");
        require(!prepareWorldVertexProgramWithGeometry(b, draw, o, c), "Invalid wspos binding accepted with geometry");
        require(o == oldO && c.vectors == oldC.vectors && c.references == oldC.references,
                "Rejected wspos binding published partial geometry constants");
    };
    for (unsigned weights : {0u, 4u, 8u}) for (bool matrix : {false, true}) {
        const auto b = binding(weights, matrix);
        WorldVertexOptions o, geometryO;
        WorldVertexConstants c, geometryC;
        require(prepareWorldVertexProgram(b, o, c), "Original wspos binding rejected");
        require(prepareWorldVertexProgramWithGeometry(b, draw, geometryO, geometryC), "Geometry wspos binding rejected");
        require(o == geometryO && c.vectors == geometryC.vectors && c.references == geometryC.references,
                "Strict and geometry preparation disagree on wspos constants");
        const auto base = b.descriptor.parameters[1][0];
        require(o.modes[1] == 10 && o.weights == weights && o.matrices[1] == matrix &&
                c.references[2][2] == base, "wspos options or parameter reference lost");
        checkUsedRowsExact(b, c, base, 3);
        checkUsedRowsExact(b, c, 76, 2);
        if (matrix) checkUsedRowsExact(b, c, 12, 4);
        for (unsigned r = 0; r < 3; ++r) for (unsigned lane = 0; lane < 4; ++lane) {
            auto bad = b;
            poisonConstant(bad, base + r, lane);
            rejected(bad);
        }
        if (matrix) {
            auto bad = b;
            poisonConstant(bad, 15, 3);
            rejected(bad);
        }
    }
    for (unsigned stage : {0u, 2u, 3u, 4u, 5u, 6u, 7u}) {
        auto bad = binding(0, false);
        bad.descriptor.modes[1] = 4;
        bad.descriptor.modes[stage] = 10;
        bad.descriptor.parameters[stage][0] = 12;
        rekeyBinding(bad);
        rejected(bad);
    }
    for (unsigned base : {254u, 255u}) {
        auto bad = binding(0, false);
        bad.descriptor.parameters[1][0] = uint8_t(base);
        rekeyBinding(bad);
        rejected(bad);
    }
    {
        auto edge = binding(0, false);
        edge.descriptor.parameters[1][0] = 253;
        for (unsigned r = 0; r < 3; ++r) setConstantRow(edge, 253 + r, rows[r]);
        rekeyBinding(edge);
        WorldVertexOptions o;
        WorldVertexConstants c;
        require(prepareWorldVertexProgram(edge, o, c) &&
                prepareWorldVertexProgramWithGeometry(edge, draw, o, c), "Last valid wspos constant range rejected");
        checkUsedRowsExact(edge, c, 253, 3);
    }
    for (unsigned weights : {4u, 8u}) {
        auto b = binding(weights, false);
        poisonConstant(b, 135);
        b.descriptor.parameters[1][0] = 140;
        for (unsigned r = 0; r < 3; ++r) setConstantRow(b, 140 + r, rows[r]);
        rekeyBinding(b);
        WorldVertexOptions o;
        WorldVertexConstants c;
        WorldPaletteProof proof;
        require(!prepareWorldVertexProgram(b, o, c), "Strict wspos accepted an unproven palette tail");
        require(prepareWorldVertexProgramWithGeometry(b, draw, o, c, &proof) && proof.fallback,
                "wspos blocked valid geometry-aware palette recovery");
        checkUsedRowsExact(b, c, 96, 3);
        checkUsedRowsExact(b, c, 140, 3);
        require(c.vectors[135] == EngineVector{}, "Unread poisoned palette row escaped sanitization");
        // A wspos row remains required even when no skinning influence uses it.
        poisonConstant(b, 141);
        rejected(b);
    }
    require(worldFragmentTextureMask("XREngine_MulFilter", 0) == 0, "Original MulFilter incorrectly requires textures");
    require(worldFragmentTextureMask("XREngine_MulFilter", 1) == 0xFFFF, "Unknown MulFilter flags were enabled");
    std::puts("WorldPositionUsage passed: original 0/4/8-weight bindings, matrices, ranges, finite rows and palette recovery.");
}
