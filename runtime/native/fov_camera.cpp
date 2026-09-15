#include "fov_settings.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

// The hooks below observe player camera construction, never the shared matrix
// builder. Keep the portable settings/math in fov_settings.cpp independent.
namespace {
using namespace DarkRecomp::Native;
constexpr uint32_t kBaseFovOffset = 7160;
constexpr uint32_t kViewportFovOffset = 0x104;
constexpr uint32_t kReferenceAspectOffset = 0x108;
constexpr uint32_t kModClientVtable = 0x820807E0;
constexpr uint32_t kP6ClientVtable = 0x82081BF0;
constexpr unsigned kProbeLimit = 128;

struct Baseline { float degrees; bool fromProperty; };
std::mutex baselineMutex;
std::unordered_map<uint32_t, Baseline> baselines;
uint8_t* baselineMemory = nullptr;

bool validSpan(uint32_t address, uint32_t size) {
    return address != 0 && uint64_t(address) + size <= PPC_MEMORY_SIZE;
}
uint32_t readWord(uint8_t* base, uint32_t address) {
    return PPC_LOAD_U32(address);
}
float readFloat(uint8_t* base, uint32_t address) {
    return std::bit_cast<float>(readWord(base, address));
}
bool probeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_FOV_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

// Native math must not change the guest's SSE rounding/flush/exception state.
struct ScopedMathMode {
    unsigned saved = _mm_getcsr();
    ScopedMathMode() {
        _mm_setcsr(saved & ~(_MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK));
    }
    ~ScopedMathMode() { _mm_setcsr(saved); }
};

void rememberBaseline(uint32_t state, float degrees, bool fromProperty) {
    if (!memory || !validSpan(state, kBaseFovOffset + 4)) return;
    std::lock_guard lock(baselineMutex);
    if (baselineMemory != memory->base()) {
        baselines.clear();
        baselineMemory = memory->base();
    }
    if (std::isfinite(degrees) && degrees >= 0.1f && degrees <= 179.0f)
        baselines.insert_or_assign(state, Baseline{degrees, fromProperty});
    else
        baselines.erase(state);
}
bool findBaseline(uint8_t* base, uint32_t state, Baseline& result) {
    std::lock_guard lock(baselineMutex);
    if (baselineMemory != base) return false;
    const auto found = baselines.find(state);
    if (found == baselines.end()) return false;
    result = found->second;
    return true;
}

enum class Result { noPlayerHandler, original, noBaseline, invalidFov, unsupportedProjection,
                    nonGameplayClient, squareCamera, adjusted };
const char* resultName(Result result) {
    switch (result) {
    case Result::original: return "original";
    case Result::noBaseline: return "no-baseline";
    case Result::invalidFov: return "invalid-fov";
    case Result::unsupportedProjection: return "unsupported-projection";
    case Result::nonGameplayClient: return "non-gameplay-client";
    case Result::squareCamera: return "authored-square-camera";
    case Result::adjusted: return "adjusted";
    default: return "no-player-handler";
    }
}
struct CameraQuery {
    uint8_t* base;
    uint32_t client, viewport, caller;
    float configured;
    uint32_t state = 0;
    Baseline baseline{};
    float liveBase = 0, original = 0, output = 0;
    Result result = Result::noPlayerHandler;
};
thread_local CameraQuery* activeQuery = nullptr;
struct ScopedCameraQuery {
    CameraQuery* previous = activeQuery;
    explicit ScopedCameraQuery(CameraQuery& query) { activeQuery = &query; }
    ~ScopedCameraQuery() { activeQuery = previous; }
};

void reportQuery(const CameraQuery& query) {
    if (!probeEnabled()) return;
    static std::atomic<unsigned> count{0};
    if (count.load(std::memory_order_relaxed) >= kProbeLimit) return;
    // Ignore rotating state-copy addresses when camera values are unchanged.
    // Meaningful changes still report the current state; the cap bounds animation.
    struct Sample {
        uint32_t client, configured, profile, liveBase, original, output;
        Result result;
        bool operator==(const Sample&) const = default;
    };
    const Sample sample{query.client, std::bit_cast<uint32_t>(query.configured),
        std::bit_cast<uint32_t>(query.baseline.degrees), std::bit_cast<uint32_t>(query.liveBase),
        std::bit_cast<uint32_t>(query.original), std::bit_cast<uint32_t>(query.output), query.result};
    static thread_local Sample previous{};
    static thread_local bool havePrevious = false;
    if (havePrevious && sample == previous) return;
    previous = sample;
    havePrevious = true;
    const unsigned index = count.fetch_add(1, std::memory_order_relaxed);
    if (index >= kProbeLimit) return;
    const auto vp = query.viewport;
    const ScopedMathMode mathMode;
    std::fprintf(stderr,
        "[FOVProbe] n=%u caller=%08X client=%08X state=%08X vp=%08X status=%s "
        "configured16x9=%.5f profileGuest=%.5f profileSource=%s liveBase=%.5f "
        "inputGuest=%.5f outputGuest=%.5f returnedGuest=%.5f cachedGuest=%.5f "
        "ref=%.6f sx=%.6f sy=%.6f pixelAspect=%.6f width=%u height=%u%s\n",
        index + 1, query.caller, query.client, query.state, vp, resultName(query.result),
        query.configured, query.baseline.degrees, query.baseline.fromProperty ? "property" : "init",
        query.liveBase, query.original, query.output, readFloat(query.base, vp + kViewportFovOffset),
        readFloat(query.base, query.client + 2112 + kViewportFovOffset), readFloat(query.base, vp + 264),
        readFloat(query.base, vp + 224), readFloat(query.base, vp + 228),
        readFloat(query.base, vp + 272),
        readWord(query.base, vp + 324) - readWord(query.base, vp + 316),
        readWord(query.base, vp + 328) - readWord(query.base, vp + 320),
        index + 1 == kProbeLimit ? " probe-limit-reached" : "");
}
} // namespace

// Component construction clears +88. Never retain a previous object's profile
// when its guest allocation is reused (component == player state +7072).
void ForgetGameplayFovBaselineMidAsmHook(PPCRegister& component) {
    if (component.u32 < 7072) return;
    std::lock_guard lock(baselineMutex);
    baselines.erase(component.u32 - 7072);
}

// 821A9584 copies component +88, which may already contain a scripted effect.
// Carry the captured profile through the state copy instead of sampling f0.
void CopyGameplayFovBaselineMidAsmHook(PPCRegister& destination, PPCRegister& source) {
    if (!memory || destination.u32 < 7072 || source.u32 < 7072) return;
    std::lock_guard lock(baselineMutex);
    if (baselineMemory != memory->base()) return;
    const auto found = baselines.find(source.u32 - 7072);
    if (found == baselines.end())
        baselines.erase(destination.u32 - 7072);
    else {
        const Baseline profile = found->second;
        baselines.insert_or_assign(destination.u32 - 7072, profile);
    }
}

// 821A3928: immediately after 821A2588 initializes state+7072. Capturing here
// also handles its equal-value branch, which can skip the 70-degree store.
void InitializeGameplayFovBaselineMidAsmHook(PPCRegister& state) {
    const ScopedMathMode mathMode;
    if (memory && validSpan(state.u32, kBaseFovOffset + 4))
        rememberBaseline(state.u32, readFloat(memory->base(), state.u32 + kBaseFovOffset), false);
}

// 82148CC8: player property hash 66295426 assigns both base +7160 and target
// +7152 from f31. Observe the branch entry even if the values already match.
// Runtime target changes at 821545C0/821215D0 intentionally do NOT recapture
// this profile, so scripted base-FOV transitions retain their relative effect.
void ConfigureGameplayFovBaselineMidAsmHook(PPCRegister& state, PPCRegister& value) {
    const ScopedMathMode mathMode;
    rememberBaseline(state.u32, static_cast<float>(value.f64), true);
}

extern "C" PPC_FUNC(__imp__sub_8249A030);
PPC_FUNC(sub_8249A030) {
    const float configured = fieldOfViewSetting();
    if (configured == 0 && !probeEnabled()) {
        __imp__sub_8249A030(ctx, base);
        return;
    }
    CameraQuery query{base, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr), configured};
    if (!validSpan(query.client, 8980) || !validSpan(query.viewport, 416)) {
        __imp__sub_8249A030(ctx, base);
        return;
    }
    const auto clientVtable = readWord(base, query.client);
    if (clientVtable != kModClientVtable && clientVtable != kP6ClientVtable) {
        query.result = Result::nonGameplayClient;
        __imp__sub_8249A030(ctx, base);
        reportQuery(query);
        return;
    }
    // 823F9AE0 selects +1000 when client+8976 >= 0. 823FBE70 then
    // overwrites this query's FOV with 90 and a square viewport at 823FBFB4.
    // Leave that authored camera AND its cached viewport entirely original.
    if (static_cast<int32_t>(readWord(base, query.client + 8976)) >= 0) {
        query.result = Result::squareCamera;
        __imp__sub_8249A030(ctx, base);
        reportQuery(query);
        return;
    }
    const ScopedCameraQuery scope(query);
    __imp__sub_8249A030(ctx, base);
    reportQuery(query);
}

// Before 82157600's original stfs f1,+0x104(viewport). The player has finished
// ALL its FOV branches; the next original instructions select perspective and
// mark this same viewport dirty. No original instruction is skipped.
void ApplyGameplayFovMidAsmHook(PPCRegister& state, PPCRegister& viewport,
    PPCRegister& client, PPCRegister& message, PPCRegister& fov) {
    auto* query = activeQuery;
    if (!query || query->client != client.u32 || query->viewport != viewport.u32 ||
        !validSpan(state.u32, kBaseFovOffset + 4) || !validSpan(message.u32, 48)) return;
    auto* base = query->base;
    if (readWord(base, message.u32) != 36 || readWord(base, message.u32 + 40) != viewport.u32 ||
        readWord(base, message.u32 + 44) != 416) return;

    const ScopedMathMode mathMode;
    query->state = state.u32;
    query->original = query->output = static_cast<float>(fov.f64);
    query->liveBase = readFloat(base, state.u32 + kBaseFovOffset);
    const bool haveBaseline = findBaseline(base, state.u32, query->baseline);
    if (query->configured == 0) { query->result = Result::original; return; }
    if (!haveBaseline) { query->result = Result::noBaseline; return; }
    if (!std::isfinite(query->original) || query->original < 0.1f || query->original > 179.0f) {
        query->result = Result::invalidFov;
        return;
    }

    // Only the verified canonical player projection. Other authored camera
    // conventions retain their original FOV and are visible to the opt-in probe.
    const float referenceAspect = readFloat(base, viewport.u32 + kReferenceAspectOffset);
    const float sx = readFloat(base, viewport.u32 + 224);
    const float sy = readFloat(base, viewport.u32 + 228);
    const float pixelAspect = readFloat(base, viewport.u32 + 272);
    if (!(std::abs(referenceAspect - 4.0f / 3.0f) < 0.00001f &&
          std::abs(sx - 1.0f) < 0.00001f && std::abs(sy - 1.0f) < 0.00001f &&
          std::abs(pixelAspect - 1.0f) < 0.00001f)) {
        query->result = Result::unsupportedProjection;
        return;
    }
    const float baseline16x9 = static_cast<float>(guestFovToHorizontal16By9(
        query->baseline.degrees, referenceAspect));
    query->output = relativeGuestFovDegrees(query->original, baseline16x9, query->configured);
    if (query->output != query->original) fov.f64 = double(query->output);
    query->result = Result::adjusted;
}
