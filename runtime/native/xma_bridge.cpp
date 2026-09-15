#include "xma_bridge.h"
#include "xma_raw_decoder.h"
#include "audio_refill_trace.h"
#include "runtime.h"
#include "renderer/engine/engine_performance.h"
#include <algorithm>
#include <cmath>

namespace DarkRecomp::Native {
namespace {
// XMA fields hold physical offsets; validation and decoding use the C alias.
constexpr uint32_t physicalAlias(uint32_t offset) { return 0xc0000000u + offset; }
}
struct XmaBridge::ValidationCache {
    // Only owned XMA contexts and C-alias physical buffers are queried here.
    // Memory serializes their commit/protection/release across this lifetime.
    // Worker-stack guard changes are outside both address domains.
    std::array<MEMORY_BASIC_INFORMATION, 16> regions{};
    size_t count = 0, next = 0;
    bool reuse;
    explicit ValidationCache(bool enabled) : reuse(enabled) {}

    bool accessible(Memory& memory, uint32_t at, uint32_t size, bool write = false) {
        if (!at || !size || uint64_t(at) + size > PPC_MEMORY_SIZE) return false;
        auto* cursor = memory.base() + at;
        auto* end = cursor + size;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info{};
            bool found = false;
            const auto address = reinterpret_cast<uintptr_t>(cursor);
            if (reuse) for (size_t i = 0; i < count; ++i) {
                const auto first = reinterpret_cast<uintptr_t>(regions[i].BaseAddress);
                if (address >= first && address - first < regions[i].RegionSize) {
                    info = regions[i]; found = true; break;
                }
            }
            if ((!found && !engineProfileVirtualQuery(EnginePhase::queryXma, cursor, &info, sizeof(info))) || info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
            DWORD p = info.Protect & 255;
            bool writable = p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                            p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
            if (write ? !writable : (!writable && p != PAGE_READONLY && p != PAGE_EXECUTE_READ)) return false;
            // Cache only positive regions, but recheck permissions for EVERY use:
            // a region accepted as compressed input need not be writable PCM.
            if (reuse && !found) {
                regions[next] = info;
                next = (next + 1) % regions.size();
                count = (std::min)(count + 1, regions.size());
            }
            cursor = (std::min)(end, static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize);
        }
        return true;
    }
    bool accessiblePhysical(Memory& memory, uint32_t offset, uint32_t size, bool write = false) {
        return size && uint64_t(offset) + size <= 0x20000000ull &&
               accessible(memory, physicalAlias(offset), size, write);
    }
};
struct XmaBridge::Stream {
    XmaRawDecoder decoder;
    uint32_t packetIndex[2]{};
    uint32_t inputPointer[2]{};
    uint32_t inputCount[2]{};
    bool inputActive[2]{};
    uint32_t publishedOffset = 0;
    uint64_t packets = 0, samples = 0;
};
XmaBridge::XmaBridge() = default;
XmaBridge::~XmaBridge() = default;
void XmaBridge::reset(uint32_t slot) { if (slot < streams_.size()) streams_[slot].reset(); }

const char* XmaBridge::decode(Memory& memory, uint32_t context, uint32_t slot) {
    ValidationCache validation(false);
    return decodeValidated(memory, context, slot, validation);
}
const char* XmaBridge::decodeBatch(Memory& memory, std::span<const uint32_t> contexts,
                                  uint32_t& failedContext) {
    return decodeSequence(memory, contexts.size(), [&](size_t i) { return contexts[i]; }, failedContext);
}
const char* XmaBridge::decodeRecords(Memory& memory, uint32_t records, uint32_t count,
                                    uint32_t firstContext, uint32_t& failedContext) {
    return decodeSequence(memory, count, [&](size_t i) {
        // Memory checked the first ID before constructing the bridge. Do not
        // read it twice, or prefetch any subsequent ID across a decode.
        return i ? memory.read32(records + uint32_t(i) * 96 + 64) : firstContext;
    }, failedContext);
}
template<class ContextAt>
const char* XmaBridge::decodeSequence(Memory& memory, size_t count, ContextAt contextAt,
                                     uint32_t& failedContext) {
    recordXmaProfileBatch(count);
    ValidationCache validation(true);
    const uint32_t pool = memory.xmaPoolBase();
    for (size_t i = 0; i < count; ++i) {
        const uint32_t context = contextAt(i);
        failedContext = context;
        if (!memory.xmaOwned(context)) return "context is not owned by this address space";
        const uint32_t slot = (context - pool) / 64;
        if (const char* error = decodeValidated(memory, context, slot, validation)) return error;
    }
    failedContext = 0;
    return nullptr;
}
const char* XmaBridge::decodeValidated(Memory& memory, uint32_t context, uint32_t slot,
                                      ValidationCache& validation) {
    auto* stats = activeAudioRefill;
    AudioRefillTimer timer(stats ? &stats->decodeTicks : nullptr,
                           stats ? &stats->maxDecodeTicks : nullptr);
    if (stats) ++stats->contexts;
    if (slot >= streams_.size() || !validation.accessible(memory, context, 64, true)) return "invalid owned context";
    uint32_t image[16];
    for (uint32_t i = 0; i < 16; ++i) image[i] = memory.read32(context + 4*i);
    auto& state = streams_[slot];
    bool inputValid[2] = {bool(image[0] & 0x00100000), bool(image[0] & 0x00200000)};
    if (stats) stats->emptyInputs += !inputValid[0] && !inputValid[1];
    if (!state && !inputValid[0] && !inputValid[1]) return nullptr;
    if ((image[0] & 0x000ff000) || (image[1] & 0x070ff000) ||
        (image[3] & 0x03ffffff) || (image[4] & 0x03ffffff))
        return "unsupported loop or subframe seek";
    uint32_t blocks = (image[0] >> 22) & 31;
    uint32_t write = (image[0] >> 27) & 31, read = image[9] & 31;
    uint32_t quota = ((image[1] >> 20) & 15) * 128;
    int channels = ((image[1] >> 29) & 1) + 1;
    constexpr int rates[] = {24000, 32000, 44100, 48000};
    int rate = rates[(image[1] >> 27) & 3];
    if (blocks < uint32_t(channels) || write >= blocks || read >= blocks || !quota ||
        !validation.accessiblePhysical(memory, image[7], blocks * 256, true)) return "invalid PCM ring or subframe quota";
    if (!(image[1] & 0x80000000)) {
        if (stats) ++stats->fullRings;
        return nullptr; // Full ring; do not consume input.
    }
    uint32_t counts[2] = {image[0] & 4095, image[1] & 4095};
    for (uint32_t b = 0; b < 2; ++b)
        if (inputValid[b] && (!counts[b] || !validation.accessiblePhysical(memory, image[5+b], counts[b] * 2048)))
            return "invalid compressed input span";
    if (!state) {
        uint32_t current = image[4] >> 31;
        if (!inputValid[current]) current ^= 1;
        if (!inputValid[current]) return nullptr;
        // Whole-packet entry only. Arbitrary seeks must not silently decode from zero.
        uint32_t header = memory.read32(physicalAlias(image[5+current]));
        uint32_t firstFrame = 32 + ((header >> 11) & 0x7fff);
        if ((image[2] & 0x03ffffff) != firstFrame) return "unsupported initial frame bit offset";
        auto created = std::make_unique<Stream>();
        if (!created->decoder.open(rate, channels)) return "raw decoder initialization failed";
        created->publishedOffset = image[2] & 0x03ffffff;
        state = std::move(created);
    } else if (state->decoder.rate() != rate || state->decoder.channels() != channels) {
        return "format changed without context reset";
    }
    // Accepting the compressed input releases guest memory, but retained PCM
    // and codec history still belong to this stream until an explicit reset.
    if (state->publishedOffset != (image[2] & 0x03ffffff))
        return "frame bit offset changed without context reset";
    // Validate both inputs before registering either. A rejected update must
    // leave the decoder usable when the caller restores its previous context.
    for (uint32_t b = 0; b < 2; ++b) {
        if (!state->inputActive[b]) continue;
        if (!inputValid[b]) return "compressed input cleared before acceptance";
        if (state->inputPointer[b] != image[5+b] || state->inputCount[b] != counts[b])
            return "compressed input replaced before acceptance";
    }
    for (uint32_t b = 0; b < 2; ++b) {
        if (inputValid[b] && !state->inputActive[b]) {
            state->inputPointer[b] = image[5+b]; state->inputCount[b] = counts[b];
            state->packetIndex[b] = 0; state->inputActive[b] = true;
        }
    }
    uint32_t capacity = blocks * 256;
    uint32_t used = ((write + blocks - read) % blocks) * 256;
    uint32_t freeBytes = capacity - used;
    uint32_t remaining = (std::min)(quota, freeBytes / (2*channels));
    if (stats) { stats->quotaFrames += quota; stats->roomFrames += freeBytes / (2*channels); }
    // Context offsets advance in 256-byte blocks, so retain sub-block output in
    // the decoder rather than exposing a rounded-up amount of PCM.
    remaining -= remaining % 128;
    uint32_t produced = 0;
    float pcm[256]; // one 128-sample mono/stereo subframe
    while (remaining >= 128) {
        int got = state->decoder.read(pcm, 128);
        if (got < 0) return "raw packet decode failed";
        if (!got) {
            uint32_t b = image[4] >> 31;
            if (!inputValid[b]) b ^= 1;
            if (!inputValid[b]) break; // No EOF: decoder keeps reservoir and overlap.
            uint32_t at = physicalAlias(image[5+b]) + state->packetIndex[b] * 2048;
            uint32_t header = memory.read32(at);
            if (header & 255) return "unsupported interleaved packet skip";
            int accepted = state->decoder.push(memory.base() + at);
            if (accepted != 0) return "raw packet acceptance failed";
            ++state->packetIndex[b]; ++state->packets;
            if (stats) ++stats->packets;
            // FFmpeg owns a padded copy now; the guest can safely reuse it.
            if (state->packetIndex[b] == counts[b]) {
                inputValid[b] = false;
                state->inputActive[b] = false;
                image[0] &= ~(0x00100000u << b);
                image[4] = (image[4] & 0x7fffffff) | ((b ^ 1) << 31);
                image[2] = (image[2] & 0xfc000000) | 32;
            } else {
                image[4] = (image[4] & 0x7fffffff) | (b << 31);
                image[2] = (image[2] & 0xfc000000) | (state->packetIndex[b] * 16384 + 32);
            }
            continue;
        }
        if (got != 128) return "raw decoder returned a partial subframe";
        uint32_t cursor = (write * 256 + produced * 2 * channels) % capacity;
        for (uint32_t i = 0; i < uint32_t(got * channels); ++i) {
            // Saturating PCM quantization; this clamps amplitude, never counts.
            float value = std::clamp(pcm[i], -1.0f, 1.0f);
            int32_t sample = int32_t(std::lrintf(value * 32767.0f));
            uint16_t bits = uint16_t(int16_t(sample));
            uint32_t out = physicalAlias(image[7]) + (cursor + i*2) % capacity;
            memory.base()[out] = uint8_t(bits >> 8);
            memory.base()[out + 1] = uint8_t(bits);
        }
        produced += got; remaining -= got;
    }
    uint32_t bytes = produced * 2 * channels;
    uint32_t nextWrite = (write + bytes / 256) % blocks;
    image[0] = (image[0] & 0x07ffffff) | (nextWrite << 27);
    if (bytes == freeBytes) image[1] &= ~0x80000000u;
    // Publish only amounts actually copied/decoded; no synthetic completion.
    for (uint32_t i : {0u, 1u, 2u, 4u}) memory.write32(context + 4*i, image[i]);
    state->publishedOffset = image[2] & 0x03ffffff;
    state->samples += produced;
    if (stats) stats->producedFrames += produced;
    static std::atomic<uint32_t> logs{0};
    if (produced && logs.fetch_add(1) < 12)
        fprintf(stderr, "[XMA] context=0x%08X samples=%u/ch total=%llu packets=%llu ring=%u/%u valid=%u\n",
                context, produced, state->samples, state->packets, nextWrite, blocks, unsigned(image[1] >> 31));
    return nullptr;
}
}
