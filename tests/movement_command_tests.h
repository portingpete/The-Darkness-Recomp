#pragma once
#include <array>
#include <bit>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_82432F90);
extern "C" PPC_FUNC(__imp__sub_82433450);
extern "C" PPC_FUNC(__imp__sub_82433380);

// Run the translated retail queue-to-packet serializer. Four absolute-axis
// updates at the same instant are the pattern in the user's DualSense trace.
// They must not make one 30 Hz tick spill into subsequent simulation ticks.
static void testMovementCommandPacking(PPCContext& ctx) {
    const auto block = memory->allocate(0x20000);
    check(block != 0, "Movement command fixture allocation");
    struct Release { uint32_t block; ~Release() { memory->release(block); } } release{block};
    auto* base = memory->base();
    const auto queue = block, storage = block + 0x1000, descriptor = block + 0x400;
    const auto packet = block + 0x4000, decoded = packet + 0x200, cursor = decoded + 0x20;
    const std::array<PPCFunc*, 3> original{__imp__sub_82432F90, __imp__sub_82433450, __imp__sub_82433380};
    const std::array<PPCFunc*, 3> candidate{sub_82432F90, sub_82433450, sub_82433380};
    const std::array<unsigned, 3> offsets{20, 28, 24};
    const std::array<const char*, 3> names{"server", "prediction", "transport"};
    for (unsigned path = 0; path < candidate.size(); ++path) {
        const bool prediction = path == 1;
        std::memset(base + block, 0, 0x20000);
        memory->write32(queue + 12, descriptor);
        memory->write32(descriptor + 4, 128);
        memory->write32(descriptor + 24, storage);
        memory->write32(queue + 16, 36);
        memory->write32(queue + 20, 0);
        const auto little16 = [&](uint32_t address, int value) {
            base[address] = uint8_t(value);
            base[address + 1] = uint8_t(unsigned(value) >> 8);
        };
        for (unsigned i = 0; i < 36; ++i) {
            const auto command = storage + i * 36;
            base[command] = 3; base[command + 1] = 6;
            base[command + 2] = i && i % 4 == 0 ? 3 : 0; // Milliseconds since prior event.
            base[command + 3] = uint8_t(240 + i); // Exercise sequence wrap as well.
            little16(command + 4, i % 4 == 2 ? 0 : 256);
            little16(command + 6, i % 4 == 0 ? 0 : 128 + i / 4);
        }
        auto call = ctx; call.r1.u64 = block + 0x1F000;
        call.r3.u64 = queue; call.r4.u64 = packet; call.f1.f64 = 1.f / 30.f;
        call.r6.u64 = 0xFFFFFFFF; // No sequence cutoff for the full prediction tick.
        auto baseline = call;
        original[path](baseline, base);
        const unsigned readOffset = offsets[path];
        std::fprintf(stderr, "[MovementPacketContract] path=%s original=%u/36 bytes=%u\n",
            names[path], memory->read32(queue + readOffset), unsigned(base[packet + 1]));
        check(memory->read32(queue + readOffset) == 25 && base[packet + 1] == 250,
            "Original serializer must reproduce the analog command backlog");
        memory->write32(queue + readOffset, 0);
        memory->write32(queue + 32, 0); memory->write32(queue + 36, 0);
        for (unsigned i = 0; i < 36; ++i) {
            base[storage + i * 36] = 3;
            base[storage + i * 36 + 1] = 6;
            base[storage + i * 36 + 2] = i && i % 4 == 0 ? 3 : 0;
        }
        candidate[path](call, base);
        std::fprintf(stderr, "[MovementPacketContract] path=%s candidate=%u/36 bytes=%u\n",
            names[path], memory->read32(queue + readOffset), unsigned(base[packet + 1]));
        check(memory->read32(queue + readOffset) == 36,
            "Same-time analog axis updates must fit in their original tick instead of backing up the retail queue");
        check(base[packet + 1] == 90, "Each same-time analog group retains one final absolute vector");
        for (unsigned i = 0; i < 36; ++i)
            check(base[storage + i * 36 + 3] == uint8_t(240 + i),
                "Original queued sequence identifiers remain available for acknowledgement");
        for (unsigned group = 0; group < 9; ++group) {
            const auto offset = group * 10, command = packet + 2 + offset;
            check(base[command] == 3 && base[command + 1] == 6 && base[command + 3] == uint8_t(243 + group * 4),
                "Packed movement retains the latest command sequence, including wrap");
            if (path == 2) check(base[command + 2] == (group ? 3 : 0), "Transport must retain elapsed milliseconds");
            else if (group) check(base[command + 2] > base[command - 8], "Different movement times must remain separate");
            memory->write32(cursor, offset + 4);
            auto read = ctx; read.r1.u64 = block + 0x1F000;
            read.r3.u64 = decoded; read.r4.u64 = packet; read.r5.u64 = cursor;
            sub_8237C3A0(read, base);
            check(std::bit_cast<float>(memory->read32(decoded)) == 256 &&
                  std::bit_cast<float>(memory->read32(decoded + 4)) == float(128 + group) &&
                  std::bit_cast<float>(memory->read32(decoded + 8)) == 0,
                "Original command decoder receives final analog direction and strength");
        }
        if (prediction) {
            memory->write32(queue + 28, 0);
            memory->write32(queue + 36, 0);
            auto partial = ctx; partial.r1.u64 = block + 0x1F000;
            partial.r3.u64 = queue; partial.r4.u64 = packet; partial.f1.f64 = 1.f / 30.f;
            partial.r6.u64 = 245; // Stop halfway through the second same-time group.
            sub_82433450(partial, base);
            check(memory->read32(queue + 28) == 6 && base[packet + 1] == 20 && base[packet + 15] == 245,
                "Prediction compaction must respect its exact command sequence cutoff");
        }
        // A direct staging append must retain retail behavior after the builder's
        // scope ends; compaction cannot leak into arbitrary command construction.
        const auto staged = block + 0x5000;
        const std::array<uint8_t, 10> absolute{3,6,12,1,0,1,128,0,0,0};
        std::memcpy(base + staged, absolute.data(), absolute.size());
        base[packet + 1] = 0;
        for (unsigned i = 0; i < 2; ++i) {
            auto append = ctx; append.r1.u64 = block + 0x1F000;
            append.r3.u64 = packet; append.r4.u64 = staged;
            sub_82433810(append, base);
        }
        check(base[packet + 1] == 20, "Tick-packet compaction cannot change direct staging appends");
    }
}
