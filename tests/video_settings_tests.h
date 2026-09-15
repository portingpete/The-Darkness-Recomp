#pragma once
#include "runtime/native/graphics_settings.h"
#include "runtime/native/video_settings_menu.h"
#include "runtime/native/fov_settings.h"
#include <fstream>

// Read through the guest's real file imports, including the startup archive.
// A correct loose registry alone cannot affect the prefetched pause menu.
static void testVideoMenuFiles(PPCContext& ctx) {
    wchar_t executable[32768]{};
    check(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))) != 0, "test executable path missing");
    const auto directory = std::filesystem::path(executable).parent_path();
    auto* base = memory->base();
    for (const auto& pair : {std::pair{"game:\\Content\\Gui\\CubeWnd.xcr", L"CubeWnd.pc.xcr"},
                             std::pair{"D:\\Content\\Xdf\\GameContext_Create.XDF", L"GameContext_Create.pc.xdf"}}) {
        std::ifstream input(directory / pair.second, std::ios::binary);
        const std::vector<char> expected{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        check(!expected.empty(), "packaged menu dependency missing");
        const auto scratch = memory->allocate(uint32_t(expected.size()) + 4096);
        check(scratch != 0, "menu file fixture allocation failed");
        struct Cleanup { uint32_t address; ~Cleanup() { memory->release(address); } } cleanup{scratch};
        const auto ios = scratch + 64, out = scratch + 80, offset = scratch + 96, data = scratch + 4096;
        memory->write32(scratch, 0xfffffffd);
        memory->write32(scratch + 4, scratch + 16);
        memory->write32(scratch + 8, 0x40);
        const auto length = uint32_t(std::strlen(pair.first));
        memory->write32(scratch + 16, (length << 16) | (length + 1));
        memory->write32(scratch + 20, scratch + 256);
        std::memcpy(base + scratch + 256, pair.first, length + 1);
        auto call = ctx;
        call.r3.u64 = out; call.r4.u64 = GENERIC_READ | SYNCHRONIZE;
        call.r5.u64 = scratch; call.r6.u64 = ios; call.r7.u64 = FILE_SHARE_READ; call.r8.u64 = 0x60;
        __imp__NtOpenFile(call, base);
        check(call.r3.u32 == 0, "guest could not open menu dependency");
        const auto handle = memory->read32(out);
        struct Close { PPCContext ctx; uint8_t* base; uint32_t handle;
            ~Close() { ctx.r3.u64 = handle; __imp__NtClose(ctx, base); } } close{ctx, base, handle};
        PPC_STORE_U64(offset, 0);
        call = ctx;
        call.r3.u64 = handle; call.r4.u64 = call.r5.u64 = call.r6.u64 = 0;
        call.r7.u64 = ios; call.r8.u64 = data; call.r9.u64 = expected.size(); call.r10.u64 = offset;
        __imp__NtReadFile(call, base);
        check(call.r3.u32 == 0 && memory->read32(ios + 4) == expected.size() &&
              std::memcmp(base + data, expected.data(), expected.size()) == 0,
              "guest read original menu bytes instead of the packaged replacement");
    }
}



// Private menu objects exercise original property parsing and input dispatch.
// This does not render, start the game, or use a GPU.
static void testVideoSettings(PPCContext& ctx) {
    testVideoMenuFiles(ctx);
    initializeVideoSettingsMenu();
    check(!graphicsSettings().motionBlur, "motion blur must default to Off");
    check(!graphicsSettings().antialiasing, "antialiasing must default to Off");
    check(graphicsSettings().brightnessPercent == 100, "brightness must default to neutral");
    for (unsigned bits = 0; bits < 16; ++bits) {
        for (const auto name : {"XREngine_Final5", "XREngine_Final4"}) {
            check(graphicsFragmentFlags(name, bits, true) == (bits & 14), "motion blur remained enabled");
            check(graphicsFragmentFlags(name, bits, false) == (bits & 6), "bloom toggle changed exposure or color mapping");
            check(graphicsFragmentFlags(name, bits | 0x100, false) == ((bits & 6) | 0x100), "unrelated flags were lost");
            check(graphicsFragmentFlags(name, bits, true, true) == bits, "motion blur On did not restore the original permutation");
            check(graphicsFragmentFlags(name, bits | 0x100, false, true) == ((bits & 7) | 0x100),
                  "motion blur On changed bloom, exposure or unrelated flags");
        }
        for (const auto name : {"XRShader_MotionMap", "XREngine_RadialBlurInvert", "XREngine_GaussClampedHurt", "XRShader_FP20_NDSP"})
            for (bool blur : {false, true})
                check(graphicsFragmentFlags(name, bits, false, blur) == bits, "unrelated effect was modified");
    }
    for (const GraphicsSettings s : {GraphicsSettings{0,180,false,false,false,false}, GraphicsSettings{1000,720,true,true,true,true},
                                   GraphicsSettings{60,1080,true,false,false,true}, GraphicsSettings{60,1440,false,true,true,false},
                                   GraphicsSettings{1000,2160,true,true,true,true}})
        for (bool aa : {false, true}) {
            auto selected = s; selected.antialiasing = aa;
            for (unsigned gamma : {50u, 95u, 100u, 150u}) for (unsigned brightness : {50u, 95u, 100u, 200u}) {
                selected.gammaPercent = gamma;
                selected.brightnessPercent = brightness;
                check(setGraphicsSettings(selected) && graphicsSettings() == selected, "graphics packing boundary corrupted another setting");
            }
        }
    setGraphicsSettings({1000,720,true,true,true,true});
    auto invalid = graphicsSettings(); invalid.frameRateLimit = 1001;
    check(!setGraphicsSettings(invalid) && graphicsSettings().frameRateLimit == 1000, "invalid cap changed settings");
    invalid = graphicsSettings(); invalid.renderHeight = 179;
    check(!setGraphicsSettings(invalid) && graphicsSettings().renderHeight == 720, "invalid height changed settings");
    invalid.renderHeight = 2161;
    check(!setGraphicsSettings(invalid) && graphicsSettings().renderHeight == 720, "oversized height changed settings");
    for (unsigned gamma : {0u,49u,151u,~0u}) {
        invalid = graphicsSettings(); invalid.gammaPercent = gamma;
        const auto saved = graphicsSettings();
        check(!setGraphicsSettings(invalid) && graphicsSettings() == saved, "invalid gamma changed settings");
    }
    for (unsigned brightness : {0u,49u,201u,~0u}) {
        invalid = graphicsSettings(); invalid.brightnessPercent = brightness;
        const auto saved = graphicsSettings();
        check(!setGraphicsSettings(invalid) && graphicsSettings() == saved, "invalid brightness changed settings");
    }
    setGraphicsSettings({});

    const uint32_t block = memory->allocate(0x10000);
    check(block != 0, "menu fixture allocation failed");
    struct Cleanup { uint32_t block; ~Cleanup() { setGraphicsSettings({}); setFieldOfViewSetting(0); memory->release(block); } } cleanup{block};
    auto* base = memory->base();
    std::memset(base + block, 0, 0x10000);
    const uint32_t button = block, parent = block + 0x1000, message = block + 0x2000;
    auto call = ctx;
    call.r3.u64 = button;
    sub_8239EC58(call, base); // Original CMWnd_CubeButton base constructor.
    call.r3.u64 = parent;
    sub_8244E650(call, base);
    memory->write32(button, 0x82074850); // The original factory's final vtable.
    memory->write32(button + 16, parent);
    memory->write32(button + 84, 1);
    check(memory->read32(0x82074850 + 0x14) == 0x823980A8 &&
          memory->read32(0x82074850 + 0x108) == 0x823981D8 &&
          memory->read32(0x82074850 + 0x114) == 0x8239ECD8,
          "CubeButton original render/press/message ABI changed");
    auto string = [&](uint32_t object, const char* text) {
        memory->write32(object, 0x82065568);
        memory->write32(object + 4, object + 16);
        PPC_STORE_U16(object + 16, 0x4002); // Refcounted byte string, retained by fixture.
        std::memcpy(base + object + 18, text, std::strlen(text) + 1);
    };
    string(block + 0x3000, "SCRIPT_PRESSED");
    string(block + 0x3100, "darkrecomp.bloom");
    call = ctx;
    call.r3.u64 = button; call.r4.u64 = block + 0x3200;
    call.r5.u64 = block + 0x3000; call.r6.u64 = block + 0x3100;
    sub_8239F0E0(call, base); // Original property parser must populate the binding.
    check(memory->read32(button + 348) == block + 0x3110, "SCRIPT_PRESSED did not bind to the expected CStr field");
    string(block + 0x3300, "ALWAYSPAINT");
    call = ctx;
    call.r3.u64 = button; call.r4.u64 = block + 0x3200;
    call.r5.u64 = block + 0x3300; call.r6.u64 = block + 0x3100;
    sub_82397F10(call, base);
    check((base[button + 396] & 128) != 0, "native row would not refresh its current label");
    auto key = [&](unsigned code) {
        memory->write32(message, 3);
        memory->write32(message + 8, code);
        auto dispatch = ctx;
        dispatch.r3.u64 = button; dispatch.r4.u64 = message;
        sub_8239ECD8(dispatch, base);
        check(dispatch.r1.u64 == ctx.r1.u64 && dispatch.r31.u64 == ctx.r31.u64,
              "menu dispatch corrupted guest stack/nonvolatile register");
    };
    takeDisplaySettingsSaveRequest();
    key(226);
    check(!graphicsSettings().bloom && takeDisplaySettingsSaveRequest(), "Left did not toggle/persist native bloom");
    key(226 | 0x8000);
    check(!graphicsSettings().bloom && !takeDisplaySettingsSaveRequest(), "release event changed bloom twice");
    key(227);
    check(graphicsSettings().bloom, "Right did not toggle native bloom");
    key(228); // Original confirm dispatch must reach the pressed callback.
    check(!graphicsSettings().bloom, "original Confirm did not activate native row");
    call = ctx; call.r3.u64 = button; call.r4.u64 = message;
    sub_823981D8(call, base);
    check(graphicsSettings().bloom && call.r3.u32 == 1, "mouse/pressed callback did not activate native row");
    memory->write32(button + 84, 0);
    key(226);
    check(graphicsSettings().bloom, "inactive row responded to left/right");
    // Bind the new row through the original property parser and input path.
    string(block + 0x3400, "darkrecomp.motionblur");
    call = ctx;
    call.r3.u64 = button; call.r4.u64 = block + 0x3200;
    call.r5.u64 = block + 0x3000; call.r6.u64 = block + 0x3400;
    sub_8239F0E0(call, base);
    memory->write32(button + 84, 1);
    takeDisplaySettingsSaveRequest();
    key(227);
    check(graphicsSettings().motionBlur && graphicsSettings().bloom && takeDisplaySettingsSaveRequest() &&
          videoSettingLabel("darkrecomp.motionblur").find("On") != std::string::npos,
          "native Motion Blur row did not enable/persist independently of bloom");
    key(227 | 0x8000);
    check(graphicsSettings().motionBlur && !takeDisplaySettingsSaveRequest(), "key release toggled motion blur twice");
    key(226);
    check(!graphicsSettings().motionBlur && takeDisplaySettingsSaveRequest() &&
          videoSettingLabel("darkrecomp.motionblur").find("Off") != std::string::npos,
          "native Motion Blur row did not disable/persist");
    string(block + 0x3500, "darkrecomp.antialiasing");
    call = ctx;
    call.r3.u64 = button; call.r4.u64 = block + 0x3200;
    call.r5.u64 = block + 0x3000; call.r6.u64 = block + 0x3500;
    sub_8239F0E0(call, base);
    const auto beforeAA = graphicsSettings();
    takeDisplaySettingsSaveRequest();
    key(227);
    auto expectedAA = beforeAA; expectedAA.antialiasing = true;
    check(graphicsSettings() == expectedAA && takeDisplaySettingsSaveRequest() &&
          videoSettingLabel("darkrecomp.antialiasing").find("FXAA") != std::string::npos,
          "native antialiasing row did not enable/persist FXAA independently");
    key(227 | 0x8000);
    check(graphicsSettings() == expectedAA && !takeDisplaySettingsSaveRequest(), "release toggled antialiasing twice");
    key(226);
    check(graphicsSettings() == beforeAA && takeDisplaySettingsSaveRequest() &&
          videoSettingLabel("darkrecomp.antialiasing").find("Off") != std::string::npos,
          "native antialiasing row did not disable/persist FXAA");
    key(228);
    check(graphicsSettings() == expectedAA, "Confirm did not enable FXAA");
    call = ctx; call.r3.u64 = button; call.r4.u64 = message;
    sub_823981D8(call, base);
    check(graphicsSettings() == beforeAA, "mouse/pressed callback did not disable FXAA");
    const auto before = graphicsSettings();
    while(takeDisplaySettingsSaveRequest()) {}
    string(block + 0x3600, "darkrecomp.gamma");
    call = ctx;
    call.r3.u64 = button; call.r4.u64 = block + 0x3200;
    call.r5.u64 = block + 0x3000; call.r6.u64 = block + 0x3600;
    sub_8239F0E0(call, base);
    key(226);
    auto darker = before; darker.gammaPercent = 95;
    check(graphicsSettings() == darker && takeDisplaySettingsSaveRequest() &&
          videoSettingLabel("darkrecomp.gamma").find("0.95") != std::string::npos,
          "gamma row did not darken independently through original input dispatch");
    key(227 | 0x8000);
    check(graphicsSettings() == darker && !takeDisplaySettingsSaveRequest(), "gamma changed on key release");
    key(227);
    check(graphicsSettings() == before && takeDisplaySettingsSaveRequest(), "gamma did not return to neutral");
    for (int i=0;i<32;++i) changeVideoSetting("darkrecomp.gamma",-1);
    check(graphicsSettings().gammaPercent==50,"gamma lower bound wrapped to a brighter value");
    for (int i=0;i<32;++i) changeVideoSetting("darkrecomp.gamma",1);
    check(graphicsSettings().gammaPercent==150,"gamma upper bound wrapped to a darker value");
    setGraphicsSettings(before);
    string(block + 0x3700, "darkrecomp.brightness");
    call = ctx;
    call.r3.u64 = button; call.r4.u64 = block + 0x3200;
    call.r5.u64 = block + 0x3000; call.r6.u64 = block + 0x3700;
    sub_8239F0E0(call, base);
    while(takeDisplaySettingsSaveRequest()) {}
    key(226);
    auto dimmer = before; dimmer.brightnessPercent = 95;
    check(graphicsSettings() == dimmer && takeDisplaySettingsSaveRequest() &&
          videoSettingLabel("darkrecomp.brightness").find("95%") != std::string::npos,
          "brightness row did not dim independently through original input dispatch");
    key(226 | 0x8000);
    check(graphicsSettings() == dimmer && !takeDisplaySettingsSaveRequest(), "brightness changed on key release");
    key(227);
    check(graphicsSettings() == before && takeDisplaySettingsSaveRequest(), "brightness did not return to neutral");
    key(228);
    auto brighter = before; brighter.brightnessPercent = 105;
    check(graphicsSettings() == brighter && takeDisplaySettingsSaveRequest(), "Confirm did not increase brightness");
    call = ctx; call.r3.u64 = button; call.r4.u64 = message;
    sub_823981D8(call, base);
    brighter.brightnessPercent = 110;
    check(graphicsSettings() == brighter && takeDisplaySettingsSaveRequest(), "Pressed callback did not increase brightness");
    for (int i=0;i<40;++i) changeVideoSetting("darkrecomp.brightness",-1);
    check(graphicsSettings().brightnessPercent==50,"brightness lower bound wrapped");
    for (int i=0;i<40;++i) changeVideoSetting("darkrecomp.brightness",1);
    check(graphicsSettings().brightnessPercent==200,"brightness upper bound wrapped");
    setGraphicsSettings(before);
    check(!changeVideoSetting("cg_prevmenu()", 1) && graphicsSettings() == before,
          "non-PC script was consumed by native settings");
    for (const auto action : {"darkrecomp.vsync", "darkrecomp.mode", "darkrecomp.fps", "darkrecomp.resolution"}) {
        check(changeVideoSetting(action, 1) && validGraphicsSettings(graphicsSettings()), "native option change failed");
        check(!videoSettingLabel(action).empty(), "native option has no label");
    }
    auto resolutionSelection = graphicsSettings(); resolutionSelection.renderHeight = 360;
    setGraphicsSettings(resolutionSelection);
    for (const auto height : {480u,720u,1080u,1440u,2160u,360u}) {
        changeVideoSetting("darkrecomp.resolution", 1);
        check(graphicsSettings().renderHeight == height, "resolution menu skipped a supported setting or failed to wrap");
        check(videoSettingLabel("darkrecomp.resolution").find(std::to_string(height) + "p") != std::string::npos,
              "resolution label does not match the selected height");
    }
    setFieldOfViewSetting(114.25f);
    check(videoSettingLabel("darkrecomp.fov").find("114.25") != std::string::npos, "menu rounded untouched FOV");
    changeVideoSetting("darkrecomp.fov", -1);
    check(fieldOfViewSetting() == 114, "FOV decrement failed");
    setFieldOfViewSetting(60);
    changeVideoSetting("darkrecomp.fov", -1);
    check(fieldOfViewSetting() == 0, "FOV Original is unreachable");
    changeVideoSetting("darkrecomp.fov", -1);
    check(fieldOfViewSetting() == 120, "FOV wrap failed");
    reportDisplaySettingsSave(false);
    check(videoSettingLabel("darkrecomp.bloom").find("Save failed") != std::string::npos, "save failure is invisible in menu");
    reportDisplaySettingsSave(true);
    // The engine gives small text two glyphs per cell. Dynamic values must
    // fit the authored eight-cell column without a font change or word wrap.
    for (const auto action : {"darkrecomp.fov", "darkrecomp.bloom", "darkrecomp.vsync",
                              "darkrecomp.fps", "darkrecomp.resolution", "darkrecomp.mode", "darkrecomp.motionblur", "darkrecomp.antialiasing", "darkrecomp.gamma", "darkrecomp.brightness"}) {
        for (int i = 0; i < 128; ++i) {
            for (bool saved : {true, false}) {
                reportDisplaySettingsSave(saved);
                const auto text = videoSettingLabel(action);
                check(text.starts_with("sc, ") && text.size() - 4 == 16 && text.starts_with("sc, < ") && text.ends_with(" >"),
                      "setting value changed the original button glyph width or font size");
            }
            changeVideoSetting(action, 1);
        }
    }
    reportDisplaySettingsSave(true);
    std::puts("Native video settings: shader flags, safe resolution, original button constructor/property/input ABI, all choices and save requests passed; no rendering performed.");
}
