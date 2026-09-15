#pragma once
#include <filesystem>
#include <cstdint>

namespace DarkRecomp::Native {
// Optional bounded, read-only engine capture. Configure before guest threads.
// Every wrapper still executes the original AOT function exactly once.
void configureRenderTrace(const std::filesystem::path& directory);
bool renderTraceEnabled() noexcept;
// Arm at most 16 bounded, read-only original boundary inspections.
void inspectNextEngineFrame() noexcept;
struct WorldDraw;
// Explicit host-frame inspections or opt-in effect probes retain bounded
// owned draw data for offline analysis, without accessing guest/GPU memory.
void traceOwnedWorldDraw(const WorldDraw&, unsigned inspection, unsigned ordinal, bool effect = false) noexcept;
struct EngineVertexProgramObservation;
void traceEngineVertexProgram(uint8_t* base, const EngineVertexProgramObservation& observation) noexcept;
void traceMissingWorldTexture(uint8_t* base,uint32_t id,uint32_t object,uint32_t storage) noexcept;
}
