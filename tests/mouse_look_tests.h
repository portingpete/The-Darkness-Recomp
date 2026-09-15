#pragma once

// Include after native_tests.cpp's check(). Run serially after Memory::load and
// PPC dispatch initialization. Requires the regenerated 823FD07C hook.
// Only private guest fixtures and temporarily restored host dispatch slots are
// changed. Constructor, packing, permission predicate, receiver and angle math
// are original PPC. Time and queue storage are deterministic fixture services;
// the post-angle world-transform callback is isolated from an absent world.
#include "runtime/native/mouse_look.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_823FCF68);
extern "C" PPC_FUNC(__imp__sub_82432E60);
extern "C" PPC_FUNC(__imp__sub_8237C3A0);
extern "C" PPC_FUNC(__imp__sub_821C8420);
void ApplyNativeMouseLookMidAsmHook(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);

namespace MouseLookTestDetail {
using Command = std::array<uint8_t, 36>;
inline thread_local std::vector<Command> commands;
inline thread_local double tickTime = 1;
inline thread_local unsigned tickCalls = 0, clockCalls = 0, angleCallbacks = 0;

static PPC_FUNC(timeSource) {
    ++tickCalls;
    // Client vtable +220 has a hidden return-buffer ABI: r3=double*, r4=client.
    PPC_STORE_U64(ctx.r3.u32, std::bit_cast<uint64_t>(tickTime));
}
static PPC_FUNC(commandClock) { ++clockCalls; ctx.f1.f64 = 90; }
static PPC_FUNC(captureQueue) {
    Command copy{};
    std::memcpy(copy.data(), base + ctx.r4.u32, copy.size());
    commands.push_back(copy);
    ctx.r3.u64 = 1;
}
static PPC_FUNC(angleUpdated) { ++angleCallbacks; }

struct SavedDispatch {
    uint8_t* base;
    uint32_t address;
    PPCFunc* previous;
    SavedDispatch(uint8_t* b, uint32_t a, PPCFunc* replacement)
        : base(b), address(a), previous(PPC_LOOKUP_FUNC(b, a)) {
        PPC_LOOKUP_FUNC(base, address) = replacement;
    }
    ~SavedDispatch() { PPC_LOOKUP_FUNC(base, address) = previous; }
};
inline int axis(const Command& command, unsigned index) {
    const unsigned offset = 4 + 2 * index;
    return std::bit_cast<int16_t>(uint16_t(unsigned(command[offset]) |
                                         (unsigned(command[offset + 1]) << 8)));
}
inline std::array<int64_t, 2> totals() {
    std::array<int64_t, 2> result{};
    for (const auto& command : commands) {
        check(command[0] == 2 && command[1] == 6 && command[2] == 90 && axis(command, 0) == 0,
              "Mouse look must retain original type/length/time and zero roll");
        result[0] += axis(command, 1);
        result[1] += axis(command, 2);
    }
    return result;
}
} // namespace MouseLookTestDetail

static void testMouseLookContract(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    using namespace MouseLookTestDetail;
    auto* base = memory->base();
    const uint32_t block = memory->allocate(0x40000);
    check(block != 0, "Mouse look fixture allocation");
    const uint32_t client = block, player = block + 0x4000, state = block + 0x6000;
    const uint32_t vector = block + 0xA000, command = block + 0xA040;
    const uint32_t buffer = block + 0xA100, cursor = block + 0xA300, decoded = block + 0xA320;
    HWND window = CreateWindowExW(0, L"STATIC", L"Mouse look contract", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) { memory->release(block); check(false, "Mouse look fixture window"); }
    auto& input = nativeInput();
    const float previousSensitivity = input.consumeMouseLook().sensitivity;
    struct Restore {
        uint32_t block;
        HWND window;
        float sensitivity;
        unsigned mathMode = _mm_getcsr();
        ~Restore() {
            auto& input = DarkRecomp::Native::nativeInput();
            input.setMouseLookEnabled(false);
            input.attachWindow(nullptr);
            input.setMouseSensitivity(sensitivity);
            DestroyWindow(window);
            DarkRecomp::Native::memory->release(block);
            MouseLookTestDetail::commands.clear();
            _mm_setcsr(mathMode);
        }
    } restore{block, window, previousSensitivity};
    SavedDispatch timer(base, 0x823ED8A0, timeSource);
    SavedDispatch clock(base, 0x824A15F8, commandClock);
    // Leave 824A16F8 intact: its flags &0x20 rejection must be exercised.
    SavedDispatch queue(base, 0x824A1720, captureQueue);
    SavedDispatch transform(base, 0x821A6810, angleUpdated);
    tickTime = 1; tickCalls = clockCalls = angleCallbacks = 0;
    commands.clear();
    std::memset(base + block, 0, 0x40000);
    check(memory->read32(0x820807E0 + 956) == 0x823FC650 &&
          memory->read32(0x820807E0 + 1056) == 0x823FCF68 &&
          memory->read32(0x82064B98 + 48) == 0x821A6928,
          "Mouse fixture requires original gameplay dispatch tables");
    check(memory->read32(0x823FC99C) == 0x4E800421 &&
          memory->read32(0x823FD07C) == 0xFF0D0000,
          "Mouse scheduling/override instruction proof differs from original image");
    auto putFloat = [&](uint32_t address, float value) {
        memory->write32(address, std::bit_cast<uint32_t>(value));
    };
    auto getFloat = [&](uint32_t address) {
        return std::bit_cast<float>(memory->read32(address));
    };
    auto fresh = [&] { auto call = ctx; call.r1.u64 = block + 0x3F000; return call; };
    auto angleNear = [](double a, double b) { return std::abs(a - b) < 0.000002; };
    auto prepareState = [&] {
        std::memset(base + state, 0, 0x4000);
        memory->write32(state, 0x82064B98);
        memory->write32(state + 16, player);
        memory->write32(player + 396, state);
        memory->write32(player + 364, 0);
        // Mode 1 avoids unrelated mode-0 world effects; the ordinary angle
        // branch and original permission predicate still execute.
        base[state + 7265] = 1;
        base[state + 10096] = 2;
        putFloat(state + 1964, 1);
        putFloat(state + 1968, 1);
    };
    auto construct = [&](float roll, float pitch, float yaw) {
        std::memset(base + command, 0xA5, 36);
        putFloat(vector, roll); putFloat(vector + 4, pitch); putFloat(vector + 8, yaw);
        auto call = fresh();
        call.r3.u64 = command; call.r4.u64 = vector; call.r5.u64 = 90;
        __imp__sub_82432E60(call, base);
        Command result{};
        std::memcpy(result.data(), base + command, result.size());
        return result;
    };
    auto receive = [&](const Command& packet) {
        // Guest serialized command buffer has a two-byte envelope. Its cursor
        // counts bytes after that envelope, including the four command bytes.
        std::memset(base + buffer, 0, 256);
        base[buffer + 1] = 10;
        std::memcpy(base + buffer + 2, packet.data(), 10);
        memory->write32(cursor, 0);
        auto call = fresh();
        call.r3.u64 = buffer; call.r4.u64 = cursor;
        call.r5.u64 = player; call.r6.u64 = client;
        __imp__sub_821C8420(call, base);
        check(memory->read32(cursor) == 10, "Original type-2 receiver must consume exactly its packet");
    };

    const auto packed = construct(0, -32767.75f, 32767.75f);
    const uint8_t expected[] = {2, 6, 90, 0xA5, 0, 0, 1, 0x80, 0xFF, 0x7F};
    check(std::memcmp(packed.data(), expected, sizeof(expected)) == 0 && packed[10] == 0xA5,
          "Original type-2 constructor must truncate toward zero and pack little-endian signed shorts");
    std::memset(base + buffer, 0, 16);
    base[buffer + 1] = 6;
    std::memcpy(base + buffer + 2, packed.data() + 4, 6);
    memory->write32(cursor, 0);
    {
        auto call = fresh(); call.r3.u64 = decoded; call.r4.u64 = buffer; call.r5.u64 = cursor;
        __imp__sub_8237C3A0(call, base);
    }
    check(getFloat(decoded) == 0 && getFloat(decoded + 4) == -32767 &&
          getFloat(decoded + 8) == 32767 && memory->read32(cursor) == 6,
          "Original decoder must preserve signed packet units and cursor");

    prepareState(); angleCallbacks = 0;
    receive(construct(0, 4096, 8192));
    check(angleNear(getFloat(state + 1044), 0.0625) && angleNear(getFloat(state + 1048), 0.125) && angleCallbacks == 1,
          "Original permission/receiver/angle chain must apply relative turns, not velocity");
    putFloat(state + 1048, 0.9375f);
    receive(construct(0, 32767, 8192));
    check(getFloat(state + 1044) == getFloat(0x8209DDA8) && angleNear(getFloat(state + 1048), 0.0625),
          "Original receiver must preserve upper absolute pitch bound and positive yaw wrap");
    receive(construct(0, -32767, -8192));
    check(getFloat(state + 1044) == getFloat(0x8209E37C) && angleNear(getFloat(state + 1048), 0.9375),
          "Original receiver must preserve lower absolute pitch bound and negative yaw wrap");
    prepareState();
    putFloat(state + 1964, 4); putFloat(state + 1968, 2); putFloat(state + 40, 0.5f);
    receive(construct(0, 3072, 6144));
    check(angleNear(getFloat(state + 1044), 1.0 / 64) && angleNear(getFloat(state + 1048), 1.0 / 32),
          "Original interpolated camera/zoom divisor must survive native relative commands");
    prepareState(); base[state + 10096] = 0;
    receive(construct(0, 4096, 8192));
    check(angleNear(getFloat(state + 1044), 0.0625 * getFloat(0x82A47868)) &&
          angleNear(getFloat(state + 1048), 0.125 * getFloat(0x82A47868)),
          "Original authored angle multiplier must remain active when its flag requests it");
    prepareState(); memory->write32(state + 388, 1);
    const auto beforeDenied = angleCallbacks;
    receive(construct(0, 4096, 8192));
    check(getFloat(state + 1044) == 0 && getFloat(state + 1048) == 0 && angleCallbacks == beforeDenied,
          "Original 82199158 permission rejection must suppress type-2 angle dispatch");
    prepareState(); memory->write32(state + 388, 8);
    receive(construct(0, 4096, 8192));
    check(getFloat(state + 1044) == 0 && getFloat(state + 1048) == 0,
          "Authored ordinary-branch angle lock must remain intact after permission succeeds");

    memory->write32(client, 0x820807E0);
    memory->write32(client + 516, 1);
    memory->write32(client + 520, 3);
    memory->write32(client + 432, 1);
    memory->write32(client + 3940, block + 0xA400);
    memory->write32(block + 0xA404, 1);
    putFloat(client + 416, 1); putFloat(client + 424, 1);
    putFloat(client + 8836, 3); putFloat(client + 8840, 4);
    auto physicalLook = [&] {
        auto call = fresh(); call.r3.u64 = client; call.f1.f64 = 2; call.f2.f64 = -3;
        __imp__sub_823FCF68(call, base);
    };
    commands.clear(); physicalLook();
    check(commands.size() == 1 && totals() == std::array<int64_t, 2>{384, 192},
          "Unscoped controller/script look must retain original 32x sensitivity and pitch sign");
    const auto originalPhysical = commands.front();
    commands.clear(); memory->write32(client + 516, 0x21);
    physicalLook();
    check(commands.empty(), "Original 824A16F8 gate must reject look before the downstream queue stub");
    memory->write32(client + 516, 1);
    input.attachWindow(window);
    input.windowMessage(window, WM_SETFOCUS, 0, 0);
    input.setMouseLookEnabled(true);
    check(input.setMouseSensitivity(1), "Mouse fixture sensitivity");
    auto tick = [&](uint32_t lr = 0x823FC9A0) {
        auto call = fresh(); call.r3.u64 = client; call.lr = lr; call.f1.f64 = 0.05;
        const unsigned before = tickCalls;
        sub_823FC650(call, base);
        check(tickCalls == before + 1, "Native wrapper must call original periodic look exactly once");
    };
    auto newEpoch = [&] {
        input.setMouseLookEnabled(false); input.setMouseLookEnabled(true);
        tick(); commands.clear();
    };
    commands.clear(); input.mouseMotion(12, -7); tick(0x823FC99C);
    check(commands.empty(), "Call instruction address must not be mistaken for its return LR");
    tick();
    check(commands.size() == 1 && totals() == std::array<int64_t, 2>{56, 96},
          "Mouse-only neutral-pad tick at LR 823FC9A0 must emit exact counts without controller scale");
    const auto nativePacket = commands.front();
    tick();
    check(commands.size() == 1, "Repeated same-frame ticks must not replay consumed mouse motion");
    prepareState(); receive(nativePacket);
    check(angleNear(getFloat(state + 1044), 56.0 / 65536) && angleNear(getFloat(state + 1048), 96.0 / 65536),
          "Production mouse command bytes must reach original receiver as relative pitch/yaw");
    commands.clear(); physicalLook();
    check(commands.size() == 1 && std::memcmp(commands.front().data(), originalPhysical.data(), 10) == 0,
          "Native mouse scope must not leak into later physical look commands");
    {
        PPCRegister c{}, s{}, p{}, y{};
        c.u64 = client; s.u64 = block + 0xB000; p.f64 = 123; y.f64 = -456;
        putFloat(s.u32 + 92, 7); putFloat(s.u32 + 96, 8);
        ApplyNativeMouseLookMidAsmHook(c, s, p, y);
        check(p.f64 == 123 && y.f64 == -456 && getFloat(s.u32 + 92) == 7 && getFloat(s.u32 + 96) == 8,
              "Unscoped midasm hook must leave registers and guest vector untouched");
    }
    commands.clear(); putFloat(client + 8836, 0); putFloat(client + 8840, 0);
    input.mouseMotion(3, 2); tick();
    check(totals() == std::array<int64_t, 2>{-16, 24},
          "Scoped override before zero test must work with both controller sensitivities zero");
    putFloat(client + 8836, 3); putFloat(client + 8840, -4);
    commands.clear(); input.mouseMotion(3, 2); tick();
    check(totals() == std::array<int64_t, 2>{16, 24}, "Mouse must preserve original invert option sign only");
    putFloat(client + 8840, 4);

    // Accumulated physical counts and individually consumed counts have the
    // same command-unit sum, including sub-unit fractions at low sensitivity.
    check(input.setMouseSensitivity(0.1f), "Low mouse sensitivity accepted");
    newEpoch();
    for (unsigned i = 0; i < 10; ++i) { input.mouseMotion(1, -1); tick(); }
    const auto fine = totals();
    newEpoch(); input.mouseMotion(10, -10); tick();
    check(totals() == fine && fine == std::array<int64_t, 2>{8, 8},
          "Sub-unit input must accumulate with no batching dead zone");
    newEpoch(); input.mouseMotion(1, 1); tick();
    check(commands.empty(), "Sub-unit fixture must retain a fractional residual");
    newEpoch(); input.mouseMotion(1, 1); tick();
    check(commands.empty(), "Capture epoch change must discard old fractional residual");

    for (uint32_t mask : {0x20u, 0x40u}) {
        newEpoch(); input.mouseMotion(1, 1); tick(); // residual 0.8 unit
        memory->write32(client + 516, 1 | mask);
        if (mask == 0x40) memory->write32(client + 7360, block + 0xA500);
        tick(); // Empty ineligible tick must reset the old residual too.
        input.mouseMotion(100, 100); tick();
        check(commands.empty(), "Menu/input-disabled ticks must discard physical counts");
        memory->write32(client + 516, 1); memory->write32(client + 7360, 0);
        tick(); input.mouseMotion(1, 1); tick();
        check(commands.empty(), "Returning from menu must replay neither counts nor fractional carry");
    }
    newEpoch(); base[client + 8816] = 2;
    std::array<uint8_t, 64> cameraBefore{};
    std::memcpy(cameraBefore.data(), base + client + 8880, cameraBefore.size());
    input.mouseMotion(100, 100); tick();
    check(commands.empty() && std::memcmp(cameraBefore.data(), base + client + 8880, cameraBefore.size()) == 0,
          "Authored/free camera gate must discard without applying dummy look arguments");
    base[client + 8816] = 0; tick();
    check(commands.empty(), "Authored/free camera exit must not replay ignored mouse counts");
    newEpoch(); input.mouseMotion(1, 1); tick();
    input.windowMessage(window, WM_KILLFOCUS, 0, 0); tick();
    input.windowMessage(window, WM_SETFOCUS, 0, 0); input.setMouseLookEnabled(true);
    input.mouseMotion(1, 1); tick();
    check(commands.empty(), "Focus epoch must discard both pending counts and fractional carry");

    check(input.setMouseSensitivity(1), "Default mouse sensitivity restored");
    newEpoch(); input.mouseMotion(20000, -10000); tick();
    check(commands.size() > 1 && totals() == std::array<int64_t, 2>{80000, 160000},
          "Fast mouse batch must split before signed-short conversion without a turn cap or wrap");
    const auto fast = totals();
    newEpoch();
    for (unsigned i = 0; i < 20; ++i) { input.mouseMotion(1000, -500); tick(); }
    check(totals() == fast, "Fast and slow delivery of the same distance must emit the same angle sum");
    check(input.setMouseSensitivity(10), "Maximum mouse sensitivity accepted");
    newEpoch(); input.mouseMotion(20, -10); tick();
    check(totals() == std::array<int64_t, 2>{800, 1600}, "Mouse sensitivity must scale physical distance linearly");
    check(input.setMouseSensitivity(1), "Default mouse sensitivity restored after scaling");
    newEpoch();
    memory->write32(client, 0x82081BF0);
    input.mouseMotion(1, 1); tick();
    check(totals() == std::array<int64_t, 2>{-8, 8}, "Original P6 client must share the native look boundary");
    memory->write32(client, 0x820807E0);

    newEpoch(); tickTime = 2;
    putFloat(client + 8820, 1); putFloat(client + 8824, 0.5f);
    input.mouseMotion(1, 0); tick();
    check(commands.size() == 4 && commands[0][0] == 1 && commands[1][0] == 2 &&
          commands[2][0] == 1 && commands[3][0] == 2 &&
          axis(commands[1], 1) == -1280 && axis(commands[1], 2) == 1920 &&
          axis(commands[3], 1) == 0 && axis(commands[3], 2) == 8,
          "Simultaneous physical stick and mouse must retain original velocity command and control-mode flush ordering");
    putFloat(client + 8820, 0); putFloat(client + 8824, 0);

    MouseLookQuantizer quantizer;
    auto corrupt = quantizer.convert({std::numeric_limits<int64_t>::max(), 0, 10, 0}, false);
    check(corrupt.rejected && corrupt.count == 0, "Corrupt motion must reject explicitly, never wrap a command short");
    auto afterReject = quantizer.convert({1, 0, 1, 0}, false);
    check(!afterReject.rejected && afterReject.count == 1 && afterReject.packets[0].yaw == 8,
          "Rejected motion must leave no quantizer backlog");
    std::puts("Mouse look: original type-2 bytes/receiver, permissions, pitch bounds, yaw wrap, neutral tick, menu/focus discard, batching and physical controller coexistence passed.");
}
