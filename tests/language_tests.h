#pragma once
#include "runtime/native/language_settings.h"

static void testConfiguredLanguage(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    const auto previous = gameLanguageSetting();
    const uint32_t block = memory->allocate(4096);
    check(block != 0, "language guest fixture allocation");
    struct Restore {
        uint32_t block; GameLanguage previous;
        ~Restore() { memory->release(block); setGameLanguageSetting(previous); }
    } restore{block, previous};
    auto* base = memory->base();
    const uint32_t output = block + 4, required = block + 16;
    auto query = [&](uint16_t capacity, bool present = true) {
        auto call = ctx;
        call.r3.u64 = 3; call.r4.u64 = 9;
        call.r5.u64 = present ? output : 0; call.r6.u64 = capacity; call.r7.u64 = required;
        __imp__ExGetXConfigSetting(call, base);
        return call.r3.u32;
    };
    for (const auto language : {GameLanguage::English, GameLanguage::German, GameLanguage::French,
                               GameLanguage::Spanish, GameLanguage::Italian}) {
        std::memset(base + block, 0xa5, 32);
        check(setGameLanguageSetting(language), "set explicit guest language");
        check(query(4) == 0 && memory->read32(output) == uint32_t(language),
              "guest XCONFIG query ignored explicit game language");
        check(base[output] == 0 && base[output + 1] == 0 && base[output + 2] == 0 &&
              base[output + 3] == uint32_t(language) && base[block] == 0xa5 && base[output + 4] == 0xa5,
              "language query failed its exact big-endian four-byte ABI");
        check(PPC_LOAD_U16(required) == 4 && base[required + 2] == 0xa5,
              "language query failed its big-endian required-size ABI");
        memory->write32(output, 0xabcdef12);
        check(query(3) == 0xc0000023 && memory->read32(output) == 0xabcdef12,
              "short language output buffer was changed");
        check(query(0, false) == 0 && PPC_LOAD_U16(required) == 4,
              "language query size-only request failed");
    }
    setGameLanguageSetting(GameLanguage::System);
    check(query(4) == 0 && memory->read32(output) == configuredConsoleLanguage(),
          "automatic guest language selection differs from Windows fallback");
    std::puts("Guest language: selectable retail IDs, big-endian ABI and buffer-size behavior passed.");
}
