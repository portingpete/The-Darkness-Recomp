#include "render_trace.h"
#include "engine_performance.h"
#include "prompt_origin.h"
#include "simple_mesh.h"
#include "texture_upload.h"
#include "stored_geometry.h"
#include "engine_texture_constants.h"
#include "engine_vertex_descriptor.h"
#include "engine_vertex_program.h"
#include "world_mesh.h"
#include "runtime/native/runtime.h"
#include "runtime/native/audio_refill_trace.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <xmmintrin.h>

// Bounded read-only input diagnostics for the original axis conversion path.
extern "C" PPC_FUNC(__imp__sub_82793C48);

namespace {
constexpr uint32_t renderContext = 0x82A69B00;
std::atomic<bool> enabled{false};
std::atomic<unsigned> requestedInspection{0};
std::mutex traceMutex;
std::filesystem::path traceDirectory;
std::ofstream traceLog;
uint64_t sequence = 0, writtenBytes = 0, epoch = 0;
uint64_t generalBytes = 0, geometryBytes = 0, textureConstantBytes = 0;
thread_local uint32_t preparingTexture = 0;
thread_local uint32_t preparingTextureFaces = 1;
thread_local DarkRecomp::Native::TextureUpload* preparingImage = nullptr;
constexpr uint64_t maxBytes = 16 * 1024 * 1024;
thread_local DarkRecomp::Native::StoredUpload* preparingGeometry = nullptr;
using StoredRequest = DarkRecomp::Native::StoredDrawRequest;
thread_local const StoredRequest* drawingGeometry = nullptr;
thread_local uint8_t* clearingBase = nullptr;

template<class T> struct ScopedPointer {
    T*& slot; T* previous;
    ScopedPointer(T*& slot, T* current) : slot(slot), previous(slot) { slot = current; }
    ~ScopedPointer() { slot = previous; }
};

// Guarded host copies reject a racing decommit. No engine memory or register
// is modified, and failed copies are never published to the renderer.
bool copyGuest(uint8_t* base, uint32_t address, void* out, size_t size) {
    if (!address || uint64_t(address) + size > 0x100000000ull) return false;
    return DarkRecomp::Native::copyRenderMemory(base,address,out,size);
}
uint32_t word(uint8_t* base, uint32_t address) {
    uint32_t result = 0;
    return copyGuest(base, address, &result, 4) ? _byteswap_ulong(result) : 0;
}
thread_local uint32_t olderSelectedIndex = 0;
thread_local uint32_t olderSelectedContainer = 0;
thread_local bool olderSelectedValid = false;
void notePromptResult(uint8_t* base, uint32_t container, uint32_t actualIndex, uint32_t image, const char* which) {
    if (!image) return;
    char name[64]{};
    uint8_t origin = DarkRecomp::Prompts::resolveXtc2Origin(base, container, actualIndex, name);
    const char* layout = "XTC2";
    if (!origin && !name[0]) {
        origin = DarkRecomp::Prompts::resolveXtcOrigin(base, container, actualIndex);
        layout = "XTC";
    }
    // Unknown observations commit too (inside the fetch scope): they
    // invalidate stale origins. Upload notes go directly to the sticky
    // collector independently of TLS scope. Bounded logs only.
    DarkRecomp::Native::previewCommitPromptOrigin(image, origin);
    if (preparingImage) preparingImage->notePromptOrigin(origin);
    DarkRecomp::Native::countPromptOriginAttempt(origin != 0);
    if (origin) {
        static std::atomic<unsigned> logs{0};
        if (logs++ < 8)
            std::fprintf(stderr, "[PromptOrigin] %s container=%08X index=%u image=%08X name=%s origin=%u\n",
                which, container, actualIndex, image, name[0] ? name : "?",
                unsigned(origin));
    } else {
        static std::atomic<unsigned> rejectLogs{0};
        if (rejectLogs++ < 6)
            std::fprintf(stderr, "[PromptOriginReject] %s/%s container=%08X index=%u image=%08X name=%s\n",
                which, layout, container, actualIndex, image, name[0] ? name : "(unresolved)");
    }
}
// A small, separate budget retains ordered engine calls at world startup even
// when intro-video snapshots have exhausted the general trace budget.
void captureWorldBoundary(uint32_t function,PPCContext& ctx,uint8_t* base) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) return;
    struct RestoreFloatingPoint {unsigned mode=_mm_getcsr();~RestoreFloatingPoint(){_mm_setcsr(mode);}} restore;
    try {
        std::lock_guard lock(traceMutex);
        static bool world=false;
        static bool level=false;
        static unsigned frames=0,events=0;
        static unsigned inspection=0;
        static size_t bytes=0;
        static const bool effectProbe=std::getenv("DARK_EFFECT_PROBE")!=nullptr;
        static bool effectCapture=false,effectSeen=false;
        static std::unordered_set<std::string> levelDraws;
        const auto requested=requestedInspection.load(std::memory_order_relaxed);
        if(requested!=inspection && !effectCapture) {
            inspection=requested;frames=events=0;bytes=0;levelDraws.clear();
        }
        if (function==0x82868FE8 && uint32_t(ctx.lr)==0x8225E3B8) world=true;
        std::array<char,96> programName{};
        const auto programAddress=word(base,renderContext+16896);
        if (world && function==0x82868FE8 && (!level || frames<3 || (effectProbe && !effectSeen))) {
            copyGuest(base,word(base,programAddress+4),programName.data(),programName.size()-1);
            if(effectProbe && !effectSeen && std::string_view(programName.data())=="XREngine_RadialBlurInvert") {
                // The ordinary frame budget may end before postprocessing.
                // Restart it once at the effect, retaining its following
                // draw/clear/resolve/present chain with the same hard limits.
                effectCapture=effectSeen=true;frames=events=0;bytes=0;levelDraws.clear();
            }
            if (!inspection && !level && std::string_view(programName.data()).starts_with("XRShader_FP20_LFM")) {
                level=true;frames=events=0;bytes=0;
            }
            // Keep complete resolve/present boundaries while bounding the
            // thousands of repeated mesh draws in each level frame.
            const auto key=std::string(programName.data())+":"+std::to_string(word(base,programAddress+16)>>8);
            if (level && !inspection && !effectCapture && !levelDraws.insert(key).second) return;
        }
        if (!world || frames>=3 || events>=512 || bytes>=2*1024*1024) return;
        std::ostringstream line;
        line<<"{\"event\":"<<++events<<",\"frame\":"<<frames<<",\"function\":"<<function<<",\"lr\":"<<uint32_t(ctx.lr)
            <<",\"r\":["<<ctx.r3.u32<<','<<ctx.r4.u32<<','<<ctx.r5.u32<<','<<ctx.r6.u32<<','<<ctx.r7.u32<<','<<ctx.r8.u32<<','<<ctx.r9.u32<<','<<ctx.r10.u32<<']';
        line<<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"thread\":"<<GetCurrentThreadId()<<",\"stack_lr\":[";
        auto stack=ctx.r1.u32;
        for(unsigned n=0;n<16;++n) {
            const auto caller=word(base,stack);
            if(caller<=stack || uint64_t(caller)-stack>1024*1024)break;
            if(n)line<<',';line<<word(base,caller-8);stack=caller;
        }
        line<<']';
        auto memory=[&](const std::string& label,uint32_t address,size_t size,const uint8_t* owned=nullptr) {
            std::vector<uint8_t> data(size);
            if (owned) std::copy_n(owned,size,data.data());
            else if (!address || !copyGuest(base,address,data.data(),size)) return;
            line<<",\""<<label<<"\":{\"address\":"<<address<<",\"hex\":\"";
            constexpr char digits[]="0123456789abcdef";
            for (auto b:data) line<<digits[b>>4]<<digits[b&15];
            line<<"\"}";
        };
        const auto device=word(base,renderContext+15748);
        memory("attributes",renderContext+16896,160);memory("viewport",renderContext+17152,24);
        memory("targets",device+12432,20);
        memory("rasterizer_state",device+10568,4);
        for (unsigned i=0;i<5;++i)memory("surface"+std::to_string(i),word(base,device+12432+i*4),40);
        const auto program=word(base,renderContext+16896);
        memory("program",program,20);memory("program_name",word(base,program+4),96);
        if (program) memory("fragment_constants",word(base,program+12),(std::min)(word(base,program+16)&255u,16u)*16);
        else memory("fixed_constants",device+6016,64);
        const auto textureTable=word(base,renderContext+17964);
        for(unsigned slot=0;slot<16;++slot) {
            const auto id=word(base,renderContext+16904+slot*2)>>16;
            if (id && textureTable && uint64_t(textureTable)+(id+1ull)*4+4<=0x100000000ull) {
                const auto resource=word(base,textureTable+(id+1)*4);
                memory("texture_resource"+std::to_string(slot),resource,192);
                const auto pointer=word(base,resource+84);
                memory("texture_object"+std::to_string(slot),pointer?pointer:word(base,resource+8)?resource+12:0,64);
            }
        }
        line<<",\"stored_vertex_id\":"<<word(base,renderContext+16532);
        auto descriptor=word(base,renderContext+16512);
        if (!descriptor && !word(base,renderContext+12564)) descriptor=renderContext+12480;
        memory("vertex_descriptor",descriptor,68);
        if (descriptor) {
            const auto count=word(base,descriptor)>>16;
            if (count && count<=4096) {
                memory("positions",word(base,descriptor+4),count*12);
                memory("colors",word(base,descriptor+52),count*4);
                uint8_t components[8]{};copyGuest(base,descriptor+40,components,8);
                for(unsigned slot=0;slot<8;++slot)if(components[slot] && components[slot]<=4)
                    memory("uv"+std::to_string(slot),word(base,descriptor+8+slot*4),count*components[slot]*4);
            }
        }
        if (function==0x82868FE8 && uint32_t(ctx.lr)==0x8225DD38 && ctx.r7.u32<=12288)
            memory("indices",ctx.r30.u32,ctx.r7.u32*2);
        if (function==0x82865FD0) {
            memory("destination",ctx.r6.u32,64);memory("resolve_rectangle",ctx.r5.u32,16);
            memory("resolve_offset",ctx.r7.u32,8);memory("resolve_clear",ctx.r10.u32,16);
        }
        if (function==0x828630D8) {memory("clear_rectangle",ctx.r5.u32,16);memory("clear_color",ctx.r6.u32,16);}
        if (function==0x82867620) {memory("frontbuffer",ctx.r4.u32,64);++frames;levelDraws.clear();}
        DarkRecomp::Native::EngineVertexBindingSnapshot binding;
        if (function==0x82868FE8 && DarkRecomp::Native::snapshotEngineVertexBindings(base,binding)) {
            const auto descriptor=DarkRecomp::Native::encodeEngineVertexDescriptor(binding.descriptor);
            memory("binding_descriptor",binding.descriptorAddress,80,descriptor.data());
            memory("binding_declaration",binding.key[0],96);
            // Original82250AD0 stores an allocator handle at declaration+0;
            // handle+16 (low two bits cleared) is the D3D declaration payload.
            const auto declarationHandle=word(base,binding.key[0]);
            memory("declaration_handle",declarationHandle,32);
            memory("native_declaration",word(base,declarationHandle+16)&~3u,256);
            memory("binding_constants",0,4096,binding.constantBytes.data());
        }
        line<<"}\n";const auto value=line.str();bytes+=value.size();
        const auto filename=effectCapture?std::string("effect-boundaries.jsonl"):inspection?"inspection-"+std::to_string(inspection)+"-boundaries.jsonl":
            std::string(level?"level-boundaries.jsonl":"world-boundaries.jsonl");
        std::ofstream file(traceDirectory/filename,std::ios::binary|std::ios::app);file<<value;
    } catch (...) {}
}
void chunk(std::ostream& metadata, uint8_t* base, const char* label, uint32_t address, uint32_t size,
           bool geometry = false, bool textureConstants = false, const uint8_t* owned = nullptr) {
    auto& categoryBytes = textureConstants ? textureConstantBytes : geometry ? geometryBytes : generalBytes;
    constexpr uint64_t textureBudget = 512 * 1024;
    const uint64_t categoryLimit = textureConstants ? textureBudget : geometry ? maxBytes / 2 : maxBytes / 2 - textureBudget;
    // Reserve half the trace for stored uploads/indices/actual draw calls;
    // earlier image and immediate draws must not consume their evidence.
    if (!address || !size || size > 1024 * 1024 || writtenBytes + size > maxBytes ||
        categoryBytes + size > categoryLimit) return;
    std::vector<uint8_t> bytes(size);
    if (owned) std::copy_n(owned, size, bytes.data());
    else if (!copyGuest(base, address, bytes.data(), bytes.size())) return;
    char filename[96];
    std::snprintf(filename, sizeof(filename), "%06llu-%s-%08X.bin", sequence, label, address);
    std::ofstream file(traceDirectory / filename, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    if (!file) { enabled = false; return; }
    writtenBytes += size;
    categoryBytes += size;
    metadata << ",\"" << label << "\":{\"address\":" << address << ",\"size\":" << size
             << ",\"file\":\"" << filename << "\"}";
}
void capture(uint32_t function, uint64_t call, PPCContext& ctx, uint8_t* base) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) return;
    bool sampled = call <= 4 || !(call & (call - 1)) || preparingTexture == 0x3158;
    const bool draw = function == 0x8225DBC8 || function == 0x8225CDD8 || function == 0x8225DE78;
    if (!sampled && !draw) return;
    try {
        std::lock_guard lock(traceMutex);
        if (!enabled || writtenBytes >= maxBytes) return;
        // Repeated text draws can hide short video/material transitions from
        // power-of-two sampling. Keep a bounded sample of distinct draw state.
        static std::unordered_set<std::string> materials;
        uint8_t state[160];
        if (draw && materials.size() < 128 && copyGuest(base, renderContext + 16896, state, sizeof(state))) {
            std::string key(reinterpret_cast<const char*>(state), 40);
            key.append(reinterpret_cast<const char*>(state + 92), 4);
            key.append(reinterpret_cast<const char*>(state + 144), 2);
            key.append(reinterpret_cast<const char*>(&function), sizeof(function));
            sampled |= materials.insert(std::move(key)).second;
        }
        if (!sampled) return;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"function\":" << function << ",\"call\":" << call
             << ",\"ms\":" << (GetTickCount64() - epoch) << ",\"thread\":" << GetCurrentThreadId()
             << ",\"lr\":" << uint32_t(ctx.lr) << ",\"r3\":" << ctx.r3.u32 << ",\"r4\":" << ctx.r4.u32
             << ",\"r5\":" << ctx.r5.u32 << ",\"r6\":" << ctx.r6.u32;
        chunk(line, base, "context", renderContext, 18000);
        const uint32_t program = word(base, renderContext + 16896);
        chunk(line, base, "program", program, 20);
        if (program) {
            chunk(line, base, "program_name", word(base, program + 4), 96);
            chunk(line, base, "program_constants", word(base, program + 12),
                  (std::min)(word(base, program + 16) & 255u, 16u) * 16);
        }
        const uint32_t matrixBase = word(base, renderContext + 8224);
        const uint32_t matrixIndex = word(base, renderContext + 8232);
        if (matrixBase && matrixIndex < 256 && uint64_t(matrixBase) + matrixIndex * 656ull + 80 <= 0x100000000ull)
            chunk(line, base, "modelview", matrixBase + matrixIndex * 656 + 16, 64);
        const uint32_t textureId = word(base, renderContext + 16904) >> 16;
        const uint32_t textureTable = word(base, renderContext + 17964);
        if (textureId && textureTable && uint64_t(textureTable) + (textureId + 1) * 4ull + 4 <= 0x100000000ull) {
            const uint32_t texture = word(base, textureTable + (textureId + 1) * 4);
            chunk(line, base, "texture0", texture, 192);
            line << ",\"texture_id\":" << textureId;
        }
        uint32_t descriptor = word(base, renderContext + 16512);
        const uint32_t storedId = function == 0x8225E2F0 ? ctx.r3.u32 : word(base, renderContext + 12564);
        if (!descriptor && !storedId) descriptor = renderContext + 12480;
        if (function == 0x8225CDD8) descriptor = ctx.r3.u32;
        if (descriptor) {
            chunk(line, base, "vertices", descriptor, 68);
            const uint32_t count = word(base, descriptor) >> 16;
            const uint32_t samples = (std::min)(count, 8u);
            chunk(line, base, "positions", word(base, descriptor + 4), samples * 12);
            chunk(line, base, "colors", word(base, descriptor + 52), samples * 4);
            uint8_t components = 0;
            if (copyGuest(base, descriptor + 40, &components, 1) && components <= 4)
                chunk(line, base, "texcoord0", word(base, descriptor + 8), samples * components * 4);
        }
        if (storedId) {
            line << ",\"stored_vb_id\":" << storedId;
            const uint32_t table = word(base, renderContext + 16636);
            if (table && storedId < 65536 && uint64_t(table) + (storedId + 1) * 4ull + 4 <= 0x100000000ull)
                chunk(line, base, "stored_vertex_resource", word(base, table + (storedId + 1) * 4), 108);
        }
        if (function == 0x8225E2F0) {
            line << ",\"stored_ib_id\":" << ctx.r4.u32;
            const uint32_t table = word(base, renderContext + 16636);
            if (table && ctx.r4.u32 < 65536 && uint64_t(table) + (ctx.r4.u32 + 1) * 4ull + 4 <= 0x100000000ull)
                chunk(line, base, "stored_index_resource", word(base, table + (ctx.r4.u32 + 1) * 4), 108);
        }
        if (function == 0x8225DBC8 && ctx.r4.u32 < 65535)
            chunk(line, base, "indices", ctx.r3.u32, (std::min)(ctx.r4.u32, 128u) * 6);
        if (function == 0x82241A68 || function == 0x82241940)
            chunk(line, base, "display", ctx.r3.u32, 4768);
        if (function == 0x82247FE8) chunk(line, base, "attributes", ctx.r3.u32, 160);
        if (function == 0x82257450) {
            chunk(line, base, "texture_prepare", ctx.r3.u32, 192);
            chunk(line, base, "container", ctx.r4.u32, 80);
            chunk(line, base, "container_vtable", word(base, ctx.r4.u32), 160);
            if (ctx.lr == 0x82255C8C) chunk(line, base, "image_info", ctx.r1.u32 + 240, 48);
        }
        if (function == 0x827A42D8 || function == 0x827A4530 || function == 0x8279B6B0) {
            line << ",\"preparing_texture\":" << preparingTexture;
            chunk(line, base, "image", ctx.r3.u32, 64);
            chunk(line, base, "image_vtable", word(base, ctx.r3.u32), 128);
            if (preparingTexture == 0x3158) {
                const uint32_t bytes = word(base, ctx.r3.u32 + 12);
                chunk(line, base, "pixels", word(base, ctx.r3.u32 + 8), bytes);
            }
        }
        line << "}\n";
        traceLog << line.str();
        traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) {
        enabled = false;
        std::fputs("[RenderTrace] Capture disabled after host I/O failure; original AOT calls continue.\n", stderr);
    }
}

void captureVideo(uint64_t call, uint8_t* base, uint32_t container, uint32_t localId,
                  uint32_t request, uint32_t caller) noexcept {
    if (!enabled.load(std::memory_order_relaxed) || (call > 4 && (call & (call - 1)))) return;
    try {
        std::lock_guard lock(traceMutex);
        if (!enabled || writtenBytes >= maxBytes) return;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"function\":" << 0x8279DDC0u
             << ",\"call\":" << call << ",\"ms\":" << (GetTickCount64() - epoch)
             << ",\"lr\":" << caller << ",\"container_local_id\":" << localId;
        chunk(line, base, "context", renderContext, 18000);
        chunk(line, base, "video_container", container, 64);
        chunk(line, base, "video_request", request, 28);
        const uint32_t image = word(base, word(base, request));
        chunk(line, base, "video_destination", image, 48);
        // Bounded prefixes are raw evidence, not complete decoded frames.
        chunk(line, base, "video_destination_prefix", word(base, image + 8),
              (std::min)(word(base, image + 12), 128u * 1024));
        const uint32_t array = word(base, container + 56);
        if (array && localId / 3 < word(base, array + 4)) {
            const uint32_t table = word(base, array + 24);
            if (table && uint64_t(table) + (localId / 3) * 4ull + 4 <= 0x100000000ull) {
                const uint32_t video = word(base, table + (localId / 3) * 4);
                chunk(line, base, "video", video, 136);
                const uint32_t decoder = word(base, video + 96);
                chunk(line, base, "video_decoder", decoder, 232);
                const uint32_t codec = word(base, decoder + 120);
                chunk(line, base, "video_codec", codec, 96);
                chunk(line, base, "video_codec_vtable", word(base, codec), 96);
                const uint32_t frame = word(base, video + 80);
                if (frame && uint64_t(frame) + 108 <= 0x100000000ull) {
                    chunk(line, base, "video_frame", frame, 108);
                    chunk(line, base, "video_y_prefix", word(base, frame + 20),
                          (std::min)(word(base, frame + 24), 128u * 1024));
                    chunk(line, base, "video_uv_prefix", word(base, frame + 68),
                          (std::min)(word(base, frame + 72), 128u * 1024));
                }
            }
        }
        line << "}\n";
        traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) {
        enabled = false;
        std::fputs("[RenderTrace] Video capture disabled after host I/O failure.\n", stderr);
    }
}

void captureImage(uint32_t function, uint8_t* base, uint32_t image, uint32_t mip) noexcept {
    if (!enabled || !image || !preparingTexture || mip) return;
    try {
        std::lock_guard lock(traceMutex);
        static std::unordered_set<uint32_t> captured;
        static uint64_t pixelBytes = 0;
        if (!enabled || writtenBytes >= maxBytes || captured.size() >= 192 ||
            !captured.insert(preparingTexture).second) return;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"function\":" << function
             << ",\"ms\":" << (GetTickCount64() - epoch) << ",\"preparing_texture\":" << preparingTexture
             << ",\"input_mip\":" << mip << ",\"resource_capture\":true";
        chunk(line, base, "image", image, 48);
        const uint32_t size = word(base, image + 12);
        if (size && size <= 1024 * 1024 && pixelBytes + size <= 6 * 1024 * 1024) {
            chunk(line, base, "pixels", word(base, image + 8), size);
            pixelBytes += size;
        }
        line << "}\n";
        traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) {
        enabled = false;
        std::fputs("[RenderTrace] Image capture disabled after host I/O failure.\n", stderr);
    }
}

// Original engine conversion 82762328, called by stored-resource preparation
// at 82252C84. The 448-byte CPU descriptor is not the 68-byte immediate layout.
// Capture both sides of the original conversion without another engine call.
void captureStoredUpload(uint64_t call, const std::array<uint32_t, 8>& args,
                         uint8_t* base, bool after) noexcept {
    if (!enabled.load(std::memory_order_relaxed) || (call > 32 && (call & (call - 1)))) return;
    try {
        std::lock_guard lock(traceMutex);
        static uint64_t storedBytes = 0;
        if (!enabled || writtenBytes >= maxBytes || storedBytes >= 4 * 1024 * 1024) return;
        const uint64_t before = writtenBytes;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"function\":" << 0x82762328u
             << ",\"call\":" << call << ",\"ms\":" << (GetTickCount64() - epoch)
             << ",\"lr\":" << args[7] << ",\"phase\":\"" << (after ? "after" : "before")
             << "\",\"stored_upload\":true,\"resource_address\":" << args[6]
             << ",\"conversion_mask\":" << args[4] << ",\"r8_at_entry\":" << args[5];
        chunk(line, base, "stored_resource", args[6], 108, true);
        chunk(line, base, "stored_descriptor", args[0], 448, true);
        chunk(line, base, "converted_formats", args[2], 16, true);
        chunk(line, base, "conversion_constants", args[3], 160, true);
        uint8_t descriptor[448], formats[16], sizes[27];
        if (copyGuest(base, args[0], descriptor, sizeof(descriptor)) &&
            copyGuest(base, args[2], formats, sizeof(formats)) &&
            copyGuest(base, 0x82A3D31C, sizes, sizeof(sizes))) {
            uint32_t stride = 0;
            bool validFormats = true;
            for (uint8_t format : formats) {
                if (format >= sizeof(sizes)) { validFormats = false; break; }
                stride += sizes[format];
            }
            const uint32_t count = word(base, args[0] + 424);
            line << ",\"vertex_count\":" << count << ",\"converted_stride\":" << stride;
            if (after && validFormats && stride && stride <= 256 && count && count <= 16384 &&
                uint64_t(count) * stride <= 512 * 1024 && storedBytes + writtenBytes - before + uint64_t(count) * stride <= 4 * 1024 * 1024)
                chunk(line, base, "converted_vertices", args[1], count * stride, true);
            if (!after) for (unsigned slot = 0; slot < 16; ++slot) {
                const uint8_t format = descriptor[392 + slot];
                if (!format || format >= sizeof(sizes)) continue;
                const std::string label = "source_stream_" + std::to_string(slot);
                chunk(line, base, label.c_str(), word(base, args[0] + slot * 4),
                      (std::min)(count, 8u) * sizes[format], true);
            }
        }
        storedBytes += writtenBytes - before;
        line << "}\n";
        traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) {
        enabled = false;
        std::fputs("[RenderTrace] Stored geometry capture disabled after host I/O failure.\n", stderr);
    }
}

void captureStoredIndices(uint64_t call, uint8_t* base, uint32_t resource, uint32_t destination,
                          uint32_t capacity, uint32_t produced, uint32_t caller) noexcept {
    if (!enabled || (call > 32 && (call & (call - 1)))) return;
    try {
        std::lock_guard lock(traceMutex);
        static uint64_t bytes = 0;
        if (!enabled || bytes >= 2 * 1024 * 1024) return;
        const auto before = writtenBytes;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"stored_indices\":true,\"call\":" << call
             << ",\"ms\":" << (GetTickCount64() - epoch) << ",\"lr\":" << caller
             << ",\"resource_address\":" << resource << ",\"capacity\":" << capacity
             << ",\"produced\":" << produced;
        chunk(line, base, "stored_resource", resource, 108, true);
        if (produced && produced == capacity && produced <= 65535 * 3 && produced % 3 == 0 &&
            bytes + writtenBytes - before + uint64_t(produced) * 2 <= 2 * 1024 * 1024)
            chunk(line, base, "converted_indices", destination, produced * 2, true);
        bytes += writtenBytes - before;
        line << "}\n"; traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) { enabled = false; }
}

void captureVertexPreparation(bool conversion, uint64_t call, uint8_t* base, uint32_t descriptor,
                              const DarkRecomp::Native::EngineVertexDescriptor& expected,
                              DarkRecomp::Native::TransformComparison comparison, uint32_t count,
                              uint32_t source = 0, uint32_t device = 0,
                              uint32_t attributes = 0, uint32_t matrices = 0, uint32_t enabledCoordinates = 0) noexcept {
    if (!enabled) return;
    try {
        std::lock_guard lock(traceMutex);
        if (!enabled || writtenBytes >= maxBytes) return;
        bool sampled = call <= 4 || !(call & (call - 1));
        static std::unordered_set<std::string> states[2];
        auto& seen = states[unsigned(conversion)];
        const auto bytes = DarkRecomp::Native::encodeEngineVertexDescriptor(expected);
        if (count && seen.size() < 32)
            sampled |= seen.insert(std::string(reinterpret_cast<const char*>(bytes.data()), 24)).second;
        if (!sampled) return;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << (conversion ? ",\"conversion_constants\":true" : ",\"vertex_descriptor\":true")
             << ",\"call\":" << call << ",\"ms\":" << (GetTickCount64() - epoch)
             << ",\"lr\":" << (conversion ? 0x822490B8u : 0x82248E98u)
             << (conversion ? ",\"vector_count\":" : ",\"reserved_vectors\":") << count
             << ",\"enabled_coordinates\":" << enabledCoordinates << ",\"original_comparison\":\""
             << (comparison == DarkRecomp::Native::TransformComparison::equal ? "bit_exact" :
                 comparison == DarkRecomp::Native::TransformComparison::different ? "different" : "unavailable")
             << "\",\"native_descriptor\":[";
        for (size_t i = 0; i < bytes.size(); ++i) { if (i) line << ','; line << unsigned(bytes[i]); }
        line << ']';
        chunk(line, base, "vertex_descriptor_bytes", descriptor, 80, false, true);
        if (conversion && count) {
            chunk(line, base, "conversion_source", source, count * 16, false, true);
            chunk(line, base, "original_conversion_constants", device + 3136, count * 16, false, true);
        } else if (!conversion) {
            chunk(line, base, "descriptor_attributes", attributes, 96, false, true);
            chunk(line, base, "descriptor_matrix_pointers", matrices, 32, false, true);
            chunk(line, base, "mode_flags", 0x82A5CD88, 108, false, true);
            chunk(line, base, "mode_reservations", 0x82A5CDF4, 27, false, true);
        }
        line << "}\n"; traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) { enabled = false; }
}

void captureTextureConstants(uint64_t call, uint8_t* base,
                             const DarkRecomp::Native::EngineTextureObservation& observation) noexcept {
    if (!enabled) return;
    try {
        std::lock_guard lock(traceMutex);
        if (!enabled || writtenBytes >= maxBytes) return;
        // Empty setup calls dominate the boot. Preserve bounded distinct
        // populated states as well as sparse empty-call samples.
        bool sampled = call <= 4 || !(call & (call - 1));
        static std::unordered_set<std::string> textureStates;
        if (observation.constants.vectorCount && textureStates.size() < 32) {
            std::string key(reinterpret_cast<const char*>(observation.input.modes.data()), 8);
            key += ':' + std::to_string(observation.input.componentMask) + ':' + std::to_string(observation.constants.vectorCount);
            for (const auto& stage : observation.constants.stages) key += stage.matrixColumns ? '1' : '0';
            key += observation.input.hasParameters ? '1' : '0';
            sampled |= textureStates.insert(std::move(key)).second;
        }
        if (!sampled) return;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"texture_constants\":true,\"call\":" << call
             << ",\"ms\":" << (GetTickCount64() - epoch) << ",\"lr\":" << 0x822490D4u
             << ",\"vector_count\":" << observation.constants.vectorCount
             << ",\"parameters_consumed\":" << observation.constants.parametersConsumed
             << ",\"preparation_enabled\":" << (observation.input.enabled ? "true" : "false")
             << ",\"has_parameters\":" << (observation.input.hasParameters ? "true" : "false")
             << ",\"component_mask\":" << observation.input.componentMask
             << ",\"original_comparison\":\""
             << (observation.comparison == DarkRecomp::Native::TransformComparison::equal ? "bit_exact" :
                 observation.comparison == DarkRecomp::Native::TransformComparison::different ? "different" : "unavailable")
             << "\",\"modes\":[";
        for (unsigned s = 0; s < 8; ++s) { if (s) line << ','; line << unsigned(observation.input.modes[s]); }
        line << ']';
        chunk(line, base, "texture_descriptor", observation.descriptorAddress, 80, false, true);
        if (observation.input.enabled) {
            chunk(line, base, "texture_attributes", observation.attributesAddress, 80, false, true);
            chunk(line, base, "texture_matrix_pointers", observation.matricesAddress, 32, false, true);
            for (unsigned s = 0; s < 8; ++s) if (observation.input.matrices[s]) {
                const auto label = "texture_matrix_" + std::to_string(s);
                chunk(line, base, label.c_str(), word(base, observation.matricesAddress + s * 4), 64, false, true);
            }
            if (observation.constants.parametersConsumed)
                chunk(line, base, "texture_parameters", word(base, observation.attributesAddress + 76),
                      observation.constants.parametersConsumed * 16, false, true);
        }
        if (observation.constants.vectorCount)
            chunk(line, base, "original_texture_constants", observation.deviceAddress + 2112, observation.constants.vectorCount * 16, false, true);
        line << "}\n"; traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) { enabled = false; }
}

void captureStoredDraw(uint64_t call, uint8_t* base, const StoredRequest& request,
                       const DarkRecomp::Native::StoredDraw& owned) noexcept {
    if (!enabled || (call > 32 && (call & (call - 1)))) return;
    try {
        std::lock_guard lock(traceMutex);
        static uint64_t bytes = 0;
        if (!enabled || bytes >= 2 * 1024 * 1024) return;
        const auto before = writtenBytes;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"stored_draw\":true,\"call\":" << call
             << ",\"ms\":" << (GetTickCount64() - epoch) << ",\"lr\":" << request.caller
             << ",\"stored_vb_id\":" << request.vertices << ",\"stored_ib_id\":" << request.indices
             << ",\"first_index\":" << request.firstIndex << ",\"triangles\":" << request.triangles
             << ",\"owned_geometry_matched\":" << (owned ? "true" : "false");
        chunk(line, base, "context", renderContext, 18000, true);
        const uint32_t program = word(base, renderContext + 16896);
        chunk(line, base, "program", program, 20, true);
        if (program) chunk(line, base, "program_name", word(base, program + 4), 96, true);
        const uint32_t matrix = word(base, renderContext + 8224), index = word(base, renderContext + 8232);
        if (matrix && index < 256 && uint64_t(matrix) + index * 656ull + 80 <= 0x100000000ull)
            chunk(line, base, "modelview", matrix + index * 656 + 16, 64, true);
        // 82248C80 accesses the selected 656-byte payload through +644,
        // including optional texture matrices and the additional matrix pointer.
        if (matrix && index < 256 && uint64_t(matrix) + index * 656ull + 672 <= 0x100000000ull)
            chunk(line, base, "matrix_block", matrix + index * 656 + 16, 656, true);
        if (owned.transforms) {
            const auto& transform = *owned.transforms;
            line << ",\"native_transform_constants\":[" << std::setprecision(std::numeric_limits<float>::max_digits10);
            for (size_t r = 0; r < 8; ++r) {
                if (r) line << ',';
                line << '[';
                for (size_t c = 0; c < 4; ++c) { if (c) line << ','; line << transform.constants.vectors[r][c]; }
                line << ']';
            }
            line << "],\"original_transform_comparison\":\""
                 << (transform.originalComparison == DarkRecomp::Native::TransformComparison::equal ? "bit_exact" :
                     transform.originalComparison == DarkRecomp::Native::TransformComparison::different ? "different" : "unavailable") << '"';
            if (transform.deviceAddress && uint64_t(transform.deviceAddress) + 2048 <= 0x100000000ull)
            chunk(line, base, "original_transform_constants", transform.deviceAddress + 1920, 128, true);
        }
        line << ",\"bindings_captured\":" << (owned.vertexBindings ? "true" : "false");
        if (owned.vertexBindings) {
            const auto& binding = *owned.vertexBindings;
            const auto descriptor = DarkRecomp::Native::encodeEngineVertexDescriptor(binding.descriptor);
            chunk(line,base,"draw_vertex_descriptor",binding.descriptorAddress,80,true,false,descriptor.data());
            chunk(line,base,"draw_vertex_constants",binding.deviceAddress+1920,4096,true,false,binding.constantBytes.data());
        }
        if (owned) {
            line << ",\"vertex_count\":" << owned.vertices->vertexCount << ",\"stride\":" << owned.vertices->stride;
            chunk(line, base, "stored_vertex_resource", owned.vertices->address, 108, true);
            chunk(line, base, "stored_index_resource", owned.indices->address, 108, true);
            const uint32_t vertex = owned.indices->indices[owned.firstIndex];
            for (unsigned slot : {0u, 1u, 9u}) {
                std::array<float, 4> attribute;
                if (!DarkRecomp::Native::decodeStoredAttribute(*owned.vertices, vertex, slot, attribute)) continue;
                line << ",\"cpu_attribute_" << slot << "\":[" << std::setprecision(std::numeric_limits<float>::max_digits10);
                for (size_t i = 0; i < attribute.size(); ++i) { if (i) line << ','; line << attribute[i]; }
                line << ']';
            }
        }
        bytes += writtenBytes - before;
        line << "}\n"; traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) { enabled = false; }
}
}

void DarkRecomp::Native::inspectNextEngineFrame() noexcept {
    if(enabled && requestedInspection.load(std::memory_order_relaxed)<16)++requestedInspection;
}
void DarkRecomp::Native::traceOwnedWorldDraw(const WorldDraw& draw,unsigned inspection,unsigned ordinal,bool effect) noexcept {
    // Two explicit host-frame inspections (512 draws), or four effect probes
    // explicitly enabled by DARK_EFFECT_PROBE. Neither reads guest/GPU memory.
    if(!enabled.load(std::memory_order_relaxed) || !inspection || inspection>2 || ordinal>=(effect?4u:512u))return;
    try {
        std::lock_guard lock(traceMutex);
        struct Dump {
            unsigned inspection=0;
            size_t geometryBytes=0,imageBytes=0,metadataBytes=0,offset=0;
            std::unordered_set<const StoredGeometry*> geometry;
            std::unordered_set<const ColorImage*> images;
            std::ofstream metadata,blobs;
            bool failed=false;
        };
        // Explicit effect probing gets a separate bounded budget, so a busy
        // world frame cannot consume it before the late composite is reached.
        static Dump dumps[2];
        auto& dump=dumps[effect?1:0];
        if(dump.inspection!=inspection) {
            dump=Dump{};dump.inspection=inspection;
            const auto stem=std::string(effect?"owned-effect-":"owned-inspection-")+std::to_string(inspection);
            dump.metadata.open(traceDirectory/(stem+".jsonl"),std::ios::binary|std::ios::trunc);
            dump.blobs.open(traceDirectory/(stem+".bin"),std::ios::binary|std::ios::trunc);
            dump.failed=!dump.metadata || !dump.blobs;
            std::fprintf(stderr,"[OwnedWorldDump] inspection=%u path=%s maxMiB=96 maxDraws=%u\n",
                inspection,(traceDirectory/(stem+".jsonl")).string().c_str(),effect?4u:512u);
        }
        if(dump.failed)return;
        // Separate budgets preserve geometry evidence even when texture data
        // exceeds its allowance. Omitted resources are explicitly labelled.
        constexpr size_t geometryLimit=24*1024*1024,imageLimit=64*1024*1024,metadataLimit=8*1024*1024;
        if(dump.metadataBytes+65536>metadataLimit)return;
        std::ostringstream out;
        auto hex=[&](const void* source,size_t count) {
            constexpr char digits[]="0123456789abcdef";
            const auto* bytes=static_cast<const uint8_t*>(source);
            out<<'"';for(size_t i=0;i<count;++i)out<<digits[bytes[i]>>4]<<digits[bytes[i]&15];out<<'"';
        };
        auto numbers=[&](const auto& values) {
            out<<'[';bool first=true;for(auto value:values){if(!first)out<<',';first=false;out<<+value;}out<<']';
        };
        auto blob=[&](const void* source,size_t count) {
            out<<"{\"offset\":"<<dump.offset<<",\"bytes\":"<<count<<'}';
            if(count)dump.blobs.write(static_cast<const char*>(source),static_cast<std::streamsize>(count));
            dump.offset+=count;
        };
        auto geometry=[&](const std::shared_ptr<const StoredGeometry>& source) {
            if(!source){out<<"null";return;}
            out<<"{\"key\":"<<reinterpret_cast<uintptr_t>(source.get());
            if(dump.geometry.insert(source.get()).second) {
                out<<",\"address\":"<<source->address<<",\"id\":"<<source->id
                   <<",\"vertexCount\":"<<source->vertexCount<<",\"stride\":"<<source->stride
                   <<",\"conversionMask\":"<<source->conversionMask<<",\"maximumIndex\":"<<source->maximumIndex
                   <<",\"formats\":";numbers(source->formats);
                out<<",\"conversionConstantsBE\":";hex(source->conversionConstants.data(),source->conversionConstants.size());
                out<<",\"resourceBE\":";hex(source->resource.data(),source->resource.size());
                if(source->bytes()<=geometryLimit-dump.geometryBytes) {
                    out<<",\"verticesBE\":";blob(source->vertices.data(),source->vertices.size());
                    out<<",\"indicesLE16\":";blob(source->indices.data(),source->indices.size()*sizeof(uint16_t));
                    dump.geometryBytes+=source->bytes();
                } else out<<",\"omitted\":\"geometry-budget\"";
            }
            out<<'}';
        };
        out<<"{\"version\":1,\"scope\":"<<std::quoted(effect?"effect-series":"host-frame")<<",\"inspection\":"<<inspection<<",\"draw\":"<<ordinal
           <<",\"material\":"<<unsigned(draw.material)<<",\"program\":"<<std::quoted(draw.fragmentName)
           <<",\"fragmentFlags\":"<<draw.fragmentFlags<<",\"attributesBE\":";
        hex(draw.attributes.data(),draw.attributes.size());
        out<<",\"viewport\":";numbers(draw.viewport);out<<",\"targets\":";numbers(draw.targets);
        out<<",\"depthRangeLEFloat\":";hex(draw.depthRange.data(),sizeof(draw.depthRange));
        out<<",\"constantsLEFloat\":";hex(draw.constants.vectors.data(),sizeof(draw.constants.vectors));
        out<<",\"referencesLE32\":";hex(draw.constants.references.data(),sizeof(draw.constants.references));
        out<<",\"fragmentConstantsLEFloat\":";hex(draw.fragmentConstants.data(),sizeof(draw.fragmentConstants));
        const auto& o=draw.options;
        out<<",\"options\":{\"weights\":"<<o.weights<<",\"positionConversion\":"<<unsigned(o.positionConversion)
           <<",\"normal\":"<<unsigned(o.normal)<<",\"tangents\":"<<unsigned(o.tangents)
           <<",\"normalizeNormal\":"<<unsigned(o.normalizeNormal)<<",\"vertexColor\":"<<unsigned(o.vertexColor)
           <<",\"modes\":";numbers(o.modes);out<<",\"coordinates\":";numbers(o.coordinates);
        out<<",\"conversions\":";numbers(o.conversions);out<<",\"matrices\":";numbers(o.matrices);out<<'}';
        out<<",\"firstIndex\":"<<draw.geometry.firstIndex<<",\"indexCount\":"<<draw.geometry.indexCount;
        out<<",\"vertices\":";geometry(draw.geometry.vertices);out<<",\"indices\":";geometry(draw.geometry.indices);
        out<<",\"textureMask\":"<<draw.textureMask<<",\"textureIds\":";numbers(draw.textureIds);
        out<<",\"textures\":[";
        for(unsigned slot=0;slot<16;++slot) {
            if(slot)out<<',';
            const auto& t=draw.textureObjects[slot];const auto& s=draw.samplers[slot];
            out<<"{\"object\":"<<t.object<<",\"storage\":"<<t.storage<<",\"width\":"<<t.width
               <<",\"height\":"<<t.height<<",\"format\":"<<t.format<<",\"exponent\":"<<t.exponent
               <<",\"faces\":"<<t.faces<<",\"mipLevels\":"<<t.mipLevels<<",\"firstMip\":"<<t.firstMip
               <<",\"sampler\":{\"valid\":"<<unsigned(s.valid)<<",\"minLinear\":"<<unsigned(s.minLinear)
               <<",\"magLinear\":"<<unsigned(s.magLinear)<<",\"mipLinear\":"<<unsigned(s.mipLinear)
               <<",\"baseOnly\":"<<unsigned(s.baseOnly)<<",\"lodValid\":"<<unsigned(s.lodValid)
               <<",\"address\":";numbers(s.address);
            out<<",\"anisotropy\":"<<unsigned(s.anisotropy)<<",\"minLevel\":"<<unsigned(s.minLevel)
               <<",\"maxLevel\":"<<unsigned(s.maxLevel)<<",\"border\":"<<unsigned(s.border)
               <<",\"biasLEFloat\":";hex(&s.bias,sizeof(s.bias));out<<"},\"image\":";
            const auto& image=draw.textures[slot];
            if(!image)out<<"null";
            else {
                out<<"{\"key\":"<<reinterpret_cast<uintptr_t>(image.get());
                if(dump.images.insert(image.get()).second) {
                    out<<",\"width\":"<<image->width<<",\"height\":"<<image->height<<",\"faces\":"<<image->faces
                       <<",\"sourceCodec\":"<<image->sourceCodec<<",\"promptOrigin\":"<<unsigned(image->promptOrigin)
                       <<",\"authoredMips\":"<<unsigned(image->authoredMips)<<",\"firstMip\":"<<image->firstMip;
                    if(image->bytes()<=imageLimit-dump.imageBytes) {
                        out<<",\"levelsRGBA8\":[";blob(image->pixels.data(),image->pixels.size());
                        for(const auto& mip:image->mips){out<<',';blob(mip.data(),mip.size());}out<<']';
                        dump.imageBytes+=image->bytes();
                    } else out<<",\"omitted\":\"image-budget\"";
                }
                out<<'}';
            }
            out<<'}';
        }
        out<<"]}\n";
        const auto line=out.str();
        if(line.size()>metadataLimit-dump.metadataBytes){dump.failed=true;return;}
        dump.metadata<<line;dump.metadataBytes+=line.size();
        dump.metadata.flush();dump.blobs.flush();
        if(!dump.metadata || !dump.blobs)dump.failed=true;
    } catch (...) {
        // Diagnostic failures must never affect draw acceptance or rendering.
    }
}
uint64_t sampleFrameQueue(uint32_t function,uint32_t caller) noexcept {
    if(!enabled.load(std::memory_order_relaxed))return 0;
    try {
        std::lock_guard lock(traceMutex);
        struct Sample {uint32_t function,caller,thread;uint64_t next;};
        static std::vector<Sample> samples;static uint64_t sequence=0;
        if(sequence>=8192)return 0;
        const auto thread=GetCurrentThreadId();const auto now=GetTickCount64();
        auto found=std::find_if(samples.begin(),samples.end(),[&](const auto& s){return s.function==function && s.caller==caller && s.thread==thread;});
        if(found==samples.end()) {
            if(samples.size()>=64)return 0;
            samples.push_back({function,caller,thread,now+1000});
        } else {if(now<found->next)return 0;found->next=now+1000;}
        return ++sequence;
    } catch(...) {return 0;}
}
void captureFrameQueue(uint64_t sequence,uint32_t function,bool returned,uint32_t manager,uint32_t frame,
                       uint32_t caller,PPCContext& ctx,uint8_t* base) noexcept {
    if(!sequence)return;
    const auto mode=_mm_getcsr();
    try {
        std::lock_guard lock(traceMutex);
        std::ostringstream out;
        out<<"{\"sequence\":"<<sequence<<",\"function\":"<<function<<",\"returned\":"<<(returned?"true":"false")
           <<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"thread\":"<<GetCurrentThreadId()<<",\"caller\":"<<caller
           <<",\"manager\":"<<manager<<",\"frame\":"<<frame<<",\"result\":"<<ctx.r3.u32<<",\"manager_words\":[";
        for(unsigned n=0;n<18;++n){if(n)out<<',';out<<(manager?word(base,manager+n*4):0);}
        out<<"],\"groups\":[";
        const auto groups=frame?word(base,frame+1336):0;
        for(unsigned n=0;n<(std::min)(groups,32u);++n) {
            if(n)out<<',';out<<'[';
            for(unsigned w=0;w<10;++w){if(w)out<<',';out<<word(base,frame+56+n*40+w*4);}
            out<<']';
        }
        out<<"],\"frame_group_count\":"<<groups<<",\"frame_serial\":"<<(frame?word(base,frame+5560):0)
           <<",\"frame_capacity\":"<<(frame?word(base,frame+12):0)<<",\"frame_used\":"<<(frame?word(base,frame+1388):0)
           <<",\"frame_context\":"<<(frame?word(base,frame+5544):0)<<",\"view_count\":"<<(frame?word(base,frame+6588):0);
        if(caller==0x820E2834) {
            const auto owner=ctx.r31.u32,scene=word(base,owner+2788),vtable=word(base,scene);
            out<<",\"producer_owner\":"<<owner<<",\"producer_context\":"<<ctx.r26.u32<<",\"producer_flags\":[";
            for(unsigned n=0;n<9;++n){if(n)out<<',';out<<word(base,owner+2720+n*4);}
            out<<"],\"scene\":"<<scene<<",\"scene_vtable\":"<<vtable<<",\"scene_render\":"<<word(base,vtable+132)
               <<",\"scene_flags\":"<<word(base,scene+160);
        }
        out<<",\"stack_lr\":[";
        auto stack=ctx.r1.u32;
        for(unsigned n=0;n<16;++n) {
            const auto parent=word(base,stack);if(parent<=stack || uint64_t(parent)-stack>1024*1024)break;
            if(n)out<<',';out<<word(base,parent-8);stack=parent;
        }
        out<<"]}\n";
        std::ofstream file(traceDirectory/"frame-queues.jsonl",std::ios::binary|std::ios::app);file<<out.str();
    } catch(...) {}
    _mm_setcsr(mode);
}

void captureSceneGate(uint64_t sequence,uint32_t function,bool returned,uint32_t scene,uint32_t frame,
                      PPCContext& ctx,uint8_t* base) noexcept {
    if(!sequence)return;
    const auto mode=_mm_getcsr();
    try {
        std::lock_guard lock(traceMutex);
        std::ostringstream out;
        out<<"{\"sequence\":"<<sequence<<",\"function\":"<<function<<",\"returned\":"<<(returned?"true":"false")
           <<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"thread\":"<<GetCurrentThreadId()
           <<",\"scene\":"<<scene<<",\"frame\":"<<frame<<",\"result\":"<<ctx.r3.u32<<",\"scene_words\":[";
        for(const auto offset:{0u,60u,68u,160u,252u,3660u,3668u,3672u,4960u}) {
            if(offset)out<<',';out<<word(base,scene+offset);
        }
        const auto clients=word(base,scene+3668),gui=word(base,scene+3672);
        const auto system=word(base,0x82A690F8),service=word(base,system+12);
        out<<"],\"clients\":["<<clients<<','<<word(base,clients+4)<<','<<word(base,clients+24)
           <<"],\"service\":["<<system<<','<<service<<','<<word(base,service+44)<<"],\"gui_words\":[";
        for(const auto offset:{0u,16u,24u,41768u,41772u,43192u,43216u}) {
            if(offset)out<<',';out<<word(base,gui+offset);
        }
        out<<"],\"groups\":[";
        const auto count=word(base,frame+1336);
        for(unsigned n=0;n<(std::min)(count,32u);++n){if(n)out<<',';out<<word(base,frame+60+n*40);}
        out<<"],\"used\":"<<word(base,frame+1388)<<",\"views\":"<<word(base,frame+6588)<<"}\n";
        std::ofstream file(traceDirectory/"scene-gates.jsonl",std::ios::binary|std::ios::app);file<<out.str();
    } catch(...) {}
    _mm_setcsr(mode);
}

void captureClientGate(uint64_t sequence,uint32_t function,bool returned,uint32_t client,uint32_t frame,
                       const std::array<uint32_t,6>& args,PPCContext& ctx,uint8_t* base) noexcept {
    if(!sequence)return;
    const auto mode=_mm_getcsr();
    try {
        std::lock_guard lock(traceMutex);
        const auto engine=word(base,client+600),vtable=word(base,engine);
        std::ostringstream out;
        out<<"{\"sequence\":"<<sequence<<",\"function\":"<<function<<",\"returned\":"<<(returned?"true":"false")
           <<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"client\":"<<client<<",\"frame\":"<<frame
           <<",\"result\":"<<ctx.r3.u32<<",\"args\":[";
        for(unsigned n=0;n<args.size();++n){if(n)out<<',';out<<args[n];}
        out<<"],\"client_words\":[";
        for(const auto offset:{0u,112u,240u,484u,516u,520u,544u,600u,608u,3236u,7020u,8976u}) {
            if(offset)out<<',';out<<word(base,client+offset);
        }
        out<<"],\"engine\":"<<engine<<",\"engine_vtable\":"<<vtable<<",\"engine_render\":"<<word(base,vtable+156)
           <<",\"groups\":[";
        const auto count=frame?word(base,frame+1336):0;
        for(unsigned n=0;n<(std::min)(count,32u);++n){if(n)out<<',';out<<word(base,frame+60+n*40);}
        out<<"]}\n";
        std::ofstream file(traceDirectory/"client-gates.jsonl",std::ios::binary|std::ios::app);file<<out.str();
    }catch(...){}
    _mm_setcsr(mode);
}

bool DarkRecomp::Native::renderTraceEnabled() noexcept {return enabled.load(std::memory_order_relaxed);}
void DarkRecomp::Native::configureRenderTrace(const std::filesystem::path& directory) {
    if (directory.empty()) return;
    // Refuse to overwrite existing evidence.
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Renderer trace directory must be new");
    traceDirectory = directory;
    traceLog.open(directory / "events.jsonl", std::ios::binary);
    if (!traceLog) throw std::runtime_error("Cannot create renderer trace metadata");
    epoch = GetTickCount64();
    enabled = true;
    std::puts("[RenderTrace] Read-only engine captures enabled; original rendering functions remain active.");
}

void DarkRecomp::Native::traceMissingWorldTexture(uint8_t* base,uint32_t id,uint32_t object,uint32_t storage) noexcept {
    if(!enabled || !id || !object || !storage)return;
    try {
        std::lock_guard lock(traceMutex);
        static std::unordered_set<uint32_t> seen;
        if(seen.size()>=64 || !seen.insert(id).second)return;
        // Bounded source-resource evidence at the engine boundary. No command
        // buffer is inspected and no texture is synthesized for a missing draw.
        std::array<uint8_t,64> header{};std::vector<uint8_t> data(65536);
        if(!copyGuest(base,object,header.data(),header.size()))return;
        const auto name="world-texture-"+std::to_string(id);
        std::ofstream h(traceDirectory/(name+".header.bin"),std::ios::binary);
        h.write(reinterpret_cast<const char*>(header.data()),header.size());
        bool readable=copyGuest(base,storage,data.data(),data.size());
        if(readable){std::ofstream p(traceDirectory/(name+".pixels.bin"),std::ios::binary);p.write(reinterpret_cast<const char*>(data.data()),data.size());}
        std::ofstream meta(traceDirectory/"world-missing-textures.jsonl",std::ios::binary|std::ios::app);
        meta<<"{\"id\":"<<id<<",\"object\":"<<object<<",\"storage\":"<<storage<<",\"header_file\":\""<<name
            <<".header.bin\",\"pixel_prefix_bytes\":"<<(readable?data.size():0)<<"}\n";
    }catch(...){}
}

void DarkRecomp::Native::traceEngineVertexProgram(uint8_t* base, const EngineVertexProgramObservation& observation) noexcept {
    if (!enabled) return;
    try {
        std::lock_guard lock(traceMutex);
        static std::unordered_set<std::string> keys;
        const std::string key(reinterpret_cast<const char*>(observation.key.data()), sizeof(observation.key));
        if (!enabled || keys.size() >= 32 || !keys.insert(key).second) return;
        ++sequence;
        std::ostringstream line;
        line << "{\"sequence\":" << sequence << ",\"vertex_program\":true,\"ms\":" << (GetTickCount64() - epoch)
             << ",\"record_address\":" << observation.selection.recordAddress << ",\"binding_address\":" << observation.selection.bindingAddress
             << ",\"original_comparison\":\"" << (observation.comparison == TransformComparison::equal ? "bit_exact" :
                 observation.comparison == TransformComparison::different ? "different" : "unavailable") << "\",\"native_key\":[";
        for (unsigned i = 0; i < 6; ++i) { if (i) line << ','; line << observation.key[i]; }
        line << ']';
        EngineVertexBindingSnapshot binding;
        const bool bindings = snapshotEngineVertexBindings(base,binding);
        line << ",\"bindings_captured\":" << (bindings ? "true" : "false");
        if (bindings) {
            const auto descriptor = encodeEngineVertexDescriptor(binding.descriptor);
            chunk(line,base,"final_vertex_descriptor",binding.descriptorAddress,80,false,true,descriptor.data());
            chunk(line,base,"final_vertex_constants",binding.deviceAddress+1920,4096,false,true,binding.constantBytes.data());
        }
        chunk(line, base, "original_vertex_key", observation.keyAddress, 24, false, true);
        chunk(line, base, "program_source_context", renderContext + 16420, 840, false, true);
        chunk(line, base, "program_model_state", observation.source.matrixAddress + 640, 8, false, true);
        if (observation.source.input.palette)
            chunk(line, base, "program_declaration", observation.source.input.declarationAddress, 25, false, true);
        for (unsigned i = 0; i < observation.selection.visitedCount; ++i) {
            char label[48]; std::snprintf(label, sizeof(label), "program_cache_node_%u", i);
            chunk(line, base, label, observation.selection.visited[i].address, 48, false, true);
        }
        line << "}\n"; traceLog << line.str(); traceLog.flush();
        if (!traceLog) enabled = false;
    } catch (...) { enabled = false; }
}

#define TRACE_ORIGINAL(address) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        static std::atomic<uint64_t> calls{0}; \
        if (enabled.load(std::memory_order_relaxed)) \
            capture(0x##address, ++calls, ctx, base); \
        __imp__sub_##address(ctx, base); \
    }

extern "C" PPC_FUNC(__imp__sub_8225DBC8);
PPC_FUNC(sub_8225DBC8) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::immediate);
    static std::atomic<uint64_t> calls{0};
    if (enabled) capture(0x8225DBC8, ++calls, ctx, base);
    DarkRecomp::Native::previewObserveTriangles(base, ctx.r3.u32, ctx.r4.u32);
    __imp__sub_8225DBC8(ctx, base);
}
TRACE_ORIGINAL(8225DD40)
extern "C" PPC_FUNC(__imp__sub_828AC000);
PPC_FUNC(sub_828AC000) {
    // Original SDK Sleep wrapper; keep the wait and full original ABI intact.
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::sleep,ctx.r13.u32==0x7FF00000);
    __imp__sub_828AC000(ctx,base);
}
TRACE_ORIGINAL(8225DE78)
extern "C" PPC_FUNC(__imp__sub_8225F320);
PPC_FUNC(sub_8225F320) {
    // Only this caller submits the decoded output as triangle-list indices.
    // The original decoder still handles packet traversal, fans and strips;
    // no packet parsing or guest register changes are done by the preview.
    const bool observe = uint32_t(ctx.lr) == 0x8225E1A0 && DarkRecomp::Native::enginePreviewEnabled();
    const uint32_t indices = ctx.r4.u32, countAddress = ctx.r5.u32;
    const uint32_t capacity = observe ? word(base, countAddress) : 0;
    __imp__sub_8225F320(ctx, base);
    if (observe)
        DarkRecomp::Native::previewObserveDecodedTriangles(base, indices, capacity, word(base, countAddress));
}
extern "C" PPC_FUNC(__imp__sub_8225E218);
PPC_FUNC(sub_8225E218) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::stored);
    static std::atomic<uint64_t> calls{0};
    if (enabled) capture(0x8225E218, ++calls, ctx, base);
    // Whole-resource draw: the original uses r3 for both VB and IB and
    // submits at 8225E2E0 only after its existing readiness/binding checks.
    // This is the held-pistol path (resource 20, 4344 indices in the elevator
    // capture); it does not pass through the subset entry 8225E2F0.
    const auto request = StoredRequest::whole(ctx.r3.u32);
    ScopedPointer<const StoredRequest> scope(drawingGeometry,&request);
    __imp__sub_8225E218(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_8225E2F0);
PPC_FUNC(sub_8225E2F0) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::stored);
    static std::atomic<uint64_t> calls{0};
    if (enabled) capture(0x8225E2F0, ++calls, ctx, base);
    const StoredRequest request{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
    ScopedPointer<const StoredRequest> scope(drawingGeometry, &request);
    __imp__sub_8225E2F0(ctx, base);
}
TRACE_ORIGINAL(8225E3C8)
TRACE_ORIGINAL(8225CDD8)
extern "C" PPC_FUNC(__imp__sub_82252878);
PPC_FUNC(sub_82252878) {
    DarkRecomp::Native::StoredUpload upload;
    if (DarkRecomp::Native::enginePreviewEnabled()) {
        try { upload = DarkRecomp::Native::storedGeometryCache().begin(base, ctx.r3.u32); }
        catch (...) { upload.failed = true; }
    }
    ScopedPointer<DarkRecomp::Native::StoredUpload> scope(preparingGeometry, &upload);
    __imp__sub_82252878(ctx, base);
    if (upload.geometry) DarkRecomp::Native::storedGeometryCache().finish(std::move(upload), base);
}
extern "C" PPC_FUNC(__imp__sub_82762328);
PPC_FUNC(sub_82762328) {
    const bool observe = enabled.load(std::memory_order_relaxed) && uint32_t(ctx.lr) == 0x82252C84;
    const std::array<uint32_t, 8> args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                       ctx.r7.u32, ctx.r8.u32, ctx.r22.u32, uint32_t(ctx.lr)};
    static std::atomic<uint64_t> calls{0};
    const uint64_t call = observe ? ++calls : 0;
    if (observe) captureStoredUpload(call, args, base, false);
    __imp__sub_82762328(ctx, base);
    if (observe) captureStoredUpload(call, args, base, true);
    if (args[7] == 0x82252C84 && preparingGeometry && preparingGeometry->geometry &&
        preparingGeometry->geometry->address == args[6])
        DarkRecomp::Native::storedGeometryCache().vertices(*preparingGeometry, base,
            args[0], args[1], args[2], args[3], args[4]);
}
extern "C" PPC_FUNC(__imp__sub_82899BF0);
PPC_FUNC(sub_82899BF0) {
    const bool stored = uint32_t(ctx.lr) == 0x82252DD4;
    const uint32_t destination = ctx.r3.u32, bytes = ctx.r5.u32, resource = ctx.r22.u32;
    __imp__sub_82899BF0(ctx, base);
    if (!stored) return;
    if (preparingGeometry && preparingGeometry->geometry && preparingGeometry->geometry->address == resource)
        DarkRecomp::Native::storedGeometryCache().indices(*preparingGeometry, base, destination,
            bytes % 6 ? 0 : bytes / 2, bytes / 2);
    static std::atomic<uint64_t> calls{0};
    if (enabled) captureStoredIndices(++calls, base, resource, destination, bytes % 6 ? 0 : bytes / 2, bytes / 2, 0x82252DD4);
}
extern "C" PPC_FUNC(__imp__sub_82765410);
PPC_FUNC(sub_82765410) {
    const bool stored = uint32_t(ctx.lr) == 0x8225342C;
    const uint32_t destination = ctx.r4.u32, countAddress = ctx.r5.u32, resource = ctx.r22.u32;
    const uint32_t capacity = stored ? word(base, countAddress) : 0;
    __imp__sub_82765410(ctx, base);
    if (!stored) return;
    const uint32_t produced = word(base, countAddress);
    if (preparingGeometry && preparingGeometry->geometry && preparingGeometry->geometry->address == resource)
        DarkRecomp::Native::storedGeometryCache().indices(*preparingGeometry, base, destination, capacity, produced);
    static std::atomic<uint64_t> calls{0};
    if (enabled) captureStoredIndices(++calls, base, resource, destination, capacity, produced, 0x8225342C);
}
extern "C" PPC_FUNC(__imp__sub_8224A2E8);
extern "C" PPC_FUNC(__imp__sub_8224DAE0);
PPC_FUNC(sub_8224DAE0) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::descriptor);
    DarkRecomp::Native::EngineDescriptorObservation observation;
    const bool observe = uint32_t(ctx.lr) == 0x82248E98 &&
        enabled &&
        DarkRecomp::Native::beginEngineDescriptorObservation(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, observation);
    __imp__sub_8224DAE0(ctx, base);
    if (observe) {
        DarkRecomp::Native::finishEngineDescriptorObservation(base, observation);
        static std::atomic<uint64_t> calls{0};
        if (enabled) captureVertexPreparation(false, ++calls, base, observation.descriptorAddress, observation.expected,
            observation.comparison, observation.expected.textureReservation, 0, 0,
            observation.attributesAddress, observation.matricesAddress, observation.input.enabledCoordinates);
    }
}
extern "C" PPC_FUNC(__imp__sub_8224A0A8);
PPC_FUNC(sub_8224A0A8) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::conversion);
    DarkRecomp::Native::EngineConversionObservation observation;
    const bool observe = uint32_t(ctx.lr) == 0x822490B8 &&
        enabled &&
        DarkRecomp::Native::beginEngineConversionObservation(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, observation);
    __imp__sub_8224A0A8(ctx, base);
    if (observe) {
        DarkRecomp::Native::finishEngineConversionObservation(base, ctx.r3.u32, observation);
        static std::atomic<uint64_t> calls{0};
        if (enabled) captureVertexPreparation(true, ++calls, base, observation.descriptorAddress, observation.constants.descriptor,
            observation.comparison, observation.constants.vectorCount, observation.sourceAddress, observation.deviceAddress);
    }
}
PPC_FUNC(sub_8224A2E8) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::textures);
    // Direct engine preparation boundary. The original still performs all
    // descriptor/device writes exactly once; host observation is read-only.
    DarkRecomp::Native::EngineTextureObservation observation;
    const bool observe = uint32_t(ctx.lr) == 0x822490D4 &&
        enabled &&
        DarkRecomp::Native::beginEngineTextureObservation(base, ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, observation);
    __imp__sub_8224A2E8(ctx, base);
    if (observe) {
        DarkRecomp::Native::finishEngineTextureObservation(base, ctx.r3.u32, observation);
        static std::atomic<uint64_t> calls{0};
        if (enabled) captureTextureConstants(++calls, base, observation);
    }
}
extern "C" PPC_FUNC(__imp__sub_82868FE8);
PPC_FUNC(sub_82868FE8) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::indexed);
    captureWorldBoundary(0x82868FE8,ctx,base);
    if (uint32_t(ctx.lr)==0x8225DD38 && ctx.r4.u32==4 && !ctx.r5.u32) {
        const auto mode=_mm_getcsr();
        DarkRecomp::Native::previewObserveImmediateWorld(base,ctx.r3.u32,ctx.r30.u32,ctx.r7.u32);
        _mm_setcsr(mode);
    }
    // Observe the engine's actual indexed call, after its early-out checks
    // and buffer binding. This does not interpret the Xbox command stream.
    if (drawingGeometry && drawingGeometry->matches(uint32_t(ctx.lr),ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32)) {
        DarkRecomp::Native::StoredDraw owned;
        const uint32_t device = ctx.r3.u32;
        if (DarkRecomp::Native::enginePreviewEnabled() && !word(base, renderContext + 16512) &&
            word(base, renderContext + 16532) == drawingGeometry->vertices && device &&
            word(base, renderContext + 15748) == device && uint64_t(device) + 12432 <= 0x100000000ull) {
            // 82868FE8 consumes the buffer bound at device+12428. Match
            // that object as well as the resource ID preserved at entry.
            try { owned = drawingGeometry->resolve(DarkRecomp::Native::storedGeometryCache(),base,uint32_t(ctx.lr),
                ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,word(base,renderContext+16532),word(base,device+12428)); } catch (...) {}
            if (owned) {
                DarkRecomp::Native::EngineTransformSnapshot transform;
                if (enabled && DarkRecomp::Native::snapshotEngineTransforms(base, transform)) owned.transforms = transform;
                DarkRecomp::Native::captureEngineVertexBindings(base,owned.vertexBindings);
            }
        }
        static std::atomic<uint64_t> calls{0};
        if (enabled) {
            auto request=*drawingGeometry;request.triangles=ctx.r7.u32/3;
            captureStoredDraw(++calls, base, request, owned);
        }
        if (owned) {
            const auto floatingPointMode=_mm_getcsr();
            DarkRecomp::Native::previewObserveWorld(base,owned);
            _mm_setcsr(floatingPointMode);
        }
    }
    // Task-59 bounded alternate-call evidence (profile-only). An indexed call
    // carrying a smoke tex0 (slot 0 id 1477/1371) that matches NEITHER the
    // immediate hook above NOR the stored hook above takes an unobserved
    // path. Log raw registers verbatim (meanings are shape-dependent; nothing
    // is interpreted): first occurrence per (caller,primitive,flag)
    // signature, 32 records total. Integer guest reads only, guest ABI and
    // host FP state preserved, original executes exactly once below.
    if(DarkRecomp::Native::profileEngineCpu) {
        const auto mode=_mm_getcsr();
        const uint32_t caller=uint32_t(ctx.lr);
        const bool immediateShape=(caller==0x8225DD38 && ctx.r4.u32==4 && !ctx.r5.u32);
        const bool storedShape=drawingGeometry && drawingGeometry->matches(caller,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32);
        if(!immediateShape && !storedShape) {
            const uint32_t tex0=word(base,renderContext+16904)>>16;
            if(tex0==1477 || tex0==1371) {
                struct Signature {uint32_t caller=0,r4=0,r5=0,tex0=0;};
                static std::array<Signature,32> seen{};
                static unsigned used=0;
                // Dedicated mutex: the 32-record global bound must hold across
                // threads without multiplying per thread. Publication and
                // classification happen under one guard; the path is rare.
                static std::mutex alternateCallMutex;
                const Signature key{caller,ctx.r4.u32,ctx.r5.u32,tex0};
                std::lock_guard<std::mutex> alternateLock(alternateCallMutex);
                bool fresh=true;
                for(unsigned i=0;i<used;++i)
                    if(seen[i].caller==key.caller && seen[i].r4==key.r4 && seen[i].r5==key.r5 &&
                        seen[i].tex0==key.tex0){fresh=false;break;}
                if(fresh && used<seen.size()) {
                    seen[used]=key;
                    ++used;
                    // Candidate descriptor/bound words, verbatim (0 when
                    // unreadable); register meanings stay shape-dependent.
                    // The 4-byte bound read must fit the 32-bit space.
                    const uint32_t state0=word(base,renderContext+16512);
                    const uint64_t boundAddress=uint64_t(ctx.r3.u32)+12452;
                    const uint32_t boundW=boundAddress<=0xFFFFFFFCull?word(base,uint32_t(boundAddress)):0;
                    std::fprintf(stderr,"[AlternateIndexedCall] caller=%08X r3=%08X r4=%u r5=%u r6=%08X r7=%u r30=%08X tex0=%u state0=%08X boundW=%08X\n",
                        caller,ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r30.u32,tex0,state0,boundW);
                }
            }
        }
        _mm_setcsr(mode);
    }
    __imp__sub_82868FE8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_828630D8);
PPC_FUNC(sub_828630D8) {
    captureWorldBoundary(0x828630D8,ctx,base);
    // The original constructs omitted rectangles, intersects viewport/scissor,
    // and rejects empty regions. Capture at its completed clipping boundary.
    const bool observe=DarkRecomp::Native::enginePreviewEnabled() &&
        (uint32_t(ctx.lr)==0x822419E8 || uint32_t(ctx.lr)==0x82239A00);
    ScopedPointer<uint8_t> scope(clearingBase,observe?base:nullptr);
    __imp__sub_828630D8(ctx,base);
}
void ObserveEngineClearRegionMidAsmHook(PPCRegister& device,PPCRegister& flags,PPCRegister& color,
    PPCRegister& depth,PPCRegister& stencil,PPCRegister& left,PPCRegister& top,PPCRegister& right,PPCRegister& bottom) {
    if(!clearingBase)return;
    const auto mode=_mm_getcsr();
    DarkRecomp::Native::previewObserveClear(clearingBase,device.u32,flags.u32,color.u32,float(depth.f64),stencil.u32,
        {left.s32,top.s32,right.s32,bottom.s32});
    _mm_setcsr(mode);
}
extern "C" PPC_FUNC(__imp__sub_82865FD0);
PPC_FUNC(sub_82865FD0) {
    captureWorldBoundary(0x82865FD0,ctx,base);
    const auto mxcsr=_mm_getcsr();
    DarkRecomp::Native::previewObserveResolve(base,ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,
        ctx.r10.u32,float(ctx.f1.f64),word(base,ctx.r1.u32+92),ctx.r9.u32,0);
    _mm_setcsr(mxcsr);
    __imp__sub_82865FD0(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_82867620);
PPC_FUNC(sub_82867620) {
    captureWorldBoundary(0x82867620,ctx,base);
    const auto mxcsr=_mm_getcsr();
    if(DarkRecomp::Native::previewWorldActive()) {
        DarkRecomp::Native::previewObservePresent(base,ctx.r4.u32);
        DarkRecomp::Native::previewEndFrame();
    }
    _mm_setcsr(mxcsr);
    __imp__sub_82867620(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_82241A68);
PPC_FUNC(sub_82241A68) {
    static std::atomic<uint64_t> calls{0};
    if (enabled) capture(0x82241A68, ++calls, ctx, base);
    if(!DarkRecomp::Native::previewWorldActive()) DarkRecomp::Native::previewEndFrame();
    __imp__sub_82241A68(ctx, base);
}
TRACE_ORIGINAL(82241940)
TRACE_ORIGINAL(82247FE8)
// Original primary wrapper resolver used by both initial and incremental uploads.
static uint32_t uploadResource(uint8_t* base,uint32_t owner) {
    std::array<uint8_t,88> bytes{};
    if(!DarkRecomp::Native::copyRenderMemory(base,owner,bytes.data(),bytes.size()))return 0;
    auto be=[&](unsigned i){return uint32_t(bytes[i])<<24|uint32_t(bytes[i+1])<<16|uint32_t(bytes[i+2])<<8|bytes[i+3];};
    return be(84)?be(84):be(8)?owner+12:0;
}
struct ScopedImageUpload {
    uint32_t priorId=preparingTexture,priorFaces=preparingTextureFaces;
    DarkRecomp::Native::TextureUpload* prior=preparingImage;
    ScopedImageUpload(DarkRecomp::Native::TextureUpload& upload,unsigned faces) {
        preparingTexture=upload.id();preparingTextureFaces=faces;preparingImage=&upload;
    }
    ~ScopedImageUpload() {preparingTexture=priorId;preparingTextureFaces=priorFaces;preparingImage=prior;}
};
static void publishImageUpload(DarkRecomp::Native::TextureUpload& upload,uint8_t* base,uint32_t owner) {
    try {
        const auto resource=uploadResource(base,owner);
        if(auto image=upload.finish(resource))DarkRecomp::Native::previewPublishTexture(upload.id(),resource,std::move(image));
        else {static std::atomic<unsigned> failures{};if(failures++<32)
            std::fprintf(stderr,"[EngineImageUpload] incomplete id=%u resource=%08X reason=%s\n",upload.id(),resource,upload.error()?upload.error():"missing completion");}
    } catch(...) {}
}
// Direct refresh can overwrite pixels or recreate an embedded descriptor
// without entering the initial/incremental upload observers. Invalidate by ID
// before the original callback; queued commands retain their owned images.
extern "C" PPC_FUNC(__imp__sub_82256008);
PPC_FUNC(sub_82256008) {
    if(DarkRecomp::Native::enginePreviewEnabled()) {
        std::array<uint8_t,2> id{};
        if(DarkRecomp::Native::copyRenderMemory(base,uint64_t(ctx.r3.u32)+168,id.data(),id.size()))
            DarkRecomp::Native::previewPrepareTexture(uint32_t(id[0])<<8|id[1]);
    }
    __imp__sub_82256008(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_82257450);
PPC_FUNC(sub_82257450) {
    if (!enabled && !DarkRecomp::Native::enginePreviewEnabled()) { __imp__sub_82257450(ctx, base); return; }
    static std::atomic<uint64_t> calls{0};
    const uint32_t owner=ctx.r3.u32,metadata=word(base,owner+168);
    const unsigned faces=((metadata>>8)&255u)==1?6:1;
    DarkRecomp::Native::TextureUpload upload(metadata>>16,ctx.r10.u32,word(base,ctx.r1.u32+92),faces);
    ScopedImageUpload scope(upload,faces);
    DarkRecomp::Native::previewPrepareTexture(preparingTexture);
    capture(0x82257450, ++calls, ctx, base);
    __imp__sub_82257450(ctx, base);
    if(DarkRecomp::Native::enginePreviewEnabled())publishImageUpload(upload,base,owner);
}
extern "C" PPC_FUNC(__imp__sub_82258BA0);
PPC_FUNC(sub_82258BA0) {
    if(!DarkRecomp::Native::enginePreviewEnabled()) {__imp__sub_82258BA0(ctx,base);return;}
    const uint32_t owner=ctx.r3.u32,metadata=word(base,owner+168),resource=uploadResource(base,owner);
    const unsigned faces=((metadata>>8)&255u)==1?6:1,count=ctx.r10.u32;
    if(!count) {__imp__sub_82258BA0(ctx,base);return;}
    const unsigned levels=((word(base,resource+44)>>6)&15)+1;
    auto seed=DarkRecomp::Native::previewCapturedTexture(metadata>>16,resource);
    // The original caller supplies the previously skipped prefix length.
    // A CPU-cache eviction must not discard the untouched resident tail.
    if(!seed && count && count<levels)try {
        auto tail=std::make_shared<DarkRecomp::ColorImage>();
        if(!DarkRecomp::Native::decodeWorldTextureImage(base,resource,*tail,count))seed=std::move(tail);
    } catch(...) {}
    DarkRecomp::Native::TextureUpload upload(metadata>>16,levels,0,faces,std::move(seed),count);
    ScopedImageUpload scope(upload,faces);
    __imp__sub_82258BA0(ctx,base);
    publishImageUpload(upload,base,owner);
}
extern "C" PPC_FUNC(__imp__sub_828AE188);
PPC_FUNC(sub_828AE188) {
    // Record owned CImage bytes before the original temporary endian swaps.
    // Offset queries also occur for skipped metadata, before the copy branch.
    const uint32_t caller=uint32_t(ctx.lr);
    if(preparingImage && DarkRecomp::Native::enginePreviewEnabled() && (caller==0x82257C18 || caller==0x82258F18)) {
        const auto mode=_mm_getcsr();
        const bool initial=caller==0x82257C18;
        preparingImage->record(base,ctx.r3.u32,ctx.r29.u32,initial?word(base,ctx.r1.u32+96):ctx.r14.u32,
            ctx.r5.u32,ctx.r4.u32,initial && word(base,ctx.r1.u32+132)!=0);
        _mm_setcsr(mode);
    }
    __imp__sub_828AE188(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_8226A7E0);
PPC_FUNC(sub_8226A7E0) {
    const uint32_t caller=uint32_t(ctx.lr);
    const bool initial=caller==0x822581F0,incremental=caller==0x822594E0;
    auto* upload=preparingImage;
    const unsigned mip=upload && (initial || incremental)?word(base,ctx.r1.u32+(initial?120:100)):0;
    const unsigned face=upload && (initial || incremental)?word(base,ctx.r1.u32+(initial?104:112)):0;
    __imp__sub_8226A7E0(ctx,base);
    if(upload && (initial || incremental))upload->complete(mip,face);
}
struct PromptFetchScope {
    PromptFetchScope() { DarkRecomp::Native::previewBeginPromptCapture(0); }
    ~PromptFetchScope() { DarkRecomp::Native::previewEndPromptCapture(); }
};
// Final selector for VirtualXTC2: r3=container, r4=ACTUAL selected localId
// (after alternative-format selection), r5=mip; returns actual CImage.
// Bounded name is copied while the container is valid; fail closed. Unknown
// commits too so stale origins cannot survive and uploads demote.
extern "C" PPC_FUNC(__imp__sub_827A4130);
PPC_FUNC(sub_827A4130) {
    const uint32_t container = ctx.r3.u32, actualIndex = ctx.r4.u32;
    __imp__sub_827A4130(ctx, base);
    const uint32_t image = ctx.r3.u32;
    notePromptResult(base, container, actualIndex, image, "XTC2");
}
// Older resolver: r3=container, r4=requested index; r3 returns selected index.
// Scoped capture for the 8279B6B0 path below; never used blindly elsewhere.
extern "C" PPC_FUNC(__imp__sub_8279A7A8);
PPC_FUNC(sub_8279A7A8) {
    const uint32_t container = ctx.r3.u32;
    __imp__sub_8279A7A8(ctx, base);
    olderSelectedContainer = container;
    olderSelectedIndex = ctx.r3.u32;
    olderSelectedValid = true;
}
#define TRACE_IMAGE_RETURN(address) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        const uint32_t mip = ctx.r5.u32; \
        PromptFetchScope promptScope; \
        __imp__sub_##address(ctx, base); \
        static std::atomic<uint64_t> calls{0}; \
        if (enabled.load(std::memory_order_relaxed)) capture(0x##address, ++calls, ctx, base); \
        captureImage(0x##address, base, ctx.r3.u32, mip); \
        if (!preparingImage && preparingTextureFaces==1) DarkRecomp::Native::previewObserveImage(base, ctx.r3.u32, preparingTexture, mip); \
    }
TRACE_IMAGE_RETURN(827A42D8)
TRACE_IMAGE_RETURN(827A4530)
extern "C" PPC_FUNC(__imp__sub_8279B6B0);
PPC_FUNC(sub_8279B6B0) {
    const uint32_t container = ctx.r3.u32;
    const uint32_t mip = ctx.r5.u32;
    PromptFetchScope promptScope;
    olderSelectedValid = false;
    __imp__sub_8279B6B0(ctx, base);
    const uint32_t image = ctx.r3.u32;
    static std::atomic<uint64_t> calls{0};
    if (enabled) capture(0x8279B6B0, ++calls, ctx, base);
    captureImage(0x8279B6B0, base, image, mip);
    // Use the resolver's final selected index only when it belongs to this
    // container; requested r4 is NOT trusted after format selection. Unknown
    // commits invalidate stale origins and demote uploads via stickiness.
    if (olderSelectedValid && olderSelectedContainer == container) {
        const uint8_t origin = DarkRecomp::Prompts::resolveXtcOrigin(base, container, olderSelectedIndex);
        DarkRecomp::Native::previewCommitPromptOrigin(image, origin);
        if (preparingImage) preparingImage->notePromptOrigin(origin);
        DarkRecomp::Native::countPromptOriginAttempt(origin != 0);
        if (origin) {
            static std::atomic<unsigned> olderLogs{0};
            if (olderLogs++ < 8)
                std::fprintf(stderr, "[PromptOrigin] XTC container=%08X index=%u image=%08X origin=%u\n",
                    container, olderSelectedIndex, image, unsigned(origin));
        } else {
            static std::atomic<unsigned> olderRejects{0};
            if (olderRejects++ < 6)
                std::fprintf(stderr, "[PromptOriginReject] XTC/XTC container=%08X index=%u image=%08X\n",
                    container, olderSelectedIndex, image);
        }
    } else if (image) {
        // No valid resolver capture: explicitly invalidate so a reused image
        // address cannot inherit a previous fetch's origin.
        DarkRecomp::Native::previewCommitPromptOrigin(image, 0);
        if (preparingImage) preparingImage->notePromptOrigin(0);
        DarkRecomp::Native::countPromptOriginAttempt(false);
    }
    olderSelectedValid = false;
    if (!preparingImage && preparingTextureFaces==1) DarkRecomp::Native::previewObserveImage(base, image, preparingTexture, mip);
}

extern "C" PPC_FUNC(__imp__sub_8279DDC0);
PPC_FUNC(sub_8279DDC0) {
    const uint32_t container = ctx.r3.u32, localId = ctx.r4.u32, request = ctx.r5.u32, caller = uint32_t(ctx.lr);
    __imp__sub_8279DDC0(ctx, base);
    DarkRecomp::Native::previewObserveVideo(base, container, localId);
    static std::atomic<uint64_t> calls{0};
    if (enabled) captureVideo(++calls, base, container, localId, request, caller);
}

PPC_FUNC(sub_82793C48) {
    const uint32_t output=ctx.r3.u32,device=ctx.r4.u32,x=ctx.r5.u32,y=ctx.r6.u32,caller=uint32_t(ctx.lr);
    __imp__sub_82793C48(ctx,base);
    if(!enabled.load(std::memory_order_relaxed))return;
    const auto mode=_mm_getcsr();
    static std::atomic<unsigned> reports{};
    if(reports.fetch_add(1,std::memory_order_relaxed)<64) {
        const auto target=word(base,device+8),vtable=word(base,target);
        std::fprintf(stderr,"[InputGuestAxis] caller=%08X device=%08X input=%u,%u converted=%d,%d target=%08X press=%08X release=%08X tag=%u\n",
                     caller,device,x,y,int32_t(word(base,output)),int32_t(word(base,output+4)),
                     target,word(base,vtable+48),word(base,vtable+52),word(base,device+20));
    }
    _mm_setcsr(mode);
}
extern "C" PPC_FUNC(__imp__sub_828AAAB8);
PPC_FUNC(sub_828AAAB8) {
    const uint32_t user=ctx.r3.u32,output=ctx.r4.u32,caller=uint32_t(ctx.lr);
    __imp__sub_828AAAB8(ctx,base);
    if(!enabled.load(std::memory_order_relaxed) || ctx.r3.u32)return;
    const auto mode=_mm_getcsr();
    const auto right=word(base,output+12);
    static thread_local uint32_t previous=0;
    if(right!=previous) {
        previous=right;
        static std::atomic<unsigned> reports{};
        if(reports.fetch_add(1,std::memory_order_relaxed)<64)
            std::fprintf(stderr,"[InputGuestState] caller=%08X user=%u packet=%u right=%d,%d\n",
                         caller,user,word(base,output),int16_t(right>>16),int16_t(right));
    }
    _mm_setcsr(mode);
}

// Observe original free/ready-list transfers, without changing their event,
// lock, register, or list behavior. Paired records expose a blocked original.
#define TRACE_FRAME_QUEUE(address,takesFrame) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        const auto mode=_mm_getcsr(); \
        const uint32_t manager=ctx.r3.u32,frame=takesFrame?0:ctx.r4.u32,caller=uint32_t(ctx.lr); \
        const auto sequence=sampleFrameQueue(0x##address,caller); \
        captureFrameQueue(sequence,0x##address,false,manager,frame,caller,ctx,base); \
        _mm_setcsr(mode); \
        __imp__sub_##address(ctx,base); \
        captureFrameQueue(sequence,0x##address,true,manager,takesFrame?ctx.r3.u32:frame,caller,ctx,base); \
    }
TRACE_FRAME_QUEUE(825A3FD8,true)
TRACE_FRAME_QUEUE(825A43B0,true)
TRACE_FRAME_QUEUE(825A45A8,false)
TRACE_FRAME_QUEUE(825A46A0,false)
#define TRACE_SCENE_GATE(address,frameReg) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        const auto mode=_mm_getcsr();const uint32_t scene=ctx.r3.u32,frame=ctx.frameReg.u32; \
        const auto sequence=sampleFrameQueue(0x##address,uint32_t(ctx.lr)); \
        captureSceneGate(sequence,0x##address,false,scene,frame,ctx,base);_mm_setcsr(mode); \
        __imp__sub_##address(ctx,base); \
        captureSceneGate(sequence,0x##address,true,scene,frame,ctx,base); \
    }
TRACE_SCENE_GATE(820C5548,r4)
TRACE_SCENE_GATE(820FADE8,r4)
TRACE_SCENE_GATE(820FACE0,r4)
TRACE_SCENE_GATE(820F9818,r4)
TRACE_SCENE_GATE(820C5618,r4)
TRACE_SCENE_GATE(820C4470,r5)
TRACE_SCENE_GATE(820FA490,r4)
#define TRACE_CLIENT_GATE(address,hasFrame) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        const auto mode=_mm_getcsr();const uint32_t client=ctx.r3.u32,frame=hasFrame?ctx.r4.u32:0; \
        const std::array<uint32_t,6> args{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32}; \
        const auto sequence=sampleFrameQueue(0x##address,uint32_t(ctx.lr)); \
        captureClientGate(sequence,0x##address,false,client,frame,args,ctx,base);_mm_setcsr(mode); \
        __imp__sub_##address(ctx,base); \
        captureClientGate(sequence,0x##address,true,client,frame,args,ctx,base); \
    }
TRACE_CLIENT_GATE(8249B238,true)
TRACE_CLIENT_GATE(8249A208,true)
TRACE_CLIENT_GATE(823F79C0,false)
// Original cinematic synchronization messages, bounded independently of frame
// sampling so a single readiness notification cannot disappear between frames.
void captureCinematicSync(uint64_t sequence,uint32_t function,bool returned,uint32_t object,
                          const std::array<uint32_t,12>& message,uint32_t caller,
                          PPCContext& ctx,uint8_t* base) noexcept {
    if(!sequence)return;
    const auto mode=_mm_getcsr();
    try {
        std::lock_guard lock(traceMutex);
        std::ostringstream out;
        out<<"{\"sequence\":"<<sequence<<",\"function\":"<<function<<",\"returned\":"<<(returned?"true":"false")
           <<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"thread\":"<<GetCurrentThreadId()<<",\"object\":"<<object
           <<",\"caller\":"<<caller<<",\"result\":"<<ctx.r3.u32<<",\"message\":[";
        for(unsigned n=0;n<message.size();++n){if(n)out<<',';out<<message[n];}
        out<<"],\"words\":[";
        for(const auto offset:{0u,360u,364u,368u,592u,636u,640u,644u,648u,652u,656u,664u}) {
            if(offset)out<<',';out<<word(base,object+offset);
        }
        const auto events=word(base,object+664);
        out<<"],\"event_count\":"<<word(base,events+4)<<",\"stack_lr\":[";
        auto stack=ctx.r1.u32;
        for(unsigned n=0;n<16;++n) {
            const auto parent=word(base,stack);if(parent<=stack || uint64_t(parent)-stack>1024*1024)break;
            if(n)out<<',';out<<word(base,parent-8);stack=parent;
        }
        out<<"]}\n";
        std::ofstream file(traceDirectory/"cinematic-sync.jsonl",std::ios::binary|std::ios::app);file<<out.str();
    }catch(...){}
    _mm_setcsr(mode);
}
#define TRACE_CINEMATIC_SYNC(address,isMessage) \
    extern "C" PPC_FUNC(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        const auto mode=_mm_getcsr();const uint32_t object=ctx.r3.u32,caller=uint32_t(ctx.lr); \
        static std::atomic<uint64_t> count{0}; \
        uint64_t sequence=enabled.load(std::memory_order_relaxed)?count.fetch_add(1,std::memory_order_relaxed)+1:0; \
        if(sequence>4096)sequence=0; \
        std::array<uint32_t,12> message{}; \
        if(sequence && isMessage)for(unsigned n=0;n<message.size();++n)message[n]=word(base,ctx.r4.u32+n*4); \
        captureCinematicSync(sequence,0x##address,false,object,message,caller,ctx,base);_mm_setcsr(mode); \
        __imp__sub_##address(ctx,base); \
        captureCinematicSync(sequence,0x##address,true,object,message,caller,ctx,base); \
    }
TRACE_CINEMATIC_SYNC(826EC0B8,true)
TRACE_CINEMATIC_SYNC(826EBEE8,false)
TRACE_CINEMATIC_SYNC(826EBCD0,false)
extern "C" PPC_FUNC(__imp__sub_827DA190);
PPC_FUNC(sub_827DA190) {
    auto* stats = DarkRecomp::Native::activeAudioRefill;
    DarkRecomp::Native::AudioRefillTimer timer(stats ? &stats->streamTicks : nullptr,
                                              stats ? &stats->maxStreamTicks : nullptr);
    const auto stream = ctx.r3.u32;
    const uint64_t before = stats ? PPC_LOAD_U64(stream + 152) : 0;
    if (!enabled.load(std::memory_order_relaxed)) {
        __imp__sub_827DA190(ctx,base);
        if (stats) {
            ++stats->streams;
            const uint64_t after = PPC_LOAD_U64(stream + 152);
            if (after >= before) stats->guestFrames += after - before;
            stats->unchangedStreams += after == before;
        }
        return;
    }
    const auto mode=_mm_getcsr();const auto object=ctx.r3.u32,node=word(base,object+36),id=word(base,node+64);
    const auto sequence=(enabled.load(std::memory_order_relaxed) && id>=35 && id<=39)
        ?sampleFrameQueue(0x827DA190,id):0;
    if(sequence)try {
        std::lock_guard lock(traceMutex);std::ostringstream out;
        out<<"{\"sequence\":"<<sequence<<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"object\":"<<object<<",\"id\":"<<id
           <<",\"node\":"<<node<<",\"node_flags\":"<<word(base,node+72)<<",\"words\":[";
        for(unsigned n=0;n<80;++n){if(n)out<<',';out<<word(base,object+n*4);}
        const auto group=word(base,object+120),count=word(base,group),records=word(base,group+8);
        out<<"],\"group\":["<<group<<','<<count<<','<<word(base,group+4)<<','<<records<<"],\"records\":[";
        for(unsigned n=0;n<(std::min)(count,8u);++n) {
            if(n)out<<',';const auto record=records+n*96,context=word(base,record+64);
            out<<"{\"staged\":[";
            for(unsigned w=0;w<24;++w){if(w)out<<',';out<<word(base,record+w*4);}
            out<<"],\"live\":[";
            for(unsigned w=0;w<16;++w){if(w)out<<',';out<<word(base,context+w*4);}
            out<<"]}";
        }
        out<<"]}\n";
        std::ofstream file(traceDirectory/"cinematic-stream.jsonl",std::ios::binary|std::ios::app);file<<out.str();
    }catch(...){}
    _mm_setcsr(mode);__imp__sub_827DA190(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_82496BA0);
PPC_FUNC(sub_82496BA0) {
    const auto mode=_mm_getcsr();const uint32_t client=ctx.r3.u32;
    const auto sequence=sampleFrameQueue(0x82496BA0,uint32_t(ctx.lr));
    if(sequence)try {
        std::lock_guard lock(traceMutex);
        const auto list=word(base,client+848),count=word(base,list+4),data=word(base,list+24);
        if(count) {
            const auto sound=word(base,client+784),vtable=word(base,sound);
            std::ostringstream out;
            out<<"{\"sequence\":"<<sequence<<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"client\":"<<client
               <<",\"sound\":"<<sound<<",\"sound_vtable\":"<<vtable<<",\"sound_ready_function\":"<<word(base,vtable+132)
               <<",\"count\":"<<count<<",\"entries\":[";
            for(unsigned n=0;n<(std::min)(count,16u);++n) {
                if(n)out<<',';const auto entry=data+n*40;
                out<<"{\"id\":"<<word(base,entry)<<",\"ready\":"<<(word(base,entry+36)>>24)<<",\"arrays\":[";
                for(unsigned a=0;a<4;++a) {
                    if(a)out<<',';const auto array=word(base,entry+8+a*8),size=word(base,array+4),values=word(base,array+24);
                    out<<"{\"count\":"<<size<<",\"values\":[";
                    for(unsigned j=0;j<(std::min)(size,16u);++j){if(j)out<<',';out<<word(base,values+j*4);}
                    out<<"]}";
                }
                out<<"],\"animation_methods\":[";
                const auto array=word(base,entry+16),size=word(base,array+4),values=word(base,array+24);
                for(unsigned j=0;j<(std::min)(size,16u);++j) {
                    if(j)out<<',';const auto object=word(base,values+j*4),table=word(base,object);
                    out<<'['<<object<<','<<table<<','<<word(base,table+124)<<']';
                }
                out<<"]}";
            }
            out<<"]}\n";
            std::ofstream file(traceDirectory/"cinematic-pending.jsonl",std::ios::binary|std::ios::app);file<<out.str();
        }
    }catch(...){}
    _mm_setcsr(mode);__imp__sub_82496BA0(ctx,base);
}
extern "C" PPC_FUNC(__imp__sub_823075F8);
extern "C" PPC_FUNC(__imp__sub_827F15E0);
PPC_FUNC(sub_827F15E0) {
    const auto mode=_mm_getcsr();const auto sound=ctx.r3.u32,id=ctx.r4.u32,caller=uint32_t(ctx.lr);
    static std::atomic<uint32_t> count{0};
    const bool record=enabled.load(std::memory_order_relaxed) && caller==0x82496C5C && count.fetch_add(1)<1024;
    uint32_t node=0;std::array<uint32_t,24> words{};
    if(record) {
        node=word(base,sound+2564)&~1u;
        for(unsigned depth=0;node && depth<64;++depth) {
            const auto key=word(base,node+64);if(key==id)break;
            node=word(base,node+(key<id?4:0))&~1u;
        }
        if(node)for(unsigned n=0;n<words.size();++n)words[n]=word(base,node+n*4);
    }
    _mm_setcsr(mode);__imp__sub_827F15E0(ctx,base);
    if(!record)return;const auto originalMode=_mm_getcsr();
    try {
        std::lock_guard lock(traceMutex);
        std::ofstream file(traceDirectory/"cinematic-sound-ready.jsonl",std::ios::binary|std::ios::app);
        file<<"{\"ms\":"<<(GetTickCount64()-epoch)<<",\"sound\":"<<sound<<",\"id\":"<<id
            <<",\"result\":"<<ctx.r3.u32<<",\"node\":"<<node<<",\"words\":[";
        for(unsigned n=0;n<words.size();++n){if(n)file<<',';file<<words[n];}
        file<<"]}\n";
    }catch(...){}
    _mm_setcsr(originalMode);
}
PPC_FUNC(sub_823075F8) {
    const auto mode=_mm_getcsr();
    const uint32_t state=ctx.r3.u32,tick=ctx.r4.u32,caller=uint32_t(ctx.lr);
    const double fraction=ctx.f1.f64;
    const auto sequence=sampleFrameQueue(0x823075F8,caller);
    std::array<uint32_t,4> values{};
    if(sequence)for(unsigned n=0;n<4;++n)values[n]=word(base,state+152+n*4);
    _mm_setcsr(mode);
    __imp__sub_823075F8(ctx,base);
    if(!sequence)return;
    const auto originalMode=_mm_getcsr();
    try {
        std::lock_guard lock(traceMutex);
        std::ofstream file(traceDirectory/"player-fade.jsonl",std::ios::binary|std::ios::app);
        file<<"{\"sequence\":"<<sequence<<",\"ms\":"<<(GetTickCount64()-epoch)<<",\"state\":"<<state
            <<",\"caller\":"<<caller<<",\"tick\":"<<tick<<",\"fraction\":"<<fraction
            <<",\"start_tick\":"<<values[0]<<",\"from_color\":"<<values[1]<<",\"to_color\":"<<values[2]
            <<",\"duration\":"<<int16_t(values[3]>>16)<<",\"result\":"<<ctx.r3.u32<<"}\n";
    }catch(...){}
    _mm_setcsr(originalMode);
}
extern "C" PPC_FUNC(__imp__sub_825A3298);
PPC_FUNC(sub_825A3298) {
    const auto mode=_mm_getcsr();const uint32_t frame=ctx.r3.u32,caller=uint32_t(ctx.lr);
    const auto sequence=sampleFrameQueue(0x825A3298,caller);
    captureFrameQueue(sequence,0x825A3298,false,0,frame,caller,ctx,base);_mm_setcsr(mode);
    __imp__sub_825A3298(ctx,base);
    captureFrameQueue(sequence,0x825A3298,true,0,frame,caller,ctx,base);
}
// Original825E91D8 builds eight histogram callbacks. Their native GPU query
// results feed the original exposure calculation through825DD950's float array.
// Preserve the originals' allocator/query lifecycle and all PPC register effects.
extern "C" PPC_FUNC(__imp__sub_825DD940);
extern "C" PPC_FUNC(__imp__sub_825DD948);
extern "C" PPC_FUNC(__imp__sub_825DD950);
PPC_FUNC(sub_825DD940) {
    const auto mode=_mm_getcsr();const auto id=word(base,ctx.r6.u32);
    _mm_setcsr(mode);__imp__sub_825DD940(ctx,base);
    const auto returnedMode=_mm_getcsr();
    DarkRecomp::Native::previewBeginHistogram(id-0x80000000u);
    _mm_setcsr(returnedMode);
}
PPC_FUNC(sub_825DD948) {
    __imp__sub_825DD948(ctx,base);const auto mode=_mm_getcsr();
    DarkRecomp::Native::previewEndHistogram();_mm_setcsr(mode);
}
PPC_FUNC(sub_825DD950) {
    const auto mode=_mm_getcsr();const auto data=ctx.r6.u32;
    const auto first=word(base,data),count=word(base,data+4),output=word(base,data+8);
    _mm_setcsr(mode);__imp__sub_825DD950(ctx,base);
    const auto returnedMode=_mm_getcsr();
    if(first==0x80000000u && count==8 && output && uint64_t(output)+32<=0x100000000ull) {
        std::array<uint64_t,8> samples{};bool complete=true;
        for(unsigned i=0;i<8;++i)complete=DarkRecomp::Native::previewReadHistogram(i,samples[i]) && complete;
        if(complete) {
            static std::atomic<unsigned> reports{};const bool report=reports.fetch_add(1)<8;
            if(report)std::fprintf(stderr,"[EngineHistogram] originalFirst=%g nativeSamples=",std::bit_cast<float>(word(base,output)));
            for(unsigned i=0;i<8;++i) {
                PPC_STORE_U32(output+i*4,std::bit_cast<uint32_t>(float(samples[i])));
                if(report)std::fprintf(stderr,"%s%llu",i?",":"",static_cast<unsigned long long>(samples[i]));
            }
            if(report)std::fputc('\n',stderr);
        }
    }
    _mm_setcsr(returnedMode);
}
