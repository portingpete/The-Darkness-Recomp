#pragma once

// Exercise the original translated glyph lookup against the loaded revision's
// table. The Russian executable changes seven characters while sharing AOT.
static void testRevisionGlyphs(PPCContext& ctx) {
    auto* base = memory->base();
    constexpr uint32_t table = 0x82073AC8;
    unsigned comparisons = 0;
    for (uint32_t index = 0; index < 256 && base[table + index]; ++index) {
        const uint8_t character = base[table + index];
        // Repeated bytes use the first occurrence in the authored table.
        uint32_t first = 0;
        while (base[table + first] != character) ++first;
        for (uint32_t font = 0; font < 2; ++font) {
            auto call = ctx;
            call.r3.u64 = character;
            call.r4.u64 = font;
            sub_8239B398(call, base);
            check(call.r3.u32 == (font ? 864 : 272) + first,
                  "AOT glyph lookup ignored the loaded revision's character table");
            ++comparisons;
        }
    }
    check(comparisons == 320, "Revision character table has an unexpected length");
    std::printf("Revision glyph lookup: %u original guest calls passed.\n", comparisons);
}

static void testRussianRevisionGlyphs(PPCContext& ctx) {
    constexpr uint32_t table = 0x82073AC8;
    constexpr std::pair<uint32_t, uint8_t> characters[]{
        {111, 0xF0}, {147, 0xF7}, {154, 0xDE}, {156, 0xA8},
        {157, 0xFE}, {158, 0xFF}, {159, 0xB8}
    };
    for (const auto [index, character] : characters)
        check(memory->base()[table + index] == character,
              "Russian executable lost its Cyrillic character mapping");
    testRevisionGlyphs(ctx);
}
