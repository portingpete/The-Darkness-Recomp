#pragma once
#include "runtime/native/developer_player.h"
#include "runtime/native/developer_tools.h"
#include "runtime/native/movement.h"
#include "runtime/native/movement_settings.h"
#include "developer_player_lookup_tests.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace DeveloperPlayerMovementFixture {
inline thread_local uint32_t componentData = 0;
inline thread_local bool walking = false;
inline thread_local std::array<float, 3> movement{};
struct Captured {};
static PPC_FUNC(animationData) { ctx.r3.u64 = componentData; }
static PPC_FUNC(noActor) { ctx.r3.u64 = 0; }
static PPC_FUNC(captureCollision) {
    // The original control handler has built its movement vector here. World
    // collision requires a live scene, so stop at that service boundary; none
    // of the authored speeds, input normalization or basis math are replaced.
    movement = walking ? std::array{float(ctx.f27.f64), float(ctx.f26.f64), float(ctx.f22.f64)}
                       : std::array{float(ctx.f26.f64), float(ctx.f23.f64), float(ctx.f27.f64)};
    throw Captured{};
}
struct Dispatch {
    uint8_t* base;
    uint32_t address;
    PPCFunc* previous;
    Dispatch(uint8_t* b, uint32_t a, PPCFunc* callback)
        : base(b), address(a), previous(PPC_LOOKUP_FUNC(b, a)) { PPC_LOOKUP_FUNC(base, address) = callback; }
    ~Dispatch() { PPC_LOOKUP_FUNC(base, address) = previous; }
};
}

static void testDeveloperPlayer(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    using namespace DeveloperPlayerMovementFixture;
    const auto unavailable = updateDeveloperPlayer(ctx, nullptr, 0, 1, false, false);
    check(!unavailable.hasActivePlayer && !unavailable.applied,
          "Missing player cannot report developer controls applied");
    DeveloperPlayerLookupFixture::Fixture lookup;
    auto queryContext = ctx;
    lookup.prepare(queryContext);
    auto* base = memory->base();
    const auto block = memory->allocate(0x40000);
    check(block != 0, "Developer movement fixture allocation");
    struct Restore {
        PPCContext& ctx;
        uint32_t block;
        ~Restore() {
            setMovementStyle(MovementStyle::Original);
            updateDeveloperPlayer(ctx, nullptr, 0, 1, false, false);
            componentData = 0;
            memory->release(block);
        }
    } restore{ctx, block};
    std::memset(base + block, 0, 0x40000);
    const auto state = lookup.state, actor = lookup.actor, server = lookup.server;
    const auto table = block, animTable = block + 0x800, animData = block + 0xA00;
    const auto output = block + 0x1000, input = output + 0x20, angles = output + 0x40;
    const auto flags = output + 0x60, resultFlags = output + 0x70, hashName = output + 0x80;
    std::memcpy(base + table, base + 0x82080F80, 0x800);
    memory->write32(table + 56, 0x820CA090);
    memory->write32(table + 104, 0x820CA088);
    memory->write32(animTable + 48, 0x820CA098);
    Dispatch getAnimation(base, 0x820CA098, animationData);
    Dispatch collision(base, 0x820CA090, captureCollision);
    Dispatch absentActor(base, 0x820CA088, noActor);
    componentData = animData;
    memory->write32(state + 64, animTable);
    PPC_STORE_U16(state + 10386, 0xFFFF);
    memory->write32(actor + 364, 0);
    auto putFloat = [&](uint32_t address, float value) { memory->write32(address, std::bit_cast<uint32_t>(value)); };
    auto getFloat = [&](uint32_t address) { return std::bit_cast<float>(memory->read32(address)); };
    putFloat(state + 10020, 2); // Walking endpoint.
    putFloat(state + 10024, 8); // Original SPEED_FORWARD.
    putFloat(state + 10028, 6); // Original SPEED_SIDESTEP.
    putFloat(state + 10032, 3); // Original SPEED_UP.
    putFloat(state + 10036, 11); // Original SPEED_JUMP.
    putFloat(server + 412, 1.0f / 30);
    const std::array authored{memory->read32(state + 10020), memory->read32(state + 10024),
        memory->read32(state + 10028), memory->read32(state + 10032), memory->read32(state + 10036)};
    const auto elapsed = memory->read32(server + 412);
    for (const auto [address, opcode] : std::array{
            std::pair{0x82134474u, 0xC1410218u}, std::pair{0x82135018u, 0xEC000632u},
            std::pair{0x82135020u, 0xED0C6828u}, std::pair{0x82135B08u, 0xEDAD0632u},
            std::pair{0x82135C8Cu, 0xEDAD0632u}}) {
        check(memory->read32(address) == opcode, "Movement hook must match original horizontal speed consumption");
    }
    // Independent retail name hashing ties the consumed fields to the authored
    // movement keys; these constants also select the original setter branches.
    for (const auto [name, expected] : std::array{
            std::pair{"SPEED_FORWARD", 0x98E0F105u}, std::pair{"SPEED_SIDESTEP", 0xC4980B31u},
            std::pair{"SPEED_UP", 0x3DB59675u}, std::pair{"SPEED_JUMP", 0x81C8788Cu}}) {
        std::strcpy(reinterpret_cast<char*>(base + hashName), name);
        auto call = ctx; call.r3.u64 = hashName; call.r4.u64 = 0;
        sub_821F5290(call, base);
        check(call.r3.u32 == expected, "Original movement property hash proof");
    }
    auto activate = [&](float multiplier, bool enabled = true) {
        memory->write32(server, 0x82080F80);
        const auto result = updateDeveloperPlayer(queryContext, base, lookup.client, multiplier, false, enabled);
        check(result.hasActivePlayer && result.applied == enabled, "Player update must resolve original server player");
        memory->write32(server, table);
    };
    auto move = [&](unsigned posture, bool freeMovement, float forward, float side, float up = 0,
                    uint32_t movedActor = 0) {
        const auto controlledActor = movedActor ? movedActor : actor;
        walking = (posture & 15) == 0;
        memory->write32(controlledActor + 420, freeMovement ? 0x4000 : posture << 8);
        putFloat(input, forward); putFloat(input + 4, side); putFloat(input + 8, up);
        memory->write32(flags, 0); PPC_STORE_U16(resultFlags, 0);
        auto call = ctx; call.r1.u64 = block + 0x3F000;
        call.r3.u64 = output; call.r4.u64 = 1; call.r5.u64 = controlledActor; call.r6.u64 = 0;
        call.r7.u64 = server; call.r8.u64 = input; call.r9.u64 = angles; call.r10.u64 = 0;
        memory->write32(call.r1.u32 + 84, flags);
        memory->write32(call.r1.u32 + 92, 0);
        memory->write32(call.r1.u32 + 100, resultFlags);
        try {
            sub_82133DD8(call, base);
            check(freeMovement, "Ground movement fixture must reach the original world collision boundary");
            return std::array{getFloat(output), getFloat(output + 4), getFloat(output + 8)};
        } catch (const Captured&) {
            check(!freeMovement, "Free-movement original handler must return its displacement");
            return movement;
        }
    };
    auto closeEnough = [](float a, float b) { return std::isfinite(a) && std::abs(a - b) < 0.0001f; };
    auto twice = [&](const auto& before, const auto& after) {
        check(std::abs(before[0]) + std::abs(before[1]) > 0.001f,
              "Original movement fixture must produce a horizontal vector");
        check(closeEnough(after[0], before[0] * 2) && closeEnough(after[1], before[1] * 2),
              "Developer multiplier must double original movement without changing input or elapsed time");
    };
    // Selector0 is the normal ground path; selector5 uses its other authored
    // horizontal branch. High mode2 (0x2000) exercises the original half-input
    // crouch gate before the ground/walking interpolation.
    for (const unsigned posture : {0u, 5u, 0x20u}) {
        activate(1); const auto baseline = move(posture, false, 0.5f, 0.25f);
        activate(2); twice(baseline, move(posture, false, 0.5f, 0.25f));
        twice(baseline, move(posture, false, 0.5f, 0.25f)); // Never compounds per update/call.
        activate(1); const auto restored = move(posture, false, 0.5f, 0.25f);
        check(closeEnough(restored[0], baseline[0]) && closeEnough(restored[1], baseline[1]), "Defaults restore original ground/crouch movement");
    }
    activate(1); const auto baseline = move(0, true, 1, 0.5f, 1);
    activate(2); const auto accelerated = move(0, true, 1, 0.5f, 1);
    twice(baseline, accelerated);
    check(closeEnough(baseline[2], accelerated[2]), "Horizontal player speed must leave original vertical movement unchanged");
    memory->write32(lookup.cameraState + 64, animTable);
    PPC_STORE_U16(lookup.cameraState + 10386, 0xFFFF);
    memory->write32(lookup.cameraActor + 364, 0);
    for (unsigned i = 0; i < authored.size(); ++i)
        memory->write32(lookup.cameraState + 10020 + i * 4, authored[i]);
    const auto npcMovement = move(0, true, 1, 0.5f, 1, lookup.cameraActor);
    check(closeEnough(npcMovement[0], baseline[0]) && closeEnough(npcMovement[1], baseline[1]) && closeEnough(npcMovement[2], baseline[2]),
          "Actual original NPC movement must remain unchanged while player speed is enabled");
    resetDeveloperTools();
    const auto resetMovement = move(0, true, 1, 0.5f, 1);
    check(closeEnough(resetMovement[0], baseline[0]) && closeEnough(resetMovement[1], baseline[1]),
          "Reset must immediately clear applied player speed before the next developer update");
    activate(2);
    const auto ownedState = memory->read32(actor + 396);
    memory->write32(actor + 396, state + 0x3000);
    // The original field loads still address the fixture state, but the selected
    // actor's state identity has changed: stale ownership must be ignored.
    PPCRegister a{}, s{}, forward{}, side{};
    a.u64 = actor; s.u64 = state; forward.f64 = 8; side.f64 = 6;
    ApplyDeveloperHorizontalSpeedMidAsmHook(a, s, forward, side);
    check(forward.f64 == 8 && side.f64 == 6, "Changed actor state must reject stale developer speed ownership");
    memory->write32(actor + 396, ownedState);
    a.u64 = actor + 0x100; forward.f64 = 8; side.f64 = 6;
    ApplyDeveloperHorizontalSpeedMidAsmHook(a, s, forward, side);
    check(forward.f64 == 8 && side.f64 == 6, "Developer player speed cannot affect NPC movement");
    activate(2, false); const auto unused = move(0, true, 1, 0.5f, 1);
    check(closeEnough(unused[0], baseline[0]) && closeEnough(unused[1], baseline[1]), "Unused controls must preserve original player movement");
    for (unsigned i = 0; i < authored.size(); ++i)
        check(memory->read32(state + 10020 + i * 4) == authored[i], "Developer speed must leave saved/authored movement fields unchanged");
    check(memory->read32(server + 412) == elapsed, "Developer speed must leave world timing unchanged");

    // Exercise the actual translated wish-vector path. The collision oracle
    // above observes the new hook in the retail handler, not a model copy.
    setMovementStyle(MovementStyle::Original); activate(1, false);
    const auto scriptedOriginal = move(5, false, 1, 0.5f);
    const auto freeOriginal = move(0, true, 1, 0.5f, 1);
    for (const auto style : {MovementStyle::Alternate}) {
        setMovementStyle(style); activate(1, false);
        const auto forward = move(0, false, 1, 0);
        const auto modeOne = move(0x10, false, 1, 0);
        const auto diagonal = move(0, false, 1, 1);
        const float maximum = 198.f / 30.f;
        check(closeEnough(std::hypot(forward[0], forward[1]), maximum), "Preset wish speed in original handler");
        check(closeEnough(std::hypot(modeOne[0], modeOne[1]), maximum),
              "Ordinary retail control mode 1 must use the selected movement preset");
        const auto diagonalSpeed = std::hypot(diagonal[0], diagonal[1]);
        check(closeEnough(diagonalSpeed, maximum), "Alternate movement normalizes diagonals");
        const auto crouch = move(0x20, false, 1, 0);
        check(closeEnough(std::hypot(crouch[0], crouch[1]), maximum *
              0.5f), "Preset crouch input crop");
        const auto other = move(0, false, 1, 0, 0, lookup.cameraActor);
        check(closeEnough(std::hypot(other[0], other[1]), 8.f), "Preset must not change NPC/camera movement");
        const auto scripted = move(5, false, 1, 0.5f);
        const auto free = move(0, true, 1, 0.5f, 1);
        for (unsigned i = 0; i < 3; ++i)
            check(closeEnough(scripted[i], scriptedOriginal[i]) && closeEnough(free[i], freeOriginal[i]),
                  "Presets must retain scripted and free movement, including vertical speed");
        activate(2);
        const auto multiplied = move(0, false, 1, 0);
        check(closeEnough(std::hypot(multiplied[0], multiplied[1]), maximum * 2), "Developer speed composes once with movement preset");
    }
    // The three hook opcodes prove that wish, platform and completed delta
    // boundaries still match the original executable used by the generator.
    check(memory->read32(0x82135068) == 0x3D4082A4 &&
          memory->read32(0x821354CC) == 0x397F00E0 &&
          memory->read32(0x82135BF8) == 0x81750184, "Alternative movement hook instruction proof");
    setMovementStyle(MovementStyle::Alternate); activate(1, false);
    PPCRegister ar{}, st{}, sp{}, mode{}, command{}, f{}, sideInput{}, wx{}, wy{}, sr{}, grounded{}, px{}, py{};
    ar.u64 = actor; st.u64 = state; sp.u64 = block + 0x2000; command.u64 = input;
    sr.u64 = server; grounded.u64 = 1; f.f64 = 1;
    putFloat(sp.u32 + 528, 1); putFloat(sp.u32 + 532, 0);
    putFloat(sp.u32 + 544, 0); putFloat(sp.u32 + 548, 1);
    memory->write32(actor + 420, 0);
    px.f64 = 5; py.f64 = -3;
    putFloat(actor + 224, 5); putFloat(actor + 228, -3);
    const auto prepare = [&] {
        ShapeAlternativeMovementWishMidAsmHook(ar, st, sp, mode, command, f, sideInput, wx, wy);
        PrepareAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded, px, py);
    };
    putFloat(sp.u32 + 144, -100); putFloat(sp.u32 + 148, -101); putFloat(sp.u32 + 152, 7);
    prepare(); ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(closeEnough(getFloat(sp.u32 + 144), 198.f / 40.f) && closeEnough(getFloat(sp.u32 + 148), 0),
          "Rust ABI must convert support-relative UPS and cancel retail 40*dt impulse gain");
    check(getFloat(sp.u32 + 152) == 7, "Preset must not replace original vertical/jump delta");
    // A collision/prediction query for another actor may be interleaved before
    // the player's final correction. It must not erase the prepared command.
    putFloat(sp.u32 + 144, -100); prepare();
    PPCRegister otherActor{}, otherState{};
    otherActor.u64 = lookup.cameraActor; otherState.u64 = lookup.cameraState;
    ShapeAlternativeMovementWishMidAsmHook(otherActor, otherState, sp, mode, command, f, sideInput, wx, wy);
    ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(closeEnough(getFloat(sp.u32 + 144), 198.f / 40.f), "Unrelated actor query cannot interrupt player movement");
    // At 198 UPS, 40*198*dt braking stops the actor within one host tick.
    putFloat(actor + 224, 5.f + 198.f / 30.f); f.f64 = 0;
    prepare(); ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(closeEnough(getFloat(sp.u32 + 144), -198.f / 40.f),
          "Reference braking must survive host displacement conversion and downstream gain");
    putFloat(actor + 224, 5); f.f64 = 1;
    putFloat(sp.u32 + 144, -100); ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(getFloat(sp.u32 + 144) == -100, "Movement command applies once");
    prepare(); setMovementStyle(MovementStyle::Original); setMovementStyle(MovementStyle::Alternate);
    ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(getFloat(sp.u32 + 144) == -100, "Preset switches invalidate pending commands");
    putFloat(server + 412, 0); prepare(); ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(getFloat(sp.u32 + 144) == -100, "Paused time must not apply alternative acceleration");
    memory->write32(server + 412, elapsed);
    check(memory->read32(0x821357A4) == 0xA17F0170, "Jump impulse hook instruction proof");
    PPCRegister jump{}; jump.f64 = 11;
    ApplyAlternativeJumpMidAsmHook(ar, st, sr, jump);
    check(closeEnough(float(jump.f64), 245.f / 30.f), "Alternate jump uses the softer-gravity impulse");
    const auto oldGravityFlags = memory->read32(state + 388);
    const auto oldSpecialFlags = PPC_LOAD_U16(state + 9748);
    memory->write32(state + 388, 0); PPC_STORE_U16(state + 9748, 0);
    PPC_STORE_U16(flags, 1); PPC_STORE_U16(flags + 2, 0);
    const auto gravity = [&](uint32_t body) {
        auto call = ctx; call.r1.u64 = block + 0x3F000;
        call.r3.u64 = output; call.r5.u64 = body; call.r6.u64 = server; call.r7.u64 = flags;
        sub_821369E0(call, base);
        return getFloat(output + 8);
    };
    putFloat(actor + 232, 245.f / 30.f);
    check(closeEnough(gravity(actor), -800.f / 900.f), "Alternate gravity uses the softer 800-unit profile");
    putFloat(actor + 232, -100.f / 30.f);
    check(closeEnough(gravity(actor), -600.f / 900.f), "Descending gravity is softer than ascent");
    putFloat(actor + 232, 0);
    check(closeEnough(gravity(actor), -240.f / 900.f), "At the apex, gravity eases to the shared minimum");
    putFloat(actor + 232, 20.f / 30.f);
    check(closeEnough(gravity(actor), -520.f / 900.f), "Apex curve receives UPS rather than guest per-tick displacement");
    putFloat(actor + 232, 0);
    check(closeEnough(gravity(lookup.cameraActor), -800.f / 900.f), "Alternative gravity cannot affect another actor");
    setMovementStyle(MovementStyle::Original);
    check(closeEnough(gravity(actor), -800.f / 900.f), "Original gravity must remain retail");
    setMovementStyle(MovementStyle::Alternate);
    memory->write32(state + 388, oldGravityFlags); PPC_STORE_U16(state + 9748, oldSpecialFlags);
    // Independent motor-unit oracle: -155 UPS plus 600 gravity for half a
    // 30 Hz tick would become -165; the force must instead end at -160 UPS.
    const auto fallForce = block + 0x4000;
    putFloat(fallForce + 48, 7); putFloat(fallForce + 52, 11);
    putFloat(actor + 232, -155.f / 30.f); putFloat(fallForce + 56, -600.f / 900.f);
    limitAlternativeFall(actor, state, server, fallForce, 0.5f, false);
    check(closeEnough(getFloat(fallForce + 56), -1.f / 3.f), "Fall cap cancels fractional motor gain in native units");
    check(getFloat(fallForce + 48) == 7 && getFloat(fallForce + 52) == 11, "Fall cap cannot change horizontal force");
    putFloat(actor + 232, 100.f / 30.f); putFloat(fallForce + 56, -800.f / 900.f);
    limitAlternativeFall(actor, state, server, fallForce, 0.5f, false);
    check(closeEnough(getFloat(fallForce + 56), -800.f / 900.f), "Fall cap leaves ascent unchanged");
    putFloat(actor + 232, -155.f / 30.f); putFloat(fallForce + 56, -600.f / 900.f);
    limitAlternativeFall(actor, state, server, fallForce, 0.5f, true);
    check(closeEnough(getFloat(fallForce + 56), -600.f / 900.f), "Ground support retains original force");
    limitAlternativeFall(lookup.cameraActor, lookup.cameraState, server, fallForce, 0.5f, false);
    check(closeEnough(getFloat(fallForce + 56), -600.f / 900.f), "Fall limit cannot change another actor");
    setMovementStyle(MovementStyle::Original);
    limitAlternativeFall(actor, state, server, fallForce, 0.5f, false);
    check(closeEnough(getFloat(fallForce + 56), -600.f / 900.f), "Original fall remains untouched");
    setMovementStyle(MovementStyle::Alternate); putFloat(actor + 232, 0);
    // The live camera follows a prediction clone, whose state +32 callback
    // shares the original motor. Its timing world is the client, not server.
    const auto predictedActor = block + 0x10000, predictedState = block + 0x12000;
    memory->write32(predictedActor + 396, predictedState);
    PPC_STORE_U16(predictedActor + 368, PPC_LOAD_U32(lookup.client + 536));
    memory->write32(predictedState, memory->read32(state));
    memory->write32(predictedState + 16, predictedActor);
    memory->write32(predictedState + 56, lookup.client);
    putFloat(lookup.client + 412, 1.f / 60.f);
    PPCRegister predictionActor{}, predictionStateRegister{}, predictionWorld{};
    predictionActor.u64 = predictedActor; predictionStateRegister.u64 = predictedState;
    predictionWorld.u64 = lookup.client;
    const auto shapePrediction = [&] {
        wx.f64 = -100;
        ShapeAlternativeMovementWishMidAsmHook(predictionActor, predictionStateRegister, sp, mode,
                                             command, f, sideInput, wx, wy);
        return float(wx.f64);
    };
    check(shapePrediction() == -100, "Unscoped clone retains original handling");
    {
        MovementPredictionScope prediction(base, lookup.client, predictedActor, predictedState);
        check(closeEnough(shapePrediction(), 198.f / 60.f), "Local prediction uses client timestep and alternate speed");
        jump.f64 = -100;
        ApplyAlternativeJumpMidAsmHook(predictionActor, predictionStateRegister, predictionWorld, jump);
        check(closeEnough(float(jump.f64), 245.f / 60.f), "Local prediction uses the same jump impulse");
        putFloat(predictedActor + 232, -155.f / 60.f);
        putFloat(fallForce + 56, -600.f / 3600.f);
        limitAlternativeFall(predictedActor, predictedState, lookup.client, fallForce, 0.5f, false);
        check(closeEnough((getFloat(predictedActor + 232) + getFloat(fallForce + 56) * 0.5f) * 60, -160.f),
              "Prediction clone cannot outrun authoritative fall cap");
        {
            MovementPredictionScope unrelated(base, lookup.client, lookup.cameraActor, lookup.cameraState);
            check(shapePrediction() == -100, "Nested NPC callback cannot borrow local prediction eligibility");
        }
        check(closeEnough(shapePrediction(), 198.f / 60.f), "Nested prediction restores caller eligibility");
        setMovementStyle(MovementStyle::Original); setMovementStyle(MovementStyle::Alternate);
        check(shapePrediction() == -100, "Preset change invalidates prediction scope");
    }
    check(shapePrediction() == -100, "Prediction callback cannot leak clone eligibility");
    check(memory->read32(0x821DC2D0) == 0x48000068, "Post-head camera hook instruction proof");
    PPCRegister cameraClient{}, cameraActor{}, cameraState{}, cameraMatrix{};
    cameraClient.u64 = lookup.client; cameraActor.u64 = actor; cameraState.u64 = state;
    cameraMatrix.u64 = block + 0x3000;
    const auto oldId = PPC_LOAD_U16(actor + 368);
    PPC_STORE_U16(actor + 368, PPC_LOAD_U32(lookup.client + 536));
    const auto checkCamera = [&](bool expected, const char* message) {
        // Independent camera with forward +X and 30-degree head roll. A level
        // horizon must retain forward/pitch, eye position and matrix padding.
        const std::array<float, 16> pose{0, -0.8660254f, 0.5f, 0,
            0, 0.5f, 0.8660254f, 0, 1, 0, 0, 0, 7, 11, 35, 1};
        for (unsigned i = 0; i < pose.size(); ++i) putFloat(cameraMatrix.u32 + i * 4, pose[i]);
        StabilizeAlternativeMovementCameraMidAsmHook(cameraClient, cameraActor, cameraState, cameraMatrix);
        check(closeEnough(getFloat(cameraMatrix.u32 + 8), expected ? 0.f : pose[2]), message);
        for (unsigned i : {3u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u})
            check(getFloat(cameraMatrix.u32 + i * 4) == pose[i], "Camera stabilization retains forward axis, collision-resolved eye and padding");
        if (expected) {
            check(closeEnough(getFloat(cameraMatrix.u32 + 4), -1) &&
                  closeEnough(getFloat(cameraMatrix.u32 + 24), 1), "Camera horizon is level with unchanged handedness");
        }
    };
    checkCamera(true, "Local preset camera levels animated roll");
    memory->write32(actor + 420, 0x2000);
    checkCamera(true, "Crouch retains its original eye/collision while leveling roll");
    memory->write32(actor + 420, 0x4000);
    checkCamera(false, "Free movement camera retains original path");
    memory->write32(actor + 420, 0);
    PPC_STORE_U16(state + 8300, 1);
    checkCamera(false, "Scripted camera target retains original path");
    PPC_STORE_U16(state + 8300, 0);
    cameraActor.u64 = lookup.cameraActor; cameraState.u64 = lookup.cameraState;
    checkCamera(false, "Another camera actor cannot select preset camera");
    cameraActor.u64 = actor; cameraState.u64 = state;
    setMovementStyle(MovementStyle::Original);
    checkCamera(false, "Original retains animated camera");
    setMovementStyle(MovementStyle::Alternate);
    PPC_STORE_U16(actor + 368, oldId);
    // Sustained air control skips the ground support hook in retail. Exercise
    // the common completion hook directly, without calling Prepare.
    grounded.u64 = 0; putFloat(actor + 224, 0); putFloat(actor + 228, 0);
    ShapeAlternativeMovementWishMidAsmHook(ar, st, sp, mode, command, f, sideInput, wx, wy);
    ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(closeEnough(getFloat(sp.u32 + 144), 52.8f / 40.f) && closeEnough(getFloat(sp.u32 + 148), 0),
          "Sustained airborne branch must reach alternate air acceleration");
    putFloat(actor + 224, 198.f / 30.f); f.f64 = 0;
    ShapeAlternativeMovementWishMidAsmHook(ar, st, sp, mode, command, f, sideInput, wx, wy);
    ApplyAlternativeMovementMidAsmHook(ar, st, sr, sp, grounded);
    check(closeEnough(getFloat(sp.u32 + 144), 0) && getFloat(sp.u32 + 152) == 7,
          "Airborne neutral input must retain horizontal momentum and original vertical output");
    setMovementStyle(MovementStyle::Original); activate(1, false);
    check(closeEnough(std::hypot(move(0, false, 1, 0)[0], move(0, false, 1, 0)[1]), 8.f),
          "Selecting Original restores retail movement immediately");
    activate(1.5f);
    const auto fasterOriginal = move(0, false, 1, 0);
    check(closeEnough(std::hypot(fasterOriginal[0], fasterOriginal[1]), 12.f),
          "Developer speed slider scales Original horizontal movement");
    {
        MovementPredictionScope prediction(base, lookup.client, predictedActor, predictedState);
        check(closeEnough(movementHorizontalSpeedScale(predictedActor, predictedState), 1.5f),
              "Original speed slider also scales the scoped prediction clone");
        check(movementHorizontalSpeedScale(lookup.cameraActor, lookup.cameraState) == 1,
              "Speed slider cannot change another actor");
    }
    setMovementStyle(MovementStyle::Alternate); activate(1.5f);
    const auto fasterAlternate = move(0, false, 1, 0);
    check(closeEnough(std::hypot(fasterAlternate[0], fasterAlternate[1]), 297.f / 30.f),
          "Speed slider composes with alternate movement once");
    jump.f64 = 0; ApplyAlternativeJumpMidAsmHook(ar, st, sr, jump);
    check(closeEnough(float(jump.f64), 245.f / 30.f), "Horizontal speed slider does not change jump impulse");
    setMovementStyle(MovementStyle::Original); activate(1, false);
    memory->write32(server, 0x82080F80);
    puts("Developer player: original ground/crouch/free movement handler, vertical and NPC isolation, restoration and unchanged saved state passed.");
}
