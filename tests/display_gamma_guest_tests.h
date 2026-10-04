#pragma once

// CPU-only retail oracle and guest-to-preview ownership contracts. Include in
// DisplayGammaTests and call once, before preview/GPU initialization. Requires
// DarkRuntime (original AOT, Memory, kernel ABI and preview command bridge).
#include "runtime/native/runtime.h"
#include "renderer/engine/display_gamma.h"
#include "renderer/engine/simple_mesh.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82870658);

namespace DisplayGammaGuestTestDetail {
inline void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
inline uint16_t read16(const uint8_t* base, uint32_t address) {
    return uint16_t(uint16_t(base[address]) << 8 | base[address + 1]);
}
inline void write16(uint8_t* base, uint32_t address, uint16_t value) {
    base[address] = uint8_t(value >> 8); base[address + 1] = uint8_t(value);
}
inline std::array<uint16_t, 1024> independentTransfer() {
    // These literal bits are independently verified retail constants, rather
    // than values derived from the production presentation implementation.
    const auto f = [](uint32_t bits) { return std::bit_cast<float>(bits); };
    std::array<uint16_t, 1024> table{};
    for (unsigned j = 0; j < table.size(); ++j) {
        float x = float(double(j) * f(0x3A802008)); // j / 1023, float32.
        if (double(x) <= 0.04045) x = float(double(x) * f(0x3D9E8391));
        else {
            x = float(double(float(double(x) + f(0x3D6147AE))) * f(0x3F72A76F));
            x = float(std::pow(double(x), 2.4000000953674316));
        }
        if (double(x) < 0.018) x = float(double(x) * 4.5);
        else {
            const float powered = float(std::pow(double(x), 0.44999998807907104));
            x = float(double(powered) * f(0x3F8CAC08) - f(0x3DCAC083));
        }
        table[j] = uint16_t((std::clamp)(int(double(x) * 1023.0 + 0.5), 0, 1023));
    }
    return table;
}
} // namespace DisplayGammaGuestTestDetail

inline void testGuestDisplayGamma(const std::filesystem::path& gameDirectory) {
    using namespace DarkRecomp;
    using namespace DarkRecomp::Native;
    using namespace DisplayGammaGuestTestDetail;
    Memory owner;
    struct Restore {
        Memory* previousMemory;
        PPCContext* previousContext;
        ~Restore() { memory = previousMemory; currentContext = previousContext; }
    } restore{memory, currentContext};
    memory = &owner;
    owner.load(gameDirectory);
    PPCContext initial{};
    owner.initThread(initial);
    auto* base = owner.base();
    constexpr uint32_t fixture = 0x03000000, extent = 0x2000;
    constexpr uint32_t input = fixture, output = fixture + 0x800;
    constexpr uint32_t abi = fixture + 0x1000;
    require(owner.commit(fixture, extent), "Cannot commit display gamma CPU fixture");
    std::memset(base + fixture, 0xCD, extent);

    // Original 82870658 consumes both outputs. Poisoning catches a success
    // stub that leaves either the ramp type or floating exponent uninitialized.
    auto query = initial;
    query.r3.u64 = abi; query.r4.u64 = abi + 4;
    __imp__VdGetCurrentDisplayGamma(query, base);
    require(query.r3.u32 == 0 && owner.read32(abi) == 2 &&
            owner.read32(abi + 4) == std::bit_cast<uint32_t>(2.22222233f),
            "VdGetCurrentDisplayGamma must initialize both HDTV ABI outputs");
    constexpr std::array<std::array<uint32_t, 2>, 8> constants{{
        {0x8209DF08, 0x3A802008}, {0x8209E26C, 0x3D9E8391},
        {0x8209E268, 0x3D6147AE}, {0x8209E258, 0x3F72A76F},
        {0x8209E208, 0x40900000}, {0x8209E1FC, 0x3F8CAC08},
        {0x8209E228, 0x3DCAC083}, {0x8209E238, 0x3F870A3D}
    }};
    for (const auto& literal : constants)
        require(owner.read32(literal[0]) == literal[1], "Retail gamma transfer constants differ");
    const auto transfer = independentTransfer();
    std::array<uint8_t, 1536> converted{};
    unsigned compared = 0;
    for (unsigned variant = 0; variant < 2; ++variant) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            for (unsigned entry = 0; entry < 128; ++entry) {
                // Variant zero is the original 8286F970 neutral ramp, including
                // its six low input bits. Variant one has distinct RGB curves.
                const unsigned numerator = entry * 65535;
                unsigned start = numerator / 127;
                unsigned end = (entry + 1) * 65535 / 127;
                if (variant) {
                    const auto point = [channel](unsigned index) {
                        const unsigned q = (std::min)(index, 127u);
                        if (channel == 0) return q * q * 1023 / (127 * 127);
                        if (channel == 1) return 33u + q * 910 / 127;
                        return 127u + q * 800 / 127;
                    };
                    start = (point(entry) << 6) | ((entry + channel * 7) & 63);
                    end = (point(entry + 1) << 6) | ((entry + channel * 7) & 63);
                }
                const uint32_t address = input + channel * 512 + entry * 4;
                write16(base, address, uint16_t(start));
                write16(base, address + 2, uint16_t(end - start));
            }
        }
        auto call = initial;
        call.fpscr.loadFromHost();
        call.r3.u64 = output; call.r4.u64 = input; call.r5.u64 = 0;
        __imp__sub_82870658(call, base);
        for (unsigned channel = 0; channel < 3; ++channel) {
            for (unsigned entry = 0; entry < 128; ++entry) {
                const unsigned offset = channel * 512 + entry * 4;
                const unsigned start = read16(base, input + offset) >> 6;
                const unsigned end = (std::min)(1023u, start + (read16(base, input + offset + 2) >> 6));
                const uint16_t expectedBase = uint16_t(transfer[start] << 6);
                const uint16_t expectedDelta = uint16_t((transfer[end] - transfer[start]) << 6);
                if (read16(base, output + offset) != expectedBase ||
                    read16(base, output + offset + 2) != expectedDelta) {
                    char message[160];
                    std::snprintf(message, sizeof(message),
                        "Original display conversion mismatch: variant %u channel %u entry %u", variant, channel, entry);
                    throw std::runtime_error(message);
                }
                compared += 2;
            }
        }
        std::memcpy(converted.data(), base + output, converted.size());
    }

    // Isolated-process queue contracts: no renderer or guest menu runs here.
    std::vector<SimpleMesh> frame;
    previewObserveDisplayGamma(base, output, true); previewEndFrame();
    require(!takePreviewFrame(frame), "Inactive preview enqueued a display gamma command");
    enableEnginePreview();
    previewObserveDisplayGamma(base, output, true); previewEndFrame();
    require(takePreviewFrame(frame) && frame.size() == 1 && frame[0].displayGamma,
            "Completed original display ramp was not queued");
    const auto saved = frame[0].displayGamma;
    require(saved->piecewise, "Original piecewise display mode was lost");
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned entry = 0; entry < 128; ++entry)
            require(saved->entries[entry * 3 + channel][0] == read16(base, output + channel * 512 + entry * 4) &&
                    saved->entries[entry * 3 + channel][1] == read16(base, output + channel * 512 + entry * 4 + 2),
                    "Display gamma bridge changed endian or planar channel layout");
    const auto immutable = *saved;
    std::memset(base + output, 0, converted.size());
    require(*saved == immutable, "Queued display gamma aliases mutable guest memory");
    std::memcpy(base + output, converted.data(), converted.size());
    previewObserveDisplayGamma(base, output, true); previewEndFrame();
    require(takePreviewFrame(frame) && frame.empty(), "Repeated display gamma was not deduplicated");
    // commit() may map a larger host range than the requested fixture. Make
    // this dedicated ABI page explicitly unreadable instead of guessing that
    // the following address is unmapped.
    DWORD previousProtection=0;
    require(VirtualProtect(base+abi,4096,PAGE_NOACCESS,&previousProtection)!=FALSE,
            "Cannot protect unreadable display ramp fixture");
    struct RestoreProtection {
        uint8_t* page;DWORD previous;
        ~RestoreProtection(){DWORD ignored=0;VirtualProtect(page,4096,previous,&ignored);}
    } restoreProtection{base+abi,previousProtection};
    previewObserveDisplayGamma(base, abi, true); previewEndFrame();
    require(takePreviewFrame(frame) && frame.empty(), "Unreadable display ramp enqueued a command");
    previewObserveDisplayGamma(base, output, false); previewEndFrame();
    require(takePreviewFrame(frame) && frame.size() == 1 && frame[0].displayGamma &&
            !frame[0].displayGamma->piecewise, "Same bytes with a different gamma mode were deduplicated");
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned entry = 0; entry < 256; ++entry)
            require(frame[0].displayGamma->entries[entry * 3 + channel][0] ==
                        (read16(base, output + channel * 512 + entry * 2) & 0xFFC0u) &&
                    frame[0].displayGamma->entries[entry * 3 + channel][1] == 0,
                    "Normal display gamma must mask six low bits and omit deltas");
    for (unsigned word = 0; word < 768; ++word)
        write16(base, output + word * 2, uint16_t(read16(base, output + word * 2) | ((word * 17 + 1) & 63)));
    previewObserveDisplayGamma(base, output, false); previewEndFrame();
    require(takePreviewFrame(frame) && frame.empty(),
            "Normal gamma ignored low bits must not create a different table");
    std::printf("Display gamma guest: %u exact original conversion words; HDTV ABI, RGB planes, owned snapshots, mode changes, deduplication and unreadable input.\n", compared);
}
