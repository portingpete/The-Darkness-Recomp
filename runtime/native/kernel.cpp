#include "runtime.h"
#include "audio_driver.h"
#include "audio_resampler_trace.h"
#include "audio_refill_trace.h"
#include "input.h"
#include "display_mode.h"
#include "thread_topology.h"
#include "native_timed_wait.h"
#include "renderer/engine/engine_performance.h"
#include "renderer/d3d11/display_context_d3d11.h"
#include "objects.h"
#include "storage.h"
#include "ppc_image_metadata.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <string>
#include <optional>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <float.h>
#include <list>
#include <condition_variable>
#include <bcrypt.h>

using namespace DarkRecomp::Native;
// Title SDK compatibility profile 2.0.5632.0, packed as major 4 bits,
// minor 4 bits, build 16 bits, qfe 8 bits. This is the existing kernel
// profile choice, not the host Windows version.
constexpr uint32_t kXboxVersionPacked = 0x20160000;
static DarkRecomp::CDisplayContextD3D11* nativeDisplay = nullptr;
static bool guestBufferAccessible(uint32_t address, uint32_t bytes, bool writable = false) {
    if (!memory || !bytes || uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    MEMORY_BASIC_INFORMATION info{};
    auto* pointer = memory->base() + address;
    if (!engineProfileVirtualQuery(EnginePhase::queryKernel, pointer, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD protect = info.Protect & 0xffu;
    if (writable && protect != PAGE_READWRITE && protect != PAGE_EXECUTE_READWRITE &&
        protect != PAGE_WRITECOPY && protect != PAGE_EXECUTE_WRITECOPY) return false;
    const auto original = info;
    uint32_t cursor = address;
    for (;;) {
        auto* regionEnd = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
        if (pointer + bytes <= regionEnd) return true;
        // Preserve the old single-region rule. Only our artificial C-view
        // boundaries may be crossed, never a genuine protection/state split.
        if (cursor < Memory::cAliasBegin || cursor >= Memory::cAliasEnd) return false;
        const uint32_t next = Memory::cViewEnd(cursor);
        if (next >= Memory::cAliasEnd || regionEnd != memory->base()+next ||
            info.AllocationBase != memory->base()+(next-Memory::cViewBytes)) return false;
        if (!engineProfileVirtualQuery(EnginePhase::queryKernel, memory->base()+next, &info, sizeof(info)) ||
            info.AllocationBase != memory->base()+next || info.State != original.State ||
            info.Protect != original.Protect || info.Type != original.Type ||
            info.AllocationProtect != original.AllocationProtect) return false;
        cursor = next;
    }
}
static bool guestOutputAccessible(uint32_t address, uint32_t bytes) {
    if (!memory || !address || !bytes || uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    auto* cursor = memory->base() + address;
    auto* end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!engineProfileVirtualQuery(EnginePhase::queryKernel, cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        cursor = (std::min)(end, static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize);
    }
    return true;
}
static void registerCurrentThread();
void CompleteGpuFenceMidAsmHook() {}
void SkipMissingFontLaunchReturn() {}
PPC_FUNC(sub_828A7A68) {
    __imp__XamShowDirtyDiscErrorUI(ctx, base);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 0;
    sub_828AABD0(ctx, base);
}
namespace {
// The owned XMA pool lives in Memory (xmaCreate/xmaFree/xmaPoolBase) so its
// lifetime ends with the address space. This unit maps guest arguments to
// that contract and validates native access rights.
bool guestBufferWritable(uint32_t address, uint32_t bytes) {
    return guestBufferAccessible(address, bytes, true);
}

}
PPC_FUNC(sub_828B0B10) {
    // Native engine-facing pool discovery: publish the real owned backing
    // instead of the unimplemented hardware register. The original leaf only
    // uses r10/r11, so all input registers are preserved.
    memory->write32(0x82A49AD0u, memory->xmaPoolBase());
}
PPC_FUNC(__imp__XMACreateContext) {
    uint32_t out = ctx.r3.u32;
    if (!out || !guestBufferWritable(out, 4)) { ctx.r3.u64 = 0xc000000d; return; }
    uint32_t context = memory->xmaCreate();
    memory->write32(out, context);
    ctx.r3.u64 = context ? 0 : 0xc0000017;
}
PPC_FUNC(__imp__XMAReleaseContext) {
    ctx.r3.u64 = memory->xmaFree(ctx.r3.u32) ? 0 : 0xc000000d;
}
extern "C" {
PPC_FUNC(__imp__sub_828B1BA8);
PPC_FUNC(__imp__sub_827D9188);
PPC_FUNC(__imp__sub_828B13A0);
PPC_FUNC(__imp__sub_828C5060);
PPC_FUNC(__imp__sub_82828518);
PPC_FUNC(__imp__sub_8281E758);
PPC_FUNC(__imp__sub_828248D0);
PPC_FUNC(__imp__sub_8281D758);
PPC_FUNC(__imp__sub_8281DB98);
PPC_FUNC(__imp__sub_8281DF58);
PPC_FUNC(__imp__sub_8281E398);
PPC_FUNC(__imp__sub_8281D308);
PPC_FUNC(__imp__sub_82820DD8);
PPC_FUNC(__imp__sub_82822988);
PPC_FUNC(__imp__sub_82828018);
PPC_FUNC(__imp__sub_82828020);
PPC_FUNC(__imp__sub_820C60F8);
PPC_FUNC(__imp__sub_820F5F28);
}
namespace {
thread_local uint32_t audioEvidenceVoice = 0;
void audioEvidenceWord(uint8_t* destination, uint32_t value) {
    value = _byteswap_ulong(value);
    std::memcpy(destination, &value, 4);
}
void audioEvidenceFloat(uint8_t* destination, float value) {
    audioEvidenceWord(destination, std::bit_cast<uint32_t>(value));
}
uint32_t audioEvidenceHandle(uint32_t pool, uint32_t handle) {
    return (handle & ~3u) ? pool + handle : handle;
}
void audioEvidenceBuffer(uint8_t* destination, uint32_t pool, uint32_t voice) {
    const uint32_t handle = (uint32_t(memory->base()[voice + 14]) << 8) | memory->base()[voice + 15];
    const uint32_t buffer = audioEvidenceHandle(pool, handle);
    if (buffer && guestBufferAccessible(buffer, 16))
        std::memcpy(destination, memory->base() + buffer + 4, 8);
}
void audioEvidenceEdges(AudioResamplerTrace::Record& row, uint32_t output,
                        uint32_t frames, uint32_t channels) {
    const uint32_t stride = ((channels + 3) / 4) * 16;
    if (!frames || frames > 256 || !channels || channels > 16 ||
        !guestBufferAccessible(output, frames * stride)) return;
    for (uint32_t c = 0; c < (std::min)(channels, 6u); ++c) {
        row.first[c] = std::bit_cast<float>(memory->read32(output + c * 4));
        row.last[c] = std::bit_cast<float>(memory->read32(output + (frames - 1) * stride + c * 4));
    }
}
}
PPC_FUNC(sub_8281E758) {
    AudioResamplerTrace::Call evidence;
    if (audioResamplerTrace.sourceWindow()) audioResamplerTrace.begin(evidence);
    auto* row = evidence.record;
    const uint32_t descriptor = ctx.r3.u32, parameters = ctx.r5.u32, state = ctx.r6.u32;
    uint32_t pool = 0, voice = 0, output = 0, frames = 0, channels = 0;
    struct RestoreVoice {
        uint32_t previous = audioEvidenceVoice;
        ~RestoreVoice() { audioEvidenceVoice = previous; }
    } restoreVoice;
    if (row) {
        row->caller = 0x8281E758;
        if (guestBufferAccessible(0x82A695EC, 8) && guestBufferAccessible(parameters, 8) &&
            guestBufferAccessible(state, 4) && guestBufferAccessible(descriptor, 12)) {
            pool = memory->read32(0x82A695EC);
            const uint32_t handle = (uint32_t(base[parameters]) << 8) | base[parameters + 1];
            voice = audioEvidenceHandle(pool, handle);
            if (voice && guestBufferAccessible(voice, 80)) {
                channels = (memory->read32(voice + 72) >> 22) & 31;
                frames = memory->read32(descriptor) & 0xFFFFFF;
                output = memory->read32(descriptor + 8);
                row->descriptor = voice;
                row->result = (channels << 24) | frames;
                std::memcpy(row->before, base + voice, 72);
                audioEvidenceBuffer(row->before + 72, pool, voice);
                std::memcpy(row->before + 80, base + state, 4);
                std::memcpy(row->before + 84, base + parameters + 4, 4);
                const uint32_t masterHandle = (uint32_t(base[0x82A695F0]) << 8) | base[0x82A695F1];
                const uint32_t master = audioEvidenceHandle(pool, masterHandle);
                if (master && guestBufferAccessible(master, 64))
                    std::memcpy(row->after + 80, base + master + 60, 4);
                audioEvidenceVoice = voice;
            } else row = nullptr;
        } else row = nullptr;
    }
    __imp__sub_8281E758(ctx, base);
    if (row) {
        std::memcpy(row->after, base + voice, 72);
        audioEvidenceBuffer(row->after + 72, pool, voice);
        std::memcpy(row->after + 84, base + state, 4);
        audioEvidenceEdges(*row, output, frames, channels);
    }
}
PPC_FUNC(sub_828248D0) {
    AudioResamplerTrace::Call evidence;
    if (audioResamplerTrace.sourceWindow()) audioResamplerTrace.begin(evidence);
    auto* row = evidence.record;
    const uint32_t descriptor = ctx.r3.u32, parameters = ctx.r5.u32, state = ctx.r6.u32;
    uint32_t input = 0, channels = 0, stride = 0;
    if (row) {
        row->caller = 0x828248D0;
        row->descriptor = state;
        if (guestBufferAccessible(descriptor, 12) && guestBufferAccessible(parameters, 20) &&
            guestBufferAccessible(state, 280)) {
            const uint32_t format = memory->read32(descriptor);
            channels = format >> 24;
            stride = ((channels + 3) / 4) * 16;
            input = memory->read32(descriptor + 4);
            if ((format & 0xFFFFFF) == 256 && channels && channels <= 8 &&
                guestBufferAccessible(input, 256 * stride)) {
                row->result = format;
                std::memcpy(row->before, base + descriptor, 12);
                std::memcpy(row->before + 12, base + parameters, 20);
                std::memcpy(row->before + 32, base + state + 256, 24);
                constexpr uint32_t phases[] = {0, 127, 128, 255};
                for (uint32_t i = 0; i < 4; ++i)
                    std::memcpy(row->before + 56 + i * 8, base + input + phases[i] * stride,
                                (std::min)(channels, 2u) * 4);
            } else row = nullptr;
        } else row = nullptr;
    }
    __imp__sub_828248D0(ctx, base);
    if (row) {
        std::memcpy(row->after, base + state + 256, 24);
        constexpr uint32_t phases[] = {0, 127, 128, 255};
        for (uint32_t i = 0; i < 4; ++i) {
            std::memcpy(row->after + 24 + i * 8, base + input + phases[i] * stride,
                        (std::min)(channels, 2u) * 4);
            std::memcpy(row->after + 56 + i * 8, base + state + i * 64,
                        (std::min)(channels, 2u) * 4);
        }
        audioEvidenceEdges(*row, input, 256, channels);
    }
}
namespace {
void traceVoiceSrc(PPCContext& ctx, uint8_t* base, uint32_t tag,
                   void (*original)(PPCContext&, uint8_t*), bool unitRate, bool floatInput) {
    AudioResamplerTrace::Call evidence;
    if (audioResamplerTrace.engineWindow()) audioResamplerTrace.begin(evidence);
    auto* row = evidence.record;
    const uint32_t channels = ctx.r3.u32, input = ctx.r4.u32, available = ctx.r5.u32;
    const uint32_t output = unitRate ? ctx.r7.u32 : ctx.r8.u32;
    const uint32_t countSlot = unitRate ? ctx.r8.u32 : ctx.r9.u32;
    const uint32_t incomingCsr = ctx.fpscr.getcsr();
    if (row) {
        row->caller = tag;
        row->descriptor = input;
        if (channels && channels <= 4 && guestBufferAccessible(countSlot, 4)) {
            row->result = memory->read32(countSlot);
            audioEvidenceWord(row->before, channels);
            audioEvidenceWord(row->before + 4, input);
            audioEvidenceWord(row->before + 8, available);
            audioEvidenceWord(row->before + 12, output);
            audioEvidenceWord(row->before + 16, countSlot);
            audioEvidenceWord(row->before + 20, uint32_t(ctx.lr));
            audioEvidenceFloat(row->before + 24, float(ctx.f1.f64));
            audioEvidenceFloat(row->before + 28, unitRate ? 1.0f : float(ctx.f2.f64));
            for (uint32_t c = 0; c < 4; ++c) {
                audioEvidenceFloat(row->before + 32 + c * 4, ctx.v1.f32[3 - c]);
                audioEvidenceFloat(row->before + 48 + c * 4, ctx.v2.f32[3 - c]);
            }
            audioEvidenceWord(row->before + 80, audioEvidenceVoice);
            audioEvidenceWord(row->before + 84, floatInput ? 1 : 2);
            const double position = ctx.f1.f64;
            if (std::isfinite(position) && position >= 0 && position + 1 < available) {
                const uint32_t size = floatInput ? 4 : 2;
                const uint64_t start = uint64_t(input) + uint64_t(uint32_t(position)) * channels * size;
                if (start <= UINT32_MAX && guestBufferAccessible(uint32_t(start), 2 * channels * size))
                    for (uint32_t n = 0; n < 2; ++n)
                        for (uint32_t c = 0; c < (std::min)(channels, 2u); ++c) {
                            const uint32_t address = uint32_t(start) + (n * channels + c) * size;
                            const float sample = floatInput ? std::bit_cast<float>(memory->read32(address)) :
                                float(int16_t((uint16_t(base[address]) << 8) | base[address + 1]));
                            audioEvidenceFloat(row->before + 64 + n * 8 + c * 4, sample);
                        }
            }
        } else row = nullptr;
    }
    ctx.fpscr.setcsr(incomingCsr);
    original(ctx, base);
    const uint32_t returnedCsr = ctx.fpscr.getcsr();
    if (row) {
        const uint32_t count = memory->read32(countSlot);
        audioEvidenceFloat(row->after, float(ctx.f1.f64));
        audioEvidenceWord(row->after + 4, count);
        audioEvidenceEdges(*row, output, count, channels);
        if (count >= 8 && count <= 256 && guestBufferAccessible(output, count * 16))
            for (uint32_t n = 0; n < 8; ++n)
                std::memcpy(row->after + 24 + n * 8, base + output + (count - 8 + n) * 16,
                            (std::min)(channels, 2u) * 4);
    }
    ctx.fpscr.setcsr(returnedCsr);
}
}
PPC_FUNC(sub_8281D758) { traceVoiceSrc(ctx, base, 0x8281D758, __imp__sub_8281D758, false, false); }
PPC_FUNC(sub_8281DB98) { traceVoiceSrc(ctx, base, 0x8281DB98, __imp__sub_8281DB98, true, false); }
PPC_FUNC(sub_8281DF58) { traceVoiceSrc(ctx, base, 0x8281DF58, __imp__sub_8281DF58, false, true); }
PPC_FUNC(sub_8281E398) { traceVoiceSrc(ctx, base, 0x8281E398, __imp__sub_8281E398, true, true); }
namespace {
// Tagged mix-stage records, enabled only by DARKRECOMP_AUDIO_MIX_TRACE=1:
// before: original descriptor[12], r4/r5/r6/lr[16], validity[4],
//         parameters[24], input phases 0/127/128/255 (first 2 channels)[32].
// after: updated descriptor[12], actual output address/channels[8],
//        input address/channels[8], validity[4], state[24], output phases[32].
// first/last: full first/last output frame (up to six channels).
// Validity: 1=input snapshot, 2=parameters, 4=state, 8=output snapshot.
void traceAudioMixStage(PPCContext& ctx, uint8_t* base, uint32_t tag,
                       void (*original)(PPCContext&, uint8_t*), bool inPlace, bool master) {
    if (!audioResamplerTrace.mixWindow()) {
        original(ctx, base);
        return;
    }
    AudioResamplerTrace::Call evidence;
    audioResamplerTrace.begin(evidence);
    auto* row = evidence.record;
    const uint32_t descriptor = ctx.r3.u32, history = ctx.r4.u32;
    const uint32_t parameters = ctx.r5.u32, state = ctx.r6.u32;
    const uint32_t incomingCsr = ctx.fpscr.getcsr();
    uint32_t input = 0, inputChannels = 0, output = 0, outputChannels = 0, valid = 0;
    auto phases = [&](uint8_t* destination, uint32_t pointer, uint32_t channels) {
        if (!channels || channels > 16) return false;
        const uint32_t stride = ((channels + 3) / 4) * 16;
        if (!guestBufferAccessible(pointer, 256 * stride)) return false;
        constexpr uint32_t at[] = {0, 127, 128, 255};
        for (uint32_t i = 0; i < 4; ++i)
            std::memcpy(destination + i * 8, base + pointer + at[i] * stride,
                        (std::min)(channels, 2u) * 4);
        return true;
    };
    if (row) {
        row->caller = tag;
        row->descriptor = descriptor;
        if (!guestBufferAccessible(descriptor, 12) || (memory->read32(descriptor) & 0xffffff) != 256) {
            row = nullptr;
        } else {
            row->result = memory->read32(descriptor);
            std::memcpy(row->before, base + descriptor, 12);
            audioEvidenceWord(row->before + 12, history);
            audioEvidenceWord(row->before + 16, parameters);
            audioEvidenceWord(row->before + 20, state);
            audioEvidenceWord(row->before + 24, uint32_t(ctx.lr));
            input = memory->read32(descriptor + 4);
            inputChannels = memory->read32(descriptor) >> 24;
            if (phases(row->before + 56, input, inputChannels)) valid |= 1;
            if (guestBufferAccessible(parameters, 24)) {
                std::memcpy(row->before + 32, base + parameters, 24);
                valid |= 2;
            }
            if (master && guestBufferAccessible(parameters, 4)) {
                const uint32_t target = memory->read32(parameters);
                if (guestBufferAccessible(target, 8)) {
                    output = memory->read32(target);
                    outputChannels = memory->read32(target + 4);
                }
            }
            audioEvidenceWord(row->before + 28, valid);
        }
    }
    ctx.fpscr.setcsr(incomingCsr);
    original(ctx, base);
    const uint32_t returnedCsr = ctx.fpscr.getcsr();
    if (row) {
        std::memcpy(row->after, base + descriptor, 12);
        if (!master) {
            output = inPlace ? input : memory->read32(descriptor + 8);
            outputChannels = memory->read32(descriptor) >> 24;
        }
        audioEvidenceWord(row->after + 12, output);
        audioEvidenceWord(row->after + 16, outputChannels);
        audioEvidenceWord(row->after + 20, input);
        audioEvidenceWord(row->after + 24, inputChannels);
        if (guestBufferAccessible(state, 24)) {
            std::memcpy(row->after + 32, base + state, 24);
            valid |= 4;
        }
        if (phases(row->after + 56, output, outputChannels)) {
            valid |= 8;
            audioEvidenceEdges(*row, output, 256, outputChannels);
        }
        audioEvidenceWord(row->after + 28, valid);
    }
    ctx.fpscr.setcsr(returnedCsr);
}
}
PPC_FUNC(sub_8281D308) { traceAudioMixStage(ctx, base, 0x8281D308, __imp__sub_8281D308, false, true); }
PPC_FUNC(sub_82820DD8) { traceAudioMixStage(ctx, base, 0x82820DD8, __imp__sub_82820DD8, false, false); }
PPC_FUNC(sub_82822988) { traceAudioMixStage(ctx, base, 0x82822988, __imp__sub_82822988, true, false); }
PPC_FUNC(sub_82828018) { traceAudioMixStage(ctx, base, 0x82828018, __imp__sub_82828018, false, false); }
PPC_FUNC(sub_82828020) { traceAudioMixStage(ctx, base, 0x82828020, __imp__sub_82828020, true, false); }
PPC_FUNC(sub_82828518) {
    AudioResamplerTrace::Call evidence;
    audioResamplerTrace.begin(evidence);
    auto* row = evidence.record;
    const uint32_t descriptor = ctx.r3.u32, history = ctx.r4.u32;
    const uint32_t parameters = ctx.r5.u32, state = ctx.r6.u32;
    uint32_t channels = 0, stride = 0, output = 0;
    if (row) {
        row->descriptor = descriptor;
        row->caller = 0x82828518; // Tagged limiter record; distinct payload layout.
        if (guestBufferAccessible(descriptor, 12) && guestBufferAccessible(history, 4) &&
            guestBufferAccessible(parameters, 20) && guestBufferAccessible(state, 12)) {
            const uint32_t format = memory->read32(descriptor);
            channels = format >> 24;
            stride = ((channels + 3) / 4) * 16;
            const uint32_t input = memory->read32(descriptor + 4);
            output = memory->read32(descriptor + 8);
            std::memcpy(row->before, base + descriptor, 12);
            std::memcpy(row->before + 12, base + history, 4);
            std::memcpy(row->before + 16, base + parameters, 20);
            std::memcpy(row->before + 36, base + state, 12);
            if ((format & 0xFFFFFF) == 256 && channels && channels <= 8 &&
                guestBufferAccessible(input, 256 * stride) &&
                guestBufferAccessible(output, 256 * stride)) {
                constexpr uint32_t phases[] = {0, 127, 128, 255};
                for (uint32_t i = 0; i < 4; ++i)
                    std::memcpy(row->before + 48 + i * 8, base + input + phases[i] * stride,
                                (std::min)(channels, 2u) * 4);
            } else row = nullptr;
        } else row = nullptr;
    }
    __imp__sub_82828518(ctx, base);
    if (row) {
        row->result = ctx.r3.u32;
        std::memcpy(row->after, base + state, 12);
        constexpr uint32_t phases[] = {0, 127, 128, 255};
        for (uint32_t i = 0; i < 4; ++i)
            std::memcpy(row->after + 12 + i * 8, base + output + phases[i] * stride,
                        (std::min)(channels, 2u) * 4);
        std::memcpy(row->after + 44, base + descriptor, 12);
        for (uint32_t c = 0; c < (std::min)(channels, 6u); ++c) {
            row->first[c] = std::bit_cast<float>(memory->read32(output + 127 * stride + c * 4));
            row->last[c] = std::bit_cast<float>(memory->read32(output + 128 * stride + c * 4));
        }
    }
}
PPC_FUNC(sub_828C5060) {
    AudioResamplerTrace::Call evidence;
    audioResamplerTrace.begin(evidence);
    auto* row = evidence.record;
    const uint32_t descriptor = ctx.r3.u32;
    if (row) {
        row->descriptor = descriptor;
        row->caller = uint32_t(ctx.lr);
        if (guestBufferAccessible(descriptor, 88))
            std::memcpy(row->before, base + descriptor, 88);
        else row = nullptr;
    }
    // Preserve the original dispatcher and selected kernel exactly once.
    __imp__sub_828C5060(ctx, base);
    if (row) {
        row->result = ctx.r3.u32;
        std::memcpy(row->after, base + descriptor, 88);
        uint32_t firstBits = 0;
        std::memcpy(&firstBits, row->before + 28, sizeof(firstBits));
        const uint32_t first = _byteswap_ulong(firstBits);
        const uint32_t end = memory->read32(descriptor + 28);
        const uint32_t capacity = memory->read32(descriptor + 24);
        const uint32_t output = memory->read32(descriptor + 20);
        const uint32_t channels = base[descriptor + 13];
        // Original PCM output planes always have a 256-frame stride. +24 is
        // the logical limit and can be smaller; it is not the plane stride.
        if (first < end && end <= capacity && capacity <= 256 && channels && channels <= 6 &&
            guestBufferAccessible(output, 256 * channels * 4)) {
            for (uint32_t c = 0; c < channels; ++c) {
                row->first[c] = std::bit_cast<float>(memory->read32(output + 4 * (c * 256 + first)));
                row->last[c] = std::bit_cast<float>(memory->read32(output + 4 * (c * 256 + end - 1)));
            }
        }
    }
}
PPC_FUNC(sub_827D9188) {
    AudioRefillPass pass(ctx.r3.u32);
    __imp__sub_827D9188(ctx, base);
}
PPC_FUNC(sub_828B1BA8) {
    auto* stats = activeAudioRefill;
    AudioRefillTimer timer(stats ? &stats->submitTicks : nullptr);
    if (stats) ++stats->batches;
    uint32_t group = ctx.r3.u32;
    if (!guestBufferWritable(group, 12))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "invalid native XMA group");
    uint32_t count = memory->read32(group), records = memory->read32(group + 8);
    if (count > 320 || (count && ((records & 15) || !guestBufferAccessible(records, count * 96))))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "invalid native XMA records");
    for (uint32_t i = 0; i < count; ++i)
        if (!memory->xmaOwned(memory->read32(records + i*96 + 64)))
            PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unowned native XMA context");
    // The original submission copies the staged image first and preserves the
    // guest SDK's register/flag effects. Synchronous decoding completes before
    // the original poll can expose live context progress to its staged record.
    __imp__sub_828B1BA8(ctx, base);
    // Keep each post-staging ID read immediately before its decode: PCM from
    // an earlier context can alias a later record. Only validation is shared.
    uint32_t failedContext = 0;
    if (const char* error = memory->xmaDecodeRecords(records, count, failedContext)) {
        fprintf(stderr, "[XMA] context=0x%08X: %s\n", failedContext, error);
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "native XMA decode failed");
    }
}
PPC_FUNC(sub_828B13A0) {
    uint32_t group = ctx.r3.u32, index = ctx.r4.u32;
    if (!guestBufferAccessible(group, 12) || index >= memory->read32(group))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "invalid native XMA reset group");
    uint64_t record = uint64_t(memory->read32(group + 8)) + uint64_t(index)*96;
    if (record + 96 > PPC_MEMORY_SIZE || !guestBufferWritable(uint32_t(record), 96))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "invalid native XMA reset record");
    uint32_t context = memory->read32(uint32_t(record) + 64);
    if (!memory->xmaOwned(context))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unowned native XMA reset context");
    __imp__sub_828B13A0(ctx, base);
    memory->xmaReset(context);
}
PPC_FUNC(sub_820C60F8) {
    // Opt-in Continue-gate diagnostic: with --profile-engine off this is a
    // direct call-through, so production behavior matches the original exactly.
    // When on, the first invocation plus later STATE CHANGES log full snapshots
    // (hard cap 32 log events); unchanged calls are evidenced by a sparse
    // time-based heartbeat (every 15s wall time, max 12 beats covering ~3min).
    // The gate tests original bit17 (rlwinm ..,15,31,31 isolates 0x00020000):
    // prior bit0 (0x1) labels are withdrawn. Entry bytes are copied to host
    // locals BEFORE the original runs; register and guest state are never
    // changed, the slot accessor is never invoked, and no bytes are interpreted.
    if (!profileEngineCpu) {
        __imp__sub_820C60F8(ctx, base);
        return;
    }
    struct GateSnapshot {
        bool valid = false;
        uint32_t context = 0, flag252 = 0, flag3724 = 0, list = 0, count = 0, array = 0;
        bool ok252 = false, ok3724 = false, okList = false, okCount = false, okArray = false;
        uint64_t recordHash = 0;
    };
    struct GateRecord { bool readable = false; uint32_t entry = 0, vtable = 0; uint8_t raw[32]{}; };
    static std::atomic<uint32_t> totalCalls{0};
    static std::atomic<uint32_t> loggedEvents{0};
    static std::atomic<uint32_t> heartbeats{0};
    static std::mutex gateLogMutex;
    static GateSnapshot last{};
    static uint64_t lastBeatMs{0};
    const uint32_t callIndex = totalCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t nowMs = GetTickCount64();
    const uint32_t context = ctx.r3.u32;
    // All guest address arithmetic below is 64-bit with a UINT32_MAX reject
    // before any narrowing cast: a near-4GB base plus an offset must not wrap
    // to a low address that passes the span check for the wrong bytes.
    auto snapshotU32at = [&](uint32_t base, uint32_t offset, uint32_t& out) {
        uint64_t address = uint64_t(base) + uint64_t(offset);
        if (address > UINT32_MAX) return false;
        if (!guestBufferAccessible(uint32_t(address), 4)) return false;
        out = memory->read32(uint32_t(address));
        return true;
    };
    GateSnapshot current{};
    current.context = context;
    // A zero context is reported unknown without dereference; every read below
    // is span-checked first, so an unmapped context only yields unknowns.
    current.ok252 = context != 0 && snapshotU32at(context, 252, current.flag252);
    current.ok3724 = context != 0 && snapshotU32at(context, 3724, current.flag3724);
    current.okList = context != 0 && snapshotU32at(context, 4092, current.list);
    current.okCount = current.okList && current.list != 0 && snapshotU32at(current.list, 4, current.count);
    current.okArray = current.okList && current.list != 0 && snapshotU32at(current.list, 24, current.array);
    GateRecord records[4];
    uint32_t recordCount = 0;
    // Entry snapshots are copied to host locals BEFORE the original runs: it may
    // mutate, free, or reuse the slot list, so nothing below may be re-read after.
    if (current.okArray && current.array != 0 && current.count != 0) {
        recordCount = current.count > 4 ? 4 : current.count;
        for (uint32_t i = 0; i < recordCount; ++i) {
            uint64_t address = uint64_t(current.array) + uint64_t(i) * 32;
            if (address > UINT32_MAX) continue;
            GateRecord& record = records[i];
            record.entry = uint32_t(address);
            if (!guestBufferAccessible(record.entry, 32)) continue;
            if (!snapshotU32at(record.entry, 0, record.vtable)) continue;
            std::memcpy(record.raw, base + record.entry, sizeof(record.raw));
            record.readable = true;
        }
    }
    // FNV-1a over the copied record bytes so later state changes are detected
    // without re-reading guest memory after the call.
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < recordCount; ++i) {
        const GateRecord& record = records[i];
        hash ^= record.readable ? 1u : 0u;
        hash *= 1099511628211ull;
        hash ^= record.vtable;
        hash *= 1099511628211ull;
        for (uint32_t b = 0; b < 32; ++b) {
            hash ^= record.raw[b];
            hash *= 1099511628211ull;
        }
    }
    current.recordHash = hash;
    current.valid = true;
    __imp__sub_820C60F8(ctx, base);
    // Only the return register is observed after the call; every guest byte
    // printed below comes from the pre-call host copies above.
    const uint32_t result = ctx.r3.u32;
    // The mutex is only held for compare-and-print, never across the guest call,
    // so logging cannot deadlock with guest execution.
    std::lock_guard<std::mutex> logLock(gateLogMutex);
    const bool changed = !last.valid || last.context != current.context ||
        last.ok252 != current.ok252 || last.ok3724 != current.ok3724 ||
        last.okList != current.okList || last.okCount != current.okCount ||
        last.okArray != current.okArray || last.flag252 != current.flag252 ||
        last.flag3724 != current.flag3724 || last.list != current.list ||
        last.count != current.count || last.array != current.array ||
        last.recordHash != current.recordHash;
    if (changed) {
        uint32_t event = loggedEvents.fetch_add(1, std::memory_order_relaxed);
        if (event >= 32) return;
        std::fprintf(stderr, "[ContinueGate %u call=%u t=%llu] ctx=0x%08X f252=%s f3724=%s list=%s count=%s array=%s\n",
                     event, callIndex, nowMs, context, current.ok252 ? "ok" : "unknown",
                     current.ok3724 ? "ok" : "unknown", current.okList ? "ok" : "unknown",
                     current.okCount ? "ok" : "unknown", current.okArray ? "ok" : "unknown");
        if (current.ok252)
            std::fprintf(stderr, "[ContinueGate %u t=%llu] f252=0x%08X bit17=%u\n", event, nowMs,
                         current.flag252, (current.flag252 >> 17) & 1u);
        if (current.ok3724)
            std::fprintf(stderr, "[ContinueGate %u t=%llu] f3724=0x%08X\n", event, nowMs, current.flag3724);
        if (current.okList)
            std::fprintf(stderr, "[ContinueGate %u t=%llu] list=0x%08X\n", event, nowMs, current.list);
        if (current.okCount)
            std::fprintf(stderr, "[ContinueGate %u t=%llu] count=%u\n", event, nowMs, current.count);
        if (current.okArray)
            std::fprintf(stderr, "[ContinueGate %u t=%llu] array=0x%08X\n", event, nowMs, current.array);
        for (uint32_t i = 0; i < recordCount; ++i) {
            const GateRecord& record = records[i];
            if (!record.readable) {
                std::fprintf(stderr, "[ContinueGate %u t=%llu] rec%u entry=0x%08X unreadable\n", event,
                             nowMs, i, record.entry);
                continue;
            }
            char raw[65]{};
            for (uint32_t b = 0; b < 32; ++b)
                std::snprintf(raw + b * 2, 3, "%02X", record.raw[b]);
            std::fprintf(stderr, "[ContinueGate %u t=%llu] rec%u entry=0x%08X vtable=0x%08X raw=%s\n",
                         event, nowMs, i, record.entry, record.vtable, raw);
        }
        std::fprintf(stderr, "[ContinueGate %u t=%llu] return r3=0x%08X\n", event, nowMs, result);
        last = current;
        lastBeatMs = nowMs;
        return;
    }
    if (nowMs - lastBeatMs >= 15000) {
        uint32_t beat = heartbeats.fetch_add(1, std::memory_order_relaxed);
        if (beat >= 12) return;
        lastBeatMs = nowMs;
        std::fprintf(stderr, "[ContinueGate heartbeat %u t=%llu] calls=%u ctx=0x%08X unchanged\n", beat,
                     nowMs, callIndex + 1, context);
    }
}
PPC_FUNC(sub_820F5F28) {
    // Opt-in save-list state-machine trace (list producer per loc_820F64E0:
    // provider virtual+80, count>0 gate, then ctx+4088 alloc/resize/record-
    // copy). Direct call-through when off. When on: entry/exit snapshots of
    // flags/state3724/operation3728/device4876/list4092 (+count when valid),
    // transitions logged (cap 32), else a 15s time heartbeat (max 12). The
    // pre-call list pointer is never dereferenced after the call: post state
    // is re-read fresh from the saved context address. No guest writes, no
    // helper invocation, no byte interpretation.
    if (!profileEngineCpu) {
        __imp__sub_820F5F28(ctx, base);
        return;
    }
    static std::atomic<uint32_t> totalCalls{0};
    static std::atomic<uint32_t> loggedEvents{0};
    static std::atomic<uint32_t> heartbeats{0};
    static std::mutex listLogMutex;
    struct ListState {
        bool valid = false;
        uint32_t f252 = 0, st3724 = 0, op3728 = 0, dev4876 = 0, list = 0, count = 0;
        bool ok252 = false, ok3724 = false, ok3728 = false, ok4876 = false, okList = false,
               okCount = false;
    };
    static ListState last{};
    static uint64_t lastBeatMs{0};
    const uint32_t callIndex = totalCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t nowMs = GetTickCount64();
    const uint32_t context = ctx.r3.u32;
    auto snap = [&](uint32_t base, uint32_t offset, uint32_t& out) {
        uint64_t address = uint64_t(base) + uint64_t(offset);
        if (address > UINT32_MAX) return false;
        if (!guestBufferAccessible(uint32_t(address), 4)) return false;
        out = memory->read32(uint32_t(address));
        return true;
    };
    auto capture = [&](ListState& state) {
        state = ListState{};
        if (context == 0) return;
        state.ok252 = snap(context, 252, state.f252);
        state.ok3724 = snap(context, 3724, state.st3724);
        state.ok3728 = snap(context, 3728, state.op3728);
        state.ok4876 = snap(context, 4876, state.dev4876);
        state.okList = snap(context, 4092, state.list);
        state.okCount = state.okList && state.list != 0 && snap(state.list, 4, state.count);
        state.valid = true;
    };
    ListState entry{};
    capture(entry);
    __imp__sub_820F5F28(ctx, base);
    // Post state is captured fresh from the saved context address; the entry
    // list pointer value is never reused for dereference after the call.
    ListState exit{};
    capture(exit);
    const uint32_t result = ctx.r3.u32;
    auto same = [](const ListState& a, const ListState& b) {
        return a.valid == b.valid && a.ok252 == b.ok252 && a.ok3724 == b.ok3724 &&
            a.ok3728 == b.ok3728 && a.ok4876 == b.ok4876 && a.okList == b.okList &&
            a.okCount == b.okCount && a.f252 == b.f252 && a.st3724 == b.st3724 &&
            a.op3728 == b.op3728 && a.dev4876 == b.dev4876 && a.list == b.list &&
            a.count == b.count;
    };
    auto print = [&](const char* tag, uint32_t event, const ListState& s) {
        std::fprintf(stderr, "[SaveList %u t=%llu] %s ctx=0x%08X f252=%s f3724=%s op3728=%s dev4876=%s list=%s count=%s\n",
                     event, nowMs, tag, context, s.ok252 ? "ok" : "unknown",
                     s.ok3724 ? "ok" : "unknown", s.ok3728 ? "ok" : "unknown",
                     s.ok4876 ? "ok" : "unknown", s.okList ? "ok" : "unknown",
                     s.okCount ? "ok" : "unknown");
        if (s.ok252)
            std::fprintf(stderr, "[SaveList %u] f252=0x%08X bit17=%u\n", event, s.f252,
                         (s.f252 >> 17) & 1u);
        if (s.ok3724)
            std::fprintf(stderr, "[SaveList %u] st3724=%u\n", event, s.st3724);
        if (s.ok3728)
            std::fprintf(stderr, "[SaveList %u] op3728=%u\n", event, s.op3728);
        if (s.ok4876)
            std::fprintf(stderr, "[SaveList %u] dev4876=%u\n", event, s.dev4876);
        if (s.okList)
            std::fprintf(stderr, "[SaveList %u] list=0x%08X%s\n", event, s.list,
                         s.list != 0 ? "" : " (empty)");
        if (s.okCount)
            std::fprintf(stderr, "[SaveList %u] count=%u\n", event, s.count);
    };
    std::lock_guard<std::mutex> logLock(listLogMutex);
    const bool changed = !last.valid || !same(last, exit) || !same(entry, exit);
    if (changed) {
        uint32_t event = loggedEvents.fetch_add(1, std::memory_order_relaxed);
        if (event >= 32) return;
        print("entry", event, entry);
        print("exit", event, exit);
        std::fprintf(stderr, "[SaveList %u t=%llu] return r3=0x%08X listBecameNonzero=%u\n", event,
                     nowMs, result,
                     (entry.okList && entry.list == 0 && exit.okList && exit.list != 0) ? 1u : 0u);
        last = exit;
        lastBeatMs = nowMs;
        return;
    }
    if (nowMs - lastBeatMs >= 15000) {
        uint32_t beat = heartbeats.fetch_add(1, std::memory_order_relaxed);
        if (beat >= 12) return;
        lastBeatMs = nowMs;
        std::fprintf(stderr, "[SaveList heartbeat %u t=%llu] calls=%u ctx=0x%08X unchanged\n", beat,
                     nowMs, callIndex + 1, context);
    }
}
static uint32_t formatWidthDigit(uint32_t width, char digit) {
    return uint32_t((std::min)(uint64_t(width) * 10 + unsigned(digit - '0'), uint64_t(UINT32_MAX)));
}
static void appendFormatNumber(std::string& output, const char* digits, uint32_t width,
                               bool zeroPad, size_t limit) {
    // Padding must be bounded by the guest output limit. Passing a guest width
    // to sprintf_s with a fixed host buffer invokes the CRT abort handler.
    size_t length = strlen(digits);
    size_t padding = width > length ? size_t(width) - length : 0;
    if (zeroPad && padding && (*digits == '-' || *digits == '+') && output.size() < limit) {
        output.push_back(*digits++);
        --length;
    }
    output.append((std::min)(padding, limit - output.size()), zeroPad ? '0' : ' ');
    output.append(digits, (std::min)(length, limit - output.size()));
}
static void appendFormatString(std::string& output, const char* value, size_t length,
                               uint32_t width, size_t limit) {
    if (output.size() >= limit) return;
    size_t padding = width > length ? size_t(width) - length : 0;
    output.append((std::min)(padding, limit - output.size()), ' ');
    output.append(value, (std::min)(length, limit - output.size()));
}
static void appendFormatGuestString(std::string& output, const uint8_t* base,
                                    uint32_t argument, uint32_t width, size_t limit) {
    if (!argument) {
        appendFormatString(output, "(null)", 6, width, limit);
        return;
    }
    const char* value = reinterpret_cast<const char*>(base + argument);
    size_t originalSize = output.size();
    __try {
        appendFormatString(output, value, strnlen(value, 4096), width, limit);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A guest read can fault after padding; discard the entire failed field.
        output.resize(originalSize);
        appendFormatString(output, "(invalid)", 9, width, limit);
    }
}
PPC_FUNC(__imp___vsnprintf) {
    uint32_t destination = ctx.r3.u32;
    uint32_t capacity = ctx.r4.u32;
    uint32_t formatAddress = ctx.r5.u32;
    uint32_t vaListAddress = ctx.r6.u32;
    if (!destination || !capacity || !formatAddress || !vaListAddress) {
        ctx.r3.u64 = static_cast<uint32_t>(-1);
        return;
    }
    // The Xbox PPC va_list points to the saved GPR area. Saved registers use
    // eight-byte slots, with the 32-bit argument in the low word.
    uint32_t cursor = vaListAddress;
    std::string output;
    output.reserve(capacity);
    const char* format = reinterpret_cast<const char*>(base + formatAddress);
    auto nextArgument = [&]() {
        uint32_t value = uint32_t(PPC_LOAD_U64(cursor));
        cursor += 8;
        return value;
    };
    for (size_t i = 0; format[i] && output.size() < capacity; ++i) {
        if (format[i] != '%') {
            output.push_back(format[i]);
            continue;
        }
        if (format[++i] == '%') {
            output.push_back('%');
            continue;
        }
        unsigned width = 0;
        bool zeroPad = format[i] == '0';
        if (zeroPad) ++i;
        while (std::isdigit(static_cast<unsigned char>(format[i])))
            width = formatWidthDigit(width, format[i++]);
        while (format[i] == '.' || std::isdigit(static_cast<unsigned char>(format[i]))) ++i;
        if (format[i] == 'l' || format[i] == 'I') {
            while (format[i] == 'l' || format[i] == 'I' || format[i] == '6' || format[i] == '4') ++i;
        }
        char specifier = format[i];
        char rendered[128] = {};
        bool number = false;
        uint32_t argument = nextArgument();
        switch (specifier) {
        case 's':
            appendFormatGuestString(output, base, argument, width, capacity);
            continue;
        case 'c':
            rendered[0] = char(argument);
            rendered[1] = '\0';
            break;
        case 'd': case 'i':
            sprintf_s(rendered, "%d", static_cast<int32_t>(argument));
            number = true;
            break;
        case 'u':
            sprintf_s(rendered, "%u", argument);
            number = true;
            break;
        case 'x': case 'X':
            sprintf_s(rendered, specifier == 'x' ? "%x" : "%X", argument);
            number = true;
            break;
        case 'p':
            sprintf_s(rendered, "0x%08X", argument);
            break;
        default:
            output.push_back('%');
            output.push_back(specifier);
            continue;
        }
        if (number) appendFormatNumber(output, rendered, width, zeroPad, capacity);
        else output += rendered;
    }
    size_t written = (std::min)(output.size(), size_t(capacity - 1));
    memcpy(base + destination, output.data(), written);
    PPC_STORE_U8(destination + uint32_t(written), 0);
    ctx.r3.s64 = output.size() < capacity ? static_cast<int32_t>(output.size()) : -1;
}
PPC_FUNC(__imp__sprintf) {
    uint32_t destination = ctx.r3.u32;
    uint32_t formatAddress = ctx.r4.u32;
    if (!destination || !formatAddress) { ctx.r3.s64 = -1; return; }
    uint32_t arguments[] = { ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    uint32_t argumentIndex = 0;
    std::string output;
    const char* format = reinterpret_cast<const char*>(base + formatAddress);
    for (size_t i = 0; format[i] && output.size() < 4095; ++i) {
        if (format[i] != '%') { output.push_back(format[i]); continue; }
        if (format[++i] == '%') { output.push_back('%'); continue; }
        unsigned width = 0;
        bool zeroPad = format[i] == '0';
        if (zeroPad) ++i;
        while (std::isdigit(static_cast<unsigned char>(format[i]))) width = formatWidthDigit(width, format[i++]);
        while (format[i] == '.' || std::isdigit(static_cast<unsigned char>(format[i]))) ++i;
        if (format[i] == 'l' || format[i] == 'I') while (format[i] == 'l' || format[i] == 'I' || format[i] == '6' || format[i] == '4') ++i;
        uint32_t argument = argumentIndex < 6 ? arguments[argumentIndex++] : 0;
        char rendered[128] = {};
        bool number = false;
        switch (format[i]) {
        case 's':
            appendFormatGuestString(output, base, argument, width, 4095);
            continue;
        case 'd': case 'i':
            sprintf_s(rendered, "%d", static_cast<int32_t>(argument));
            number = true;
            break;
        case 'u':
            sprintf_s(rendered, "%u", argument);
            number = true;
            break;
        case 'x': case 'X':
            sprintf_s(rendered, format[i] == 'x' ? "%x" : "%X", argument);
            number = true;
            break;
        case 'p':
            sprintf_s(rendered, "0x%08X", argument);
            break;
        case 'c':
            rendered[0] = char(argument); break;
        default:
            output.push_back('%'); output.push_back(format[i]); continue;
        }
        if (number) appendFormatNumber(output, rendered, width, zeroPad, 4095);
        else output += rendered;
    }
    memcpy(base + destination, output.data(), output.size());
    PPC_STORE_U8(destination + uint32_t(output.size()), 0);
    ctx.r3.s64 = static_cast<int32_t>(output.size());
}
PPC_FUNC(__imp__DbgPrint) {
    uint32_t address = ctx.r3.u32;
    if (address) {
        const char* message = reinterpret_cast<const char*>(base + address);
        size_t length = strnlen(message, 4096);
        fprintf(stderr, "[Guest Output] %.*s\n", static_cast<int>(length), message);
    }
    ctx.r3.u64 = 0;
}
// The native single-player port has one local player in slot0. State1 is
// local sign-in; no Xbox Live session or online entitlement is reported.
PPC_FUNC(__imp__XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32==0?1:0; }
namespace {
constexpr uint32_t kXContentDataSize = 308;
constexpr uint32_t kOverlappedSize = 28;
bool overlappedWritable(uint32_t ptr) {
    return ptr && guestBufferWritable(ptr, kOverlappedSize);
}
std::optional<std::string> readGuestCString(uint32_t ptr, size_t maxLen) {
    if (!ptr || !maxLen) return std::nullopt;
    std::string out;
    out.reserve(16);
    for (size_t i = 0; i < maxLen; ++i) {
        uint64_t addr64 = uint64_t(ptr) + i;
        if (addr64 > UINT32_MAX) return std::nullopt;
        uint32_t addr = uint32_t(addr64);
        if (!guestBufferAccessible(addr, 1)) return std::nullopt;
        char c = static_cast<char>(memory->base()[addr]);
        if (c == '\0') return out;
        out.push_back(c);
    }
    return std::nullopt;
}
bool parseGuestContent(uint32_t ptr, Storage::ContentInfo& out) {
    if (!ptr || !guestBufferAccessible(ptr, kXContentDataSize)) return false;
    uint32_t device = memory->read32(ptr);
    uint32_t type = memory->read32(ptr + 4);
    if (type == 0) return false;
    if (device != 0 && device != Storage::kSaveDeviceId) return false;
    out.deviceId = device == 0 ? Storage::kSaveDeviceId : device;
    out.contentType = type;
    out.displayName.clear();
    for (uint32_t i = 0; i < 128; ++i) {
        uint16_t c = (uint16_t(memory->base()[ptr + 8 + i * 2]) << 8) |
                     uint16_t(memory->base()[ptr + 8 + i * 2 + 1]);
        if (c == 0) break;
        out.displayName.push_back(static_cast<char16_t>(c));
    }
    char raw[42]{};
    memcpy(raw, memory->base() + ptr + 264, 42);
    size_t len = strnlen(raw, 42);
    if (len == 0 || len > 42) return false;
    if (len < 42 && raw[len] != '\0') return false;
    out.fileName.assign(raw, len);
    if (!Storage::ValidFileName(out.fileName)) return false;
    return true;
}
void writeGuestContent(uint32_t ptr, const Storage::ContentInfo& content) {
    memset(memory->base() + ptr, 0, kXContentDataSize);
    memory->write32(ptr, content.deviceId);
    memory->write32(ptr + 4, content.contentType);
    size_t dlen = (std::min)(content.displayName.size(), size_t(127));
    for (size_t i = 0; i < dlen; ++i) {
        uint16_t c = static_cast<uint16_t>(content.displayName[i]);
        memory->base()[ptr + 8 + i * 2] = uint8_t(c >> 8);
        memory->base()[ptr + 8 + i * 2 + 1] = uint8_t(c);
    }
    memcpy(memory->base() + ptr + 264, content.fileName.c_str(), content.fileName.size());
}
bool isStorageEvent(uint32_t id) {
    // Only true NtCreateEvent objects qualify. Semaphores, files, threads,
    // listeners, and enumerators share the handle table but must be rejected
    // before any storage effects; otherwise SetEvent silently fails while the
    // caller observes pending without notification.
    auto target = object(id);
    return target && target->isEvent && target->handle;
}
bool storageCallbackValid(uint32_t routine) {
    // Same guards as PPCSafeIndirect (ppc/ppc_context.h:608): the normalized
    // address must be an aligned AOT call target with a mapped function.
    // A merely readable heap buffer must not pass; invocation after storage
    // effects would otherwise fault outside the guarded path.
    uint32_t address = routine & ~1u;
    if (!address) return true;
    if (address & 3) return false;
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE) return false;
    if (!guestBufferAccessible(address, 4)) return false;
    return PPC_LOOKUP_FUNC(memory->base(), address) != nullptr;
}
bool storageOverlapTargetsValid(uint32_t overlapped) {
    if (!overlapped) return true;
    uint32_t event = memory->read32(overlapped + 12);
    uint32_t routine = memory->read32(overlapped + 16);
    if (event && !isStorageEvent(event)) return false;
    if ((routine & ~1u) && !storageCallbackValid(routine)) return false;
    return true;
}
uint32_t completeStorageOverlapped(PPCContext& ctx, uint32_t overlapped, uint32_t error,
                                   uint32_t length, uint32_t extended) {
    if (!overlapped) return error;
    uint32_t event = memory->read32(overlapped + 12);
    uint32_t routine = memory->read32(overlapped + 16);
    if (event && !isStorageEvent(event)) return ERROR_INVALID_PARAMETER;
    if ((routine & ~1u) && !storageCallbackValid(routine)) return ERROR_INVALID_PARAMETER;
    memory->write32(overlapped, error);
    memory->write32(overlapped + 4, length);
    memory->write32(overlapped + 8, 0xFFFFFFFE);
    memory->write32(overlapped + 24, extended ? extended : error);
    if (event) {
        if (auto ev = object(event)) SetEvent(ev->handle);
    }
    if (routine & ~1u) queueGuestXamApc(ctx, routine, error, length, overlapped);
    return 0x3E5;
}
}  // namespace
// Bounded profile-only storage-import trace (cap 16, separate from the
// provider cap so enumeration traffic cannot consume provider events). Early
// rejections previously returned silently, making "never called" and
// "rejected/empty" indistinguishable; every path reports inputs+result when
// profiling. Profile-off behavior is unchanged: a single bool read.
// extra carries the handle out-slot on reject paths and the item/fetch count
// on success paths.
static void TraceStorageImport(const char* kind, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                               uint32_t r7, uint32_t extra, uint32_t result) {
    if (!profileEngineCpu)
        return;
    static std::atomic<uint32_t> storageEvents{0};
    static std::mutex storageMutex;
    if (storageEvents.load(std::memory_order_relaxed) >= 16)
        return;
    std::lock_guard<std::mutex> storageLock(storageMutex);
    uint32_t event = storageEvents.fetch_add(1, std::memory_order_relaxed);
    if (event >= 16)
        return;
    std::fprintf(stderr, "[StorageProbe %u %s] r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X extra=0x%08X => 0x%08X\n",
                 event, kind, r3, r4, r5, r6, r7, extra, result);
}
PPC_FUNC(__imp__XamShowDeviceSelectorUI) {
    // Proven caller ABI (build_native/generated/ppc_recomp.60.cpp:45700 loc_827A7520):
    // r3=user, r4=contentType(1), r5=flags, r6=requestedBytes(u64), r7=deviceOut*, r8=overlapped*.
    uint32_t user = ctx.r3.u32;
    uint32_t contentType = ctx.r4.u32;
    uint64_t requested = ctx.r6.u64;
    uint32_t deviceOut = ctx.r7.u32;
    uint32_t overlapped = ctx.r8.u32;
    if (user != 0) {
        ctx.r3.u64 = 0x525;
        return;
    }
    if (contentType != 1 || !deviceOut || !guestBufferWritable(deviceOut, 4)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (overlapped && !overlappedWritable(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!storageOverlapTargetsValid(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t state = Storage::DeviceState(Storage::kSaveDeviceId);
    if (state) {
        ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, state, 0, state);
        return;
    }
    {
        // Bounded one-shot backing checks (selection is rare, never per-frame):
        // space-query errors mean the store is unavailable, and a real probe
        // write proves the directory is actually writable, not just present.
        std::error_code ec;
        auto space = std::filesystem::space(Storage::SaveRoot(), ec);
        if (ec) {
            uint32_t err = 0x48F;
            ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
            return;
        }
        if (requested && requested > space.available) {
            ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, 0x70, 0, 0x70);
            return;
        }
        static std::atomic<uint32_t> probeCounter{0};
        char probeName[64]{};
        snprintf(probeName, sizeof(probeName), ".writetest.%lu.%lu.tmp",
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(probeCounter.fetch_add(1)));
        std::filesystem::path probe = Storage::SaveRoot() / probeName;
        HANDLE probeHandle =
            CreateFileW(probe.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (probeHandle == INVALID_HANDLE_VALUE) {
            uint32_t err = 0x05;
            ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
            return;
        }
        DWORD probeWritten = 0;
        const uint8_t probeByte = 0;
        bool probeOk = WriteFile(probeHandle, &probeByte, 1, &probeWritten, nullptr) && probeWritten == 1;
        CloseHandle(probeHandle);
        std::error_code removeEc;
        std::filesystem::remove(probe, removeEc);
        if (!probeOk) {
            uint32_t err = 0x05;
            ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
            return;
        }
    }
    memory->write32(deviceOut, Storage::kSaveDeviceId);
    std::fprintf(stderr, "[NativeStorage] Device selector: user=%u -> device 1 (%ls)\n", user,
                 Storage::SaveRoot().c_str());
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, 0, 4, 0);
}
PPC_FUNC(__imp__XamContentGetDeviceState) {
    uint32_t device = ctx.r3.u32;
    uint32_t overlapped = ctx.r4.u32;
    if (overlapped && !overlappedWritable(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    // No host effects (DeviceState creates the save root) before every
    // nonzero overlap target is proven valid.
    if (!storageOverlapTargetsValid(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t state = Storage::DeviceState(device);
    if (device != Storage::kSaveDeviceId) state = 0x48F;
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, state, 0, state);
}
PPC_FUNC(__imp__XamContentCreateEnumerator) {
    uint32_t user = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t flags = ctx.r6.u32, count = ctx.r7.u32;
    uint32_t sizeOut = ctx.r8.u32, handleOut = ctx.r9.u32;
    if (user != 0) {
        TraceStorageImport("CreateEnumerator", user, device, type, flags, count, handleOut, 0x525);
        ctx.r3.u64 = 0x525;
        return;
    }
    if (!count || count > 256 || !handleOut || !guestBufferWritable(handleOut, 4) ||
        (sizeOut && !guestBufferWritable(sizeOut, 4))) {
        TraceStorageImport("CreateEnumerator", user, device, type, flags, count, handleOut,
                           ERROR_INVALID_PARAMETER);
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (device != 0 && device != Storage::kSaveDeviceId) {
        TraceStorageImport("CreateEnumerator", user, device, type, flags, count, handleOut, 0x48F);
        ctx.r3.u64 = 0x48F;
        return;
    }
    // Original saved-game caller passes 0x1000 (loc_827A75A0); the local
    // user0 store holds no common content, so the flag changes nothing here.
    if (type == 0 || (flags & ~(0xFu | 0x1000u))) {
        TraceStorageImport("CreateEnumerator", user, device, type, flags, count, handleOut,
                           ERROR_INVALID_PARAMETER);
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    std::vector<Storage::ContentInfo> items;
    uint32_t listStatus = Storage::EnumerateChecked(type, items);
    if (listStatus) {
        TraceStorageImport("CreateEnumerator", user, device, type, flags, count, 0, listStatus);
        ctx.r3.u64 = listStatus;
        return;
    }
    size_t itemCount = items.size();
    uint32_t id = storeObject(nullptr);
    if (auto e = object(id)) {
        std::lock_guard guard(e->ioMutex);
        e->isEnumerator = true;
        e->enumFetch = count;
        e->enumCursor = 0;
        e->enumItems = std::move(items);
    }
    if (sizeOut) memory->write32(sizeOut, count * kXContentDataSize);
    memory->write32(handleOut, id);
    {
        // Legitimate menu-gate input: which containers chechassavegames/GOTSAVES
        // observes. Bounded, enumerator creation is menu-time rare, never per-frame.
        // Host remains an opaque-byte store; only names already returned to guest.
        std::string names;
        const size_t logged = (std::min)(items.size(), size_t(4));
        for (size_t i = 0; i < logged; ++i) {
            if (i) names += ',';
            names += items[i].fileName.substr(0, 32);
        }
        std::fprintf(stderr, "[NativeStorage] Enumerator type=0x%08X items=%zu fetch=%u names=%s\n", type,
                     itemCount, count, names.c_str());
    }
    TraceStorageImport("CreateEnumerator", user, device, type, flags, count, uint32_t(itemCount), 0);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamEnumerate) {
    uint32_t handle = ctx.r3.u32;
    uint32_t buffer = ctx.r5.u32, bytes = ctx.r6.u32, countOut = ctx.r7.u32;
    uint32_t overlapped = ctx.r8.u32;
    auto e = object(handle);
    if (!e || !e->isEnumerator) {
        TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, overlapped,
                           ERROR_INVALID_PARAMETER);
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (overlapped && !overlappedWritable(overlapped)) {
        TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, overlapped,
                           ERROR_INVALID_PARAMETER);
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!buffer || bytes < kXContentDataSize || !guestBufferWritable(buffer, bytes) ||
        (countOut && !guestBufferWritable(countOut, 4))) {
        uint32_t err = ERROR_INVALID_PARAMETER;
        TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, overlapped, err);
        ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
        return;
    }
    if (!storageOverlapTargetsValid(overlapped)) {
        TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, overlapped,
                           ERROR_INVALID_PARAMETER);
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    std::lock_guard guard(e->ioMutex);
    size_t remaining = e->enumItems.size() - (std::min)(e->enumCursor, e->enumItems.size());
    if (!remaining) {
        if (countOut) memory->write32(countOut, 0);
        TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, 0, 0x12);
        ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, 0x12, 0, 0x12);
        return;
    }
    uint32_t room = bytes / kXContentDataSize;
    uint32_t fetch = (std::min)(uint32_t(remaining), (std::min)(room, e->enumFetch));
    if (!fetch) {
        TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, uint32_t(remaining), 0x7A);
        ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, 0x7A, 0, 0x7A);
        return;
    }
    for (uint32_t i = 0; i < fetch; ++i)
        writeGuestContent(buffer + i * kXContentDataSize, e->enumItems[e->enumCursor + i]);
    {
        // Bounded fetch witness: which fileNames the guest actually received
        // before NoCheckpoint vs GotCheckpoint branches. First 4 fetches only.
        static std::atomic<uint32_t> enumLogs{0};
        if (enumLogs.fetch_add(1) < 4) {
            std::string names;
            const uint32_t logged = (std::min)(fetch, uint32_t(4));
            for (uint32_t i = 0; i < logged; ++i) {
                if (i) names += ',';
                names += e->enumItems[e->enumCursor + i].fileName.substr(0, 32);
            }
            std::fprintf(stderr, "[NativeStorage] Enumerate handle=0x%08X fetch=%u cursor=%zu names=%s\n",
                         handle, fetch, e->enumCursor, names.c_str());
        }
    }
    e->enumCursor += fetch;
    if (countOut) memory->write32(countOut, fetch);
    TraceStorageImport("Enumerate", handle, 0, buffer, bytes, countOut, fetch, 0);
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, 0, fetch, 0);
}
PPC_FUNC(__imp__XamContentCreateEx) {
    // Proven wrapper ABI (build_native/generated/ppc_recomp.72.cpp:13462 sub_828AAC60):
    // caller stack+180 is copied to callee stack+84 immediately before the import;
    // qword content size occupies r10.u64 and the overlapped pointer is stack84.
    uint32_t user = ctx.r3.u32, rootPtr = ctx.r4.u32, dataPtr = ctx.r5.u32;
    uint32_t flags = ctx.r6.u32, dispOut = ctx.r7.u32, licenseOut = ctx.r8.u32;
    uint64_t stack64 = uint64_t(ctx.r1.u32) + 84;
    if (stack64 + 4 > 0x100000000ull) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t stackAddr = uint32_t(stack64);
    if (!guestBufferAccessible(stackAddr, 4)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t overlapped = memory->read32(stackAddr);
    if (user != 0 || !rootPtr || !dataPtr) {
        ctx.r3.u64 = user != 0 ? 0x525 : ERROR_INVALID_PARAMETER;
        return;
    }
    if ((dispOut && !guestBufferWritable(dispOut, 4)) ||
        (licenseOut && !guestBufferWritable(licenseOut, 4)) ||
        (overlapped && !overlappedWritable(overlapped))) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    auto root = readGuestCString(rootPtr, 33);
    Storage::ContentInfo content;
    if (!root || !Storage::ValidRootName(*root) || !parseGuestContent(dataPtr, content)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!storageOverlapTargetsValid(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (content.contentType != Storage::kContentSavedGame) {
        uint32_t err = 0x48F;
        if (content.contentType == 2 || content.contentType == 3) err = 0x57;
        ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
        return;
    }
    uint32_t disp = 0;
    std::filesystem::path path;
    uint32_t result = Storage::CreateContent(*root, content, flags, &disp, &path);
    if (!result) {
        if (dispOut) memory->write32(dispOut, disp);
        if (licenseOut) memory->write32(licenseOut, 0);
    } else if (dispOut) {
        memory->write32(dispOut, 0);
    }
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, result, disp, result);
}
PPC_FUNC(__imp__XamContentClose) {
    uint32_t rootPtr = ctx.r3.u32, overlapped = ctx.r4.u32;
    if (overlapped && !overlappedWritable(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    auto root = readGuestCString(rootPtr, 33);
    if (!root || !Storage::ValidRootName(*root)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    // CloseContent erases the mount: validate targets before that mutation.
    if (!storageOverlapTargetsValid(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t result = Storage::CloseContent(*root);
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, result, 0, result);
}
PPC_FUNC(__imp__XamContentDelete) {
    uint32_t user = ctx.r3.u32, dataPtr = ctx.r4.u32, overlapped = ctx.r5.u32;
    if (user != 0) {
        ctx.r3.u64 = 0x525;
        return;
    }
    if (!dataPtr) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (overlapped && !overlappedWritable(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    Storage::ContentInfo content;
    if (!parseGuestContent(dataPtr, content)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    // DeleteContent destroys the container and drops mounts: validate targets
    // before that irreversible mutation.
    if (!storageOverlapTargetsValid(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t result = Storage::DeleteContent(content);
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, result, 0, result);
}
PPC_FUNC(__imp__XamInputGetCapabilities) {
    ctx.r3.u64 = nativeInput().getCapabilities(*memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    uint32_t descriptor = ctx.r3.u32;
    uint32_t driver = ctx.r4.u32;
    uint32_t callback = 0, argument = 0;
    if (guestBufferAccessible(descriptor, 8)) {
        callback = memory->read32(descriptor);
        argument = memory->read32(descriptor + 4);
    }
    uint32_t status = AudioRenderDriver::instance().registerClient(callback, argument, driver);
    AudioDriverCounters counters = AudioRenderDriver::instance().counters();
    fprintf(stderr,
            "[Audio] register callback=0x%08X argument=0x%08X driver=0x%08X status=0x%08X "
            "deviceReady=%d worker=%d\n",
            callback, argument, driver, status, int(counters.deviceReady),
            int(counters.workerRunning));
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    uint32_t status = AudioRenderDriver::instance().unregisterClient(ctx.r3.u32);
    fprintf(stderr, "[Audio] unregister token=0x%08X status=0x%08X\n", ctx.r3.u32, status);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    uint32_t status = AudioRenderDriver::instance().submitFrame(ctx.r3.u32, ctx.r4.u32);
    AudioDriverCounters counters = AudioRenderDriver::instance().counters();
    static std::atomic<uint64_t> logged{0};
    if (status != 0 || (logged.fetch_add(1) % 512) == 0)
        fprintf(stderr,
                "[Audio] submit token=0x%08X samples=0x%08X status=0x%08X submitted=%llu "
                "completed=%llu\n",
                ctx.r3.u32, ctx.r4.u32, status, counters.framesSubmitted, counters.buffersCompleted);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
    uint32_t status = audioDriverVoiceCategoryVolumeChangeMask(ctx.r3.u32, ctx.r4.u32);
    static std::atomic<uint64_t> logged{0};
    if (status != 0 || (logged.fetch_add(1) % 512) == 0)
        fprintf(stderr, "[Audio] volume-change-mask token=0x%08X out=0x%08X status=0x%08X\n",
                ctx.r3.u32, ctx.r4.u32, status);
    ctx.r3.u64 = status;
}
PPC_FUNC(__imp__XamShowDirtyDiscErrorUI) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XamGetSystemVersion) { ctx.r3.u64 = kXboxVersionPacked; }
PPC_FUNC(__imp__XamGetExecutionId) {
    // Proven caller ABI (build_native/generated/ppc_recomp.72.cpp:13774 sub_828AAE40):
    // r3=&guestPointer (stack80 slot), import checks signed NTSTATUS, then reads
    // the BE pointer back and compares the u16 at +12. Xenia xam_info.cc
    // XamGetExecutionId_entry(lpdword info_ptr) resolves the XEX execution-info
    // optional header, propagates its status, and writes the guest header pointer.
    uint32_t out = ctx.r3.u32;
    if (!out || !guestBufferWritable(out, 4)) {
        ctx.r3.u64 = 0xC000000D;
        return;
    }
    uint32_t field = 0;
    try {
        field = memory->headerField(0x40006);
    } catch (...) {
        ctx.r3.u64 = 0xC0000225;
        return;
    }
    // The original caller dereferences the full 24-byte execution-info record
    // (u16 read at +12), so the whole span must be resident, not just the pointer.
    if (!field || !guestBufferAccessible(field, 24)) {
        ctx.r3.u64 = 0xC0000225;
        return;
    }
    memory->write32(out, field);
    ctx.r3.u64 = 0;
}
namespace {
// Offline gamer-preference defaults (INT32 only), matching the documented
// values in Xenia user_profile.cc. The title's six requested settings are all
// covered; anything else (unknown IDs, non-INT32 types) is honestly rejected.
struct ProfileDefault {
    uint32_t id;
    uint32_t value;
};
constexpr ProfileDefault kProfileDefaults[] = {
    {0x10040002, 0}, {0x10040003, 3}, {0x10040004, 0}, {0x10040005, 0},
    {0x10040006, 0xFA}, {0x1004000C, 0}, {0x1004000D, 0}, {0x1004000E, 0x64},
    {0x10040012, 1}, {0x10040013, 0}, {0x10040015, 0}, {0x10040018, 0},
    {0x1004001D, 0xFFFF0000u}, {0x1004001E, 0xFF00FF00u}, {0x10040022, 1},
    {0x10040023, 0}, {0x10040024, 0}, {0x10040026, 0}, {0x10040027, 0},
    {0x10040028, 0}, {0x10040029, 0}, {0x10040038, 0}, {0x10040039, 0},
};
constexpr uint32_t kProfileRecordSize = 40;
bool profileDefaultValue(uint32_t id, uint32_t& value) {
    for (const auto& entry : kProfileDefaults) {
        if (entry.id == id) {
            value = entry.value;
            return true;
        }
    }
    return false;
}
}  // namespace
PPC_FUNC(__imp__XamUserReadProfileSettings) {
    // Proven wrapper ABI (build_native/generated/ppc_recomp.69.cpp:32384 sub_82883970):
    // r3=title, r4=user, r5=xuidCount(0), r6=xuids(null), r7=settingCount,
    // r8=ids, r9=sizePtr, r10=buffer, overlap at stack84. Matches Xenia
    // xam_user.cc XamUserReadProfileSettings(entry) with unk=0. All six
    // requested settings are INT32, so needed = 8 + 40*N with no trailing data.
    uint32_t title = ctx.r3.u32, user = ctx.r4.u32;
    uint32_t xuidCount = ctx.r5.u32, xuids = ctx.r6.u32;
    uint32_t count = ctx.r7.u32, idsPtr = ctx.r8.u32;
    uint32_t sizePtr = ctx.r9.u32, buffer = ctx.r10.u32;
    uint64_t stack64 = uint64_t(ctx.r1.u32) + 84;
    if (stack64 + 4 > 0x100000000ull) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t stackAddr = uint32_t(stack64);
    if (!guestBufferAccessible(stackAddr, 4)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    uint32_t overlapped = memory->read32(stackAddr);
    // Xenia ordering: XUID modes, count range, size pointer, size shortfall
    // (122, touching overlap nothing), then user, unknown settings, title.
    if (xuidCount != 0 || xuids != 0) {
        static std::atomic<uint32_t> xuidLogs{0};
        if (xuidLogs.fetch_add(1) < 4)
            std::fprintf(stderr, "[NativeProfile] XUID-keyed query unsupported (count=%u)\n", xuidCount);
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (count < 1 || count > 32 || !idsPtr) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!sizePtr || !guestBufferWritable(sizePtr, 4)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!guestBufferAccessible(idsPtr, count * 4)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    // Snapshot inputs before any output: ids/size first, so aliasing between
    // the id array, size cell, and data buffer cannot corrupt the query.
    uint32_t ids[32];
    for (uint32_t i = 0; i < count; ++i) ids[i] = memory->read32(idsPtr + i * 4);
    uint32_t bufSize = memory->read32(sizePtr);
    uint32_t needed = 8 + kProfileRecordSize * count;
    // Xenia xam_user.cc: nonzero size with a null buffer is invalid, not a
    // size query; only size-0/null reports the needed size with 122.
    if (!buffer && bufSize) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!buffer || bufSize < needed) {
        if (!bufSize) memory->write32(sizePtr, needed);
        ctx.r3.u64 = 122;
        return;
    }
    if (!guestBufferWritable(buffer, needed)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (!storageOverlapTargetsValid(overlapped)) {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    if (user != 0) {
        static std::atomic<uint32_t> userLogs{0};
        if (userLogs.fetch_add(1) < 4)
            std::fprintf(stderr, "[NativeProfile] Query for unsigned-in user %u rejected\n", user);
        uint32_t err = 0x525;
        ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
        return;
    }
    if (title != 0) {
        uint32_t liveTitle = 0;
        try {
            uint32_t exec = memory->headerField(0x40006);
            if (exec && guestBufferAccessible(exec, 16)) liveTitle = memory->read32(exec + 12);
        } catch (...) {
            liveTitle = 0;
        }
        if (title != liveTitle) {
            uint32_t err = ERROR_INVALID_PARAMETER;
            ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
            return;
        }
    }
    uint32_t value = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!profileDefaultValue(ids[i], value)) {
            static std::atomic<uint32_t> unknownLogs{0};
            if (unknownLogs.fetch_add(1) < 4)
                std::fprintf(stderr, "[NativeProfile] Unknown setting 0x%08X rejected\n", ids[i]);
            uint32_t err = ERROR_INVALID_PARAMETER;
            ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, err, 0, err);
            return;
        }
    }
    {
        static std::atomic<uint32_t> successLogs{0};
        if (successLogs.fetch_add(1) < 4) {
            // Bounded branch witness for cg_loadlastvalidprofile: ids + sizes
            // prove the profile query before menu selection. No byte semantics.
            char idBuf[160]{};
            const uint32_t logged = (std::min)(count, uint32_t(4));
            size_t pos = 0;
            for (uint32_t i = 0; i < logged && pos + 11 < sizeof(idBuf); ++i)
                pos += snprintf(idBuf + pos, sizeof(idBuf) - pos, "%s0x%08X", i ? "," : "", ids[i]);
            std::fprintf(stderr, "[NativeProfile] Served %u setting(s) title=0x%08X user=%u bufSize=%u needed=%u ids=%s\n",
                         count, title, user, bufSize, needed, idBuf);
        }
    }
    memory->write32(buffer, count);
    memory->write32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < count; ++i) {
        profileDefaultValue(ids[i], value);
        uint32_t record = buffer + 8 + i * kProfileRecordSize;
        memset(base + record, 0, kProfileRecordSize);
        memory->write32(record, 1);
        memory->write32(record + 8, user);
        memory->write32(record + 16, ids[i]);
        base[record + 24] = 1;
        memory->write32(record + 32, value);
    }
    ctx.r3.u64 = completeStorageOverlapped(ctx, overlapped, 0, 0, 0);
}
PPC_FUNC(__imp__XamLoaderLaunchTitle) { }
PPC_FUNC(__imp__XamInputGetState) {
    ctx.r3.u64 = nativeInput().getState(*memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
PPC_FUNC(__imp__XamInputSetState) {
    ctx.r3.u64 = nativeInput().setState(*memory, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}
PPC_FUNC(__imp__XMsgStartIORequest) {
    fprintf(stderr, "[XAM] XMsgStartIORequest app=%u message=0x%08X overlapped=0x%08X buffer=0x%08X size=%u\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    // Offline titles use this XAM message path to submit asynchronous setup
    // requests. The native filesystem completes its own I/O, so these control
    // messages are successful with no additional host work.
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMsgInProcessCall) {
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_XNetRandom) {
    uint32_t buffer = ctx.r4.u32, length = ctx.r5.u32;
    if ((!buffer && length) || uint64_t(buffer) + length > 0x100000000ull) {
        ctx.r3.u64 = 10014; // WSAEFAULT
        return;
    }
    if (length && BCryptGenRandom(nullptr, base + buffer, length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "Windows random number generation failed");
    ctx.r3.u64 = 0;
}

namespace DarkRecomp::Native {
void DarkRecomp::Native::setNativeDisplayContext(DarkRecomp::CDisplayContextD3D11* display) { nativeDisplay = display; }
void DarkRecomp::Native::nativeDisplayPresent() { if (nativeDisplay) nativeDisplay->Present(); }

void initializeKernel() {
    registerCurrentThread();
    uint32_t storage = 0x80000000;
    for (const auto& entry : kDataImports) {
        std::string_view name(entry.name);
        // Unknown data contracts point to reserved, inaccessible pages. A read
        // identifies the export instead of turning its ordinal into a pointer.
        memory->write32(entry.address, storage);
        bool supported = name == "XexExecutableModuleHandle" || name == "KeDebugMonitorData" ||
            name == "KeCertMonitorData" || name == "ExLoadedCommandLine" || name == "XboxKrnlVersion" ||
            name == "VdGlobalDevice" || name == "VdGpuClockInMHz" || name == "VdHSIOCalibrationLock" ||
            name == "KeTimeStampBundle";
        if (supported && !memory->commit(storage, 0x2000)) throw std::runtime_error("Cannot allocate data export");
        if (name == "XexExecutableModuleHandle") {
            memory->write32(storage, storage + 0x1000);
            memory->write32(storage + 0x1000 + 0x58, Memory::xexHeader);
        } else if (name == "ExLoadedCommandLine") {
            memcpy(memory->base() + storage, "\"default.xex\"", 14);
        } else if (name == "XboxKrnlVersion") {
            // Title SDK compatibility profile, stored as big-endian u16
            // major/minor/build/qfe, matching the packed query below.
            const uint8_t version[] = {
                0, uint8_t((kXboxVersionPacked >> 28) & 0xF),
                0, uint8_t((kXboxVersionPacked >> 24) & 0xF),
                uint8_t(kXboxVersionPacked >> 16), uint8_t(kXboxVersionPacked >> 8),
                0, uint8_t(kXboxVersionPacked),
            };
            memcpy(memory->base() + storage, version, sizeof(version));
        } else if (name == "VdGpuClockInMHz") {
            memory->write32(storage, 500);
        } else if (name == "KeTimeStampBundle") {
            // 24-byte Xbox uptime bundle: +0/+8 zero, +16 guest uptime
            // milliseconds (32-bit modulo), +20 zero. The old updater is
            // stopped before the reset so it never races the memset; the
            // restart keeps the Memory-owned epoch, so uptime stays
            // continuous across reinitializations, and the current value is
            // published synchronously instead of after the first tick.
            memory->stopTimestamp();
            memset(memory->base() + storage, 0, 24);
            memory->startTimestamp(storage);
        } else if (name == "VdHSIOCalibrationLock") {
            memory->write32(storage + 16, 0xffffffff);
        }
        fprintf(stderr, "[DataImport] %s -> 0x%08X (%s)\n", entry.name, storage, supported ? "native" : "guarded: unimplemented");
        storage += 0x10000;
    }
}
}

PPC_FUNC(__imp__RtlImageXexHeaderField) {
    if (ctx.r3.u32 != Memory::xexHeader) PPC_RECOMP_FAILURE(ctx, ctx.r3.u32, "unknown XEX module header");
    ctx.r3.u64 = memory->headerField(ctx.r4.u32);
}
PPC_FUNC(__imp__XexCheckExecutablePrivilege) {
    uint32_t flags = memory->headerField(0x30000);
    uint32_t privilege = ctx.r3.u32;
    ctx.r3.u64 = privilege < 32 && flags && (memory->read32(flags) & (uint32_t(1) << privilege)) ? 1 : 0;
}
// 8223E338..8223E388 reads the selected mode through display+24 ->
// collection+24 -> pointer[index], with index at display+304. Its +64/+68
// dimensions feed both frontbuffer allocations and color/depth descriptors.
// Preserve +24/+28: the original settings code matches those logical mode
// identifiers again after startup. Changing them triggers a needless device reset.
bool DarkRecomp::Native::configureGuestRenderMode(uint32_t display) {
    if (!guestBufferAccessible(display, 308)) return false;
    const uint32_t index = memory->read32(display + 304);
    const uint32_t collection = memory->read32(display + 24);
    if (index >= 32 || !collection || !guestBufferAccessible(collection, 28)) return false;
    const uint32_t entries = memory->read32(collection + 24);
    if (!entries || !guestBufferAccessible(entries, (index + 1) * 4)) return false;
    const uint32_t selected = memory->read32(entries + index * 4);
    if (!selected || !guestBufferWritable(selected, 80)) return false;
    const auto mode = nativeVideoMode();
    memory->write32(selected + 64, mode.width);
    memory->write32(selected + 68, mode.height);
    return true;
}
extern "C" PPC_FUNC(__imp__sub_8223E268);
PPC_FUNC(sub_8223E268) {
    const auto mode = nativeVideoMode();
    if (ctx.r3.u32 == 0x82A8B610 && (mode.width != 1280 || mode.height != 720)) {
        if (!configureGuestRenderMode(ctx.r3.u32))
            PPC_RECOMP_FAILURE(ctx, 0x8223E268, "cannot configure selected native render mode");
        std::fprintf(stderr, "[DisplayMode] selected guest buffers=%ux%u before original allocation\n", mode.width, mode.height);
    }
    __imp__sub_8223E268(ctx, base);
}

bool DarkRecomp::Native::fitLegacyMenuMatrix(uint32_t drawContext) {
    const auto mode = nativeVideoMode();
    if (uint64_t(mode.width) * 9 <= uint64_t(mode.height) * 16 || !guestBufferWritable(drawContext, 684)) return false;
    const uint32_t width = memory->read32(drawContext + 676) - memory->read32(drawContext + 668);
    const uint32_t height = memory->read32(drawContext + 680) - memory->read32(drawContext + 672);
    if (width != mode.width || height != mode.height) return false;
    auto readFloat = [&](uint32_t offset) { return std::bit_cast<float>(memory->read32(drawContext + offset)); };
    auto writeFloat = [&](uint32_t offset, float value) { memory->write32(drawContext + offset, std::bit_cast<uint32_t>(value)); };
    const float sx = readFloat(336), sy = readFloat(340), matrixX = readFloat(272), translationX = readFloat(320);
    // Match a full-screen 640x480 canvas, including later matrix rebuilds.
    // Height-based HUDs, subviews and other logical canvas sizes do not match.
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(matrixX) || !std::isfinite(translationX) ||
        std::abs(sx - float(width)/640) > .0001f || std::abs(sy - float(height)/480) > .0001f) return false;
    const float virtualWidth = float(height) * (16.0f / 9.0f);
    writeFloat(272, matrixX * virtualWidth / width);
    writeFloat(320, translationX + (width - virtualWidth) * 0.5f * (matrixX / sx));
    // Preserve logical scales: the original layout and future rebuilds still
    // consume them. Only the just-produced rendering transform is fitted.
    return true;
}
extern "C" PPC_FUNC(__imp__sub_823471F8);
PPC_FUNC(sub_823471F8) {
    const uint32_t caller = uint32_t(ctx.lr), drawContext = ctx.r3.u32;
    __imp__sub_823471F8(ctx, base);
    const bool fitted = fitLegacyMenuMatrix(drawContext);
    static thread_local uint32_t seen[24]{};
    static thread_local unsigned count = 0;
    if (count < 24 && std::find(seen, seen+count, caller) == seen+count) {
        seen[count++] = caller;
        std::fprintf(stderr, "[DisplayMenu] caller=%08X context=%08X fitted=%u\n", caller, drawContext, unsigned(fitted));
    }
}

extern "C" PPC_FUNC(__imp__sub_8234C7E0);
PPC_FUNC(sub_8234C7E0) {
    const auto mode = nativeVideoMode();
    const uint32_t caller = uint32_t(ctx.lr), drawContext = ctx.r3.u32;
    // 8236D930 draws a binding icon with the supplied 640x480 scale, then
    // restores the font's independent scale at 8236DDE4. Fit the label's
    // anchor to the same canvas, retaining glyph size and right alignment.
    if ((caller == 0x8236E070 || caller == 0x8236E0C4) &&
        uint64_t(mode.width)*9 > uint64_t(mode.height)*16 && ctx.r14.u32 &&
        guestBufferAccessible(ctx.r14.u32, 8) && guestBufferAccessible(drawContext, 684) &&
        guestBufferAccessible(ctx.r26.u32, 4)) {
        auto value = [&](uint32_t address) { return std::bit_cast<float>(memory->read32(address)); };
        const float iconScale = value(ctx.r14.u32), fontScale = value(drawContext + 336);
        const uint32_t width = memory->read32(drawContext+676)-memory->read32(drawContext+668);
        const uint32_t height = memory->read32(drawContext+680)-memory->read32(drawContext+672);
        if (width == mode.width && height == mode.height &&
            std::isfinite(iconScale) && std::abs(iconScale - float(width)/640) < .0001f &&
            std::abs(fontScale - iconScale) > .0001f) {
            const bool rightAligned = memory->read32(ctx.r26.u32) == 1;
            ctx.f1.f64 = fitMenuLabelX(float(ctx.f1.f64), float(ctx.f29.f64)*value(drawContext+344),
                                      rightAligned, fontScale, mode);
        }
    }
    __imp__sub_8234C7E0(ctx, base);
}

static void writeVideoMode(uint8_t* base, uint32_t address) {
    if (!address) return;
    memset(base + address, 0, 0x30);
    const auto mode = DarkRecomp::Native::nativeVideoMode();
    memory->write32(address + 0x00, mode.width);
    memory->write32(address + 0x04, mode.height);
    memory->write32(address + 0x08, 0);
    memory->write32(address + 0x0c, 1);
    memory->write32(address + 0x10, 1);
    memory->write32(address + 0x14, 0x42700000);
    memory->write32(address + 0x18, 1);
    memory->write32(address + 0x1c, 0x4a);
    memory->write32(address + 0x20, 1);
}

PPC_FUNC(__imp__XGetVideoMode) {
    writeVideoMode(base, ctx.r3.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdQueryVideoMode) {
    writeVideoMode(base, ctx.r3.u32);
    ctx.r3.u64 = 0;
}
// These kernel display lifecycle calls only install console callbacks on retail
// hardware. There is no Xbox GPU in the native bring-up runtime, so preserving
// their successful, side-effect-free contract lets title initialization reach
// the first actual renderer operation.
PPC_FUNC(__imp__RtlFillMemoryUlong) {
    uint32_t destination = ctx.r3.u32;
    uint32_t length = ctx.r4.u32 & ~3u;
    uint32_t value = ctx.r5.u32;
    for (uint32_t offset = 0; offset < length; offset += 4) {
        PPC_STORE_U32(destination + offset, value);
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdPersistDisplay) { if (ctx.r4.u32) memory->write32(ctx.r4.u32, 0); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdSetDisplayMode) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    uint32_t callback = ctx.r3.u32;
    uint32_t callbackContext = ctx.r4.u32;
    if (callback) {
        // Complete one synthetic interrupt so the title worker can leave its
        // idle wait while the native backend has no Xenon interrupt source.
        PPCContext saved = ctx;
        ctx.r3.u64 = 0;
        ctx.r4.u64 = callbackContext;
        PPCSafeIndirect(ctx, base, callback);
        ctx = saved;
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeEnterCriticalRegion) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeLeaveCriticalRegion) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KiApcNormalRoutineNop) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAM) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdQueryVideoFlags) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) { ctx.r3.u64 = 0; }


PPC_FUNC(__imp__VdInitializeRingBuffer) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) { ctx.r3.u64 = 0; }

static std::mutex launchMutex;
static std::vector<uint8_t> launchData;
static bool launchDataPresent = false;
PPC_FUNC(__imp__XamLoaderGetLaunchData) {
    std::lock_guard lock(launchMutex);
    if (!launchDataPresent) { ctx.r3.u64 = ERROR_NOT_FOUND; return; }
    uint32_t count = (std::min)(ctx.r4.u32, uint32_t(launchData.size()));
    if (count && !ctx.r3.u32) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
    if (count) memcpy(base + ctx.r3.u32, launchData.data(), count);
    ctx.r3.u64 = ERROR_SUCCESS;
}
PPC_FUNC(__imp__XamLoaderSetLaunchData) {
    std::lock_guard lock(launchMutex);
    if (!ctx.r3.u32) { launchData.clear(); launchDataPresent = false; ctx.r3.u64 = 0; return; }
    if (ctx.r4.u32 > 0x1000) { ctx.r3.u64 = ERROR_INVALID_PARAMETER; return; }
    launchData.assign(base + ctx.r3.u32, base + ctx.r3.u32 + ctx.r4.u32);
    launchDataPresent = true;
    ctx.r3.u64 = ERROR_SUCCESS;
}
PPC_FUNC(__imp__ExGetXConfigSetting) {
    uint32_t category = ctx.r3.u16, setting = ctx.r4.u16;
    if (category != 3 || (setting != 9 && setting != 10 && setting != 12 && (setting < 1 || setting > 7)))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported console configuration setting");
    uint32_t language = 1; // English is the fallback among this title's installed languages.
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_GERMAN: language = 3; break;
        case LANG_FRENCH: language = 4; break;
        case LANG_SPANISH: language = 5; break;
        case LANG_ITALIAN: language = 6; break;
    }
    if (ctx.r7.u32) *reinterpret_cast<uint16_t*>(base + ctx.r7.u32) = _byteswap_ushort(4);
    if (!ctx.r5.u32) { ctx.r3.u64 = ctx.r6.u16 ? 0xc00000f1 : 0; return; }
    if (ctx.r6.u16 < 4) { ctx.r3.u64 = 0xc0000023; return; }
    if (setting == 9) memory->write32(ctx.r5.u32, language);
    else if (setting == 12) {
        DYNAMIC_TIME_ZONE_INFORMATION zone{};
        if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) { ctx.r3.u64 = 0xc0000001; return; }
        // The title tests the inverse of retail flag bit 1 when enabling DST.
        memory->write32(ctx.r5.u32, zone.DynamicDaylightTimeDisabled ? 2 : 0);
    } else {
        TIME_ZONE_INFORMATION zone{};
        if (GetTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) { ctx.r3.u64 = 0xc0000001; return; }
        uint32_t value = 0;
        if (setting == 1) value = uint32_t(zone.Bias);
        else if (setting == 6) value = uint32_t(zone.StandardBias);
        else if (setting == 7) value = uint32_t(zone.DaylightBias);
        else if (setting == 4 || setting == 5) {
            const auto& date = setting == 4 ? zone.StandardDate : zone.DaylightDate;
            value = (uint32_t(date.wMonth) << 24) | (uint32_t(date.wDay) << 16) |
                    (uint32_t(date.wDayOfWeek) << 8) | date.wHour;
        } else {
            const wchar_t* name = setting == 2 ? zone.StandardName : zone.DaylightName;
            int count = 0;
            for (size_t i = 0; name[i] && count < 4; ++i)
                if ((!i || name[i - 1] == L' ') && name[i] < 128) value |= uint32_t(name[i]) << (24 - count++ * 8);
        }
        memory->write32(ctx.r5.u32, value);
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlTimeFieldsToTime) {
    auto field = [&](size_t index) { return _byteswap_ushort(*reinterpret_cast<uint16_t*>(base + ctx.r3.u32 + index * 2)); };
    SYSTEMTIME time{field(0), field(1), field(7), field(2), field(3), field(4), field(5), field(6)};
    FILETIME result{};
    bool valid = SystemTimeToFileTime(&time, &result);
    if (valid) *reinterpret_cast<uint64_t*>(base + ctx.r4.u32) = _byteswap_uint64((uint64_t(result.dwHighDateTime) << 32) | result.dwLowDateTime);
    ctx.r3.u64 = valid;
}
PPC_FUNC(__imp__RtlTimeToTimeFields) {
    uint64_t value = _byteswap_uint64(*reinterpret_cast<uint64_t*>(base + ctx.r3.u32));
    FILETIME input{DWORD(value), DWORD(value >> 32)};
    SYSTEMTIME time{};
    if (!FileTimeToSystemTime(&input, &time)) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "invalid timestamp conversion");
    uint16_t fields[]{time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, time.wDayOfWeek};
    for (uint32_t i = 0; i < 8; ++i) *reinterpret_cast<uint16_t*>(base + ctx.r4.u32 + i * 2) = _byteswap_ushort(fields[i]);
}

PPC_FUNC(__imp__KeQueryPerformanceFrequency) { ctx.r3.u64 = kTimebaseFrequency; }
PPC_FUNC(__imp__RtlLowerChar) {
    uint8_t value = ctx.r3.u8;
    ctx.r3.u64 = value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}
PPC_FUNC(__imp__RtlInitAnsiString) {
    uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
    size_t length = source ? strnlen(reinterpret_cast<char*>(base + source), 0xfffe) : 0;
    memory->write32(destination, (uint32_t(length) << 16) | (source ? uint32_t(length + 1) : 0));
    memory->write32(destination + 4, source);
}
PPC_FUNC(__imp__FscSetCacheElementCount) {
    // Windows owns the filesystem cache; its size is not a title-local setting.
    // Report the unsupported request so the original caller can handle it.
    fprintf(stderr, "[File] Console cache-size request is unsupported on Windows (device=%u, count=%u)\n",
            ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0xc00000bb;
}
PPC_FUNC(__imp__KeGetCurrentProcessType) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__KeQueryPerformanceCounter) { ctx.r3.u64 = PPCQueryTimebase(); }
PPC_FUNC(__imp__VdSwap) { }
PPC_FUNC(__imp__VdGetSystemCommandBuffer) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
static void acquireGuestSpinLock(uint8_t* base, uint32_t address, uint32_t owner) {
    auto* lock = reinterpret_cast<volatile LONG*>(base + address);
    LONG expected = 0;
    LONG desired = static_cast<LONG>(_byteswap_ulong(owner));
    while (InterlockedCompareExchange(lock, desired, 0) != 0) {
        SwitchToThread();
    }
}
static void releaseGuestSpinLock(uint8_t* base, uint32_t address) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(base + address), 0);
}
PPC_FUNC(__imp__KfAcquireSpinLock) { acquireGuestSpinLock(base, ctx.r3.u32, ctx.r13.u32); }
PPC_FUNC(__imp__KfReleaseSpinLock) { releaseGuestSpinLock(base, ctx.r3.u32); }
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) { acquireGuestSpinLock(base, ctx.r3.u32, ctx.r13.u32); }
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) { releaseGuestSpinLock(base, ctx.r3.u32); }
PPC_FUNC(__imp__KfLowerIrql) { }
PPC_FUNC(__imp__KeEnableFpuExceptions) {
    if (ctx.r3.u32) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "guest floating-point exception delivery is not implemented");
    unsigned control;
    _controlfp_s(&control, _MCW_EM, _MCW_EM);
    ctx.fpscr.loadFromHost();
    ctx.msr &= ~0x900u; // Clear PPC MSR FE0 and FE1.
}
PPC_FUNC(__imp__KeQuerySystemTime) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    uint64_t value = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    *reinterpret_cast<uint64_t*>(base + ctx.r3.u32) = _byteswap_uint64(value);
}

static uint64_t tlsBitmap = 0;
static std::mutex tlsMutex;
PPC_FUNC(__imp__KeTlsAlloc) {
    std::lock_guard lock(tlsMutex);
    for (uint32_t index = 0; index < 64; ++index) {
        if (!(tlsBitmap & (uint64_t(1) << index))) {
            tlsBitmap |= uint64_t(1) << index;
            memory->write32(memory->dynamicTls(ctx) + index * 4, 0);
            ctx.r3.u64 = index;
            return;
        }
    }
    ctx.r3.u64 = 0xffffffff;
}
PPC_FUNC(__imp__KeTlsFree) {
    std::lock_guard lock(tlsMutex);
    uint32_t index = ctx.r3.u32;
    if (index >= 64) { ctx.r3.u64 = 0; return; }
    memory->clearDynamicTls(index);
    tlsBitmap &= ~(uint64_t(1) << index);
    ctx.r3.u64 = 1;
}
PPC_FUNC(__imp__KeTlsGetValue) {
    uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = index < 64 ? memory->read32(memory->dynamicTls(ctx) + index * 4) : 0;
}
PPC_FUNC(__imp__KeTlsSetValue) {
    uint32_t index = ctx.r3.u32;
    if (index >= 64) { ctx.r3.u64 = 0; return; }
    memory->write32(memory->dynamicTls(ctx) + index * 4, ctx.r4.u32);
    ctx.r3.u64 = 1;
}

struct HostCriticalSection {
    CRITICAL_SECTION native;
    HostCriticalSection() { InitializeCriticalSection(&native); }
    ~HostCriticalSection() { DeleteCriticalSection(&native); }
};
static std::unordered_map<uint32_t, std::unique_ptr<HostCriticalSection>> criticalSections;
static std::mutex criticalMutex;
static HostCriticalSection& critical(PPCContext& ctx, bool create) {
    std::lock_guard lock(criticalMutex);
    auto it = criticalSections.find(ctx.r3.u32);
    if (it == criticalSections.end()) {
        bool staticInitializer = memory->read32(ctx.r3.u32 + 16) == 0xffffffff &&
            memory->read32(ctx.r3.u32 + 20) == 0 && memory->read32(ctx.r3.u32 + 24) == 0;
        if (!create && !staticInitializer) PPC_RECOMP_FAILURE(ctx, ctx.r3.u32, "uninitialized critical section");
        it = criticalSections.emplace(ctx.r3.u32, std::make_unique<HostCriticalSection>()).first;
    }
    return *it->second;
}
PPC_FUNC(__imp__RtlInitializeCriticalSection) {
    critical(ctx, true);
    // XRTL_CRITICAL_SECTION: dispatcher header then lock/recursion/owner.
    memset(base + ctx.r3.u32, 0, 28);
    memory->write32(ctx.r3.u32, 0x01000400);
    memory->write32(ctx.r3.u32 + 8, ctx.r3.u32 + 8);
    memory->write32(ctx.r3.u32 + 12, ctx.r3.u32 + 8);
    memory->write32(ctx.r3.u32 + 16, 0xffffffff);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) { __imp__RtlInitializeCriticalSection(ctx, base); }
PPC_FUNC(__imp__RtlEnterCriticalSection) {
    auto& section = critical(ctx, false).native;
    EnterCriticalSection(&section);
    // Original callback waiters read RecursionCount to fully release/reacquire
    // this lock. Publish its big-endian value while we own the native section.
    memory->write32(ctx.r3.u32 + 20, uint32_t(section.RecursionCount));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    auto& section = critical(ctx, false).native;
    // Update before handing ownership to another thread, whose acquisition
    // publishes its own depth. A post-release store could overwrite that value.
    memory->write32(ctx.r3.u32 + 20, uint32_t(section.RecursionCount - 1));
    LeaveCriticalSection(&section);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    auto& section = critical(ctx, false).native;
    const BOOL acquired = TryEnterCriticalSection(&section);
    if (acquired) memory->write32(ctx.r3.u32 + 20, uint32_t(section.RecursionCount));
    ctx.r3.u64 = acquired;
}

KernelObject::~KernelObject() {
    if (handle) CloseHandle(handle);
    if (allocation) memory->release(allocation);
}
static std::mutex objectMutex;
static std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> objects;
static uint32_t nextHandle = 0x40000000;
static std::unordered_map<uint32_t, std::pair<std::shared_ptr<KernelObject>, uint32_t>> references;
static std::unordered_map<uint32_t, std::weak_ptr<KernelObject>> threadsById;
static thread_local std::shared_ptr<KernelObject> currentThread;

std::vector<HANDLE> DarkRecomp::Native::nativeThreadSampleHandles() {
    constexpr size_t limit = 64;
    constexpr DWORD access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_LIMITED_INFORMATION;
    std::vector<HANDLE> handles;
    // Allocate before acquiring any duplicate handles. Appending HANDLEs up to
    // this bound cannot allocate; the catch also protects partial snapshots.
    handles.reserve(limit);
    try {
        std::lock_guard lock(objectMutex);
        for (const auto& [id, weakThread] : threadsById) {
            if (handles.size() == limit) break;
            const auto thread = weakThread.lock();
            if (!thread || !thread->isThread || !thread->handle) continue;
            DWORD code = 0;
            if (!GetExitCodeThread(thread->handle, &code) || code != STILL_ACTIVE) continue;
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), thread->handle, GetCurrentProcess(),
                                 &duplicate, access, FALSE, 0)) continue;
            if (!GetExitCodeThread(duplicate, &code) || code != STILL_ACTIVE) {
                CloseHandle(duplicate);
                continue;
            }
            handles.push_back(duplicate);
        }
    } catch (...) {
        for (HANDLE handle : handles) CloseHandle(handle);
        throw;
    }
    return handles;
}

PPC_FUNC(__imp__XamNotifyCreateListener) {
    // ABI: full 64-bit notification mask in r3, max version in r4. The
    // listener owns a real manual-reset event so waits and GetNext drain
    // observe actual delivery; the event resets when the queue empties.
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) { ctx.r3.u64 = 0; return; }
    auto listener = std::make_shared<KernelObject>(event);
    listener->notificationListener = true;
    listener->notificationAreas = ctx.r3.u64;
    listener->notificationMaxVersion = ctx.r4.u32;
    std::lock_guard lock(objectMutex);
    uint32_t handle = nextHandle++;
    objects.emplace(handle, std::move(listener));
    ctx.r3.u64 = handle;
}

PPC_FUNC(__imp__XNotifyGetNext) {
    auto listener = object(ctx.r3.u32);
    if (!listener || !listener->notificationListener) { ctx.r3.u64 = 0; return; }
    uint32_t filter = ctx.r4.u32;
    std::lock_guard lock(listener->ioMutex);
    auto match = listener->notifications.end();
    if (!filter) {
        if (!listener->notifications.empty()) match = listener->notifications.begin();
    } else {
        match = std::find_if(listener->notifications.begin(), listener->notifications.end(),
            [filter](const auto& notification) { return notification.first == filter; });
    }
    if (match == listener->notifications.end()) { ctx.r3.u64 = 0; return; }
    if (ctx.r5.u32) memory->write32(ctx.r5.u32, match->first);
    if (ctx.r6.u32) memory->write32(ctx.r6.u32, match->second);
    listener->notifications.erase(match);
    if (listener->notifications.empty() && listener->handle) ResetEvent(listener->handle);
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__XNotifyPositionUI) {
    // The Xbox notification tray has no native desktop surface in this host.
    ctx.r3.u64 = 0;
}
namespace {
// Owned background-music playback-controller policy. Only the documented
// client/controller pairs may claim it; no host audio is touched.
struct XmpPolicy {
    uint32_t client = 0;
    uint32_t controller = 0;
    uint32_t playback = 0;
    bool valid = false;
};
XmpPolicy xmpPolicy;
std::mutex xmpMutex;
void notifyListeners(uint32_t id, uint32_t data) {
    uint32_t maskIndex = (id >> 25) & 63;
    uint64_t bit = uint64_t(1) << maskIndex;
    uint32_t eventVersion = (id >> 16) & 0x1FF;
    std::lock_guard lock(objectMutex);
    std::unordered_set<const KernelObject*> notified;
    for (const auto& [handle, listener] : objects) {
        if (!listener->notificationListener) continue;
        if (!(listener->notificationAreas & bit)) continue;
        if (eventVersion > listener->notificationMaxVersion) continue;
        // Duplicated handles share one listener queue and wait event.
        if (!notified.insert(listener.get()).second) continue;
        std::lock_guard queueLock(listener->ioMutex);
        listener->notifications.emplace_back(id, data);
        if (listener->handle) SetEvent(listener->handle);
    }
}
}
PPC_FUNC(__imp__XMsgStartIORequestEx) {
    // This message API returns HRESULT, not NTSTATUS.
    uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32;
    uint32_t buffer = ctx.r6.u32, length = ctx.r7.u32;
    if (app != 0xFAu) { ctx.r3.u64 = 0x80070057; return; }
    if (message != 0x7001Au) { ctx.r3.u64 = 0x80004001; return; }
    if (overlapped) { ctx.r3.u64 = 0x80070032; return; }
    if (length != 12 || !guestBufferAccessible(buffer, 12)) { ctx.r3.u64 = 0x80070057; return; }
    uint32_t client = memory->read32(buffer);
    uint32_t controller = memory->read32(buffer + 4);
    uint32_t playback = memory->read32(buffer + 8);
    bool legal = (client == 2 && controller == 0) || (client == 0 && controller == 1);
    if (!legal || playback > 1) { ctx.r3.u64 = 0x80070057; return; }
    {
        std::lock_guard lock(xmpMutex);
        xmpPolicy = {client, controller, playback, true};
        fprintf(stderr, "[XMP] playback controller client=%u controller=%u playback=%u\n",
            client, controller, playback);
        notifyListeners(0x0A000003u, playback ? 0u : 1u);
    }
    // Success clears the calling thread's last error (layout per the
    // original raw setter/getter sub_828AAEA8/sub_828AAEF8).
    uint32_t flag = 1, threadObj = 0;
    if (guestBufferAccessible(ctx.r13.u32 + 336, 4)) flag = memory->read32(ctx.r13.u32 + 336);
    if (!flag && guestBufferAccessible(ctx.r13.u32 + 256, 4)) threadObj = memory->read32(ctx.r13.u32 + 256);
    if (!flag && threadObj && guestBufferAccessible(threadObj + 352, 4))
        memory->write32(threadObj + 352, 0);
    ctx.r3.u64 = 0;
}
uint32_t storeObject(HANDLE handle) {
    std::lock_guard lock(objectMutex);
    uint32_t id = nextHandle++;
    objects.emplace(id, std::make_shared<KernelObject>(handle));
    return id;
}
std::shared_ptr<KernelObject> object(uint32_t id) {
    if (id == 0xfffffffe && currentContext) {
        if (!currentThread) {
            HANDLE handle = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                                 &handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr;
            currentThread = std::make_shared<KernelObject>(handle);
            currentThread->isThread = true;
            currentThread->guestAddress = memory->read32(currentContext->r13.u32 + 0x100);
            std::lock_guard lock(objectMutex);
            threadsById[GetCurrentThreadId()] = currentThread;
        }
        return currentThread;
    }
    std::lock_guard lock(objectMutex);
    auto it = objects.find(id);
    return it == objects.end() ? nullptr : it->second;
}
static void registerCurrentThread() { object(0xfffffffe); }
struct GuestApc {
    bool xam = false;
    uint32_t routine = 0, a = 0, b = 0, c = 0;
};
static thread_local std::list<std::unique_ptr<GuestApc>> pendingApcs;
static void CALLBACK deliverGuestApc(ULONG_PTR value) {
    auto it = std::find_if(pendingApcs.begin(), pendingApcs.end(), [value](const auto& item) {
        return reinterpret_cast<ULONG_PTR>(item.get()) == value;
    });
    if (it == pendingApcs.end() || !currentContext) { fflush(stderr); ExitProcess(3); }
    auto request = std::move(*it);
    pendingApcs.erase(it);
    struct Restore {
        PPCContext* context = currentContext;
        uint32_t csr = simde_mm_getcsr();
        ~Restore() { currentContext = context; simde_mm_setcsr(csr); }
    } restore;
    PPCContext callback = *currentContext;
    callback.r1.u64 = (callback.r1.u32 - 0x200) & ~15u;
    callback.r3.u64 = request->a;
    callback.r4.u64 = request->b;
    callback.r5.u64 = request->c;
    currentContext = &callback;
    PPCSafeIndirect(callback, memory->base(), request->routine);
}
void queueGuestApc(PPCContext& ctx, uint32_t routine, uint32_t argument, uint32_t ios) {
    auto request = std::make_unique<GuestApc>();
    request->xam = false;
    request->routine = routine & ~1u;
    request->a = argument;
    request->b = ios;
    request->c = 0;
    ULONG_PTR key = reinterpret_cast<ULONG_PTR>(request.get());
    pendingApcs.push_back(std::move(request));
    if (!QueueUserAPC(deliverGuestApc, GetCurrentThread(), key)) {
        pendingApcs.pop_back();
        PPC_RECOMP_FAILURE(ctx, routine, "Windows could not queue the file completion APC");
    }
}
void queueGuestXamApc(PPCContext& ctx, uint32_t routine, uint32_t error, uint32_t length,
                      uint32_t overlapped) {
    auto request = std::make_unique<GuestApc>();
    request->xam = true;
    request->routine = routine & ~1u;
    request->a = error;
    request->b = length;
    request->c = overlapped;
    ULONG_PTR key = reinterpret_cast<ULONG_PTR>(request.get());
    pendingApcs.push_back(std::move(request));
    if (!QueueUserAPC(deliverGuestApc, GetCurrentThread(), key)) {
        pendingApcs.pop_back();
        PPC_RECOMP_FAILURE(ctx, routine, "Windows could not queue the storage completion APC");
    }
}
static DWORD timeoutMilliseconds(uint8_t* base, uint32_t pointer) {
    if (!pointer) return INFINITE;
    int64_t ticks = int64_t(_byteswap_uint64(*reinterpret_cast<uint64_t*>(base + pointer)));
    uint64_t relative;
    if (ticks <= 0) relative = uint64_t(-(ticks + 1)) + 1;
    else {
        FILETIME now; GetSystemTimeAsFileTime(&now);
        uint64_t current = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        relative = uint64_t(ticks) > current ? uint64_t(ticks) - current : 0;
    }
    return DWORD((std::min)((relative + 9999) / 10000, uint64_t(INFINITE - 1)));
}
PPC_FUNC(__imp__NtCreateEvent) {
    uint32_t out = ctx.r3.u32, attributes = ctx.r4.u32, type = ctx.r5.u32, initial = ctx.r6.u32;
    if (!out || type > 1) { ctx.r3.u64 = 0xc000000d; return; }
    if (attributes) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "named event attributes are not implemented");
    HANDLE event = CreateEventW(nullptr, type == 0, initial != 0, nullptr);
    if (!event) { ctx.r3.u64 = 0xc0000017; return; }
    uint32_t id = storeObject(event);
    if (auto created = object(id)) created->isEvent = true;
    memory->write32(out, id);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtSetEvent) {
    auto event = object(ctx.r3.u32);
    if (!event) { ctx.r3.u64 = 0xc0000008; return; }
    using SetEventFunction = LONG (NTAPI*)(HANDLE, LONG*);
    static auto set = reinterpret_cast<SetEventFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetEvent"));
    if (!set) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "Windows NtSetEvent is unavailable");
    LONG previous = 0;
    LONG result = set(event->handle, &previous);
    if (result >= 0 && ctx.r4.u32) memory->write32(ctx.r4.u32, previous);
    ctx.r3.u64 = uint32_t(result);
}
PPC_FUNC(__imp__NtClearEvent) {
    auto event = object(ctx.r3.u32);
    ctx.r3.u64 = event && ResetEvent(event->handle) ? 0 : 0xc0000008;
}
PPC_FUNC(__imp__NtCreateSemaphore) {
    uint32_t out = ctx.r3.u32;
    if (!out || ctx.r5.s32 < 0 || ctx.r6.s32 <= 0 || ctx.r5.s32 > ctx.r6.s32) { ctx.r3.u64 = 0xc000000d; return; }
    if (ctx.r4.u32) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "named semaphore attributes are not implemented");
    HANDLE sem = CreateSemaphoreW(nullptr, ctx.r5.s32, ctx.r6.s32, nullptr);
    if (!sem) { ctx.r3.u64 = 0xc0000017; return; }
    memory->write32(out, storeObject(sem));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtReleaseSemaphore) {
    auto sem = object(ctx.r3.u32);
    if (!sem) { ctx.r3.u64 = 0xc0000008; return; }
    LONG previous;
    if (!ReleaseSemaphore(sem->handle, ctx.r4.s32, &previous)) { ctx.r3.u64 = 0xc0000047; return; }
    if (ctx.r5.u32) memory->write32(ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
    auto target = object(ctx.r3.u32);
    if (!target) { ctx.r3.u64 = 0xc0000008; return; }
    DWORD timeout = timeoutMilliseconds(base, ctx.r6.u32);
    const int64_t ticks = ctx.r6.u32 ? int64_t(PPC_LOAD_U64(ctx.r6.u32)) : 0;
    DWORD result = nativeTimedWait(target->handle, ctx.r5.u32 != 0, ctx.r6.u32 ? &ticks : nullptr);
    static std::atomic<uint32_t> waitLogs = 0;
    uint32_t log = waitLogs.fetch_add(1);
    if (log < 40 || (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT))
        fprintf(stderr, "[Sync] wait tid=%lu handle=0x%08X timeout=%u result=0x%08X guest=0x%08X lr=0x%08X\n", GetCurrentThreadId(), ctx.r3.u32, uint32_t(timeout), uint32_t(result), ctx.lastFunction, uint32_t(ctx.lr));
    ctx.r3.u64 = result == WAIT_FAILED ? 0xc0000008 : result;
}PPC_FUNC(__imp__NtClose) {
    std::lock_guard lock(objectMutex);
    ctx.r3.u64 = objects.erase(ctx.r3.u32) ? 0 : 0xc0000008;
}
PPC_FUNC(__imp__KeDelayExecutionThread) {
    if (!ctx.r5.u32) { ctx.r3.u64 = 0xc000000d; return; }
    const int64_t ticks = int64_t(PPC_LOAD_U64(ctx.r5.u32));
    const BOOL alertable = ctx.r4.u32 != 0;
    if (ticks == 0) {
        // The original SDK also uses a zero interval as a scheduler yield.
        ctx.r3.u64 = SleepEx(0, alertable) == WAIT_IO_COMPLETION ? 0xc0 : 0;
        return;
    }
    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        bool inUse = false;
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    static thread_local Timer timer;
    // An APC can reenter this import. Lease the cached handle until the outer
    // wait returns, so the nested wait cannot consume or reset its signal.
    struct Lease {
        Timer& timer;
        HANDLE handle;
        bool cached;
        explicit Lease(Timer& value) : timer(value), cached(!value.inUse) {
            if (cached) { timer.inUse = true; handle = timer.handle; }
            else handle = CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        ~Lease() {
            if (handle) CancelWaitableTimer(handle);
            if (cached) timer.inUse = false;
            else if (handle) CloseHandle(handle);
        }
    } lease(timer);
    LARGE_INTEGER due;
    due.QuadPart = ticks;
    DWORD result;
    if (lease.handle && SetWaitableTimer(lease.handle, &due, 0, nullptr, nullptr, FALSE)) {
        // Keep the original signed 100 ns interval: negative is relative,
        // positive is an absolute UTC deadline. Do not round up to milliseconds.
        result = WaitForSingleObjectEx(lease.handle, INFINITE, alertable);
        if (result == WAIT_FAILED)
            PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "Native delay timer wait failed");
    } else {
        // Older Windows versions lack high-resolution timers. Preserve the
        // existing wait behavior there, including interruptible SDK INFINITE.
        const DWORD timeout = ticks == INT64_MIN ? INFINITE : timeoutMilliseconds(base, ctx.r5.u32);
        result = SleepEx(timeout, alertable);
    }
    ctx.r3.u64 = result == WAIT_IO_COMPLETION ? 0xc0 : 0;
}

struct ThreadExit { uint32_t code; };
struct GuestThreadStart {
    PPCContext ctx{};
    uint32_t startup = 0;
    uint32_t entry = 0;
    uint32_t argument = 0;
    uint32_t allocation = 0;
    std::shared_ptr<KernelObject> object;
};
static DWORD callGuestThread(GuestThreadStart* start) {
    __try {
        if (start->startup) {
            start->ctx.r3.u64 = start->entry;
            start->ctx.r4.u64 = start->argument;
            PPCSafeIndirect(start->ctx, memory->base(), start->startup);
        } else {
            start->ctx.r3.u64 = start->argument;
            PPCSafeIndirect(start->ctx, memory->base(), start->entry);
        }
        return start->ctx.r3.u32;
    } __except (GetExceptionCode() == 0xe06d7363 ? EXCEPTION_CONTINUE_SEARCH : exceptionFilter(GetExceptionInformation())) {
        fflush(stderr);
        ExitProcess(3);
    }
}
static DWORD WINAPI guestThreadMain(void* raw) {
    std::unique_ptr<GuestThreadStart> start(static_cast<GuestThreadStart*>(raw));
    currentContext = &start->ctx;
    currentThread = start->object;
    start->ctx.fpscr.loadFromHost();
    fprintf(stderr, "[Thread] worker start tid=%lu entry=0x%08X argument=0x%08X\n", GetCurrentThreadId(), start->entry, start->argument);
    DWORD result = 0;
    try { result = callGuestThread(start.get()); }
    catch (const ThreadExit& exit) { result = exit.code; }
    if (start->object && start->object->guestAddress) {
        // Join poller sub_828A7F50 reports 259 while the low byte at +4 is
        // zero, otherwise the code at +320. Publish the code first so the
        // terminated flag never exposes a stale code.
        memory->write32(start->object->guestAddress + 320, result);
        uint32_t state = memory->read32(start->object->guestAddress + 4);
        memory->write32(start->object->guestAddress + 4, (state & 0xffffff00u) | 1u);
    }
    fprintf(stderr, "[Thread] worker exit tid=%lu entry=0x%08X result=%lu\n", GetCurrentThreadId(), start->entry, result);
    currentContext = nullptr;
    currentThread.reset();
    return result;
}
PPC_FUNC(__imp__ExCreateThread) {
    uint32_t out = ctx.r3.u32, stackSize = ctx.r4.u32, idOut = ctx.r5.u32;
    uint32_t startup = ctx.r6.u32, entry = ctx.r7.u32, argument = ctx.r8.u32, flags = ctx.r9.u32;
    if (!out || !entry || stackSize > 0x1000000) { ctx.r3.u64 = 0xc000000d; return; }
    if (flags & ~0x3f000001u) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported guest thread creation flags");
    uint32_t affinity = (flags >> 24) & 0x3f;
    uint8_t processor = 0;
    if (affinity) while (!(affinity & (1u << processor))) ++processor;
    stackSize = ((std::max)(stackSize, 0x10000u) + 4095) & ~4095u;
    uint32_t allocation = memory->allocate(stackSize + 0x2000, 0x1000, 0x71000000, 0x7f000000);
    if (!allocation) { ctx.r3.u64 = 0xc0000017; return; }
    auto start = std::make_unique<GuestThreadStart>();
    start->entry = entry; start->startup = startup; start->argument = argument; start->allocation = allocation;
    start->ctx.r13.u64 = allocation;
    start->ctx.r1.u64 = allocation + 0x2000 + stackSize - 0x100;
    memory->initThreadStorage(allocation, allocation + 0x2000, stackSize, 0);
    // The SDK encodes its six logical processor choices in flags[24:29].
    // Original mixer barriers index per-processor slots using PCR.Number.
    base[allocation + 0x10c] = processor;
    DWORD oldProtection;
    if (!VirtualProtect(base + allocation + 0x1000, 0x1000, PAGE_NOACCESS, &oldProtection)) {
        memory->release(allocation); ctx.r3.u64 = 0xc0000017; return;
    }
    DWORD id;
    HANDLE handle = CreateThread(nullptr, 64 * 1024 * 1024, guestThreadMain, start.get(),
        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (!handle) { memory->release(allocation); ctx.r3.u64 = 0xc0000017; return; }
    memory->write32(allocation + Memory::threadObjectOffset + 0x14c, id);
    uint32_t guestHandle = storeObject(handle);
    auto stored = object(guestHandle);
    stored->isThread = true;
    stored->affinity = affinity ? affinity : 0x3f;
    stored->guestAddress = allocation + Memory::threadObjectOffset;
    stored->allocation = allocation;
    start->object = stored;
    { std::lock_guard lock(objectMutex); threadsById[id] = stored; }
    memory->write32(out, guestHandle);
    if (idOut) memory->write32(idOut, id);
    start.release();
    fprintf(stderr, "[Thread] created handle=0x%08X id=%lu entry=0x%08X startup=0x%08X\n", guestHandle, id, entry, startup);
    if (!(flags & 1)) {
        if (ResumeThread(handle) == DWORD(-1)) PPC_RECOMP_FAILURE(ctx, entry, "Windows failed to start guest thread");
    }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__ExTerminateThread) { throw ThreadExit{ctx.r3.u32}; }
PPC_FUNC(__imp__RtlNtStatusToDosError) {
    using Convert = ULONG (WINAPI*)(LONG);
    static auto convert = reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    if (!convert) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "Windows status conversion is unavailable");
    ctx.r3.u64 = convert(ctx.r3.s32);
}
PPC_FUNC(__imp__NtSuspendThread) {
    auto thread = object(ctx.r3.u32);
    if (!thread || !thread->isThread) { ctx.r3.u64 = 0xc0000008; return; }
    DWORD previous = SuspendThread(thread->handle);
    if (previous == DWORD(-1)) { ctx.r3.u64 = 0xc0000001; return; }
    if (ctx.r4.u32) memory->write32(ctx.r4.u32, previous);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtResumeThread) {
    auto thread = object(ctx.r3.u32);
    if (!thread || !thread->isThread) { ctx.r3.u64 = 0xc0000008; return; }
    DWORD previous = ResumeThread(thread->handle);
    if (previous == DWORD(-1)) { ctx.r3.u64 = 0xc0000001; return; }
    if (ctx.r4.u32) memory->write32(ctx.r4.u32, previous);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__ObReferenceObjectByHandle) {
    auto target = object(ctx.r3.u32);
    if (!target || !target->guestAddress) { ctx.r3.u64 = 0xc0000008; return; }
    if (!ctx.r5.u32) { ctx.r3.u64 = 0xc000000d; return; }
    if (ctx.r4.u32 && (!target->isThread || ctx.r4.u32 != 0x80000000)) { ctx.r3.u64 = 0xc0000024; return; }
    // A host fault under objectMutex cannot unwind its lock_guard with /EHsc.
    // Validate the complete output before publishing references or aliases.
    if (!guestOutputAccessible(ctx.r5.u32, 4)) { ctx.r3.u64 = 0xc0000005; return; }
    std::lock_guard lock(objectMutex);
    auto& reference = references[target->guestAddress];
    reference.first = target;
    ++reference.second;
    memory->write32(ctx.r5.u32, target->guestAddress);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__ObDereferenceObject) {
    std::lock_guard lock(objectMutex);
    auto it = references.find(ctx.r3.u32);
    if (it == references.end()) PPC_RECOMP_FAILURE(ctx, ctx.r3.u32, "invalid kernel object dereference");
    if (--it->second.second == 0) references.erase(it);
}
PPC_FUNC(__imp__ObLookupThreadByThreadId) {
    if (!ctx.r4.u32) { ctx.r3.u64 = 0xc000000d; return; }
    if (!guestOutputAccessible(ctx.r4.u32, 4)) { ctx.r3.u64 = 0xc0000005; return; }
    std::lock_guard lock(objectMutex);
    auto it = threadsById.find(ctx.r3.u32);
    auto thread = it == threadsById.end() ? nullptr : it->second.lock();
    if (!thread) { ctx.r3.u64 = 0xc0000225; return; }
    auto& reference = references[thread->guestAddress];
    reference.first = thread; ++reference.second;
    memory->write32(ctx.r4.u32, thread->guestAddress);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeResumeThread) {
    std::shared_ptr<KernelObject> thread;
    {
        std::lock_guard lock(objectMutex);
        auto it = references.find(ctx.r3.u32);
        if (it != references.end()) thread = it->second.first;
    }
    if (!thread || !thread->isThread || !thread->handle) {
        ctx.r3.u64 = 0xc0000008;
        return;
    }
    if (ResumeThread(thread->handle) == DWORD(-1)) {
        ctx.r3.u64 = 0xc0000001;
        return;
    }
    ctx.r3.u64 = 0;
}PPC_FUNC(__imp__ObOpenObjectByPointer) {
    if (!ctx.r4.u32) { ctx.r3.u64 = 0xc000000d; return; }
    if (!guestOutputAccessible(ctx.r4.u32, 4)) { ctx.r3.u64 = 0xc0000005; return; }
    std::lock_guard lock(objectMutex);
    auto it = references.find(ctx.r3.u32);
    if (it == references.end()) { ctx.r3.u64 = 0xc0000008; return; }
    uint32_t handle = nextHandle++;
    objects.emplace(handle, it->second.first);
    memory->write32(ctx.r4.u32, handle);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtDuplicateObject) {
    auto original = object(ctx.r3.u32);
    if (!original) { ctx.r3.u64 = 0xc0000008; return; }
    if (ctx.r5.u32 & ~1u) PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported handle duplication options");
    if (ctx.r4.u32 && !guestOutputAccessible(ctx.r4.u32, 4)) { ctx.r3.u64 = 0xc0000005; return; }
    std::lock_guard lock(objectMutex);
    if (ctx.r4.u32) {
        uint32_t handle = nextHandle++;
        objects.emplace(handle, original);
        memory->write32(ctx.r4.u32, handle);
    }
    if (ctx.r5.u32 & 1) objects.erase(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
static std::shared_ptr<KernelObject> referencedThread(uint32_t address) {
    std::lock_guard lock(objectMutex);
    auto it = references.find(address);
    return it == references.end() ? nullptr : it->second.first;
}
PPC_FUNC(__imp__KeSetBasePriorityThread) {
    auto thread = referencedThread(ctx.r3.u32);
    if (!thread) PPC_RECOMP_FAILURE(ctx, ctx.r3.u32, "unknown thread object in priority change");
    int32_t previous = thread->priority;
    int32_t priority = ctx.r4.s32;
    if (!SetThreadPriority(thread->handle, std::clamp(priority, -2, 2)))
        PPC_RECOMP_FAILURE(ctx, ctx.r3.u32, "Windows thread priority change failed");
    thread->priority = priority;
    ctx.r3.s64 = previous;
}
PPC_FUNC(__imp__KeSetAffinityThread) {
    auto thread = referencedThread(ctx.r3.u32);
    if (!thread || !ctx.r4.u32 || (ctx.r4.u32 & ~0x3fu)) { ctx.r3.u64 = 0xc000000d; return; }
    DWORD_PTR allowed, system;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) { ctx.r3.u64 = 0xc0000001; return; }
    GROUP_AFFINITY group{};
    if(!GetThreadGroupAffinity(thread->handle,&group)) {ctx.r3.u64=0xc0000001;return;}
    DWORD_PTR selected = nativeGuestAffinity(ctx.r4.u32,allowed,group.Group);
    if (!selected) { ctx.r3.u64 = 0xc000000d; return; }
    if (!SetThreadAffinityMask(thread->handle, selected)) { ctx.r3.u64 = 0xc0000001; return; }
    if (ctx.r5.u32) memory->write32(ctx.r5.u32, thread->affinity);
    thread->affinity = ctx.r4.u32;
    uint8_t processor = 0;
    while (!(ctx.r4.u32 & (1u << processor))) ++processor;
    InterlockedExchange8(reinterpret_cast<volatile CHAR*>(base + thread->guestAddress - Memory::threadObjectOffset + 0x10c), CHAR(processor));
    static std::atomic<unsigned> reports{};
    if(reports.fetch_add(1,std::memory_order_relaxed)<32)
        std::fprintf(stderr,"[Scheduling] guestMask=%02X nativeMask=%llX group=%u guestProcessor=%u\n",
            ctx.r4.u32,static_cast<unsigned long long>(selected),unsigned(group.Group),unsigned(processor));
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NtAllocateVirtualMemory) {
    uint32_t basePointer = ctx.r3.u32, sizePointer = ctx.r4.u32;
    uint32_t flags = ctx.r5.u32, protect = ctx.r6.u32;
    if (!basePointer || !sizePointer || !memory->read32(sizePointer) || !(flags & 0x3000)) {
        ctx.r3.u64 = 0xc000000d; return;
    }
    if (memory->read32(basePointer) || (flags & ~0x3000u) || protect != PAGE_READWRITE)
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported virtual allocation mode");
    uint32_t size = memory->read32(sizePointer);
    uint32_t address = memory->allocate(size);
    if (!address) { ctx.r3.u64 = 0xc0000017; return; }
    memory->write32(basePointer, address);
    memory->write32(sizePointer, memory->allocationSize(address));
    fprintf(stderr, "[Memory] allocated 0x%X at 0x%08X\n", size, address);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__MmGetPhysicalAddress) { ctx.r3.u64 = ctx.r3.u32>=0xa0000000u?Memory::physicalAddress(ctx.r3.u32):ctx.r3.u32; ctx.r4.u64 = 0; }
PPC_FUNC(__imp__MmQueryAllocationSize) { ctx.r3.u64 = memory->allocationSize(ctx.r3.u32); }
PPC_FUNC(__imp__MmQueryAddressProtect) { ctx.r3.u64 = PAGE_READWRITE; }

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
    uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, protection = ctx.r5.u32;
    uint32_t minimum = ctx.r6.u32, maximum = ctx.r7.u32, alignment = ctx.r8.u32;
    if (flags > 2 || minimum != 0 || maximum != 0xffffffff)
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported physical allocation bounds");
    if ((protection & 0xff) != PAGE_READWRITE || (protection & ~0xa0000604u))
        PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "unsupported physical memory protection");
    uint32_t page = (protection & 0x20000000) ? 65536 : (protection & 0x80000000) ? 0x1000000 : 4096;
    uint32_t region = (page == 65536) ? 0xa0000000 : (page == 0x1000000) ? 0xc0000000 : 0xe0000000;
    uint64_t rounded = (uint64_t(size) + page - 1) & ~uint64_t(page - 1);
    uint64_t aligned = ((uint64_t(alignment) + page - 1) / page) * page;
    aligned = (std::max)(aligned, uint64_t(page));
    if (!size || rounded > 0x20000000 || aligned > 0x20000000) { ctx.r3.u64 = 0; return; }
    uint32_t address = memory->allocate(uint32_t(rounded), uint32_t(aligned), region,
        region==0xe0000000u?0xffd00000ull:uint64_t(region)+0x20000000);
    fprintf(stderr, "[Memory] physical allocation size=0x%X page=0x%X address=0x%08X\n", size, page, address);
    ctx.r3.u64 = address;
}
PPC_FUNC(__imp__MmFreePhysicalMemory) {
    uint32_t address = ctx.r4.u32;
    if (address && !memory->release(address)) {
        // Physical pools are suballocated by the title; an interior address
        // must not decommit the live parent pool.
        fprintf(stderr, "[Memory] physical suballocation release address=0x%08X\\n", address);
    }
}
PPC_FUNC(__imp__MmQueryStatistics) {
    uint32_t address = ctx.r3.u32;
    if (!address) { ctx.r3.u64 = 0xc000000d; return; }
    if (memory->read32(address) != 104) { ctx.r3.u64 = 0xc0000023; return; }
    // The 104-byte retail ABI has KernelPages at +8 and Title.AvailablePages
    // at +12. The old bridge confused these with virtual-memory byte counts.
    uint32_t heapBytes = memory->allocatedBytes();
    uint32_t imagePages = uint32_t(PPC_IMAGE_SIZE / 4096);
    constexpr uint32_t stackPages = 0x1000000 / 4096;
    constexpr uint32_t physicalPages = 512 * 1024 * 1024 / 4096;
    uint32_t usedPages = heapBytes / 4096 + imagePages + stackPages;
    memset(base + address, 0, 104);
    memory->write32(address, 104);
    memory->write32(address + 4, physicalPages);
    memory->write32(address + 12, physicalPages - usedPages);
    memory->write32(address + 16, 0x50000000);
    memory->write32(address + 20, heapBytes);
    memory->write32(address + 24, usedPages);
    memory->write32(address + 32, stackPages);
    memory->write32(address + 36, imagePages);
    memory->write32(address + 40, heapBytes / 4096);
    memory->write32(address + 44, heapBytes / 4096);
    memory->write32(address + 100, physicalPages - 1);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeBugCheck) { PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "guest KeBugCheck"); }
PPC_FUNC(__imp__KeBugCheckEx) { PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "guest KeBugCheckEx"); }
PPC_FUNC(__imp__DbgBreakPoint) { PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "guest DbgBreakPoint assertion"); }
PPC_FUNC(__imp__HalReturnToFirmware) { PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "guest firmware transition requested"); }
PPC_FUNC(__imp__XamLoaderTerminateTitle) { PPC_RECOMP_FAILURE(ctx, uint32_t(ctx.lr), "guest requested title termination before gameplay"); }
