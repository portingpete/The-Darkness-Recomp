#pragma once

// Include after native_tests.cpp's check(); Memory::load/initThread must already
// have run. This characterizes the original guest kernels, including their
// doubled gain carry. It neither decodes XMA nor creates an audio sink.
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>

extern "C" PPC_FUNC(__imp__sub_828C5060);

static void testAudioResampler(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "Resampler requires loaded Memory and initThread");
    auto* base = memory->base();
    struct Restore {
        Memory* owner;
        PPCContext& context;
        PPCContext saved;
        uint32_t hostCsr;
        uint32_t allocation = 0;
        ~Restore() {
            context = saved;
            context.fpscr.setcsr(hostCsr);
            if (allocation) owner->release(allocation);
        }
    } restore{memory, ctx, ctx, ctx.fpscr.getcsr()};

    // These are guest VAs in the loaded memory image, not PE raw offsets.
    // Assert the original instruction/data oracle before characterizing it.
    struct Instruction { uint32_t address, word; };
    constexpr Instruction instructions[] = {
        {0x828D4088, 0x5527083C}, // stereo: r7 = 2*n
        {0x828D40B8, 0xF8E10070}, // std r7,112(r1)
        {0x828D40E0, 0xC9610070}, // lfd f11,112(r1)
        {0x828D40EC, 0xEC0B037A}, // fmadds f0,f11,f13,f0
        {0x828D40F0, 0xD0030024}, // stfs f0,36(r3)
        {0x828D3E34, 0x5525083C}, // mono: r5 = 2*n
        {0x828D3E68, 0x7CA707B4}, // extsw r7,r5
        {0x828D3E90, 0xF8E10060}, // std r7,96(r1)
        {0x828D3E94, 0xC9610060}, // lfd f11,96(r1)
        {0x828D3EC0, 0xEC0B037A}, // fmadds f0,f11,f13,f0
        {0x828D3EC4, 0xD0030024}, // stfs f0,36(r3)
        {0x828D4178, 0x10800484}, // vor v4,v0,v0
        {0x828D417C, 0x10600484}, // vor v3,v0,v0
        {0x828D41B4, 0x15082090}, // vmulfp128 v8,v8,v4
        {0x828D41BC, 0x15AD1890}, // vmulfp128 v13,v13,v3
    };
    for (const auto& instruction : instructions)
        check(memory->read32(instruction.address) == instruction.word,
              "Resampler original instruction oracle changed");
    auto putFloat = [&](uint32_t address, float value) {
        memory->write32(address, std::bit_cast<uint32_t>(value));
    };
    auto getFloat = [&](uint32_t address) {
        return std::bit_cast<float>(memory->read32(address));
    };
    for (uint32_t lane = 0; lane < 4; ++lane) {
        check(getFloat(0x82009190 + lane * 4) == float(lane),
              "Resampler live gain-lane constant changed");
        check(getFloat(0x82009160 + lane * 4) == float(lane + 4),
              "Resampler upper gain-lane constant changed");
    }
    check(getFloat(0x82007A90) == 4.0f && getFloat(0x82000DD4) == 8.0f &&
          getFloat(0x8209DE30) == 1.0f / 32768.0f,
          "Resampler gain-step or signed PCM16 scale constant changed");
    constexpr uint32_t table = 0x82008E18;
    check(memory->read32(table + 106 * 4) == 0x828D3DC0 &&
          memory->read32(table + 107 * 4) == 0x828D3FE8 &&
          memory->read32(table + 31 * 4) == 0x828CE588 &&
          memory->read32(table + 32 * 4) == 0x828CE6B8,
          "Resampler equal-rate PCM16 mono/stereo dispatch table changed");

    constexpr uint32_t allocationBytes = 0x4000;
    restore.allocation = memory->allocate(allocationBytes);
    check(restore.allocation != 0, "Resampler fixture allocation failed");
    const uint32_t block = restore.allocation, record = block + 0x100;
    uint32_t cases = 0;
    auto run = [&](uint32_t channels, uint32_t flags,
                   std::initializer_list<uint32_t> spans, bool equalGain,
                   uint32_t inputOffset, uint32_t outputOffset, const char* label) {
        const std::string name = std::string("Resampler ") + label +
            " ch=" + std::to_string(channels) + " flags=" + std::to_string(flags) +
            (equalGain ? " equal gain" : " ramp");
        auto require = [&](bool success, const std::string& message) {
            if (!success) throw std::runtime_error(name + ": " + message);
        };
        auto expectNear = [&](double actual, double expected, const std::string& message) {
            require(std::isfinite(actual) && std::abs(actual - expected) <= 0.00001,
                    message + " actual=" + std::to_string(actual) +
                    " expected=" + std::to_string(expected));
        };
        // Both payloads have surrounding sentinels. The whole allocation is
        // checked after every call, except the record and written sample cells.
        std::memset(base + block, 0xA5, allocationBytes);
        std::memset(base + record, 0, 88);
        const uint32_t input = block + 0x1040 + inputOffset;
        const uint32_t output = block + 0x3040 + outputOffset;
        for (uint32_t frame = 0; frame < 1024; ++frame) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const uint16_t sample = channel == 0 ? 16384 : 8192;
                const uint32_t address = input + (frame * channels + channel) * 2;
                base[address] = uint8_t(sample >> 8);
                base[address + 1] = uint8_t(sample);
            }
        }
        base[record + 12] = 1; // Signed PCM16, interleaved input.
        base[record + 13] = uint8_t(channels);
        memory->write32(record + 16, 48000);
        memory->write32(record + 20, output);
        memory->write32(record + 24, 256); // Fixed 256-float channel stride.
        memory->write32(record + 32, 48000);
        const double initialGain = equalGain ? 1.0 : 0.25;
        const double targetGain = equalGain ? 1.0 : 0.75;
        putFloat(record + 36, float(initialGain));
        putFloat(record + 40, float(targetGain));
        memory->write32(record + 80, 3); // Recompute ratio and kernel selection.
        memory->write32(record + 84, flags); // 0x80 disables vector selection.
        const uint32_t selected = channels == 1
            ? (flags == 1 ? 0x828D3DC0 : 0x828CE588)
            : (flags == 1 ? 0x828D3FE8 : 0x828CE6B8);
        std::array<uint8_t, allocationBytes> original{};
        std::memcpy(original.data(), base + block, allocationBytes);
        auto intact = [&](uint32_t produced) {
            for (uint32_t offset = 0; offset < allocationBytes; ++offset) {
                const uint32_t address = block + offset;
                if (address >= record && address < record + 88) continue;
                if (address >= output && address < output + channels * 1024 &&
                    (address - output) % 1024 < produced * 4) continue;
                if (base[address] != original[offset])
                    require(false, "input, output guard, or unwritten sample changed at offset " +
                            std::to_string(offset));
            }
        };
        std::array<double, 256> expectedGain{};
        uint32_t produced = 0, consumed = 0;
        double carriedGain = initialGain;
        for (uint32_t available : spans) {
            require(produced < 256 && available > 0 && consumed + available <= 1024,
                    "invalid fixture span");
            const uint32_t source = input + consumed * channels * 2;
            // Faithful fresh-span handoff: +0/+4/+8 reset; +28/+36 persist.
            memory->write32(record, source);
            memory->write32(record + 4, available);
            memory->write32(record + 8, 0);
            const uint32_t remaining = 256 - produced;
            const uint32_t count = (std::min)(available, remaining);
            const bool vector = flags == 1 && count % (channels == 1 ? 16 : 8) == 0 &&
                ((source | (output + produced * 4)) & 15) == 0;
            const double delta = (targetGain - carriedGain) / remaining;
            for (uint32_t frame = 0; frame < count; ++frame)
                expectedGain[produced + frame] = carriedGain + frame * delta;
            // Original vector instructions save 2*n*d; their sample lanes
            // still advance only d per frame. Scalar fallback saves n*d.
            carriedGain += (vector ? 2.0 : 1.0) * count * delta;
            ctx = restore.saved;
            ctx.r3.u64 = record;
            __imp__sub_828C5060(ctx, base); // Bypass any native tracing wrapper.
            produced += count;
            consumed += count;
            require(ctx.r3.u32 == count && memory->read32(record + 8) == count &&
                    memory->read32(record + 28) == produced,
                    "returned consumption or input/output progression mismatch");
            require(memory->read32(record + 76) == selected &&
                    memory->read32(record + 80) == 0,
                    "wrong selected kernel or dirty flags not cleared");
            // +76 stays the vector wrapper even when its internal alignment
            // or count branch falls back to the scalar kernel.
            require(memory->read32(record) == source &&
                    memory->read32(record + 4) == available &&
                    memory->read32(record + 20) == output &&
                    memory->read32(record + 24) == 256 &&
                    memory->read32(record + 16) == 48000 &&
                    memory->read32(record + 32) == 48000 &&
                    memory->read32(record + 84) == flags &&
                    base[record + 12] == 1 && base[record + 13] == channels,
                    "immutable descriptor fields changed");
            expectNear(getFloat(record + 36), carriedGain, "carried gain");
            expectNear(getFloat(record + 40), targetGain, "target gain");
            expectNear(getFloat(record + 44), 1.0, "sample-rate ratio");
            expectNear(getFloat(record + 48), 0.0, "equal-rate fractional state");
            for (uint32_t channel = 0; channel < channels; ++channel) {
                for (uint32_t frame = 0; frame < produced; ++frame) {
                    expectNear(getFloat(output + channel * 1024 + frame * 4),
                         (channel == 0 ? 0.5 : 0.25) * expectedGain[frame],
                         "output ch=" + std::to_string(channel) +
                         " frame=" + std::to_string(frame));
                }
            }
            intact(produced);
        }
        require(produced == 256 && consumed == 256, "incomplete output block");
        // A full destination must cause no further consumption or writes.
        std::array<uint8_t, 88> fullRecord{};
        std::memcpy(fullRecord.data(), base + record, fullRecord.size());
        std::array<uint8_t, 2048> fullOutput{};
        std::memcpy(fullOutput.data(), base + output, channels * 1024);
        ctx.r3.u64 = record;
        __imp__sub_828C5060(ctx, base);
        require(ctx.r3.u32 == 0 &&
                std::memcmp(fullRecord.data(), base + record, fullRecord.size()) == 0 &&
                std::memcmp(fullOutput.data(), base + output, channels * 1024) == 0,
                "full output did not stop cleanly");
        intact(256);
        ++cases;
    };

    for (uint32_t channels : {1u, 2u}) {
        for (uint32_t flags : {1u, 0x81u}) {
            for (bool equalGain : {false, true}) {
                run(channels, flags, {1024}, equalGain, 0, 0, "contiguous1024");
                run(channels, flags, {128, 128}, equalGain, 0, 0, "split128+128");
                run(channels, flags, {64, 192}, equalGain, 0, 0, "split64+192");
            }
        }
    }
    run(2, 1, {128, 128}, false, 2, 0, "unaligned-input scalar fallback");
    run(2, 1, {128, 128}, false, 0, 4, "unaligned-output scalar fallback");
    run(2, 1, {127, 129}, false, 0, 0, "count/alignment scalar fallback");
    std::printf("Original audio resampler characterization passed (%u cases; doubled vector carry preserved).\n",
                cases);
}
