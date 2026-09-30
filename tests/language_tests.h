#pragma once
#include "runtime/native/language_settings.h"

static void testConfiguredLanguage(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    const auto previous = gameLanguageSetting();
    const auto previousEffective = configuredConsoleLanguage();
    const uint32_t block = memory->allocate(4096);
    check(block != 0, "language guest fixture allocation");
    struct Restore {
        uint32_t block; GameLanguage previous; uint32_t previousEffective;
        ~Restore() {
            memory->release(block);
            initializeGameLanguageSetting(previous);
            overrideGameLanguageForRun(static_cast<GameLanguage>(previousEffective));
        }
    } restore{block, previous, previousEffective};
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
        check(initializeGameLanguageSetting(language), "initialize explicit guest language");
        check(query(4) == 0 && memory->read32(output) == uint32_t(language),
              "guest XCONFIG query ignored explicit game language");
        check(base[output] == 0 && base[output + 1] == 0 && base[output + 2] == 0 &&
              base[output + 3] == uint32_t(language) && base[block] == 0xa5 && base[output + 4] == 0xa5,
              "language query failed its exact big-endian four-byte ABI");
        check(PPC_LOAD_U16(required) == 4 && base[required + 2] == 0xa5,
              "language query failed its big-endian required-size ABI");
        check(setGameLanguageSetting(GameLanguage::System) && gameLanguageSetting() == GameLanguage::System &&
              query(4) == 0 && memory->read32(output) == uint32_t(language),
              "guest language query followed a pending menu preference");
        memory->write32(output, 0xabcdef12);
        check(query(3) == 0xc0000023 && memory->read32(output) == 0xabcdef12,
              "short language output buffer was changed");
        check(query(0, false) == 0 && PPC_LOAD_U16(required) == 4,
              "language query size-only request failed");
    }
    check(initializeGameLanguageSetting(GameLanguage::English) && overrideGameLanguageForRun(GameLanguage::Spanish) &&
          gameLanguageSetting() == GameLanguage::English && query(4) == 0 && memory->read32(output) == 5,
          "guest language override changed preferences or failed to latch the run's language");
    check(setGameLanguageSetting(GameLanguage::German) && query(4) == 0 && memory->read32(output) == 5,
          "menu language change replaced the current guest language override");
    const auto invalid = static_cast<GameLanguage>(2);
    check(!setGameLanguageSetting(invalid) && !initializeGameLanguageSetting(invalid) &&
          !overrideGameLanguageForRun(invalid) && gameLanguageSetting() == GameLanguage::German &&
          query(4) == 0 && memory->read32(output) == 5,
          "invalid preference/startup/override language mutated the guest language state");
    check(initializeGameLanguageSetting(gameLanguageSetting()) && query(4) == 0 && memory->read32(output) == 3,
          "next launch did not apply the pending preference to the guest ABI");
    initializeGameLanguageSetting(GameLanguage::System);
    check(query(4) == 0 && memory->read32(output) == configuredConsoleLanguage(),
          "automatic guest language selection differs from Windows fallback");
    std::puts("Guest language: startup latch, pending preferences, run-only overrides, big-endian ABI and bounds passed.");
}
