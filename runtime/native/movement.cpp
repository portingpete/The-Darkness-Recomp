#include "movement.h"
#include "movement_settings.h"
#include "input.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace DarkRecomp::Native;
std::atomic<uint8_t*> activeBase{nullptr};
std::atomic<uint64_t> activePlayer{0};
std::atomic<float> speedMultiplier{1};
std::atomic<uint32_t> activeClient{0};
std::atomic<uint32_t> activeServer{0};
struct Velocity { float x, y; };
struct Vertical { float jump, gravity; };
extern "C" Velocity dark_movement_step(uint32_t, uint32_t, float, float, float, float, float, float);
extern "C" Vertical dark_movement_vertical(uint32_t);
extern "C" float dark_movement_gravity(uint32_t, float);
extern "C" float dark_movement_limit_fall(uint32_t, float);
struct MotorStep { uint32_t actor; float fraction; bool ordinary = false, grounded = false; };
thread_local MotorStep* activeMotor = nullptr;
struct Command {
    uint8_t* base = nullptr;
    uint32_t actor = 0, state = 0, stack = 0;
    uint64_t revision = 0;
    MovementStyle style = MovementStyle::Original;
    float x = 0, y = 0, maximum = 0, deltaX = 0, deltaY = 0;
    bool prepared = false;
    bool grounded = false;
    float fraction = 1;
};
thread_local Command pending;
struct ProbeCounts { uint64_t shaped = 0, prepared = 0, applied = 0, air = 0; };
thread_local ProbeCounts probeCounts;
struct PredictionStep {
    uint32_t actor = 0, state = 0, world = 0;
    uint64_t selected = 0, revision = 0;
};
thread_local PredictionStep prediction;
float readFloat(uint8_t* base, uint32_t address) { return std::bit_cast<float>(PPC_LOAD_U32(address)); }
bool selectedPlayer(uint32_t actor, uint32_t state) {
    const auto selected = activePlayer.load(std::memory_order_acquire);
    if (!selected || !actor || !state || !memory ||
        activeBase.load(std::memory_order_acquire) != memory->base()) return false;
    auto* base = memory->base();
    if (PPC_LOAD_U32(actor + 396) != state) return false;
    if (uint32_t(selected >> 32) == actor && uint32_t(selected) == state)
        return (PPC_LOAD_U32(actor + 364) & 0x00100000) == 0;
    return prediction.actor == actor && prediction.state == state && prediction.selected == selected &&
        prediction.world == activeClient.load(std::memory_order_acquire) &&
        prediction.revision == movementStyleRevision();
}
bool matches(const PPCRegister& actor, const PPCRegister& state, const PPCRegister& stack) {
    return pending.actor == actor.u32 && pending.state == state.u32 && pending.stack == stack.u32 &&
        pending.base == (memory ? memory->base() : nullptr) && pending.style == movementStyle() &&
        pending.revision == movementStyleRevision() && selectedPlayer(actor.u32, state.u32);
}
void prepareStep(float dt, float vx, float vy, bool grounded) {
    float wx = pending.x, wy = pending.y, maximum = pending.maximum;
    const auto next = dark_movement_step(unsigned(pending.style), grounded, vx, vy, wx, wy, maximum, dt * pending.fraction);
    // The motor integrates acceleration over its fractional tick. Cancel both
    // the 40*dt control gain and that fraction after solving at the actual dt.
    const float gain = 40.f * pending.fraction;
    pending.deltaX = (next.x - vx) / gain; pending.deltaY = (next.y - vy) / gain;
    pending.prepared = std::isfinite(pending.deltaX) && std::isfinite(pending.deltaY);
    pending.grounded = grounded;
    if (movementProbeEnabled() && pending.prepared) ++probeCounts.prepared;
}
}
namespace DarkRecomp::Native {
MovementPredictionScope::MovementPredictionScope(uint8_t* base, uint32_t client,
    uint32_t actor, uint32_t state) noexcept
    : actor_(prediction.actor), state_(prediction.state), world_(prediction.world),
      selected_(prediction.selected), revision_(prediction.revision) {
    prediction = {};
    const auto selected = activePlayer.load(std::memory_order_acquire);
    if ((movementStyle() == MovementStyle::Original && speedMultiplier.load(std::memory_order_acquire) == 1) || !selected || !memory ||
        base != memory->base() || base != activeBase.load(std::memory_order_acquire) ||
        !client || client != activeClient.load(std::memory_order_acquire) || !actor || !state ||
        actor == uint32_t(selected >> 32)) return;
    const auto id = PPC_LOAD_U32(client + 536);
    if (!id || PPC_LOAD_U16(actor + 368) != id ||
        PPC_LOAD_U16(uint32_t(selected >> 32) + 368) != id ||
        PPC_LOAD_U32(actor + 396) != state || PPC_LOAD_U32(state + 16) != actor ||
        PPC_LOAD_U32(state + 56) != client ||
        PPC_LOAD_U32(state) != PPC_LOAD_U32(uint32_t(selected))) return;
    prediction = {actor, state, client, selected, movementStyleRevision()};
}
MovementPredictionScope::~MovementPredictionScope() {
    prediction = {actor_, state_, world_, selected_, revision_};
}
float movementHorizontalSpeedScale(uint32_t actor, uint32_t state) noexcept {
    if (movementStyle() != MovementStyle::Original || !selectedPlayer(actor, state)) return 1;
    return speedMultiplier.load(std::memory_order_acquire);
}
bool movementProbeEnabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("DARK_MOVEMENT_PROBE");
        return value && value[0] == '1' && !value[1];
    }();
    return enabled;
}
void configureMovementPlayer(uint8_t* base, uint32_t actor, uint32_t state, float speed, bool noclip, uint32_t client, uint32_t server) noexcept {
    if (!memory || base != memory->base() || !actor || !state || noclip ||
        (movementStyle() == MovementStyle::Original && speed == 1 && !movementProbeEnabled()) ||
        !std::isfinite(speed) || speed < 0.25f || speed > 4) {
        activePlayer.store(0, std::memory_order_release);
        return;
    }
    // Regular updates must not briefly unpublish a valid player: another
    // thread could otherwise miss one motor/camera query and use retail feel.
    if (base != activeBase.load(std::memory_order_acquire) ||
        client != activeClient.load(std::memory_order_acquire) ||
        server != activeServer.load(std::memory_order_acquire)) activePlayer.store(0, std::memory_order_release);
    activeBase.store(base, std::memory_order_release);
    speedMultiplier.store(speed, std::memory_order_release);
    activeClient.store(client, std::memory_order_release);
    activeServer.store(server, std::memory_order_release);
    activePlayer.store((uint64_t(actor) << 32) | state, std::memory_order_release);
    if (movementProbeEnabled()) {
        thread_local uint64_t previous = 0;
        const auto pair = (uint64_t(actor) << 32) | state;
        if (pair != previous) {
            previous = pair;
            std::fprintf(stderr, "[MovementProbe] configured style=%u actor=%08X state=%08X\n", unsigned(movementStyle()), actor, state);
        }
    }
}

void limitAlternativeFall(uint32_t actor, uint32_t state, uint32_t server,
    uint32_t force, float fraction, bool grounded) noexcept {
    const auto style = movementStyle();
    if (grounded || style == MovementStyle::Original || !force || !server || !selectedPlayer(actor, state)) return;
    auto* base = memory->base();
    const auto flags = PPC_LOAD_U32(actor + 420);
    if (((flags >> 8) & 15) != 0 || ((flags >> 12) & 15) > 2 ||
        (PPC_LOAD_U32(actor + 364) & 0x20000000) || (PPC_LOAD_U16(state + 9748) & 8)) return;
    const float dt = readFloat(base, server + 412);
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f || !std::isfinite(fraction) ||
        fraction <= 0 || fraction > 3 || dt * fraction > 0.1f) return;
    const float velocity = readFloat(base, actor + 232), acceleration = readFloat(base, force + 56);
    if (!std::isfinite(velocity) || !std::isfinite(acceleration)) return;
    // The outer motor commits v += a*fraction in displacement-per-tick units.
    // Limit that complete result, after gravity and the original control work.
    const float next = (velocity + acceleration * fraction) / dt;
    const float limited = dark_movement_limit_fall(unsigned(style), next);
    if (limited != next)
        PPC_STORE_U32(force + 56, std::bit_cast<uint32_t>((limited * dt - velocity) / fraction));
}
}

// Ground wish velocity is complete here; original collision queries follow.
// The normal posture and its crouch mode are eligible. Scripted postures,
// ladders, swimming, flying and noclip retain their original handlers.
void ShapeAlternativeMovementWishMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& stack, PPCRegister& mode, PPCRegister& command,
    PPCRegister& forward, PPCRegister& side, PPCRegister& x, PPCRegister& y) {
    const auto style = movementStyle();
    if (style == MovementStyle::Original || !selectedPlayer(actor.u32, state.u32)) return;
    // An unrelated actor query must not erase an in-progress player command.
    pending = {};
    auto* base = memory->base();
    // Retail treats 0 and 1 as ordinary movement, 2 as crouch, 3 as
    // suppressed input and 4 as free movement. Do not exclude mode 1.
    if (((PPC_LOAD_U32(actor.u32 + 420) >> 8) & 15) != 0 || mode.u32 > 2) return;
    // These special states also enter the retail airborne branch. Preserve
    // them rather than treating every route through that branch as a jump.
    if ((PPC_LOAD_U32(actor.u32 + 364) & 0x20000000) || (PPC_LOAD_U16(state.u32 + 9748) & 8)) return;
    const auto server = prediction.actor == actor.u32 ? prediction.world : activeServer.load(std::memory_order_acquire);
    if (!server) return;
    const float dt = readFloat(base, server + 412);
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f) return;
    float f = float(forward.f64), s = float(side.f64);
    const float crouch = mode.u32 == 2 ? 0.5f : 1.f;
    const auto modifiers = nativeInput().movementModifiers();
    // World units/second: slower traversal while retaining the quick response.
    const float referenceSpeed = (modifiers & 1) ? 99.f : 198.f;
    const float maximum = referenceSpeed * speedMultiplier.load(std::memory_order_acquire);
    float wx = maximum * (f * readFloat(base, stack.u32 + 528) + s * readFloat(base, stack.u32 + 544));
    float wy = maximum * (f * readFloat(base, stack.u32 + 532) + s * readFloat(base, stack.u32 + 548));
    const float length = std::hypot(wx, wy), cap = maximum * crouch;
    if (!std::isfinite(length) || !std::isfinite(cap) || cap <= 0) return;
    if (length > cap) { wx *= cap / length; wy *= cap / length; }
    float fraction = 1;
    if (activeMotor && activeMotor->actor == actor.u32) {
        fraction = activeMotor->fraction;
        if (!std::isfinite(fraction) || fraction <= 0 || fraction > 3 || dt * fraction > 0.1f) {
            return;
        }
    }
    // The retail support/vertical calculation expects displacement per tick.
    x.f64 = wx * dt; y.f64 = wy * dt;
    pending = {base, actor.u32, state.u32, stack.u32, movementStyleRevision(), style, wx, wy, cap};
    pending.fraction = fraction;
    if (movementProbeEnabled()) ++probeCounts.shaped;
}

// At 821354CC the original support/platform velocity has been resolved.
// Read the actual actor velocity, so walls, knockback and load/respawn cannot
// leave an independent host-side momentum accumulator behind.
void PrepareAlternativeMovementMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& server, PPCRegister& stack, PPCRegister& grounded,
    PPCRegister& platformX, PPCRegister& platformY) {
    if (!matches(actor, state, stack)) return;
    auto* base = memory->base();
    const float dt = readFloat(base, server.u32 + 412);
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f) return;
    const float vx = (readFloat(base, actor.u32 + 224) - float(platformX.f64)) / dt;
    const float vy = (readFloat(base, actor.u32 + 228) - float(platformY.f64)) / dt;
    prepareStep(dt, vx, vy, grounded.u32 != 0);
}

// Both normal branches converge at 82135BF8 after their jump/vertical work.
// After three unsupported ticks the air branch bypasses 821354CC entirely.
void ApplyAlternativeMovementMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& server,
    PPCRegister& stack, PPCRegister& grounded) {
    if (!matches(actor, state, stack)) return;
    auto* base = memory->base();
    if (!pending.prepared) {
        if (grounded.u32 != 0) return;
        const float dt = readFloat(base, server.u32 + 412);
        if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f) return;
        prepareStep(dt, readFloat(base, actor.u32 + 224) / dt, readFloat(base, actor.u32 + 228) / dt, false);
    }
    if (!pending.prepared) return;
    PPC_STORE_U32(stack.u32 + 144, std::bit_cast<uint32_t>(pending.deltaX));
    PPC_STORE_U32(stack.u32 + 148, std::bit_cast<uint32_t>(pending.deltaY));
    if (movementProbeEnabled()) { ++probeCounts.applied; if (!pending.grounded) ++probeCounts.air; }
    if (activeMotor && activeMotor->actor == actor.u32) {
        activeMotor->ordinary = true;
        activeMotor->grounded = pending.grounded;
    }
    pending = {};
}

// The live player-state vtable's +32 callback runs both server input and local
// prediction. A scoped clone must use the same wish/jump/gravity/fall rules;
// otherwise the rendered player falls ahead of the capped authoritative body.
extern "C" PPC_FUNC(__imp__sub_821C9100);
PPC_FUNC(sub_821C9100) {
    const auto state = ctx.r3.u32, world = ctx.r4.u32, actor = ctx.r5.u32;
    MovementPredictionScope scope(base, world, actor, state);
    if (movementProbeEnabled() && prediction.actor) {
        thread_local unsigned samples = 0;
        if (samples++ < 24) std::fprintf(stderr,
            "[MovementPrediction] actor=%08X state=%08X client=%08X sameController=1\n", actor, state, world);
    }
    __imp__sub_821C9100(ctx, base);
}

// The outer motor can integrate only a fraction of a server tick. Record the
// callback's time arguments rather than assuming every force query is a tick.
extern "C" PPC_FUNC(__imp__sub_82136B10);
PPC_FUNC(sub_82136B10) {
    const auto actor = ctx.r3.u32, server = ctx.r5.u32, dispatch = ctx.r6.u32, output = ctx.r9.u32;
    const double start = ctx.f1.f64, duration = ctx.f2.f64;
    MotorStep step{actor, float(duration)};
    auto* previous = activeMotor;
    struct RestoreMotor {
        MotorStep* previous;
        Command command;
        ~RestoreMotor() { activeMotor = previous; pending = command; }
    } restore{previous, pending};
    if (dispatch == 0 && movementStyle() != MovementStyle::Original &&
        selectedPlayer(actor, PPC_LOAD_U32(actor + 396))) {
        activeMotor = &step;
        pending = {};
    }
    __imp__sub_82136B10(ctx, base);
    if (dispatch == 0 && step.ordinary)
        limitAlternativeFall(actor, PPC_LOAD_U32(actor + 396), server, output, step.fraction, step.grounded);
    if (!movementProbeEnabled() || dispatch != 0 || !output) return;
    const auto selected = activePlayer.load(std::memory_order_acquire);
    if (!selected || uint32_t(selected >> 32) != actor) return;
    const float vx = readFloat(base, actor + 224), vy = readFloat(base, actor + 228);
    const float ax = readFloat(base, output + 48), ay = readFloat(base, output + 52);
    if (vx == 0 && vy == 0 && ax == 0 && ay == 0) return;
    thread_local unsigned count = 0;
    if (count++ >= 5000) return;
    std::fprintf(stderr, "[MovementMotor] ms=%llu actor=%08X start=%.9g duration=%.9g dt=%.9g vx=%.9g vy=%.9g ax=%.9g ay=%.9g\n",
        static_cast<unsigned long long>(GetTickCount64()), actor, start, duration, readFloat(base, server + 412), vx, vy, ax, ay);
}

void ApplyAlternativeJumpMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& server, PPCRegister& impulse) {
    const auto style = movementStyle();
    if (style == MovementStyle::Original || !selectedPlayer(actor.u32, state.u32)) return;
    auto* base = memory->base();
    const auto flags = PPC_LOAD_U32(actor.u32 + 420);
    if (((flags >> 8) & 15) != 0 || ((flags >> 12) & 15) > 2) return;
    const float dt = readFloat(base, server.u32 + 412);
    if (std::isfinite(dt) && dt > 0 && dt <= 0.1f) impulse.f64 = dark_movement_vertical(unsigned(style)).jump * dt;
}

extern "C" PPC_FUNC(__imp__sub_821369E0);
PPC_FUNC(sub_821369E0) {
    const auto actor = ctx.r5.u32, output = ctx.r3.u32, server = ctx.r6.u32;
    __imp__sub_821369E0(ctx, base);
    const auto style = movementStyle();
    if (style == MovementStyle::Original) return;
    const auto state = PPC_LOAD_U32(actor + 396);
    if (!selectedPlayer(actor, state)) return;
    const auto flags = PPC_LOAD_U32(actor + 420);
    if (((flags >> 8) & 15) != 0 || ((flags >> 12) & 15) > 2 ||
        (PPC_LOAD_U32(actor + 364) & 0x20000000) || (PPC_LOAD_U16(state + 9748) & 8)) return;
    const float gravity = readFloat(base, output + 8);
    if (gravity >= 0 || !std::isfinite(gravity) || !server) return;
    const float dt = readFloat(base, server + 412);
    if (!std::isfinite(dt) || dt <= 0 || dt > 0.1f) return;
    // The guest stores displacement/tick, while the apex curve uses UPS.
    // Ground support/takeoff keeps the original upward profile.
    const bool grounded = activeMotor && activeMotor->actor == actor && activeMotor->grounded;
    const float tuned = grounded ? dark_movement_vertical(unsigned(style)).gravity :
        dark_movement_gravity(unsigned(style), readFloat(base, actor + 232) / dt);
    PPC_STORE_U32(output + 8, std::bit_cast<uint32_t>(gravity * (tuned / 800.f)));
}

// Dense, opt-in camera oracle. Observe the completed retail camera rather than
// the sparse post-head hook: the client can render a different actor instance
// from the authoritative motor. QPC shares FrameTimeline's clock domain.
extern "C" PPC_FUNC(__imp__sub_821DC078);
PPC_FUNC(sub_821DC078) {
    const auto client = ctx.r3.u32, matrix = ctx.r4.u32, actor = ctx.r5.u32;
    const double cameraTime = ctx.f1.f64;
    __imp__sub_821DC078(ctx, base);
    static const bool enabled = [] {
        const auto* value = std::getenv("DARK_MOVEMENT_CAMERA_PROBE");
        return value && value[0] == '1' && !value[1];
    }();
    if (!enabled || !client || !actor || !matrix ||
        client != activeClient.load(std::memory_order_acquire) ||
        PPC_LOAD_U16(actor + 368) != PPC_LOAD_U32(client + 536)) return;
    thread_local unsigned samples = 0;
    if (samples++ >= 12000) return;
    const auto savedMath = _mm_getcsr();
    struct RestoreMath { unsigned saved; ~RestoreMath() { _mm_setcsr(saved); } } restore{savedMath};
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    const auto selected = activePlayer.load(std::memory_order_acquire);
    const auto authoritative = uint32_t(selected >> 32);
    const auto state = PPC_LOAD_U32(actor + 396), flags = PPC_LOAD_U32(actor + 420);
    const auto server = activeServer.load(std::memory_order_acquire);
    const auto at = [&](uint32_t object, unsigned offset) {
        return object ? readFloat(base, object + offset) : 0.f;
    };
    std::fprintf(stderr,
        "[MovementView] qpc=%llu hz=%llu style=%u client=%08X actor=%08X state=%08X auth=%08X "
        "posture=%u mode=%u cameraTime=%.9g dt=%.9g root=%.9g,%.9g,%.9g "
        "velocity=%.9g,%.9g,%.9g authRoot=%.9g,%.9g,%.9g authVelocity=%.9g,%.9g,%.9g "
        "eye=%.9g,%.9g,%.9g forward=%.9g,%.9g,%.9g right=%.9g,%.9g,%.9g\n",
        static_cast<unsigned long long>(now.QuadPart), static_cast<unsigned long long>(frequency.QuadPart),
        unsigned(movementStyle()), client, actor, state, authoritative, (flags >> 8) & 15, (flags >> 12) & 15,
        cameraTime, at(server, 412), at(actor, 128), at(actor, 132), at(actor, 136),
        at(actor, 224), at(actor, 228), at(actor, 232), at(authoritative, 128), at(authoritative, 132),
        at(authoritative, 136), at(authoritative, 224), at(authoritative, 228), at(authoritative, 232),
        at(matrix, 48), at(matrix, 52), at(matrix, 56), at(matrix, 32), at(matrix, 36), at(matrix, 40),
        at(matrix, 0), at(matrix, 4), at(matrix, 8));
}

// Keep the complete head/eye pose and camera collision calculation. Level only
// the horizon after the ordinary head-camera helper, before weapon effects.
void StabilizeAlternativeMovementCameraMidAsmHook(PPCRegister& client, PPCRegister& actor,
    PPCRegister& state, PPCRegister& matrix) {
    if (movementStyle() == MovementStyle::Original || !memory || !activePlayer.load(std::memory_order_acquire) ||
        activeBase.load(std::memory_order_acquire) != memory->base() ||
        !client.u32 || client.u32 != activeClient.load(std::memory_order_acquire) || !actor.u32 || !state.u32 || !matrix.u32) return;
    auto* base = memory->base();
    if (PPC_LOAD_U32(actor.u32 + 396) != state.u32 ||
        PPC_LOAD_U16(actor.u32 + 368) != PPC_LOAD_U32(client.u32 + 536)) return;
    const auto flags = PPC_LOAD_U32(actor.u32 + 420);
    if (((flags >> 8) & 15) != 0 || ((flags >> 12) & 15) > 2 ||
        (PPC_LOAD_U32(actor.u32 + 364) & 0x24100000) ||
        (PPC_LOAD_U32(state.u32 + 10240) & 0x00C00000) || PPC_LOAD_U16(state.u32 + 8300) ||
        (PPC_LOAD_U16(state.u32 + 10302) && (PPC_LOAD_U8(state.u32 + 10300) & 8))) return;
    const float fx = readFloat(base, matrix.u32 + 32), fy = readFloat(base, matrix.u32 + 36), fz = readFloat(base, matrix.u32 + 40);
    const float horizontal = std::hypot(fx, fy), length = std::hypot(horizontal, fz);
    if (!std::isfinite(length) || length < 0.5f || length > 1.5f || horizontal < 0.0001f) return;
    float rx = fy / horizontal, ry = -fx / horizontal;
    const float alignment = rx * readFloat(base, matrix.u32) + ry * readFloat(base, matrix.u32 + 4);
    if (!std::isfinite(alignment)) return;
    if (alignment < 0) { rx = -rx; ry = -ry; }
    float ux = -ry * fz / length, uy = rx * fz / length, uz = (ry * fx - rx * fy) / length;
    const float upAlignment = ux * readFloat(base, matrix.u32 + 16) + uy * readFloat(base, matrix.u32 + 20) + uz * readFloat(base, matrix.u32 + 24);
    if (!std::isfinite(upAlignment)) return;
    if (upAlignment < 0) { ux = -ux; uy = -uy; uz = -uz; }
    const auto store = [&](unsigned offset, float value) { PPC_STORE_U32(matrix.u32 + offset, std::bit_cast<uint32_t>(value)); };
    store(0, rx); store(4, ry); store(8, 0);
    store(16, ux); store(20, uy); store(24, uz);
    if (movementProbeEnabled()) {
        thread_local unsigned samples = 0;
        if (samples++ % 60 == 0) std::fprintf(stderr,
            "[MovementCamera] actor=%08X state=%08X mode=%u levelHorizon=1 eye=%.6g,%.6g,%.6g\n",
            actor.u32, state.u32, (flags >> 12) & 15,
            readFloat(base, matrix.u32 + 48), readFloat(base, matrix.u32 + 52), readFloat(base, matrix.u32 + 56));
    }
}

// Opt-in live oracle. Observe the real control handler and the actor's next
// collision-resolved position; fixture wish vectors alone cannot prove motion.
extern "C" PPC_FUNC(__imp__sub_82133DD8);
PPC_FUNC(sub_82133DD8) {
    if (!movementProbeEnabled()) { __imp__sub_82133DD8(ctx, base); return; }
    const auto actor = ctx.r5.u32, server = ctx.r7.u32, input = ctx.r8.u32, output = ctx.r3.u32;
    const auto style = movementStyle();
    __imp__sub_82133DD8(ctx, base);
    const auto savedMath = _mm_getcsr();
    struct RestoreMath { unsigned saved; ~RestoreMath() { _mm_setcsr(saved); } } restore{savedMath};
    thread_local unsigned unmatched = 0;
    const auto selected = activePlayer.load(std::memory_order_acquire);
    const auto state = PPC_LOAD_U32(actor + 396);
    const bool matched = selected && uint32_t(selected >> 32) == actor && uint32_t(selected) == state;
    if (!matched && unmatched++ >= 20) return;
    if (!state) return;
    const auto flags = PPC_LOAD_U32(actor + 420);
    thread_local unsigned samples = 0;
    const auto client = activeClient.load(std::memory_order_acquire);
    if (matched && samples++ % 300 == 0 && client) {
        const auto vp = client + 2112;
        std::fprintf(stderr, "[MovementScale] actor=%08X client=%08X forward=%.6g walk=%.6g side=%.6g jump=%.6g "
            "actorZ=%.6g vp0T=%.6g,%.6g,%.6g vp1T=%.6g,%.6g,%.6g vp2T=%.6g,%.6g,%.6g\n",
            actor, client, readFloat(base, state + 10024), readFloat(base, state + 10020),
            readFloat(base, state + 10028), readFloat(base, state + 10036), readFloat(base, actor + 136),
            readFloat(base, vp + 48), readFloat(base, vp + 52), readFloat(base, vp + 56),
            readFloat(base, vp + 112), readFloat(base, vp + 116), readFloat(base, vp + 120),
            readFloat(base, vp + 176), readFloat(base, vp + 180), readFloat(base, vp + 184));
    }
    std::fprintf(stderr, "[MovementTick] ms=%llu style=%u actor=%08X state=%08X selected=%u expected=%08X posture=%u mode=%u "
        "px=%.6g py=%.6g pz=%.6g vx=%.6g vy=%.6g vz=%.6g inputF=%.6g inputS=%.6g dt=%.6g "
        "outX=%.6g outY=%.6g outZ=%.6g shaped=%llu prepared=%llu applied=%llu air=%llu\n",
        static_cast<unsigned long long>(GetTickCount64()), unsigned(style), actor, state, unsigned(matched), uint32_t(selected >> 32),
        (flags >> 8) & 15, (flags >> 12) & 15,
        readFloat(base, actor + 128), readFloat(base, actor + 132), readFloat(base, actor + 136),
        readFloat(base, actor + 224), readFloat(base, actor + 228), readFloat(base, actor + 232),
        readFloat(base, input), readFloat(base, input + 4), readFloat(base, server + 412),
        readFloat(base, output), readFloat(base, output + 4), readFloat(base, output + 8),
        static_cast<unsigned long long>(probeCounts.shaped), static_cast<unsigned long long>(probeCounts.prepared),
        static_cast<unsigned long long>(probeCounts.applied), static_cast<unsigned long long>(probeCounts.air));
}
