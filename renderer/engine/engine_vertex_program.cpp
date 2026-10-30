#include "engine_vertex_program.h"
#include "native_layer.h"
#include "runtime/native/stall_profiler.h"
#include "engine_performance.h"
#include "thread_counters.h"
#include "simple_mesh.h"
#include "render_trace.h"
#include "ppc_recomp_shared.h"
#include <windows.h>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00;
std::atomic<uint64_t> attempts{0}, captured{0}, compared{0}, equal{0}, found{0}, missing{0};
ThreadCounters<3> bindingCounts;
void incrementBindingCounter(unsigned counter) {
    thread_local const auto writer=bindingCounts.acquireWriter();
    writer.increment(counter);
}
struct PreparedBinding {
    uint8_t* base = nullptr;
    EngineVertexProgramSource source;
    EngineVertexBindingState binding;
};
thread_local std::optional<PreparedBinding> preparedBinding;
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool copy(uint8_t* base, uint64_t address, void* out, size_t size) {
    if (!base || !address || address + size > 0x100000000ull) return false;
    return copyRenderMemory(base,address,out,size);
}
bool initializedModes(uint8_t* base) {
    std::array<uint8_t, 108> flags; std::array<uint8_t, 27> counts;
    if (!copy(base, 0x82A5CD88, flags.data(), flags.size()) || !copy(base, 0x82A5CDF4, counts.data(), counts.size())) return false;
    struct ModeBytes {std::array<uint8_t,108> flags;std::array<uint8_t,27> counts;};
    // Mode definitions are immutable. Own their exact BE representation,
    // while retaining both complete guarded guest reads before validation.
    static const ModeBytes expected=[] {
        ModeBytes bytes;
        const auto& modes=engineVertexModes();
        for(unsigned i=0;i<modes.size();++i) {
            for(unsigned byte=0;byte<4;++byte)bytes.flags[i*4+byte]=uint8_t(modes[i].flags>>(24-byte*8));
            bytes.counts[i]=modes[i].reservation;
        }
        return bytes;
    }();
    return std::memcmp(flags.data(),expected.flags.data(),flags.size())==0 &&
        std::memcmp(counts.data(),expected.counts.data(),counts.size())==0;
}
bool readSource(uint8_t* base, EngineVertexProgramSource& out) {
    // Each local is consumed only after its complete guarded copy succeeds.
    std::array<uint8_t, 12> selector; std::array<uint8_t, 840> state; std::array<uint8_t, 8> model;
    if (!copy(base, context + 8224, selector.data(), selector.size()) || !copy(base, context + 16420, state.data(), state.size())) return false;
    EngineVertexProgramSource result;
    result.matrixBase = be32(selector.data()); result.matrixIndex = be32(selector.data() + 8);
    const uint64_t address = uint64_t(result.matrixBase) + uint64_t(result.matrixIndex) * 656 + 16;
    if (!result.matrixBase || result.matrixIndex >= 256 || (address & 3) || !copy(base, address + 640, model.data(), model.size())) return false;
    result.matrixAddress = uint32_t(address); result.paletteAddress = be32(model.data());
    result.treeRoot = be32(state.data() + 836);
    auto& input = result.input;
    input.declarationFlags = be32(state.data()); input.conversionMask = be32(state.data() + 8);
    input.declarationAddress = be32(state.data() + 108);
    // The original clears unrelated dirty bits before returning; retain only
    // bits that influence this key when rechecking its source.
    input.rendererFlags = be32(state.data() + 760) & 0x180000;
    input.enabledCoordinates = be32(state.data() + 764); input.modelFlags = be32(model.data() + 4);
    input.materialFlags = be32(state.data() + 568); input.palette = result.paletteAddress != 0;
    for (unsigned s = 0; s < 8; ++s) { input.modes[s] = state[524+s]; input.coordinateSources[s] = state[532+s]; }
    if (input.palette) {
        uint8_t second = 0;
        if (!input.declarationAddress || !copy(base, uint64_t(input.declarationAddress) + 24, &second, 1)) return false;
        input.secondBlendIndices = second != 0;
    }
    out = result; return true;
}
}
bool buildEngineVertexProgramKey(const EngineVertexProgramInput& input, EngineVertexProgramKey& output) noexcept {
    EngineVertexDescriptor descriptor; descriptor.declarationFlags = input.declarationFlags;
    if (!(input.declarationFlags & 0x400)) descriptor.flags |= 0x01000000;
    if (!(input.rendererFlags & 0x80000) || (input.rendererFlags & 0x100000)) descriptor.flags |= 0x02000000;
    EngineVertexDescriptorInput texture; texture.modes = input.modes; texture.materialFlags = input.materialFlags;
    texture.enabledCoordinates = input.rendererFlags & 0x100000 ? 0 : input.enabledCoordinates;
    for (unsigned s = 0; s < 8; ++s)
        if (input.modes[s] != 4 && !(input.modelFlags & (4u << s))) texture.matrixMask |= 1u << s;
    if (!buildEngineVertexDescriptor(descriptor, texture, descriptor)) return false;
    if (input.palette) descriptor.flags = (descriptor.flags & ~0xF0000u) | (input.secondBlendIndices ? 0x80000u : 0x40000u);
    const auto& s = input.coordinateSources;
    // Preserve the original packed-byte operations, including high source
    // bits. Treating every byte as a clean 3-bit selector changes valid keys.
    uint32_t a = (s[4] & 7u) | std::rotl(uint32_t(s[7]), 9);
    a = (std::rotl(a, 3) & 0xFFFFFFF8u) | (s[3] & 7u);
    a = (std::rotl(a, 3) & 0xFFFFFFF8u) | (s[2] & 7u);
    a = (std::rotl(a, 3) & 0xFFFFFFF8u) | (s[1] & 7u);
    uint32_t b = (s[5] & 199u) | std::rotl(uint32_t(s[6]), 3);
    b = ((std::rotl(b, 15) & 0xFFFF8000u) | (s[0] & 7u)) & 0xFF1FFFFFu;
    descriptor.coordinateMapping = std::rotl((std::rotl(a, 3) & 0xFFFFFFF8u) | b, 8) & 0xFFFFFF00u;
    descriptor.flags = (descriptor.flags & ~0xFF00u) | (texture.matrixMask << 8);
    if (input.conversionMask) {
        if (input.conversionMask & 1) descriptor.flags |= 0x04000000;
        descriptor.flags |= (descriptor.flags & 0xFFFFFF00u) + (std::rotl(input.conversionMask, 31) & 15u);
    }
    // Key1..5 are exactly the descriptor's encoded first20 bytes. The unused
    // descriptor tail need not be encoded and decoded to produce these words.
    EngineVertexProgramKey result{input.declarationAddress,descriptor.coordinateMapping,descriptor.flags,
        be32(descriptor.modes.data()),be32(descriptor.modes.data()+4),descriptor.declarationFlags};
    output = result; return true;
}
bool lookupEngineVertexProgram(uint8_t* base, uint32_t root, const EngineVertexProgramKey& key,
                               EngineVertexProgramSelection& output) noexcept {
    EngineVertexProgramSelection result;
    uint32_t node = root & ~1u;
    while (node) {
        if (node < 40 || (node & 3) || result.visitedCount == result.visited.size()) return false;
        const uint32_t record = node - 40;
        for (unsigned i = 0; i < result.visitedCount; ++i) if (result.visited[i].address == record) return false;
        auto& snapshot = result.visited[result.visitedCount++]; snapshot.address = record;
        if (!copy(base, record, snapshot.bytes.data(), snapshot.bytes.size())) return false;
        EngineVertexProgramKey candidate;
        for (unsigned i = 0; i < 6; ++i) candidate[i] = be32(snapshot.bytes.data() + 16 + i*4);
        // Unsigned lexicographic six-word comparison is identical to the
        // original three big-endian uint64 comparisons.
        if (candidate == key) {
            result.recordAddress = record; result.bindingAddress = be32(snapshot.bytes.data() + 8);
            if (!result.bindingAddress && be32(snapshot.bytes.data())) {
                const uint32_t storage = be32(snapshot.bytes.data() + 4);
                if (storage > 0xFFFFFFFBu) return false;
                if (storage) result.bindingAddress = storage + 4;
            }
            break;
        }
        node = be32(snapshot.bytes.data() + (candidate < key ? 44 : 40)) & ~1u;
    }
    output = result; return true;
}
bool beginEngineVertexProgramObservation(uint8_t* base, uint32_t stack, EngineVertexProgramObservation& output) noexcept {
    ++attempts;
    if (stack < 320 || (stack & 15)) return false;
    EngineVertexProgramObservation result; result.keyAddress = stack - 224;
    EngineVertexProgramSource after;
    if (!initializedModes(base) || !readSource(base, result.source) || !buildEngineVertexProgramKey(result.source.input, result.key) ||
        !lookupEngineVertexProgram(base, result.source.treeRoot, result.key, result.selection) ||
        !readSource(base, after) || after != result.source) return false;
    ++captured; output = result; return true;
}
void finishEngineVertexProgramObservation(uint8_t* base, EngineVertexProgramObservation& observation) noexcept {
    preparedBinding.reset();
    observation.comparison = TransformComparison::unavailable;
    EngineVertexProgramSource after; std::array<uint8_t, 24> key{}; std::array<uint8_t, 4> selected{};
    if (!initializedModes(base) || !readSource(base, after) || after != observation.source ||
        !copy(base, observation.keyAddress, key.data(), key.size()) || !copy(base, context + 17188, selected.data(), selected.size())) return;
    for (unsigned i = 0; i < observation.selection.visitedCount; ++i) {
        std::array<uint8_t, 48> node{};
        const auto& snapshot = observation.selection.visited[i];
        if (!copy(base, snapshot.address, node.data(), node.size()) || node != snapshot.bytes) return;
    }
    bool match = be32(selected.data()) == observation.selection.recordAddress;
    for (unsigned i = 0; i < 6; ++i) match &= be32(key.data() + i*4) == observation.key[i];
    ++compared; if (match) ++equal;
    if (observation.selection.recordAddress) ++found; else ++missing;
    observation.comparison = match ? TransformComparison::equal : TransformComparison::different;
    if (match && observation.selection.recordAddress && observation.selection.bindingAddress) {
        PreparedBinding prepared; prepared.base = base; prepared.source = observation.source;
        auto& binding = prepared.binding;
        std::array<uint8_t,80> descriptor{}; std::array<uint8_t,4> device{}, activeBinding{};
        // Original frame320: descriptor at local128, key at local96.
        const uint64_t descriptorAddress = uint64_t(observation.keyAddress) + 32;
        if (!copy(base, descriptorAddress, descriptor.data(), descriptor.size()) ||
            !copy(base, context + 15748, device.data(), 4)) return;
        binding.deviceAddress = be32(device.data());
        if (!binding.deviceAddress || (binding.deviceAddress & 15) ||
            !copy(base, uint64_t(binding.deviceAddress) + 12688, activeBinding.data(), 4) ||
            be32(activeBinding.data()) != observation.selection.bindingAddress) return;
        for (unsigned i=0;i<5;++i) if (be32(descriptor.data()+i*4) != observation.key[i+1]) return;
        binding.descriptor = decodeEngineVertexDescriptor(descriptor);
        binding.key = observation.key; binding.descriptorAddress = uint32_t(descriptorAddress);
        binding.recordAddress = observation.selection.recordAddress; binding.bindingAddress = observation.selection.bindingAddress;
        binding.matrixAddress = observation.source.matrixAddress;
        preparedBinding = prepared;
        incrementBindingCounter(2);
    }
}
static bool snapshotEngineVertexBindingsInto(uint8_t* base, EngineVertexBindingSnapshot& result) noexcept {
    incrementBindingCounter(0);
    if (!preparedBinding || preparedBinding->base != base) return false;
    const auto& prepared = *preparedBinding;
    auto current = [&] {
        EngineVertexProgramSource source;
        std::array<uint8_t,4> device, record, binding;
        // The retained key was already proved from this exact source during
        // preparation. Rebuilding it three times per draw adds no validation.
        return readSource(base, source) && source == prepared.source &&
            copy(base, context+15748, device.data(), 4) && be32(device.data()) == prepared.binding.deviceAddress &&
            copy(base, context+17188, record.data(), 4) && be32(record.data()) == prepared.binding.recordAddress &&
            copy(base, uint64_t(prepared.binding.deviceAddress)+12688, binding.data(), 4) &&
            be32(binding.data()) == prepared.binding.bindingAddress;
    };
    static_cast<EngineVertexBindingState&>(result)=prepared.binding;
    const uint64_t address = uint64_t(result.deviceAddress) + 1920;
    // Bracket the owned copy and full byte comparison with source/binding
    // checks. The comparison has the same guarded reads as the copy.
    if (!current() || !copy(base, address, result.constantBytes.data(), result.constantBytes.size()) ||
        !equalRenderMemory(base,address,result.constantBytes.data(),result.constantBytes.size()) || !current()) return false;
    incrementBindingCounter(1); return true;
}
bool snapshotEngineVertexBindings(uint8_t* base,EngineVertexBindingSnapshot& output) noexcept {
    EngineVertexBindingSnapshot result(EngineVertexBindingSnapshot::captureStorage());
    if(!snapshotEngineVertexBindingsInto(base,result))return false;
    output=result;return true;
}
bool captureEngineVertexBindings(uint8_t* base,std::optional<EngineVertexBindingSnapshot>& output) noexcept {
    if(snapshotEngineVertexBindingsInto(base,output.emplace(EngineVertexBindingSnapshot::captureStorage())))return true;
    output.reset();return false;
}
// Retain the completed original selection directly during ordinary rendering.
// Diagnostic mode additionally reconstructs/traverses the pre-call cache tree.
// The original still builds its descriptor and selects the record exactly once.
void retainCompletedVertexBinding(uint8_t* base,uint32_t stack) noexcept {
    preparedBinding.reset();
    if(stack<320 || (stack&15))return;
    PreparedBinding prepared;prepared.base=base;
    EngineVertexProgramSource after;
    auto& binding=prepared.binding;
    std::array<uint8_t,24> key{};
    std::array<uint8_t,80> descriptor{};
    std::array<uint8_t,4> device{},selected{},active{};
    std::array<uint8_t,48> record{},again{};
    const std::array completedReads{
        RenderMemoryRead{stack-224,key.data(),key.size()},
        RenderMemoryRead{stack-192,descriptor.data(),descriptor.size()},
        RenderMemoryRead{context+15748,device.data(),device.size()},
        RenderMemoryRead{context+17188,selected.data(),selected.size()}};
    if(!initializedModes(base) || !readSource(base,prepared.source) ||
       !buildEngineVertexProgramKey(prepared.source.input,binding.key) ||
       !copyRenderMemoryBatch(base,completedReads))return;
    binding.deviceAddress=be32(device.data());binding.recordAddress=be32(selected.data());
    if(!binding.deviceAddress || (binding.deviceAddress&15) || !binding.recordAddress || (binding.recordAddress&3) ||
       !copy(base,binding.recordAddress,record.data(),record.size()) ||
       !copy(base,uint64_t(binding.deviceAddress)+12688,active.data(),4))return;
    for(unsigned i=0;i<6;++i)
        if(be32(key.data()+i*4)!=binding.key[i] || be32(record.data()+16+i*4)!=binding.key[i])return;
    for(unsigned i=0;i<5;++i)if(be32(descriptor.data()+i*4)!=binding.key[i+1])return;
    binding.bindingAddress=be32(record.data()+8);
    if(!binding.bindingAddress && be32(record.data())) {
        const auto storage=be32(record.data()+4);
        if(storage>0xFFFFFFFBu)return;
        if(storage)binding.bindingAddress=storage+4;
    }
    if(!binding.bindingAddress || binding.bindingAddress!=be32(active.data()) ||
       !readSource(base,after) || after!=prepared.source ||
       !copy(base,binding.recordAddress,again.data(),again.size()) || again!=record)return;
    binding.descriptor=decodeEngineVertexDescriptor(descriptor);
    binding.descriptorAddress=stack-192;binding.matrixAddress=prepared.source.matrixAddress;
    preparedBinding=std::move(prepared);incrementBindingCounter(2);
}
// NATIVE_LAYER_BEGIN: vertex-binding currency without copying the bank. readSource() and the
// current() checks are reproduced with direct loads of exactly the fields they compare. The
// owned 4 KiB bank copy and full re-compare are not needed: the native layer reads the used
// constant ranges from the live device bank at snapshot time, with no guest code in between.
namespace {
// Direct-load readSource(): the same fields, conditions and result. Faults reach the caller's guard.
bool fastReadSource(const uint8_t* base, EngineVertexProgramSource& result) {
    result = {};
    result.matrixBase = nativeGuest32(base, context + 8224);
    result.matrixIndex = nativeGuest32(base, context + 8232);
    const uint64_t address = uint64_t(result.matrixBase) + uint64_t(result.matrixIndex) * 656 + 16;
    // copy(base, address + 640, model, 8): nonzero address and within the guest space.
    if (!result.matrixBase || result.matrixIndex >= 256 || (address & 3) || !(address + 640) ||
        address + 640 + 8 > 0x100000000ull) return false;
    const uint32_t model = uint32_t(address + 640);
    const uint32_t state = context + 16420;
    result.matrixAddress = uint32_t(address);
    result.paletteAddress = nativeGuest32(base, model);
    result.treeRoot = nativeGuest32(base, state + 836);
    auto& input = result.input;
    input.declarationFlags = nativeGuest32(base, state);
    input.conversionMask = nativeGuest32(base, state + 8);
    input.declarationAddress = nativeGuest32(base, state + 108);
    input.rendererFlags = nativeGuest32(base, state + 760) & 0x180000;
    input.enabledCoordinates = nativeGuest32(base, state + 764);
    input.modelFlags = nativeGuest32(base, model + 4);
    input.materialFlags = nativeGuest32(base, state + 568);
    input.palette = result.paletteAddress != 0;
    for (unsigned s = 0; s < 8; ++s) { input.modes[s] = base[state + 524 + s]; input.coordinateSources[s] = base[state + 532 + s]; }
    if (input.palette) {
        const uint64_t second = uint64_t(input.declarationAddress) + 24;
        if (!input.declarationAddress || second + 1 > 0x100000000ull) return false;
        input.secondBlendIndices = base[second] != 0;
    }
    return true;
}
// 1 = current, 0 = the original capture fails. Faults are handled by the caller's guard.
__attribute__((hot)) __declspec(noinline) int fastBindingImpl(const uint8_t* base, const PreparedBinding& prepared) {
    EngineVertexProgramSource result;
    if (!fastReadSource(base, result) || !(result == prepared.source)) return 0;
    if (nativeGuest32(base, context + 15748) != prepared.binding.deviceAddress) return 0;
    if (nativeGuest32(base, context + 17188) != prepared.binding.recordAddress) return 0;
    const uint64_t binding = uint64_t(prepared.binding.deviceAddress) + 12688;
    if (!binding || binding + 4 > 0x100000000ull) return 0;
    if (nativeGuest32(base, uint32_t(binding)) != prepared.binding.bindingAddress) return 0;
    // The original also copies the whole bank (device+1920, 4096 bytes) through copy().
    const uint64_t bank = uint64_t(prepared.binding.deviceAddress) + 1920;
    if (!bank || bank + 4096 > 0x100000000ull) return 0;
    return 1;
}
int guardedBinding(const uint8_t* base, const PreparedBinding& prepared) noexcept {
#ifdef _WIN32
    __try { return fastBindingImpl(base, prepared); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
              ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return -1; }
#else
    // Preserve the original POSIX guarded-copy path until a native guard is available.
    return -1;
#endif
}
// The immutable mode tables initializedModes() compares against, built the same way.
struct FastModeBytes { std::array<uint8_t,108> flags; std::array<uint8_t,27> counts; };
const FastModeBytes& fastExpectedModes() {
    static const FastModeBytes expected = [] {
        FastModeBytes bytes;
        const auto& modes = engineVertexModes();
        for (unsigned i = 0; i < modes.size(); ++i) {
            for (unsigned byte = 0; byte < 4; ++byte) bytes.flags[i*4+byte] = uint8_t(modes[i].flags >> (24 - byte*8));
            bytes.counts[i] = modes[i].reservation;
        }
        return bytes;
    }();
    return expected;
}
// Mirrors retainCompletedVertexBinding() for stack >= 320 and 16-byte aligned: 1 = retained
// (prepared filled), 0 = the original does not retain. The original's second readSource() and
// record re-read are not needed: this thread just ran 82248C80 and no guest code runs in between.
__attribute__((hot)) __declspec(noinline) int fastRetainImpl(const uint8_t* base, uint32_t stack, const FastModeBytes& modes,
                                                             PreparedBinding& prepared) {
    if (std::memcmp(base + 0x82A5CD88u, modes.flags.data(), modes.flags.size()) ||
        std::memcmp(base + 0x82A5CDF4u, modes.counts.data(), modes.counts.size())) return 0;
    if (!fastReadSource(base, prepared.source)) return 0;
    auto& binding = prepared.binding;
    if (!buildEngineVertexProgramKey(prepared.source.input, binding.key)) return 0;
    std::array<uint8_t, 24> key;
    std::array<uint8_t, 80> descriptor;
    std::array<uint8_t, 48> record;
    std::memcpy(key.data(), base + stack - 224, key.size());
    std::memcpy(descriptor.data(), base + stack - 192, descriptor.size());
    binding.deviceAddress = nativeGuest32(base, context + 15748);
    binding.recordAddress = nativeGuest32(base, context + 17188);
    if (!binding.deviceAddress || (binding.deviceAddress & 15) || !binding.recordAddress || (binding.recordAddress & 3)) return 0;
    const uint64_t activeAddress = uint64_t(binding.deviceAddress) + 12688;
    if (uint64_t(binding.recordAddress) + record.size() > 0x100000000ull || activeAddress + 4 > 0x100000000ull) return 0;
    std::memcpy(record.data(), base + binding.recordAddress, record.size());
    const uint32_t active = nativeGuest32(base, uint32_t(activeAddress));
    for (unsigned i = 0; i < 6; ++i)
        if (be32(key.data() + i*4) != binding.key[i] || be32(record.data() + 16 + i*4) != binding.key[i]) return 0;
    for (unsigned i = 0; i < 5; ++i) if (be32(descriptor.data() + i*4) != binding.key[i+1]) return 0;
    binding.bindingAddress = be32(record.data() + 8);
    if (!binding.bindingAddress && be32(record.data())) {
        const auto storage = be32(record.data() + 4);
        if (storage > 0xFFFFFFFBu) return 0;
        if (storage) binding.bindingAddress = storage + 4;
    }
    if (!binding.bindingAddress || binding.bindingAddress != active) return 0;
    binding.descriptor = decodeEngineVertexDescriptor(descriptor);
    binding.descriptorAddress = stack - 192;
    binding.matrixAddress = prepared.source.matrixAddress;
    return 1;
}
int guardedRetain(const uint8_t* base, uint32_t stack, const FastModeBytes& modes, PreparedBinding& prepared) noexcept {
#ifdef _WIN32
    __try { return fastRetainImpl(base, stack, modes, prepared); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
              ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return -1; }
#else
    // Preserve the original POSIX guarded-copy path until a native guard is available.
    return -1;
#endif
}
}
__attribute__((hot)) NativeLayerResult currentEngineVertexBindingFast(uint8_t* base, EngineVertexBindingState& output) noexcept {
    // Counters follow the original only on success; any other outcome runs the original capture.
    if (!preparedBinding || preparedBinding->base != base) return NativeLayerResult::empty;
    const int current = guardedBinding(base, *preparedBinding);
    if (current < 0) return NativeLayerResult::fallback;
    if (!current) return NativeLayerResult::empty;
    incrementBindingCounter(0);
    incrementBindingCounter(1);
    output = preparedBinding->binding;
    return NativeLayerResult::ok;
}
void retainCompletedVertexBindingLayer(uint8_t* base, uint32_t stack) noexcept {
    const auto mode = nativeLayerMode();
    if (mode == NativeLayerMode::off) { retainCompletedVertexBinding(base, stack); return; }
    if (stack < 320 || (stack & 15)) { preparedBinding.reset(); return; }
    const auto& modes = fastExpectedModes();
    PreparedBinding prepared;
    prepared.base = base;
    const int fast = guardedRetain(base, stack, modes, prepared);
    if (mode == NativeLayerMode::check) {
        retainCompletedVertexBinding(base, stack); // The original result is the one retained.
        if (fast >= 0) {
            const bool same = (fast == 1) == preparedBinding.has_value() &&
                (fast != 1 || (preparedBinding->source == prepared.source && preparedBinding->binding == prepared.binding));
            nativeLayerReportCheck("retain", same);
        }
        return;
    }
    if (fast < 0) { retainCompletedVertexBinding(base, stack); return; }
    preparedBinding.reset();
    if (fast == 1) { preparedBinding = prepared; incrementBindingCounter(2); }
}
// NATIVE_LAYER_END
void printEngineVertexProgramCounters() {
    const auto matches = equal.load(), comparisons = compared.load();
    std::fprintf(stderr, "[EngineVertexProgram] attempts=%llu captured=%llu compared=%llu bitExact=%llu different=%llu found=%llu missing=%llu (owned keys/cache selection; not native world shaders)\n",
        attempts.load(), captured.load(), comparisons, matches, comparisons - matches, found.load(), missing.load());
    const auto bindings=bindingCounts.snapshot();
    std::fprintf(stderr, "[EngineVertexBindings] descriptorsRetained=%llu attempts=%llu captured=%llu (owned final descriptors/draw-time CPU constants; no world draw claim)\n",
        bindings[2],bindings[0],bindings[1]);
}
}
extern "C" PPC_FUNC(__imp__sub_82248C80);
PPC_FUNC(sub_82248C80) {
    DarkRecomp::Native::StallProfiler::Scope stallCall(DarkRecomp::Native::StallProfiler::Section::Guest, "sub_82248C80", 0x82248C80, uint32_t(ctx.lr));
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::vertexProgram);
    using namespace DarkRecomp::Native;
    if(enginePreviewEnabled() && !renderTraceEnabled()) {
        preparedBinding.reset();
        const auto stack=ctx.r1.u32;
        __imp__sub_82248C80(ctx,base);
        retainCompletedVertexBindingLayer(base,stack); // NATIVE_LAYER
        return;
    }
    EngineVertexProgramObservation observation;
    // A failed/unobserved preparation must not leave a prior descriptor usable.
    preparedBinding.reset();
    const bool observe = enginePreviewEnabled() && beginEngineVertexProgramObservation(base, ctx.r1.u32, observation);
    __imp__sub_82248C80(ctx, base);
    if (observe) {
        finishEngineVertexProgramObservation(base, observation);
        traceEngineVertexProgram(base, observation);
    }
}
