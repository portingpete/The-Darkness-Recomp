#pragma once

// Focused regressions for geometry-aware palette validation. Included by
// world_renderer_tests.cpp; uses its require/put helpers and namespaces.
static EngineVertexBindingSnapshot paletteBinding(unsigned weights, float scale) {
    EngineVertexBindingSnapshot b;
    auto& d = b.descriptor;
    d.flags = weights << 16;
    d.modes.fill(4);
    const auto bytes = encodeEngineVertexDescriptor(d);
    for (unsigned i = 0; i < 5; ++i) for (unsigned n = 0; n < 4; ++n)
        b.key[i + 1] = (b.key[i + 1] << 8) | bytes[i * 4 + n];
    auto constant = [&](unsigned n, EngineVector v) {
        for (unsigned lane = 0; lane < 4; ++lane)
            put(b.constantBytes.data() + n * 16 + lane * 4, std::bit_cast<uint32_t>(v[lane]));
    };
    constant(0, {1, 0, 0, 0}); constant(1, {0, 1, 0, 0});
    constant(2, {0, 0, 1, 0}); constant(3, {0, 0, 0, 1});
    constant(7, {0.5f, -0.5f, 0.25f, 0}); constant(8, {1, 2, 3, scale});
    constant(10, {0.2f, 0.4f, 0.6f, 0.8f});
    for (unsigned bone = 0; bone < 52; ++bone) for (unsigned row = 0; row < 3; ++row) {
        EngineVector v{};
        v[0] = float(bone + 1); v[1] = float(row + 1) / 4; v[2] = float(bone + row) / 8; v[3] = 1;
        constant(96 + bone * 3 + row, v);
    }
    return b;
}
static void rekeyBinding(EngineVertexBindingSnapshot& b) {
    const auto bytes = encodeEngineVertexDescriptor(b.descriptor);
    b.key = {};
    for (unsigned i = 0; i < 5; ++i) for (unsigned n = 0; n < 4; ++n)
        b.key[i + 1] = (b.key[i + 1] << 8) | bytes[i * 4 + n];
}
// Poison one lane with the live failure bits (vector 135, lane 0, FFC00000).
static void poisonConstant(EngineVertexBindingSnapshot& b, unsigned vector, unsigned lane = 0) {
    put(b.constantBytes.data() + vector * 16 + lane * 4, 0xFFC00000u);
}
static uint32_t constantBits(const EngineVertexBindingSnapshot& b, unsigned vector, unsigned lane) {
    const auto* p = b.constantBytes.data() + vector * 16 + lane * 4;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static void setConstantRow(EngineVertexBindingSnapshot& b, unsigned row, EngineVector v) {
    for (unsigned lane = 0; lane < 4; ++lane)
        put(b.constantBytes.data() + row * 16 + lane * 4, std::bit_cast<uint32_t>(v[lane]));
}
static std::shared_ptr<StoredGeometry> paletteVertices(const std::vector<std::array<float, 3>>& positions,
                                                       const std::vector<std::array<float, 4>>& blend,
                                                       const std::vector<std::array<float, 4>>& weights,
                                                       const std::vector<std::array<float, 4>>& blend2 = {},
                                                       const std::vector<std::array<float, 4>>& weights2 = {}) {
    const bool wide = !blend2.empty();
    auto g = std::make_shared<StoredGeometry>();
    g->address = 0xD4030794;
    g->vertexCount = unsigned(positions.size());
    g->formats[0] = 4; g->formats[12] = 4; g->formats[13] = 4;
    if (wide) g->formats[14] = g->formats[15] = 4;
    g->stride = 16 * (wide ? 5u : 3u);
    g->vertices.resize(size_t(g->vertexCount) * g->stride);
    auto word4 = [&](uint8_t* p, unsigned k, float f) { put(p + k * 4, std::bit_cast<uint32_t>(f)); };
    for (unsigned i = 0; i < g->vertexCount; ++i) {
        auto* base = g->vertices.data() + size_t(i) * g->stride;
        for (unsigned k = 0; k < 3; ++k) word4(base, k, positions[i][k]);
        word4(base, 3, 1);
        for (unsigned k = 0; k < 4; ++k) word4(base + 16, k, blend[i][k]);
        for (unsigned k = 0; k < 4; ++k) word4(base + 32, k, weights[i][k]);
        if (wide) {
            for (unsigned k = 0; k < 4; ++k) word4(base + 48, k, blend2[i][k]);
            for (unsigned k = 0; k < 4; ++k) word4(base + 64, k, weights2[i][k]);
        }
    }
    return g;
}
static std::shared_ptr<StoredGeometry> paletteIndices(std::vector<uint16_t> list) {
    auto g = std::make_shared<StoredGeometry>();
    g->indices = std::move(list);
    return g;
}
static void checkUsedRowsExact(const EngineVertexBindingSnapshot& b, const WorldVertexConstants& c,
                               unsigned first, unsigned count) {
    for (unsigned r = first; r < first + count; ++r) for (unsigned lane = 0; lane < 4; ++lane)
        require(std::bit_cast<uint32_t>(c.vectors[r][lane]) == constantBits(b, r, lane),
                "Successful snapshot changed an actually-used finite constant");
}
static void paletteUsageContract() {
    // Synthetic fixture (void modes, scale 1). Observed live facts from
    // boot-20260909-154653 are only the NaN location/bits (absolute vector
    // 135, lane 0, FFC00000) and weights=8; live scale/mode data were not
    // observed.
    auto b = paletteBinding(8, 1.0f);
    poisonConstant(b, 135);
    WorldVertexOptions strictOptions;
    WorldVertexConstants strictConstants;
    require(!prepareWorldVertexProgram(b, strictOptions, strictConstants),
            "Strict geometry-free API accepted an unproven palette tail");
    const std::vector<std::array<float, 3>> positions{{{-0.5f, -0.25f, 0.5f}}, {{0, -0.25f, 0.5f}}, {{0.5f, -0.25f, 0.5f}}};
    // All eight influences per vertex read addresses 0..5 only.
    const std::vector<std::array<float, 4>> blend{{{0, 1, 2, 3}}, {{0, 1, 2, 3}}, {{0, 1, 2, 3}}};
    const std::vector<std::array<float, 4>> weights{{{0.25f, 0.25f, 0.0f, 0.5f}}, {{0.25f, 0.25f, 0.25f, 0.25f}}, {{1, 0, 0, 0}}};
    const std::vector<std::array<float, 4>> blend2{{{4, 5, 4, 5}}, {{4, 5, 4, 5}}, {{4, 5, 4, 5}}};
    const std::vector<std::array<float, 4>> weights2{{{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}};
    auto verts = paletteVertices(positions, blend, weights, blend2, weights2);
    auto indices = paletteIndices({0, 1, 2});
    StoredDraw draw;
    draw.vertices = verts; draw.indices = indices;
    draw.firstIndex = 0; draw.indexCount = 3;
    draw.vertexBindings = b;
    WorldVertexOptions o;
    WorldVertexConstants c;
    WorldPaletteProof proof;
    require(prepareWorldVertexProgramWithGeometry(b, draw, o, c, &proof),
            "Unused palette-tail NaN rejected despite geometry proof");
    require(proof.fallback && proof.usageProven && !proof.referencesVector135,
            "Palette fallback proof misreported usage");
    require(proof.usedRows == 8 && proof.ignoredRows == 148 && proof.firstIgnoredRow == 104,
            "Palette fallback proof counted the wrong rows");
    require(proof.firstIgnoredNonfiniteRow == 135, "Ignored live NaN row was not identified");
    checkUsedRowsExact(b, c, 96, 8);
    checkUsedRowsExact(b, c, 0, 4);
    checkUsedRowsExact(b, c, 7, 2);
    checkUsedRowsExact(b, c, 10, 1);
    for (unsigned r = 8; r < 156; ++r)
        require(c.vectors[96 + r] == EngineVector{0, 0, 0, 0}, "Proven-unread palette row was not sanitized");
    // The identical NaN referenced by any active influence must fail,
    // including a zero-weight influence in the second blend set.
    auto referenced = b;
    auto refBlend2 = blend2;
    refBlend2[1][3] = 39.0f;
    auto refWeights2 = weights2;
    refWeights2[1][3] = 0.0f;
    auto refVerts = paletteVertices(positions, blend, weights, refBlend2, refWeights2);
    StoredDraw refDraw;
    refDraw.vertices = refVerts; refDraw.indices = indices;
    refDraw.firstIndex = 0; refDraw.indexCount = 3;
    refDraw.vertexBindings = referenced;
    WorldVertexOptions sentinelOptions;
    sentinelOptions.weights = 987654321;
    sentinelOptions.positionConversion = sentinelOptions.normal = true;
    sentinelOptions.tangents = sentinelOptions.normalizeNormal = true;
    sentinelOptions.vertexColor = false;
    sentinelOptions.modes.fill(0xAB);
    sentinelOptions.coordinates.fill(0xCD);
    sentinelOptions.conversions.fill(true);
    sentinelOptions.matrices.fill(true);
    WorldVertexConstants sentinelConstants;
    for (auto& v : sentinelConstants.vectors) v = {9.0f, 9.0f, 9.0f, 9.0f};
    for (auto& r : sentinelConstants.references) r = {7, 7, 7, 7};
    WorldVertexOptions failedOptions = sentinelOptions;
    WorldVertexConstants failedConstants = sentinelConstants;
    WorldPaletteProof failedProof;
    require(!prepareWorldVertexProgramWithGeometry(referenced, refDraw, failedOptions, failedConstants, &failedProof),
            "Referenced palette NaN accepted, including through a zero weight");
    require(failedOptions == sentinelOptions, "Rejected palette proof changed options output");
    for (unsigned r = 0; r < 256; ++r) for (unsigned lane = 0; lane < 4; ++lane)
        require(failedConstants.vectors[r][lane] == 9.0f, "Rejected palette proof changed constants output");
    for (const auto& r : failedConstants.references) require((r == std::array<uint32_t, 4>{7, 7, 7, 7}),
            "Rejected palette proof changed references output");
    require(failedProof.usageProven && failedProof.referencesVector135 && !failedProof.fallback,
            "Rejected palette proof lost vector135 evidence");
    // Upper-range and malformed indices fail.
    auto malformed = [&](std::array<float, 4> firstBlend, std::array<float, 4> firstBlend2, const char* what) {
        auto badBlend = blend;
        auto badBlend2 = blend2;
        badBlend[0] = firstBlend;
        badBlend2[0] = firstBlend2;
        auto badVerts = paletteVertices(positions, badBlend, weights, badBlend2, weights2);
        StoredDraw badDraw;
        badDraw.vertices = badVerts; badDraw.indices = indices;
        badDraw.firstIndex = 0; badDraw.indexCount = 3;
        badDraw.vertexBindings = b;
        WorldVertexOptions badOptions;
        WorldVertexConstants badConstants;
        require(!prepareWorldVertexProgramWithGeometry(b, badDraw, badOptions, badConstants, nullptr), what);
    };
    malformed({154.0f, 1, 2, 3}, {4, 5, 4, 5}, "Upper-range palette address accepted");
    malformed({std::bit_cast<float>(0x7FC00000u), 1, 2, 3}, {4, 5, 4, 5}, "NaN blend index accepted");
    malformed({std::bit_cast<float>(0x7F800000u), 1, 2, 3}, {4, 5, 4, 5}, "Infinite blend index accepted");
    malformed({0, 1, 2, 3}, {std::bit_cast<float>(0xFF800000u), 5, 4, 5}, "Second-set nonfinite index accepted");
    {
        // Out-of-range vertex, subset overflow, and nonfinite scale fail.
        auto badIndices = paletteIndices({0, 1, 9});
        StoredDraw badDraw;
        badDraw.vertices = verts; badDraw.indices = badIndices;
        badDraw.firstIndex = 0; badDraw.indexCount = 3;
        badDraw.vertexBindings = b;
        WorldVertexOptions badOptions;
        WorldVertexConstants badConstants;
        require(!prepareWorldVertexProgramWithGeometry(b, badDraw, badOptions, badConstants, nullptr),
                "Out-of-range vertex accepted");
        badDraw.indices = indices;
        badDraw.firstIndex = 2; badDraw.indexCount = 3;
        require(!prepareWorldVertexProgramWithGeometry(b, badDraw, badOptions, badConstants, nullptr),
                "Index subset overflow accepted");
        auto badScale = paletteBinding(8, std::bit_cast<float>(0x7FC00000u));
        poisonConstant(badScale, 135);
        require(!prepareWorldVertexProgram(badScale, badOptions, badConstants),
                "Strict API accepted a nonfinite palette scale");
        require(!prepareWorldVertexProgramWithGeometry(badScale, draw, badOptions, badConstants, nullptr),
                "Geometry proof accepted a nonfinite palette scale");
        auto badBase = paletteBinding(8, 1.0f);
        badBase.descriptor.palette = 200;
        rekeyBinding(badBase);
        require(!prepareWorldVertexProgram(badBase, badOptions, badConstants),
                "Strict API accepted an out-of-range palette base");
        require(!prepareWorldVertexProgramWithGeometry(badBase, draw, badOptions, badConstants, nullptr),
                "Geometry proof accepted an out-of-range palette base");
    }
    // A shared index buffer must use the actual draw subset, not whole-buffer
    // bounds: vertex 3 reads the poisoned row but draws avoiding it succeed.
    {
        const std::vector<std::array<float, 3>> fourPos{{{-0.5f, -0.25f, 0.5f}}, {{0, -0.25f, 0.5f}}, {{0.5f, -0.25f, 0.5f}}, {{0, 0.5f, 0.5f}}};
        auto fourBlend = std::vector<std::array<float, 4>>{{{0, 1, 2, 3}}, {{0, 1, 2, 3}}, {{1, 2, 3, 4}}, {{39, 0, 1, 2}}};
        auto fourWeights = std::vector<std::array<float, 4>>{{{1, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}};
        auto fourBlend2 = std::vector<std::array<float, 4>>{{{4, 5, 4, 5}}, {{4, 5, 4, 5}}, {{5, 4, 5, 4}}, {{0, 1, 2, 3}}};
        auto fourWeights2 = std::vector<std::array<float, 4>>{{{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}, {{1, 1, 1, 1}}};
        auto sharedVerts = paletteVertices(fourPos, fourBlend, fourWeights, fourBlend2, fourWeights2);
        auto sharedIndices = paletteIndices({0, 1, 2, 3});
        WorldVertexOptions subsetOptions;
        WorldVertexConstants subsetConstants;
        WorldPaletteProof subsetProof;
        StoredDraw subsetDraw;
        subsetDraw.vertices = sharedVerts; subsetDraw.indices = sharedIndices;
        subsetDraw.firstIndex = 0; subsetDraw.indexCount = 3;
        subsetDraw.vertexBindings = b;
        require(prepareWorldVertexProgramWithGeometry(b, subsetDraw, subsetOptions, subsetConstants, &subsetProof),
                "Valid shared-index subset rejected by whole-buffer bounds");
        require(subsetProof.fallback && !subsetProof.referencesVector135, "Shared-index subset misreported usage");
        subsetDraw.firstIndex = 2; subsetDraw.indexCount = 2;
        WorldPaletteProof hitProof;
        require(!prepareWorldVertexProgramWithGeometry(b, subsetDraw, subsetOptions, subsetConstants, &hitProof),
                "Shared-index subset hitting the poisoned row accepted");
        require(hitProof.referencesVector135, "Shared-index subset lost vector135 evidence");
    }
    // A supposedly unused palette NaN consumed by another stage still fails.
    {
        auto overlap = paletteBinding(8, 1.0f);
        overlap.descriptor.modes[0] = 1;
        overlap.descriptor.parameters[0][0] = 135;
        rekeyBinding(overlap);
        poisonConstant(overlap, 135);
        WorldVertexOptions overlapOptions;
        WorldVertexConstants overlapConstants;
        require(!prepareWorldVertexProgramWithGeometry(overlap, draw, overlapOptions, overlapConstants, nullptr),
                "Palette NaN reused as a stage parameter accepted");
        auto conversion = paletteBinding(8, 1.0f);
        conversion.descriptor.modes[0] = 0;
        conversion.descriptor.flags |= 1u;
        conversion.descriptor.conversions[0] = 135;
        rekeyBinding(conversion);
        poisonConstant(conversion, 135);
        require(!prepareWorldVertexProgramWithGeometry(conversion, draw, overlapOptions, overlapConstants, nullptr),
                "Palette NaN reused as a conversion range accepted");
        auto matrix = paletteBinding(8, 1.0f);
        matrix.descriptor.modes[1] = 0;
        matrix.descriptor.flags |= (1u << 9);
        matrix.descriptor.matrices[1] = 134;
        rekeyBinding(matrix);
        poisonConstant(matrix, 135);
        require(!prepareWorldVertexProgramWithGeometry(matrix, draw, overlapOptions, overlapConstants, nullptr),
                "Palette NaN reused as a texture matrix accepted");
    }
    // Narrow (<=4 weight) draws prove through the first blend set alone:
    // indices 2,3 read relative rows 2..5 (4 rows starting at 98).
    {
        auto narrow = paletteBinding(2, 1.0f);
        poisonConstant(narrow, 135);
        WorldVertexOptions narrowStrictOptions;
        WorldVertexConstants narrowStrictConstants;
        require(!prepareWorldVertexProgram(narrow, narrowStrictOptions, narrowStrictConstants),
                "Strict API accepted a narrow palette tail");
        const std::vector<std::array<float, 4>> narrowBlend{{{2, 3, 0, 1}}, {{2, 3, 0, 1}}, {{2, 3, 0, 1}}};
        const std::vector<std::array<float, 4>> narrowWeights{{{0.5f, 0.5f, 0, 0}}, {{0.5f, 0.5f, 0, 0}}, {{0.5f, 0.5f, 0, 0}}};
        auto narrowVerts = paletteVertices(positions, narrowBlend, narrowWeights);
        StoredDraw narrowDraw;
        narrowDraw.vertices = narrowVerts; narrowDraw.indices = indices;
        narrowDraw.firstIndex = 0; narrowDraw.indexCount = 3;
        narrowDraw.vertexBindings = narrow;
        WorldVertexOptions narrowOptions;
        WorldVertexConstants narrowConstants;
        WorldPaletteProof narrowProof;
        require(prepareWorldVertexProgramWithGeometry(narrow, narrowDraw, narrowOptions, narrowConstants, &narrowProof),
                "Unused narrow palette tail rejected despite proof");
        require(narrowProof.fallback && narrowProof.usedRows == 4 && !narrowProof.referencesVector135,
                "Narrow palette proof counted the wrong rows");
        checkUsedRowsExact(narrow, narrowConstants, 98, 4);
    }
    std::puts("PaletteUsage passed: unused-tail fallback, zero-weight/upper-range/malformed/subset/overlap rejections, exact used constants, unchanged output on failure.");
}
static void paletteArithmeticContract() {
    // D3D11 MUL/ARL semantics verified directly in
    // engine_world_template.generated.h: R3 = MUL(_vMI, c[0+8].w) with
    // A0.x = (int)ARL(R3.x) for every influence lane, ARL = floor, then
    // MUL/MAD by the weight with no zero-weight skip. D3D11 flushes
    // denormals to signed zero and allows MUL latitude, so the proof
    // brackets the exact double product with its float neighbors and proves
    // the union of both floor addresses.
    auto prove = [](EngineVertexBindingSnapshot binding, std::array<float, 4> pair, WorldPaletteProof* out,
                     WorldVertexConstants* constantsOut = nullptr) {
        const std::vector<std::array<float, 3>> pos{{{0, 0, 0.5f}}};
        const std::vector<std::array<float, 4>> blend{{pair}};
        const std::vector<std::array<float, 4>> wgt{{{1, 0, 0, 0}}};
        auto verts = paletteVertices(pos, blend, wgt);
        auto ind = paletteIndices({0});
        StoredDraw draw;
        draw.vertices = verts; draw.indices = ind;
        draw.firstIndex = 0; draw.indexCount = 1;
        draw.vertexBindings = binding;
        WorldVertexOptions o;
        WorldVertexConstants c;
        const bool ok = prepareWorldVertexProgramWithGeometry(binding, draw, o, c, out);
        if (ok && constantsOut) *constantsOut = c;
        return ok;
    };
    auto tailNaN = [] {
        auto b = paletteBinding(2, 1.0f);
        poisonConstant(b, 135);
        return b;
    };
    WorldPaletteProof proof;
    // Nonzero subnormal index diverges under GPU flush: reject.
    require(!prove(tailNaN(), {std::bit_cast<float>(0x00000001u), 0, 0, 0}, &proof),
            "Subnormal blend index accepted");
    require(!proof.usageProven, "Subnormal index misreported proven usage");
    // Finite subnormal constants are accepted and preserved by the strict
    // finite-copy API by design; only the conservative geometry fallback
    // rejects subnormal operands. Prove both, in either sign.
    for (uint32_t scaleBits : {0x00000001u, 0x80000001u}) {
        auto clean = paletteBinding(2, 1.0f);
        put(clean.constantBytes.data() + 8 * 16 + 3 * 4, scaleBits);
        WorldVertexOptions cleanOptions;
        WorldVertexConstants cleanConstants;
        require(prepareWorldVertexProgram(clean, cleanOptions, cleanConstants),
                "Strict API rejected finite subnormal constants");
        require(std::bit_cast<uint32_t>(cleanConstants.vectors[8][3]) == scaleBits,
                "Strict copy did not preserve subnormal scale bits");
        auto b = clean;
        poisonConstant(b, 135);
        require(!prove(b, {2.0f, 3, 0, 0}, nullptr), "Geometry fallback accepted a subnormal palette scale");
    }
    // Subnormal exact product of two normals (either sign) is rejected.
    {
        auto b = tailNaN();
        setConstantRow(b, 8, {0, 0, 0, std::ldexp(1.0f, -30)});
        require(!prove(b, {std::ldexp(1.0f, -100), 0, 0, 0}, nullptr),
                "Subnormal index/scale product accepted");
        require(!prove(b, {-std::ldexp(1.0f, -100), 0, 0, 0}, nullptr),
                "Negative subnormal index/scale product accepted");
    }
    // Exact zero keeps its exact address even with an unused poisoned tail.
    {
        WorldPaletteProof zeroProof;
        require(prove(tailNaN(), {0.0f, 0, 0, 0}, &zeroProof), "Exact zero index rejected");
        require(zeroProof.fallback && zeroProof.usedRows == 3 && zeroProof.firstIgnoredNonfiniteRow == 135,
                "Exact zero index misreported usage");
    }
    // Plain negative addresses fail.
    require(!prove(tailNaN(), {-1.0f, 0, 0, 0}, nullptr), "Negative blend index accepted");
    // Overflow of the exact product, and huge finite addresses, fail.
    {
        auto big = tailNaN();
        setConstantRow(big, 8, {0, 0, 0, 2.0f});
        require(!prove(big, {std::bit_cast<float>(0x7F7FFFFFu), 0, 0, 0}, nullptr),
                "Overflowing index/scale product accepted");
        require(!prove(tailNaN(), {std::bit_cast<float>(0x7F7FFFFFu), 0, 0, 0}, nullptr),
                "Huge finite palette address accepted");
    }
    // Genuine boundary straddle, independently verified: index bits 40000001
    // (2.000000238418579) with scale bits 3FFFFFFE (1.999999761581421) has
    // exact double product 3.999999999999943, rounded float 4.0, so the
    // bracketing floats floor to 3 and 4 and rows 3..6 (vectors 99..102)
    // are all proven. A NaN reachable only through upward rounding (102)
    // must still fail. Both active lanes use the straddling index.
    {
        const float straddleIndex = std::bit_cast<float>(0x40000001u);
        auto straddle = tailNaN();
        put(straddle.constantBytes.data() + 8 * 16 + 3 * 4, 0x3FFFFFFEu);
        auto poisoned102 = straddle;
        poisonConstant(poisoned102, 102);
        WorldPaletteProof straddleProof;
        require(!prove(poisoned102, {straddleIndex, straddleIndex, 0, 0}, &straddleProof),
                "NaN reachable only on one rounded address accepted");
        require(straddleProof.usageProven && !straddleProof.referencesVector135,
                "Straddled proof misreported usage evidence");
        WorldPaletteProof unionProof;
        WorldVertexConstants unionConstants;
        require(prove(straddle, {straddleIndex, straddleIndex, 0, 0}, &unionProof, &unionConstants),
                "Rounded address union rejected");
        require(unionProof.fallback && unionProof.usedRows == 4 && unionProof.firstIgnoredRow == 96 &&
                unionProof.firstIgnoredNonfiniteRow == 135, "Rounded address union counted the wrong rows");
        checkUsedRowsExact(straddle, unionConstants, 99, 4);
    }
    // Exactly representable products keep their single address: index 5 with
    // scale 1 reads exactly rows 5..7 with no neighbor expansion.
    {
        WorldPaletteProof exactProof;
        WorldVertexOptions o;
        WorldVertexConstants c;
        const std::vector<std::array<float, 3>> pos{{{0, 0, 0.5f}}};
        const std::vector<std::array<float, 4>> blend{{{5.0f, 5.0f, 0, 0}}};
        const std::vector<std::array<float, 4>> wgt{{{1, 0, 0, 0}}};
        auto b = tailNaN();
        auto verts = paletteVertices(pos, blend, wgt);
        auto ind = paletteIndices({0});
        StoredDraw draw;
        draw.vertices = verts; draw.indices = ind;
        draw.firstIndex = 0; draw.indexCount = 1;
        draw.vertexBindings = b;
        require(prepareWorldVertexProgramWithGeometry(b, draw, o, c, &exactProof), "Exact product rejected");
        require(exactProof.usedRows == 3, "Exact product was conservatively expanded");
        checkUsedRowsExact(b, c, 101, 3);
    }
    std::puts("PaletteArithmetic passed: subnormal/overflow/negative/boundary rejections, exact zero/product behavior.");
}
// Hardware/WARP regression: a sanitized fallback draw submits and renders
// bit-identical depth to its fully-finite twin. The shader always applies
// clip = rows0..3 DOT (skinned + c7) with w = c8.y, so the fixture pins an
// explicit identity chain: identity clip rows, zero c7 translation, identity
// bones 0..1, w = 1. The triangle then covers the center pixel by
// construction, and both twins must draw it.
static void paletteFallbackPass(WorldRendererD3D11& renderer) {
    auto binding = paletteBinding(2, 1.0f);
    // Depth-only fixture provides no color stream: clear VERTEX_COLOR so the
    // geometry agrees with renderer step6 (format10 required when
    // options.vertexColor is true) instead of weakening validation.
    binding.descriptor.flags |= 0x01000000u;
    rekeyBinding(binding);
    setConstantRow(binding, 7, {0, 0, 0, 0});
    setConstantRow(binding, 8, {0, 1, 0, 1});
    setConstantRow(binding, 96, {1, 0, 0, 0}); setConstantRow(binding, 97, {0, 1, 0, 0}); setConstantRow(binding, 98, {0, 0, 1, 0});
    setConstantRow(binding, 99, {1, 0, 0, 0}); setConstantRow(binding, 100, {0, 1, 0, 0}); setConstantRow(binding, 101, {0, 0, 1, 0});
    const std::vector<std::array<float, 3>> positions{{{-0.75f, -0.75f, 0.5f}}, {{0, 0.75f, 0.5f}}, {{0.75f, -0.75f, 0.5f}}};
    const std::vector<std::array<float, 4>> blend{{{0, 1, 0, 0}}, {{0, 1, 0, 0}}, {{0, 1, 0, 0}}};
    const std::vector<std::array<float, 4>> weights{{{1, 0, 0, 0}}, {{1, 0, 0, 0}}, {{1, 0, 0, 0}}};
    auto geometry = paletteVertices(positions, blend, weights);
    geometry->indices = {0, 1, 2};
    StoredDraw draw;
    draw.vertices = geometry; draw.indices = geometry;
    draw.firstIndex = 0; draw.indexCount = 3;
    draw.vertexBindings = binding;
    WorldDraw reference;
    require(prepareWorldVertexProgram(binding, reference.options, reference.constants), "Finite palette twin rejected");
    auto poisoned = binding;
    poisonConstant(poisoned, 135);
    draw.vertexBindings = poisoned;
    WorldDraw sanitized;
    WorldPaletteProof proof;
    require(prepareWorldVertexProgramWithGeometry(poisoned, draw, sanitized.options, sanitized.constants, &proof),
            "Fallback snapshot rejected an unused palette tail");
    require(proof.fallback && proof.usedRows == 4 && proof.firstIgnoredNonfiniteRow == 135,
            "Fallback snapshot misreported GPU usage");
    WorldClear clear;
    clear.targets = {1, 0, 0, 0, 2}; clear.viewport = {0, 0, 64, 64}; clear.flags = 49;
    auto submit = [&](WorldDraw& candidate) {
        candidate.geometry.vertices = geometry;
        candidate.geometry.indices = geometry;
        candidate.geometry.indexCount = 3;
        candidate.viewport = clear.viewport;
        candidate.targets = clear.targets;
        candidate.attributes[97] = 8;
        put(candidate.attributes.data() + 92, 0x4006);
        candidate.attributes[96] = 4;
        candidate.attributes[120] = 0x80; candidate.attributes[121] = 2;
        candidate.attributes[124] = 127; candidate.attributes[125] = 255; candidate.attributes[126] = 255;
        renderer.clear(clear);
        require(renderer.draw(candidate), "Palette fallback GPU draw rejected");
        return renderer.readSurface(2, true);
    };
    auto covered = [&](const std::vector<uint8_t>& pixels, const char* what) {
        require(pixels.size() == 64 * 64 * 4, what);
        uint32_t center = 0;
        std::memcpy(&center, pixels.data() + (32 * 64 + 32) * 4, 4);
        require((center >> 24) == 127, what);
    };
    const auto expectedPixels = submit(reference);
    covered(expectedPixels, "Finite twin drew no covered center pixel");
    const auto actualPixels = submit(sanitized);
    covered(actualPixels, "Sanitized fallback draw lost covered stencil pixels");
    require(actualPixels == expectedPixels, "Sanitized fallback draw changed depth/stencil pixels");
    std::puts("PaletteFallback GPU passed: sanitized draw submits and matches its finite twin.");
}
