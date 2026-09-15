#pragma once
#include "renderer/engine/engine_transforms.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <span>

namespace DarkRecomp {
namespace Native { struct EngineVertexBindingSnapshot; }
// Explicit original-template conditions. These are not retail program-key bits.
// The live draw bridge must prove that mapping before selecting this shader.
struct EngineVertexShaderOptions {
    uint32_t weights = 0; // MWComp0..8; MPQuat, CubeVec and normals are excluded.
    bool positionConversion = false, textureConversion = false;
    bool textureMatrix = false, vertexColor = false;
};
struct EngineShaderVertex {
    Native::EngineVector position{}, uv{}, color{}, indices{}, weights{}, indices2{}, weights2{};
};
struct EngineVertexShaderConstants {
    std::array<Native::EngineVector, 256> vectors{};
    uint32_t palette = 96, position = 76, color = 10, texture = 78, matrix = 12;
    uint32_t paletteVectors = 0; // Owned, available palette extent (at most 156).
};
// Copy the registers consumed by explicitly chosen conditions from a final
// original binding. Unused registers are zeroed, so unrelated nonfloat bits
// cannot contaminate the native upload. This does NOT select a shader variant.
bool importEngineVertexShaderConstants(const Native::EngineVertexBindingSnapshot& binding,
    const EngineVertexShaderOptions& options, EngineVertexShaderConstants& output) noexcept;

// Native shader generated from the original template, with original Xenon HLSL
// arithmetic helpers. No guest memory, bytecode, or original calls are used here.
class EngineVertexShaderD3D11 {
public:
    EngineVertexShaderD3D11(ID3D11Device* device, EngineVertexShaderOptions options);
    // Validate all reads before changing D3D state. Full constant storage and
    // consumed inputs must be finite. Bone addresses follow floor(index*c8.w),
    // without implicit weight normalization, remainder weights, or clamping.
    bool bind(ID3D11DeviceContext* context, const EngineVertexShaderConstants& constants,
              std::span<const EngineShaderVertex> vertices);
    ID3DBlob* bytecode() const { return code.Get(); }
private:
    EngineVertexShaderOptions options;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantsBuffer, referencesBuffer;
};
}
