#pragma once
#include <filesystem>
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <array>

namespace DarkRecomp::Native {
// Optional bounded, read-only engine capture. Configure before guest threads.
// Every wrapper still executes the original AOT function exactly once.
void configureRenderTrace(const std::filesystem::path& directory);
// On-demand host-frame evidence only: does not enable guest/startup tracing.
void configureShadowCapture(const std::filesystem::path& directory);
bool worldCaptureEnabled() noexcept;
bool renderTraceEnabled() noexcept;
// Arm at most 16 bounded, read-only original boundary inspections.
void inspectNextEngineFrame() noexcept;
struct WorldDraw;
// Explicit host-frame inspections or opt-in effect probes retain bounded
// owned draw data for offline analysis, without accessing guest/GPU memory.
void traceOwnedWorldDraw(const WorldDraw&, unsigned inspection, unsigned ordinal, bool effect = false) noexcept;
struct WorldClear;
struct WorldResolve;
struct WorldTexture;
void traceWorldFrame(unsigned inspection,bool begin,uint64_t present,unsigned scale,unsigned commands=0,unsigned draws=0) noexcept;
void traceWorldClear(const WorldClear&,unsigned inspection,unsigned command,unsigned attachments) noexcept;
void traceWorldResolve(const WorldResolve&,unsigned inspection,unsigned command,unsigned reason,const std::array<uint32_t,4>* copied=nullptr) noexcept;
void traceWorldPresent(const WorldTexture&,unsigned inspection,unsigned command,unsigned reason) noexcept;
void traceWorldDrawResult(unsigned inspection,unsigned command,unsigned draw,unsigned reason,unsigned flags,unsigned textureMask,unsigned resolvedMask) noexcept;
// Caller supplies compact rows; format is the source DXGI_FORMAT value. Alpha
// readbacks contain only the original 16-bit alpha channel of RGBA16_FLOAT.
void traceWorldGpu(unsigned inspection,unsigned command,unsigned draw,unsigned pass,std::string_view stage,std::string_view role,
    uint64_t key,unsigned slot,unsigned width,unsigned height,unsigned format,unsigned subresource,unsigned rowBytes,
    std::string_view encoding,const void* bytes,size_t size,std::string_view omitted={}) noexcept;
struct EngineVertexProgramObservation;
void traceEngineVertexProgram(uint8_t* base, const EngineVertexProgramObservation& observation) noexcept;
void traceMissingWorldTexture(uint8_t* base,uint32_t id,uint32_t object,uint32_t storage) noexcept;
}
