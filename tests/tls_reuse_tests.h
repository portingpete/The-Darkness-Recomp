#pragma once

static void testTlsReuse(PPCContext& ctx) {
    constexpr uint32_t slot = 47;
    auto* base = memory->base();
    const uint32_t saved = memory->read32(memory->dynamicTls(ctx) + slot * 4);
    std::array<uint32_t, 2> pcr{};
    std::array<std::promise<void>, 2> ready;
    std::array<std::future<std::array<uint32_t, 2>>, 2> workers;
    std::promise<void> proceed;
    const auto gate = proceed.get_future().share();
    bool released = false;
    auto releaseGate = [&] { if (!released) { proceed.set_value(); released = true; } };
    auto cleanup = [&] {
        releaseGate();
        for (auto& worker : workers) if (worker.valid()) worker.wait();
        for (uint32_t address : pcr) if (address) memory->release(address);
        ctx.r3.u64 = slot; ctx.r4.u64 = saved; __imp__KeTlsSetValue(ctx, base);
    };
    try {
        for (unsigned i = 0; i < pcr.size(); ++i) {
            pcr[i] = memory->allocate(0x2000);
            check(pcr[i] != 0, "TLS worker storage allocation failed");
            memory->initThreadStorage(pcr[i], pcr[i] + 0x1000, 0x1000, 0);
            const auto address = pcr[i];
            workers[i] = std::async(std::launch::async, [&, i, address] {
                PPCContext worker{}; worker.r13.u64 = address;
                worker.r3.u64 = slot; worker.r4.u64 = 0xABCD0000u + i;
                __imp__KeTlsSetValue(worker, base);
                worker.r3.u64 = slot - 1; worker.r4.u64 = 0x12340000u + i;
                __imp__KeTlsSetValue(worker, base);
                ready[i].set_value();
                gate.wait();
                // Read the raw guest slot first, so clearing only in the Get
                // import cannot hide stale values from inline guest access.
                const uint32_t raw = memory->read32(memory->dynamicTls(worker) + slot * 4);
                worker.r3.u64 = slot; __imp__KeTlsGetValue(worker, base);
                return std::array<uint32_t, 2>{raw, worker.r3.u32};
            });
        }
        for (auto& signal : ready)
            check(signal.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                  "TLS worker did not publish its original value");
        const std::array<uint32_t, 3> contexts{ctx.r13.u32, pcr[0], pcr[1]};
        std::array<std::vector<uint8_t>, 3> expected;
        for (unsigned i = 0; i < contexts.size(); ++i) {
            expected[i].assign(base + contexts[i], base + contexts[i] + 0x1000);
            PPCContext snapshot{}; snapshot.r13.u64 = contexts[i];
            const auto relative = memory->dynamicTls(snapshot) + slot * 4 - contexts[i];
            std::fill_n(expected[i].begin() + relative, 4, uint8_t(0));
        }
        // All 64 slots were allocated by NativeRuntime. Recycle exactly 47
        // while the other two threads retain values for its old owner.
        ctx.r3.u64 = slot; __imp__KeTlsFree(ctx, base);
        check(ctx.r3.u32 == 1, "TLS slot release failed");
        __imp__KeTlsAlloc(ctx, base);
        check(ctx.r3.u32 == slot, "TLS slot was not reused");
        bool cleared = true;
        for (unsigned i = 0; i < contexts.size(); ++i)
            cleared &= !memcmp(expected[i].data(), base + contexts[i], expected[i].size());
        releaseGate();
        for (unsigned i = 0; i < workers.size(); ++i) {
            const auto result = workers[i].get();
            std::printf("TlsReuse[worker=%u] raw=%08X import=%08X expected=00000000\n",
                        i, result[0], result[1]);
            cleared &= result[0] == 0 && result[1] == 0;
        }
        check(cleared, "Reallocated TLS slot retained another thread's old value or changed neighboring storage");

        // Once thread storage is freed, reusing its address for ordinary data
        // must remove it from TLS clearing, for both memory release APIs.
        for (unsigned i = 0; i < pcr.size(); ++i) {
            check(i ? memory->releaseContaining(pcr[i] + 64) : memory->release(pcr[i]),
                  "TLS worker storage release failed");
            const auto address = memory->allocate(0x2000, 4096, pcr[i], uint64_t(pcr[i]) + 0x2000);
            check(address == pcr[i], "TLS storage address was not reused for ordinary data");
            memset(base + address, 0xA5, 0x2000);
        }
        ctx.r3.u64 = slot; __imp__KeTlsFree(ctx, base);
        __imp__KeTlsAlloc(ctx, base);
        check(ctx.r3.u32 == slot, "Second TLS reuse failed");
        for (uint32_t address : pcr)
            check(std::all_of(base + address, base + address + 0x2000, [](uint8_t value) { return value == 0xA5; }),
                  "TLS clearing modified memory reused after thread storage release");
    } catch (...) { cleanup(); throw; }
    cleanup();
    puts("TLS reuse clears every live guest slot and preserves released storage, adjacent TLS, and static thread data.");
}
