#include "menu_pointer.h"
#include "input.h"
#include "keyboard_menu_guest.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <xmmintrin.h>

namespace {
using namespace DarkRecomp::Native;
std::mutex displayMutex;
MenuPointerDisplay pointerDisplay;
std::atomic<uint32_t> modalDepth{0};
std::atomic<uint64_t> menuTick{0};
std::atomic<uint32_t> observedRoot{0};
std::atomic<uint32_t> observedFrontend{0};
thread_local MenuPointerSession pointerSession;
std::mutex projectionMutex;
MenuPointerProjectionSession projectionSession;
struct CubeRenderScope { uint32_t frontend = 0, root = 0, cube = 0; };
thread_local CubeRenderScope* cubeRenderScope = nullptr;
bool probeEnabled() noexcept;

bool ownsPointer(uint32_t root) noexcept {
    const auto tick = menuTick.load(std::memory_order_acquire);
    return root && root == observedRoot.load(std::memory_order_acquire) && tick &&
           GetTickCount64() - tick < 250;
}

// The FrontEnd also owns the gameplay HUD and title/attract window. Only a
// visible tree containing a control can take ownership of the menu pointer.
bool hasMenuControl(uint8_t* base, uint32_t window, unsigned& budget, unsigned depth = 0) {
    if (!window || !budget || depth > 16 || uint64_t(window) + 272 > PPC_MEMORY_SIZE) return false;
    --budget;
    if (PPC_LOAD_U8(window + 88) || (PPC_LOAD_U32(window + 84) & 8)) return false;
    const auto style = PPC_LOAD_U32(window + 80);
    if (depth && (style & 4) && !(style & 0x400)) return true;
    const auto array = PPC_LOAD_U32(window + 12);
    if (!array || uint64_t(array) + 28 > PPC_MEMORY_SIZE) return false;
    const auto count = PPC_LOAD_U32(array + 4), items = PPC_LOAD_U32(array + 24);
    if (count > 1024 || !items || uint64_t(items) + uint64_t(count) * 4 > PPC_MEMORY_SIZE) return false;
    for (uint32_t i = 0; i < count && budget; ++i)
        if (hasMenuControl(base, PPC_LOAD_U32(items + i * 4), budget, depth + 1)) return true;
    return false;
}

uint32_t observeFrontend(uint8_t* base, uint32_t frontend) {
    if (!memory || base != memory->base() || !frontend ||
        uint64_t(frontend) + 1036 > PPC_MEMORY_SIZE) return 0;
    const auto root = PPC_LOAD_U32(frontend + 16);
    if (!menuPointerTreeHasControl(base, root)) {
        if (observedFrontend.load(std::memory_order_acquire) == frontend)
            observedRoot.store(0, std::memory_order_release);
        return 0;
    }
    observedFrontend.store(frontend, std::memory_order_release);
    observedRoot.store(root, std::memory_order_release);
    menuTick.store(GetTickCount64(), std::memory_order_release);
    if (probeEnabled()) {
        static std::atomic<unsigned> reports{0};
        thread_local uint32_t previousRoot = 0;
        if (previousRoot != root && reports.fetch_add(1, std::memory_order_relaxed) < 128)
            std::fprintf(stderr, "[MenuPointerOwner] frontend=%08X root=%08X vtable=%08X children=%08X flags=%08X menu=%u look=%u\n",
                frontend, root, PPC_LOAD_U32(root), PPC_LOAD_U32(root + 12), PPC_LOAD_U32(root + 84),
                nativeInput().guestMenuAllowsPointer(), nativeInput().mouseLookEnabled());
        previousRoot = root;
    }
    return root;
}

bool probeEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_MENU_POINTER_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

void probePointerState(uint8_t* base, uint32_t root, const char* reason,
                       const MenuCursorSnapshot& pointer) {
    if (!probeEnabled()) return;
    struct Observed { uint32_t root = 0; uint64_t movement = 0, presses = 0, epoch = 0; };
    thread_local std::array<Observed, 32> observed{};
    thread_local size_t next = 0;
    Observed* previous = nullptr;
    for (auto& entry : observed) if (entry.root == root) { previous = &entry; break; }
    if (!previous) { previous = &observed[next++ % observed.size()]; *previous = {}; }
    if (previous->root == root && previous->movement == pointer.movement &&
        previous->presses == pointer.presses && previous->epoch == pointer.epoch) return;
    *previous = {root, pointer.movement, pointer.presses, pointer.epoch};
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 512)
        std::fprintf(stderr, "[MenuPointerState] root=%08X vtable=%08X parent=%08X flags=%08X closed=%u reason=%s valid=%u position=%d,%d movement=%llu presses=%llu epoch=%llu menu=%u look=%u\n",
            root, PPC_LOAD_U32(root), PPC_LOAD_U32(root + 16), PPC_LOAD_U32(root + 84),
            PPC_LOAD_U8(root + 88), reason, pointer.valid, pointer.x, pointer.y,
            (unsigned long long)pointer.movement, (unsigned long long)pointer.presses,
            (unsigned long long)pointer.epoch, nativeInput().guestMenuAllowsPointer(),
            nativeInput().mouseLookEnabled());
}

// Calls run only from a live original window method on the guest UI thread.
// Its native callbacks, focus, disabled flags and SCRIPT_PRESSED remain owners.
void processPointer(PPCContext& ctx, uint8_t* base, uint32_t root) {
    if (!memory || base != memory->base() || !root || uint64_t(root) + 272 > PPC_MEMORY_SIZE)
        return;
    const auto pointer = nativeInput().menuCursor();
    // Nested layouts do not own input. The outer root's hit tester already
    // descends through them and respects original visibility and z order.
    if (PPC_LOAD_U32(root + 16) || PPC_LOAD_U8(root + 88) ||
        (PPC_LOAD_U32(root + 84) & 8)) {
        probePointerState(base, root, "nested-or-hidden", pointer); return;
    }
    if (!modalDepth.load(std::memory_order_acquire) && !ownsPointer(root)) {
        probePointerState(base, root, "not-active-owner", pointer); return;
    }
    probePointerState(base, root, "ready", pointer);
    menuTick.store(GetTickCount64(), std::memory_order_release);
    const auto events = pointerSession.update(root, pointer.epoch, pointer.movement,
                                             pointer.presses, pointer.valid);
    if (!events.hover && !events.activate) return;
    MenuPointerDisplay display;
    { std::lock_guard lock(displayMutex); display = pointerDisplay; }
    const int32_t width = int32_t(PPC_LOAD_U32(root + 104)) - int32_t(PPC_LOAD_U32(root + 96));
    const int32_t height = int32_t(PPC_LOAD_U32(root + 108)) - int32_t(PPC_LOAD_U32(root + 100));
    std::optional<MenuPointerPosition> point;
    bool projected = false;
    {
        std::lock_guard lock(projectionMutex);
        const auto projection = projectionSession.forRoot(root, GetTickCount64());
        projected = projection.has_value();
        if (projection) point = mapMenuPointer(display, pointer.x, pointer.y, width, height, *projection);
    }
    if (!projected) point = mapMenuPointer(display, pointer.x, pointer.y, width, height);
    if (!point) return;
    const uint32_t hit = dispatchMenuPointer(ctx, base, root, *point, events.activate);
    if (probeEnabled()) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 512)
            std::fprintf(stderr, "[MenuPointer] root=%08X logical=%dx%d client=%d,%d point=%d,%d hit=%08X actionable=%u press=%u\n",
                root, width, height, pointer.x, pointer.y, point->x, point->y,
                hit, hit != 0, events.activate);
    }
}
}

namespace DarkRecomp::Native {
bool menuPointerTreeHasControl(uint8_t* base, uint32_t root) {
    unsigned budget = 128;
    return hasMenuControl(base, root, budget);
}
uint32_t dispatchMenuPointer(PPCContext& ctx, uint8_t* base, uint32_t root,
                             MenuPointerPosition point, bool activate) {
    auto call = ctx;
    call.r1.u64 -= 512;
    const uint32_t position = call.r1.u32 + 80, message = call.r1.u32 + 112;
    // A click outside an actionable window must never confirm the previously
    // focused row. Query the same original hit tester, requiring focusability.
    PPC_STORE_U32(position, uint32_t(point.x + int32_t(PPC_LOAD_U32(root + 128))));
    PPC_STORE_U32(position + 4, uint32_t(point.y + int32_t(PPC_LOAD_U32(root + 132))));
    call.r3.u64 = root; call.r4.u64 = position; call.r5.u64 = 1;
    sub_8244D660(call, base);
    const uint32_t hit = call.r3.u32;
    const bool actionable = hit && hit != root && (PPC_LOAD_U32(hit + 80) & 4) &&
                            !(PPC_LOAD_U32(hit + 80) & 0x400) && !(PPC_LOAD_U32(hit + 84) & 8);
    if (!actionable) return 0;
    // Xbox's unused mouse-motion method8244DAC8 begins with an unconditional
    // assertion. Use its supported keyboard-navigation focus method instead;
    // that also avoids starting an original drag/capture for the native click.
    call.r3.u64 = hit;
    const uint32_t table = PPC_LOAD_U32(hit), flagsBefore = PPC_LOAD_U32(hit + 84);
    const uint32_t focus = PPC_LOAD_U32(PPC_LOAD_U32(hit) + 60);
    PPCSafeIndirect(call, base, focus);
    if (probeEnabled() && activate)
        std::fprintf(stderr, "[MenuPointerDispatch] base=%p hit=%08X vtable=%08X focus=%08X style=%08X flags=%08X->%08X focusResult=%08X\n",
            base, hit, table, focus, PPC_LOAD_U32(hit + 80), flagsBefore,
            PPC_LOAD_U32(hit + 84), call.r3.u32);
    if (!activate) return hit;
    // Use the original confirm message for the newly hit window. This is the
    // title's normal button/menu callback route, including option buttons,
    // profile dialogs and custom native settings rows; it is never an XInput
    // press that can accidentally survive into the next menu or gameplay.
    std::memset(base + message, 0, 64);
    PPC_STORE_U32(message, 3);
    PPC_STORE_U32(message + 4, 0x820510B0); // original empty CStr backing
    PPC_STORE_U32(message + 8, 228);       // interface confirm
    PPC_STORE_U32(message + 16, 160);      // original XInput A source before interface mapping
    PPC_STORE_U32(message + 24, 0x437F0000); // original pressed digital value (255f)
    // The original mouse method delivers to the hit window's +276 handler.
    // Root +272 gives authored keyboard bindings priority: modal roots can
    // consume confirm to move focus instead of pressing the clicked button.
    call.r3.u64 = hit; call.r4.u64 = message;
    const uint32_t handler = PPC_LOAD_U32(PPC_LOAD_U32(hit) + 276);
    call.lr = 0;
    PPCSafeIndirect(call, base, handler);
    if (probeEnabled())
        std::fprintf(stderr, "[MenuPointerDispatch] hit=%08X handler=%08X result=%08X flags=%08X closed=%u\n",
            hit, handler, call.r3.u32, PPC_LOAD_U32(hit + 84), PPC_LOAD_U8(root + 88));
    return hit;
}
void initializeMenuPointer() noexcept {}
void setMenuPointerDisplay(uint32_t imageWidth, uint32_t imageHeight,
                           uint32_t outputWidth, uint32_t outputHeight) noexcept {
    std::lock_guard lock(displayMutex);
    pointerDisplay = {imageWidth, imageHeight, outputWidth, outputHeight};
}
bool guestMenuPointerActive() noexcept {
    if (modalDepth.load(std::memory_order_acquire)) return true;
    return ownsPointer(observedRoot.load(std::memory_order_acquire));
}
}

extern "C" PPC_FUNC(__imp__sub_8244D760);
PPC_FUNC(sub_8244D760) {
    const uint32_t root = ctx.r3.u32;
    __imp__sub_8244D760(ctx, base);
    processPointer(ctx, base, root);
}

extern "C" PPC_FUNC(__imp__sub_8239C988);
PPC_FUNC(sub_8239C988) {
    const uint32_t root = ctx.r3.u32;
    __imp__sub_8239C988(ctx, base);
    processPointer(ctx, base, root);
}

extern "C" PPC_FUNC(__imp__sub_82452D48);
PPC_FUNC(sub_82452D48) {
    struct ModalScope {
        ModalScope() { modalDepth.fetch_add(1, std::memory_order_acq_rel); }
        ~ModalScope() { modalDepth.fetch_sub(1, std::memory_order_acq_rel); }
    } scope;
    __imp__sub_82452D48(ctx, base);
}

// Associate the rendered Cube with the active FrontEnd root. Glyph-plane
// parameters belong to this render invocation, not a previous/covered menu.
extern "C" PPC_FUNC(__imp__sub_82359428);
PPC_FUNC(sub_82359428) {
    CubeRenderScope capture;
    const uint32_t cube = ctx.r3.u32;
    if (DarkRecomp::Native::memory && base == DarkRecomp::Native::memory->base() && cube >= 1072) {
        const uint32_t frontend = cube - 1072;
        if (uint64_t(frontend) + 43232 <= PPC_MEMORY_SIZE && PPC_LOAD_U32(frontend) == 0x82071A10) {
            const auto root = PPC_LOAD_U32(frontend + 16);
            if (DarkRecomp::Native::menuPointerTreeHasControl(base, root)) capture = {frontend, root, cube};
        }
    }
    struct Scope {
        CubeRenderScope* previous = cubeRenderScope;
        explicit Scope(CubeRenderScope* current) { cubeRenderScope = current; }
        ~Scope() { cubeRenderScope = previous; }
    } scope(capture.root ? &capture : nullptr);
    __imp__sub_82359428(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82354DE0);
PPC_FUNC(sub_82354DE0) {
    using namespace DarkRecomp::Native;
    const auto* scope = cubeRenderScope;
    const uint32_t params = ctx.r4.u32, caller = uint32_t(ctx.lr);
    if (scope && scope->cube == ctx.r3.u32 && PPC_LOAD_U32(scope->frontend + 16) == scope->root &&
        params && uint64_t(params) + 160 <= PPC_MEMORY_SIZE) {
        const auto scalar = [&](uint32_t address) { return double(std::bit_cast<float>(PPC_LOAD_U32(address))); };
        const auto frame = PPC_LOAD_U32(params);
        uint32_t viewport = 0;
        if (frame && uint64_t(frame) + 6604 <= PPC_MEMORY_SIZE) {
            const auto index = PPC_LOAD_U32(frame + 6600);
            if (index <= 1024) {
                const auto slot = PPC_LOAD_U8(frame + 6592 + index);
                viewport = PPC_LOAD_U32(frame + 4 * (1391 + slot));
            }
        }
        if (viewport && uint64_t(viewport) + 416 <= PPC_MEMORY_SIZE) {
            // The queued draw refreshes its private viewport copy. Refresh an
            // equivalent scratch copy with the original math, preserving the
            // live viewport and caller registers even when its cache is dirty.
            auto calculation = ctx;
            calculation.r1.u64 -= 1024;
            const uint32_t computedViewport = calculation.r1.u32 + 80;
            std::memcpy(base + computedViewport, base + viewport, 416);
            calculation.r3.u64 = computedViewport;
            const unsigned originalMathMode = _mm_getcsr();
            sub_8275EEF8(calculation, base);
            _mm_setcsr(originalMathMode);
            MenuPointerPlane plane;
            plane.frameWidth = PPC_LOAD_U32(scope->frontend + 1028);
            plane.frameHeight = PPC_LOAD_U32(scope->frontend + 1032);
            plane.viewportX = int32_t(PPC_LOAD_U32(viewport + 316));
            plane.viewportY = int32_t(PPC_LOAD_U32(viewport + 320));
            const double viewportWidth = int32_t(PPC_LOAD_U32(viewport + 324)) - plane.viewportX;
            const double viewportHeight = int32_t(PPC_LOAD_U32(viewport + 328)) - plane.viewportY;
            plane.width = scalar(params + 80); plane.height = scalar(params + 84); plane.size = scalar(params + 88);
            plane.centered = PPC_LOAD_U8(params + 128) != 0; plane.halfOrigin = PPC_LOAD_U8(params + 131) != 0;
            plane.originX = scalar(params + 144); plane.originY = scalar(params + 148);
            plane.insetX = scalar(params + 152); plane.insetY = scalar(params + 156);
            MenuPointerMatrix model;
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned column = 0; column < 4; ++column) model[row][column] = scalar(params + 16 + row * 16 + column * 4);
            // Both original callers provide the exact drawn plane model. The
            // viewport's cached focal scalars normalize by its rectangle; this
            // also handles57CA0's draw2D model that cancels camera perspective.
            const double px = scalar(computedViewport + 280) / viewportWidth;
            const double py = -scalar(computedViewport + 284) / viewportHeight;
            const bool perspective = !(PPC_LOAD_U32(computedViewport + 256) & 0x00800000);
            const auto projection = perspective ? projectMenuPointerPlane(plane, model, px, py, viewportWidth, viewportHeight) : std::optional<MenuPointerProjection>{};
            if (projection) {
                std::lock_guard lock(projectionMutex);
                const auto tick = GetTickCount64();
                const bool worldPlane = caller == 0x82358520;
                //57CA0 also renders small cached/thumbnail planes for this
                // Cube. They must not replace its actual58038 glyph plane.
                projectionSession.observe(scope->root, tick, *projection, worldPlane);
            }
            if (probeEnabled()) {
                static std::atomic<unsigned> reports{0};
                thread_local uint32_t previousRoot = 0;
                thread_local unsigned frames = 0;
                if ((previousRoot != scope->root || ++frames % 300 == 0) && reports.fetch_add(1) < 128) {
                    std::fprintf(stderr, "[MenuPointerPlane] root=%08X caller=%08X params=%08X viewport=%08X rect=%g,%g,%g,%g frame=%g,%g plane=%g,%g,%g centered=%u half=%u margins=%g,%g,%g,%g focal=%g,%g dirty=%u model=",
                        scope->root, caller, params, viewport, plane.viewportX, plane.viewportY, viewportWidth, viewportHeight,
                        plane.frameWidth, plane.frameHeight, plane.width, plane.height, plane.size, plane.centered, plane.halfOrigin,
                        plane.originX, plane.originY, plane.insetX, plane.insetY, px, py, PPC_LOAD_U8(viewport + 336));
                    for (const auto& row : model) for (double value : row) std::fprintf(stderr, "%g,", value);
                    if (projection) {
                        std::fprintf(stderr, " clip=");
                        for (const auto* row : {&projection->x, &projection->y, &projection->w})
                            for (double value : *row) std::fprintf(stderr, "%g,", value);
                    }
                    std::fprintf(stderr, " camera=%g,%g,%g,%g,%g,%g flags=%08X clipRect=%d,%d,%d,%d rawProjection=",
                        scalar(viewport + 260), scalar(viewport + 264), scalar(viewport + 224), scalar(viewport + 228),
                        scalar(viewport + 272), scalar(viewport + 276), PPC_LOAD_U32(viewport + 256),
                        int32_t(PPC_LOAD_U32(viewport + 240)), int32_t(PPC_LOAD_U32(viewport + 244)),
                        int32_t(PPC_LOAD_U32(viewport + 248)), int32_t(PPC_LOAD_U32(viewport + 252)));
                    for (unsigned i = 0; i < 16; ++i) std::fprintf(stderr, "%g,", scalar(computedViewport + i * 4));
                    std::fputc('\n', stderr);
                    previousRoot = scope->root;
                }
            }
        }
    }
    __imp__sub_82354DE0(ctx, base);
}

// FrontEnd owns the active window at +16, as used by its original mouse hit
// function82368500. Full render +112 observes ownership even when paused
// gameplay stops reporting GUI state. Cube render +196 is optional, so it
// cannot establish pointer activity reliably. Observation does not change the
// gameplay input epoch or dispatch while a render caller still uses the menu.
extern "C" PPC_FUNC(__imp__sub_8236C880);
PPC_FUNC(sub_8236C880) {
    const uint32_t frontend = ctx.r3.u32;
    __imp__sub_8236C880(ctx, base);
    observeFrontend(base, frontend);
}

// Dispatch after the owning FrontEnd has finished its normal update and
// released the old root reference. Other parentless window pumps cannot steal
// the session baseline or replay a click into a covered menu.
extern "C" PPC_FUNC(__imp__sub_8236C508);
PPC_FUNC(sub_8236C508) {
    const uint32_t frontend = ctx.r3.u32;
    __imp__sub_8236C508(ctx, base);
    DarkRecomp::Native::updateKeyboardMenuGuest(ctx, base, frontend);
    const auto root = observeFrontend(base, frontend);
    if (root) processPointer(ctx, base, root);
}

extern "C" PPC_FUNC(__imp__sub_82452798);
PPC_FUNC(sub_82452798) {
    const uint32_t window = ctx.r3.u32, message = ctx.r4.u32;
    const bool report = probeEnabled() && PPC_LOAD_U32(message) == 3;
    if (report) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 128)
            std::fprintf(stderr, "[MenuMessage] window=%08X vtable=%08X style=%08X flags=%08X msg=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
                window, PPC_LOAD_U32(window), PPC_LOAD_U32(window + 80), PPC_LOAD_U32(window + 84),
                PPC_LOAD_U32(message), PPC_LOAD_U32(message + 4), PPC_LOAD_U32(message + 8),
                PPC_LOAD_U32(message + 12), PPC_LOAD_U32(message + 16), PPC_LOAD_U32(message + 20),
                PPC_LOAD_U32(message + 24), PPC_LOAD_U32(message + 28));
    }
    __imp__sub_82452798(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82452650);
PPC_FUNC(sub_82452650) {
    const uint32_t window = ctx.r3.u32, message = ctx.r4.u32;
    if (probeEnabled() && PPC_LOAD_U32(message) == 3) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 128)
            std::fprintf(stderr, "[MenuRootMessage] window=%08X msg=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
                window, PPC_LOAD_U32(message), PPC_LOAD_U32(message + 4), PPC_LOAD_U32(message + 8),
                PPC_LOAD_U32(message + 12), PPC_LOAD_U32(message + 16), PPC_LOAD_U32(message + 20),
                PPC_LOAD_U32(message + 24), PPC_LOAD_U32(message + 28));
    }
    __imp__sub_82452650(ctx, base);
}
