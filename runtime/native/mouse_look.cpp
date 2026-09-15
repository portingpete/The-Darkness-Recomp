#include "mouse_look.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace DarkRecomp::Native;
struct MouseCommand {
    uint8_t* base;
    uint32_t client;
    MouseLookPacket packet;
    bool reached = false;
};
thread_local MouseCommand* activeMouseCommand = nullptr;
struct ScopedMouseCommand {
    MouseCommand* previous = activeMouseCommand;
    explicit ScopedMouseCommand(MouseCommand& command) { activeMouseCommand = &command; }
    ~ScopedMouseCommand() { activeMouseCommand = previous; }
};
struct MouseStream {
    uint8_t* base = nullptr;
    uint32_t client = 0;
    uint64_t epoch = 0;
    MouseLookQuantizer quantizer;
};
thread_local MouseStream stream;
bool probeEnabled() {
    static const bool enabled = [] {
        const char* setting = std::getenv("DARK_MOUSE_LOOK_PROBE");
        return setting && setting[0] == '1' && setting[1] == '\0';
    }();
    return enabled;
}
struct ScopedMathMode {
    unsigned saved = _mm_getcsr();
    ScopedMathMode() { _mm_setcsr(saved & ~(_MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK)); }
    ~ScopedMathMode() { _mm_setcsr(saved); }
};
}

// The original relative-look function has already selected its camera route and
// flushed pending control flags here. Override only our scoped mouse command,
// before its zero test and original type-2 construction/enqueue. All game,
// controller and script calls retain the original sensitivity calculation.
void ApplyNativeMouseLookMidAsmHook(PPCRegister& client, PPCRegister& stack,
                                   PPCRegister& pitch, PPCRegister& yaw) {
    auto* command = activeMouseCommand;
    if (!command || command->client != client.u32 || !DarkRecomp::Native::memory ||
        command->base != DarkRecomp::Native::memory->base()) return;
    auto* base = command->base;
    pitch.f64 = command->packet.pitch;
    yaw.f64 = command->packet.yaw;
    PPC_STORE_U32(stack.u32 + 92, std::bit_cast<uint32_t>(float(pitch.f64)));
    PPC_STORE_U32(stack.u32 + 96, std::bit_cast<uint32_t>(float(yaw.f64)));
    command->reached = true;
}

extern "C" PPC_FUNC(__imp__sub_823FC650);
extern "C" PPC_FUNC(__imp__sub_823FCF68);
PPC_FUNC(sub_823FC650) {
    const uint32_t client = ctx.r3.u32, caller = uint32_t(ctx.lr);
    __imp__sub_823FC650(ctx, base);
    // This periodic call is independent of changed XInput packets and of the
    // original function's neutral-stick early-out. Never consume for another
    // client/caller (for example a menu-only or diagnostic invocation).
    if (caller != 0x823FC9A0 || !DarkRecomp::Native::memory ||
        base != DarkRecomp::Native::memory->base() || !client ||
        uint64_t(client) + 8844 > PPC_MEMORY_SIZE) return;
    const uint32_t vtable = PPC_LOAD_U32(client);
    if (vtable != 0x820807E0 && vtable != 0x82081BF0) return;
    const auto delta = DarkRecomp::Native::nativeInput().consumeMouseLook();
    if (stream.base != base || stream.client != client || stream.epoch != delta.epoch) {
        stream.quantizer.reset();
        stream.base = base; stream.client = client; stream.epoch = delta.epoch;
    }
    // Mode 2 routes relative look into an authored camera matrix before the
    // packet hook. Never feed placeholder arguments into that separate path.
    // The original update marks an active GUI (7360) in flag 0x40; the
    // command enqueue path rejects flag 0x20. Clear fractions even on an empty
    // tick so opening a menu cannot carry old motion into resumed gameplay.
    if ((PPC_LOAD_U8(client + 8816) & 6) != 0 ||
        (PPC_LOAD_U32(client + 516) & 0x60) != 0 || PPC_LOAD_U32(client + 7360) != 0) {
        stream.quantizer.reset();
        return;
    }
    if (!delta.x && !delta.y) return;
    const ScopedMathMode mathMode;
    // The sign of the original vertical sensitivity carries the invert option;
    // its magnitude must not reintroduce controller speed scaling.
    const float vertical = std::bit_cast<float>(PPC_LOAD_U32(client + 8840));
    const auto packets = stream.quantizer.convert(delta, vertical < 0);
    unsigned submitted = 0;
    int64_t pitchUnits = 0, yawUnits = 0;
    for (unsigned i = 0; i < packets.count; ++i) {
        MouseCommand command{base, client, packets.packets[i]};
        const ScopedMouseCommand scope(command);
        // Separate context preserves the original periodic call's returned
        // registers. The guest relative-look function owns its normal stack.
        auto call = ctx;
        call.r3.u64 = client;
        call.f1.f64 = call.f2.f64 = 1;
        __imp__sub_823FCF68(call, base);
        if (!command.reached || call.r3.u32 == 0) { stream.quantizer.reset(); break; }
        ++submitted;
        pitchUnits += command.packet.pitch;
        yawUnits += command.packet.yaw;
    }
    if (probeEnabled()) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 2048)
            std::fprintf(stderr, "[MouseLook] client=%08X dx=%lld dy=%lld sensitivity=%.6g invert=%u packets=%u submitted=%u rejected=%u pitchUnits=%lld yawUnits=%lld\n",
                client, static_cast<long long>(delta.x), static_cast<long long>(delta.y),
                delta.sensitivity, vertical < 0, packets.count, submitted, packets.rejected,
                static_cast<long long>(pitchUnits), static_cast<long long>(yawUnits));
    }
}
