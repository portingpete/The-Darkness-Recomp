#pragma once
#include "runtime/native/xma_raw_decoder.h"
#include <array>
#include <cmath>

namespace xma_batch_fixture {
struct Buffers {
    std::array<uint32_t, 3> contexts{}, outputs{};
    uint32_t input = 0, records = 0;
    ~Buffers() {
        for (uint32_t context : contexts) if (context) memory->xmaFree(context);
        for (uint32_t output : outputs) if (output) memory->release(output);
        if (input) memory->release(input);
        if (records) memory->release(records);
    }
    static uint32_t physical(uint32_t bytes) {
        const auto result = memory->allocate(bytes, 4096, 0xa0000000, 0xc0000000);
        check(result != 0, "XMA batch physical allocation failed");
        return result;
    }
    static uint32_t alias(uint32_t address) {
        return 0xc0000000u + Memory::physicalAddress(address);
    }
    void create() {
        records = memory->allocate(3 * 96);
        check(records != 0, "XMA batch record allocation failed");
        input = physical(8192);
        for (size_t i = 0; i < contexts.size(); ++i) {
            contexts[i] = memory->xmaCreate();
            check(contexts[i] != 0, "XMA batch context allocation failed");
            outputs[i] = physical(8192);
        }
    }
    void setup(const std::vector<uint8_t>& packets) {
        check(packets.size() >= 4096, "XMA batch needs two source packets");
        memcpy(memory->base() + input, packets.data(), 4096);
        for (size_t i = 0; i < contexts.size(); ++i) {
            const auto context = contexts[i];
            memory->xmaReset(context);
            memset(memory->base() + context, 0, 64);
            memset(memory->base() + outputs[i], 0xA5, 8192);
            memory->write32(context, (16u << 22) | 0x00100001u);
            memory->write32(context + 4, 0xA0100000u); // Stereo, one subframe.
            memory->write32(context + 8, 32);
            memory->write32(context + 20, Memory::physicalAddress(input));
            memory->write32(context + 28, Memory::physicalAddress(outputs[i]));
            memory->write32(records + uint32_t(i) * 96 + 64, context);
        }
    }
    std::vector<uint8_t> snapshot() const {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < contexts.size(); ++i) {
            bytes.insert(bytes.end(), memory->base() + contexts[i], memory->base() + contexts[i] + 64);
            bytes.insert(bytes.end(), memory->base() + outputs[i], memory->base() + outputs[i] + 8192);
        }
        return bytes;
    }
};
struct Protection {
    uint32_t address, size;
    DWORD previous = 0;
    Protection(uint32_t at, uint32_t bytes, DWORD protection) : address(at), size(bytes) {
        check(VirtualProtect(memory->base() + address, size, protection, &previous) != 0,
              "XMA batch fixture protection failed");
    }
    ~Protection() {
        DWORD ignored;
        VirtualProtect(memory->base() + address, size, previous, &ignored);
    }
};
}

static void testXmaBridgeBatch(const std::vector<uint8_t>& packets,
                             const std::vector<int16_t>& expected) {
    using namespace xma_batch_fixture;
    Buffers fixture;
    fixture.create();
    uint32_t failed = 0xDEADBEEFu;
    check(!memory->xmaDecodeBatch({}, failed) && failed == 0, "Empty XMA batch failed");

    fixture.setup(packets);
    for (unsigned pass = 0; pass < 2; ++pass)
        for (auto context : fixture.contexts)
            check(!memory->xmaDecode(context), "Independent XMA reference decode failed");
    const auto independent = fixture.snapshot();
    fixture.setup(packets);
    for (unsigned pass = 0; pass < 2; ++pass)
        check(!memory->xmaDecodeBatch(fixture.contexts, failed) && failed == 0,
              "Ordered XMA batch failed");
    check(fixture.snapshot() == independent, "XMA batch PCM/status differ from ordered independent decoding");
    fixture.setup(packets);
    for (unsigned pass = 0; pass < 2; ++pass)
        check(!memory->xmaDecodeRecords(fixture.records, 3, failed) && failed == 0,
              "Ordered XMA record batch failed");
    check(fixture.snapshot() == independent, "XMA record batch changed ordered PCM/status");
    check(expected.size() >= 512, "XMA batch needs two reference subframes");
    for (auto output : fixture.outputs) for (size_t i = 0; i < 512; ++i) {
        auto* pcm = memory->base() + output + i * 2;
        check(int16_t(uint16_t(pcm[0]) << 8 | pcm[1]) == expected[i], "XMA batch changed reference PCM");
    }

    // Deliberately put the staged records IN context 0's PCM output. Its
    // first subframe overwrites record 1's ID before the old loop reads it.
    // The records API must observe that changed ID, not a prefetched list.
    const uint32_t aliasedRecords = fixture.outputs[0];
    auto setupAliasedRecords = [&] {
        fixture.setup(packets);
        memory->write32(aliasedRecords + 64, fixture.contexts[0]);
        memory->write32(aliasedRecords + 96 + 64, fixture.contexts[1]);
    };
    setupAliasedRecords();
    check(!memory->xmaDecode(memory->read32(aliasedRecords + 64)), "Aliased-record reference prefix failed");
    const uint32_t overwrittenContext = memory->read32(aliasedRecords + 96 + 64);
    check(overwrittenContext != fixture.contexts[1], "PCM fixture did not overwrite the later record ID");
    const char* aliasError = memory->xmaDecode(overwrittenContext);
    check(aliasError != nullptr, "Overwritten record unexpectedly names an owned XMA context");
    const auto aliasReference = fixture.snapshot();
    setupAliasedRecords();
    const char* actualAliasError = memory->xmaDecodeRecords(aliasedRecords, 2, failed);
    check(actualAliasError && !strcmp(actualAliasError, aliasError) && failed == overwrittenContext &&
              fixture.snapshot() == aliasReference,
          "XMA records batch prefetched IDs across aliased PCM writes or changed prefix progress");

    // Repeated IDs are ordered operations, not a set to deduplicate.
    fixture.setup(packets);
    for (unsigned i = 0; i < 2; ++i)
        check(!memory->xmaDecode(fixture.contexts[0]), "Duplicate-ID reference failed");
    const auto repeatedReference = fixture.snapshot();
    fixture.setup(packets);
    const std::array repeated{fixture.contexts[0], fixture.contexts[0]};
    check(!memory->xmaDecodeBatch(repeated, failed) && fixture.snapshot() == repeatedReference,
          "XMA batch deduplicated or reordered repeated contexts");

    // Compare first-error behavior with the uncached entry, including successful
    // prefix progress and untouched failing/suffix contexts and their PCM.
    const uint32_t freed = memory->xmaCreate();
    check(freed && memory->xmaFree(freed), "XMA batch freed-context setup failed");
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        auto ids = fixture.contexts;
        auto setupFailure = [&] {
            fixture.setup(packets);
            if (scenario == 0) memory->write32(ids[1], memory->read32(ids[1]) | 0x1000);
            if (scenario == 1) ids[1] = 0;
            if (scenario == 2) ids[1] = freed;
            if (scenario == 3) ids[1] = fixture.contexts[1] + 1;
        };
        setupFailure();
        check(!memory->xmaDecode(ids[0]), "XMA failure-prefix reference failed");
        const char* referenceError = memory->xmaDecode(ids[1]);
        check(referenceError != nullptr, "XMA failure reference unexpectedly passed");
        const auto referenceState = fixture.snapshot();
        setupFailure();
        const char* batchError = memory->xmaDecodeBatch(ids, failed);
        check(batchError && !strcmp(batchError, referenceError) && failed == ids[1] &&
                  fixture.snapshot() == referenceState,
              "XMA batch changed first-error identity, prefix progress or suffix state");
    }

    // Warm a successful batch, then alter protection at the SAME address.
    // Context and PCM need write rights; compressed input only needs read rights.
    for (unsigned target = 0; target < 3; ++target) {
        for (DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_NOACCESS)}) {
            fixture.setup(packets);
            check(!memory->xmaDecodeBatch(fixture.contexts, failed), "XMA protection warmup failed");
            fixture.setup(packets);
            const auto before = fixture.snapshot();
            const uint32_t address = target == 0 ? fixture.contexts[0] & ~4095u :
                Buffers::alias(target == 1 ? fixture.outputs[0] : fixture.input);
            const bool readableInput = target == 2 && protection == PAGE_READONLY;
            const char* error;
            {
                Protection changed(address, 4096, protection);
                error = memory->xmaDecodeBatch(fixture.contexts, failed);
            }
            check(readableInput ? !error && failed == 0 : error && failed == fixture.contexts[0],
                  "XMA batch reused stale protection or confused read/write rights");
            if (!readableInput) check(fixture.snapshot() == before, "Denied XMA span changed progress or PCM");
            fixture.setup(packets);
            check(!memory->xmaDecodeBatch(fixture.contexts, failed), "Restored XMA protection was not revalidated");
        }
    }

    // Within ONE batch, context 0 caches a read-only input region. Context 1
    // cannot use that same region as writable PCM, even with a full ring.
    fixture.setup(packets);
    memory->write32(fixture.contexts[1] + 28, Memory::physicalAddress(fixture.input));
    memory->write32(fixture.contexts[1] + 4, 0x20100000u);
    std::array<uint8_t, 64> suffix{};
    memcpy(suffix.data(), memory->base() + fixture.contexts[2], suffix.size());
    {
        Protection inputReadOnly(Buffers::alias(fixture.input), 4096, PAGE_READONLY);
        check(memory->xmaDecodeBatch(fixture.contexts, failed) && failed == fixture.contexts[1],
              "Cached readable region bypassed PCM write validation or full-ring ordering");
    }
    check((memory->read32(fixture.contexts[0]) >> 27) == 2 &&
              !memcmp(suffix.data(), memory->base() + fixture.contexts[2], suffix.size()) &&
              !memcmp(packets.data(), memory->base() + fixture.input, 4096),
          "XMA permission failure lost prefix progress, touched suffix or overwrote input");

    for (DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_NOACCESS)}) {
        fixture.setup(packets);
        check(!memory->xmaDecode(fixture.contexts[0]), "Protected-prefix reference failed");
        const auto prefix = fixture.snapshot();
        fixture.setup(packets);
        {
            Protection laterOutput(Buffers::alias(fixture.outputs[1]), 4096, protection);
            check(memory->xmaDecodeBatch(fixture.contexts, failed) && failed == fixture.contexts[1],
                  "XMA batch skipped a later context's output permission check");
        }
        check(fixture.snapshot() == prefix, "Later output failure changed completed prefix or undecoded suffix");
    }

    // Full-ring input validation remains skipped exactly where the original
    // decoder skips it, AFTER the output span has been checked.
    fixture.setup(packets);
    memory->write32(fixture.contexts[0] + 4, 0x20100000u);
    memory->write32(fixture.contexts[0] + 20, 0x60000000u);
    const std::array firstOnly{fixture.contexts[0]};
    const auto fullRing = fixture.snapshot();
    check(!memory->xmaDecodeBatch(firstOnly, failed) && fixture.snapshot() == fullRing,
          "XMA batch reordered full-ring input validation");

    // A validated span crossing a region boundary must still check its tail.
    for (bool inputTail : {false, true}) {
        fixture.setup(packets);
        uint32_t tail;
        if (inputTail) {
            memcpy(memory->base() + fixture.input + 2048, packets.data(), 4096);
            memory->write32(fixture.contexts[0], (16u << 22) | 0x00100002u);
            memory->write32(fixture.contexts[0] + 20, Memory::physicalAddress(fixture.input + 2048));
            tail = Buffers::alias(fixture.input) + 4096;
        } else {
            memory->write32(fixture.contexts[0] + 28, Memory::physicalAddress(fixture.outputs[0] + 2048));
            tail = Buffers::alias(fixture.outputs[0]) + 4096;
        }
        const auto before = fixture.snapshot();
        {
            Protection deniedTail(tail, 4096, PAGE_NOACCESS);
            check(memory->xmaDecodeBatch(fixture.contexts, failed) && failed == fixture.contexts[0],
                  "XMA batch accepted an inaccessible span tail");
        }
        check(fixture.snapshot() == before, "Denied XMA span tail changed context or PCM");
    }

    // Released physical aliases stay mapped but become PAGE_NOACCESS. Reusing
    // their exact addresses must start a new validation lifetime.
    for (bool releaseInput : {false, true}) {
        fixture.setup(packets);
        check(!memory->xmaDecodeBatch(fixture.contexts, failed), "XMA release warmup failed");
        fixture.setup(packets);
        auto& allocation = releaseInput ? fixture.input : fixture.outputs[0];
        const auto address = allocation;
        check(memory->release(address), "XMA batch release failed");
        allocation = 0;
        check(memory->xmaDecodeBatch(fixture.contexts, failed) && failed == fixture.contexts[0],
              "XMA batch reused a cached released region");
        allocation = memory->allocate(8192, 4096, address, uint64_t(address) + 8192);
        check(allocation == address, "XMA batch expected exact physical address reuse");
        fixture.setup(packets);
        check(!memory->xmaDecodeBatch(fixture.contexts, failed), "XMA batch rejected valid address reuse");
    }
    fixture.setup(packets);
    check(!memory->xmaDecodeBatch(fixture.contexts, failed), "XMA context-reuse warmup failed");
    const uint32_t oldContext = fixture.contexts[0];
    check(memory->xmaFree(oldContext), "XMA batch context release failed");
    check(memory->xmaDecodeBatch(fixture.contexts, failed) && failed == oldContext,
          "XMA batch accepted freed context ownership");
    fixture.contexts[0] = 0;
    fixture.contexts[0] = memory->xmaCreate();
    check(fixture.contexts[0] == oldContext, "XMA batch expected exact context-slot reuse");
    fixture.setup(packets);
    for (unsigned pass = 0; pass < 2; ++pass)
        check(!memory->xmaDecodeBatch(fixture.contexts, failed), "XMA batch failed after context-slot reuse");
    check(fixture.snapshot() == independent, "XMA batch retained stale decoder state across slot reuse");
    puts("XMA batches preserve ordered PCM/status, first-error progress, span permissions and per-batch validation lifetime.");
}

static void testXmaBridgeRejectedInputUpdate(uint32_t context, uint32_t input, uint32_t output,
                                           const std::vector<uint8_t>& packets,
                                           const std::vector<int16_t>& expected) {
    auto* base = memory->base();
    check(packets.size() >= 4096 && expected.size() >= 512,
          "Rejected-input reference needs two packets and two stereo subframes");
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        memory->xmaReset(context);
        memset(base + context, 0, 64);
        memcpy(base + input, packets.data(), 4096);
        memset(base + output, 0xA5, 8192);
        // Keep buffer 1 active after accepting the first of its two packets.
        memory->write32(context, (16u << 22) | 0x00200000u);
        memory->write32(context + 4, 0xA0100002u);
        memory->write32(context + 8, 32);
        memory->write32(context + 16, 0x80000000u);
        memory->write32(context + 24, Memory::physicalAddress(input));
        memory->write32(context + 28, Memory::physicalAddress(output));
        check(memory->xmaDecode(context) == nullptr, "Rejected-input first decode failed");
        check((memory->read32(context) & 0x00300000u) == 0x00200000u &&
                  (memory->read32(context) >> 27) == 2,
              "Rejected-input fixture did not retain buffer 1 and one subframe");
        std::array<uint8_t, 64> validContext{};
        memcpy(validContext.data(), base + context, validContext.size());

        // Offer a new buffer 0, then illegally clear, replace or resize the
        // unaccepted buffer 1. Rejecting the pair must not register buffer 0.
        memory->write32(context, memory->read32(context) | 0x00100001u);
        memory->write32(context + 20, Memory::physicalAddress(input + 2048));
        if (invalid == 0) memory->write32(context, memory->read32(context) & ~0x00200000u);
        if (invalid == 1) memory->write32(context + 24, Memory::physicalAddress(output));
        if (invalid == 2) memory->write32(context + 4, memory->read32(context + 4) - 1);
        std::array<uint8_t, 64> rejectedContext{};
        memcpy(rejectedContext.data(), base + context, rejectedContext.size());
        const std::vector<uint8_t> beforePcm(base + output, base + output + 8192);
        check(memory->xmaDecode(context) != nullptr, "XMA accepted a changed unaccepted input");
        check(!memcmp(rejectedContext.data(), base + context, rejectedContext.size()) &&
                  !memcmp(beforePcm.data(), base + output, beforePcm.size()),
              "Rejected input update changed context progress or PCM");

        memcpy(base + context, validContext.data(), validContext.size());
        check(memory->xmaDecode(context) == nullptr,
              "Rejected input pair registered buffer 0 and poisoned the restored decoder");
        check((memory->read32(context) >> 27) == 4,
              "Restored input update did not advance exactly one subframe");
        for (uint32_t i = 0; i < 512; ++i) {
            const auto sample = int16_t(uint16_t(base[output + 2*i]) << 8 |
                                        uint16_t(base[output + 2*i + 1]));
            check(sample == expected[i], "Rejected input update lost or replayed buffered PCM");
        }
        check(std::all_of(base + output + 1024, base + output + 8192,
                          [](uint8_t value) { return value == 0xA5; }),
              "Restored input update wrote beyond two subframes");
    }
    memory->xmaReset(context);
    puts("XMA rejected input pairs preserve decoder state and resume exact buffered PCM.");
}

static void testXmaBridgeBufferedSeek(uint32_t context, uint32_t input, uint32_t output,
                                    const std::vector<uint8_t>& packets,
                                    const std::vector<int16_t>& expected) {
    auto* base = memory->base();
    check(expected.size() >= 512, "Buffered-seek reference needs two stereo subframes");
    auto setup = [&] {
        memory->xmaReset(context);
        memset(base + context, 0, 64);
        memcpy(base + input, packets.data(), 2048);
        memset(base + output, 0xA5, 8192);
        memory->write32(context, (16u << 22) | 0x00100001u);
        // Stereo 24 kHz, one 128-sample subframe per call, a 1024-sample ring.
        memory->write32(context + 4, 0xA0100000u);
        memory->write32(context + 8, 32);
        memory->write32(context + 20, Memory::physicalAddress(input));
        memory->write32(context + 28, Memory::physicalAddress(output));
        check(memory->xmaDecode(context) == nullptr, "Buffered-seek first decode failed");
        check(!(memory->read32(context) & 0x00300000u),
              "Buffered-seek packet was not accepted before the seek");
        check((memory->read32(context) >> 27) == 2 &&
                  (memory->read32(context + 4) & 0x80000000u),
              "Buffered-seek fixture must retain PCM with room for another subframe");
        // Acceptance ends the guest input's lifetime, not the decoder's frame.
        // The remaining PCM must survive input reuse and an empty input image.
        memset(base + input, 0xDD, 2048);
        memory->write32(context, memory->read32(context) & ~4095u);
        memory->write32(context + 20, 0);
    };
    auto checkTwoSubframes = [&] {
        check((memory->read32(context) >> 27) == 4,
              "Buffered-seek drain did not advance exactly one subframe");
        for (uint32_t i = 0; i < 512; ++i) {
            int16_t sample = int16_t(uint16_t(base[output + 2*i]) << 8 |
                                     uint16_t(base[output + 2*i + 1]));
            check(sample == expected[i], "Buffered-seek drain lost or replayed retained PCM");
        }
        check(std::all_of(base + output + 1024, base + output + 8192,
                          [](uint8_t value) { return value == 0xA5; }),
              "Buffered-seek drain wrote beyond two subframes");
    };

    // Positive control: accepted input can disappear while its PCM drains.
    setup();
    check(memory->xmaDecode(context) == nullptr, "Accepted input prevented buffered PCM drain");
    checkTwoSubframes();

    // Both an in-packet seek and a packet-sized jump must be rejected while
    // the decoder still holds the first frame, even with no active input.
    for (uint32_t offset : {64u, 16416u}) {
        setup();
        const uint32_t published = memory->read32(context + 8);
        memory->write32(context + 8, offset);
        std::array<uint8_t, 64> beforeContext{};
        memcpy(beforeContext.data(), base + context, beforeContext.size());
        std::vector<uint8_t> beforePcm(base + output, base + output + 8192);
        const char* error = memory->xmaDecode(context);
        check(error != nullptr, "XMA accepted a frame-offset change while accepted input still had buffered PCM");
        check(!memcmp(beforeContext.data(), base + context, beforeContext.size()) &&
                  !memcmp(beforePcm.data(), base + output, beforePcm.size()),
              "Rejected buffered seek changed context progress or PCM");
        memory->write32(context + 8, published);
        check(memory->xmaDecode(context) == nullptr, "Rejected buffered seek poisoned the decoder");
        checkTwoSubframes();
    }
    memory->xmaReset(context);
    puts("XMA retained PCM survives input reuse; unsupported buffered seeks reject without consuming samples.");
}

static void testXmaBridge(PPCContext& ctx, const char* capturePath) {
    std::ifstream file(capturePath, std::ios::binary);
    std::vector<uint8_t> capture((std::istreambuf_iterator<char>(file)), {});
    check(capture.size() >= 180 && !memcmp(capture.data(), "XMACAP01", 8), "Invalid XMA capture");
    auto le32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 |
        uint32_t(p[2])<<16 | uint32_t(p[3])<<24; };
    std::vector<uint8_t> packets;
    size_t at = 180;
    for (int b = 0; b < 2; ++b) {
        check(at + 16 <= capture.size() && le32(capture.data()+at+12) == 1, "Invalid capture packets");
        at += 16; check(at + 2048 <= capture.size(), "Truncated capture");
        packets.insert(packets.end(), capture.begin()+at, capture.begin()+at+2048); at += 2048;
    }
    XmaRawDecoder reference;
    check(reference.open(24000, 2), "Bridge reference open failed");
    std::vector<int16_t> expected;
    float decoded[1024];
    for (int b = 0; b < 2; ++b) {
        check(reference.push(packets.data()+2048*b) == 0, "Reference packet failed");
        for (;;) {
            int got = reference.read(decoded, 512);
            check(got >= 0, "Reference read failed"); if (!got) break;
            for (int i = 0; i < got*2; ++i)
                expected.push_back(int16_t(std::lrintf(std::clamp(decoded[i], -1.f, 1.f)*32767.f)));
        }
    }
    auto* base = memory->base();
    ctx.r3.u64 = 0; sub_828B0B10(ctx, base);
    uint32_t records = memory->allocate(96), group = memory->allocate(16);
    // The SDK stores physical offsets in the XMA image and CPU pointers in
    // its surrounding record. These must use the shared physical backing.
    auto physical = [&](uint32_t size) { return memory->allocate(size,65536,0xa0000000,0xc0000000); };
    uint32_t input = physical(4096), output = physical(8192);
    uint32_t overlap = physical(512), outCell = memory->allocate(4);
    check(records && group && input && output && overlap && outCell, "Bridge allocation failed");
    memset(base+records, 0, 96); memcpy(base+input, packets.data(), 4096);
    memory->write32(group, 1); memory->write32(group+4, 0); memory->write32(group+8, records);
    ctx.r3.u64 = group; sub_828B1840(ctx, base);
    check(ctx.r3.u32 == 0, "Bridge original group creation failed");
    uint32_t context = memory->read32(records+64);
    auto setup = [&](bool staged, bool combined, bool onlyFirst = false) {
        memset(base+records, 0, 48); memset(base+output, 0xA5, 8192);
        memory->write32(records, (16u<<22) | (combined ? 0x00100002 : onlyFirst ? 0x00100001 : 0x00300001));
        memory->write32(records+4, 0xA0800000 | (combined || onlyFirst ? 0 : 1));
        memory->write32(records+8, 32);
        memory->write32(records+20, Memory::physicalAddress(input));
        memory->write32(records+24, Memory::physicalAddress(input+2048));
        memory->write32(records+28, Memory::physicalAddress(output));
        memory->write32(records+32, Memory::physicalAddress(overlap));
        memory->write32(records+68, output); memory->write32(records+72, overlap);
        memory->write32(records+76, 0); base[records+82] = base[records+83] = 0;
        memory->write32(records+84, input); memory->write32(records+88, input+2048);
        memory->write32(group+4, staged ? 0x60000 : 0x40000);
        if (!staged) memcpy(base+context, base+records, 48);
    };
    auto kick = [&] {
        ctx.r3.u64 = group; sub_828B1BA8(ctx, base); check(ctx.r3.u32 == 0, "Bridge submit failed");
        ctx.r3.u64 = group; sub_828B1A58(ctx, base); check(ctx.r3.u32 == 1, "Bridge poll not ready");
    };
    auto available = [&] { ctx.r3.u64 = group; ctx.r4.u64 = 0; sub_828B1048(ctx, base); return ctx.r3.u32; };
    auto consume = [&](uint32_t n, std::vector<int16_t>& got) {
        ctx.r3.u64 = group; ctx.r4.u64 = 0; ctx.r5.u64 = n; ctx.r6.u64 = outCell;
        sub_828B10C0(ctx, base);
        uint32_t count = ctx.r3.u32, ptr = memory->read32(outCell);
        check(count && count <= n && ptr >= output && uint64_t(ptr)+count*4 <= output+4096,
              "Original consumer returned invalid extent");
        for (uint32_t i = 0; i < count*2; ++i)
            got.push_back(int16_t(uint16_t(base[ptr+2*i])<<8 | base[ptr+2*i+1]));
    };
    auto reset = [&] {
        ctx.r3.u64 = group; ctx.r4.u64 = 0; sub_828B13A0(ctx, base);
        check(ctx.r3.u32 == 0, "Original context reset failed");
    };
    for (int scenario = 0; scenario < 3; ++scenario) {
        if (scenario) reset();
        setup(scenario != 1, scenario == 2);
        kick(); check(available() == 1024, "First packet did not fill ring with real PCM");
        // Poll normalizes accepted input pointers/counts in the staged record.
        std::vector<uint8_t> full(base+records, base+records+48);
        std::vector<uint8_t> pcmFull(base+output, base+output+4096);
        kick();
        check(!memcmp(full.data(), base+records, 48) && !memcmp(pcmFull.data(), base+output, 4096),
              "Full ring consumed input or overwrote unread PCM");
        std::vector<int16_t> got;
        constexpr uint32_t sizes[] = {37, 129, 251, 503};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t n = available();
            if (n) consume((std::min)(n, sizes[i%4]), got);
            kick();
            if (!available()) break;
        }
        check(got == expected, "Original submit/poll/consumer lost, replayed or reordered PCM");
        check(!(memory->read32(records) & 0x00300000), "Accepted inputs still marked valid");
        kick(); check(available() == 0, "Empty input synthesized EOF tail");
        check(std::all_of(base+output+4096, base+output+8192, [](uint8_t v){return v==0xA5;}),
              "PCM ring overflowed");
    }
    // Reuse the exact input address without resetting the stream: accepting a
    // padded packet copy must allow the original input allocation to be reused.
    reset(); setup(true, false, true); kick();
    check(!(memory->read32(records) & 0x00100000), "Packet copy not released to guest");
    memcpy(base+input, packets.data()+2048, 2048);
    memory->write32(records, memory->read32(records) | 0x00100001);
    memory->write32(records+20, Memory::physicalAddress(input)); memory->write32(records+84, input);
    std::vector<int16_t> reused;
    for (uint32_t i=0; i<64; ++i) {
        if (auto n=available()) consume(n, reused);
        kick(); if (!available()) break;
    }
    check(reused == expected, "Same-address input generation replayed or lost PCM");
    // Reject unsupported seek/loop and invalid spans before writing any PCM.
    reset(); memcpy(base+input, packets.data(), 4096); setup(false, false);
    memory->write32(context, memory->read32(context) | 0x1000);
    check(memory->xmaDecode(context) != nullptr && base[output] == 0xA5, "Loop silently accepted");
    setup(false, false); memory->write32(context+20, 0x60000000);
    check(memory->xmaDecode(context) != nullptr && base[output] == 0xA5, "Unmapped input accepted");
    setup(false, false); memory->write32(context+20, input);
    check(memory->xmaDecode(context) != nullptr && base[output] == 0xA5, "CPU pointer accepted as physical input");
    setup(false, false); memory->write32(context+28, 0x1fffff80);
    check(memory->xmaDecode(context) != nullptr && base[output] == 0xA5, "PCM ring escaped physical backing");
    setup(false, false); memory->write32(context+8, 64);
    check(memory->xmaDecode(context) != nullptr && base[output] == 0xA5, "Arbitrary initial seek accepted");
    setup(false, false); memory->write32(context, (1u<<22) | 0x00300001);
    check(memory->xmaDecode(context) != nullptr && base[output] == 0xA5, "Unfillable stereo ring accepted");
    reset(); setup(true, false); kick();
    uint32_t oldContext = context;
    ctx.r3.u64 = group; sub_828B0DE0(ctx, base); check(ctx.r3.u32 == 0, "Bridge release failed");
    check(!memory->xmaOwned(oldContext) && memory->xmaDecode(oldContext), "Freed decoder still callable");
    ctx.r3.u64 = group; sub_828B1840(ctx, base); check(ctx.r3.u32 == 0, "Bridge recreate failed");
    context = memory->read32(records+64); check(context == oldContext, "Expected slot reuse");
    setup(true, false); kick(); std::vector<int16_t> first;
    consume(1024, first);
    check(std::equal(first.begin(), first.end(), expected.begin()), "Reused context retained old decoder state");
    testXmaBridgeRejectedInputUpdate(context, input, output, packets, expected);
    testXmaBridgeBufferedSeek(context, input, output, packets, expected);
    testXmaBridgeBatch(packets, expected);
    ctx.r3.u64 = group; sub_828B0DE0(ctx, base);
    for (uint32_t p : {records, group, input, output, overlap, outCell}) memory->release(p);
    printf("XMA bridge: %zu samples/ch through original live/staged submit, poll and partial consumer; wrap, backpressure, input reuse, reset and release verified.\n", expected.size()/2);
}
