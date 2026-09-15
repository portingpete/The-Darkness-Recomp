#include "engine_vertex_shader.h"
#include "engine_vertex_template.generated.h"
#include "renderer/engine/engine_vertex_program.h"
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace DarkRecomp {
namespace {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(hr));
}
bool finite(const Native::EngineVector& vector) {
    for (float value : vector) if (!std::isfinite(value)) return false;
    return true;
}
bool range(uint32_t first, uint32_t count) { return first < 256 && count <= 256 - first; }
}
bool importEngineVertexShaderConstants(const Native::EngineVertexBindingSnapshot& binding,
    const EngineVertexShaderOptions& options, EngineVertexShaderConstants& output) noexcept {
    if (options.weights > 8) return false;
    const auto descriptor = Native::encodeEngineVertexDescriptor(binding.descriptor);
    auto word = [](const uint8_t* b) { return uint32_t(b[0])<<24 | uint32_t(b[1])<<16 | uint32_t(b[2])<<8 | b[3]; };
    for (unsigned i=0;i<5;++i) if (word(descriptor.data()+i*4) != binding.key[i+1]) return false;
    EngineVertexShaderConstants result;
    result.palette = binding.descriptor.palette; result.position = binding.descriptor.positionConversion;
    result.color = binding.descriptor.color; result.texture = binding.descriptor.conversions[0];
    result.matrix = binding.descriptor.matrices[0]; result.paletteVectors = options.weights ? 156 : 0;
    auto take = [&](uint32_t first, uint32_t count) {
        if (!range(first,count)) return false;
        for (unsigned v=first;v<first+count;++v) for (unsigned lane=0;lane<4;++lane) {
            const uint32_t bits = word(binding.constantBytes.data()+v*16+lane*4);
            if ((bits & 0x7F800000u) == 0x7F800000u) return false;
            result.vectors[v][lane] = std::bit_cast<float>(bits);
        }
        return true;
    };
    if (!take(0,4) || !take(7,1) || !take(result.color,1) ||
        (options.weights && (!take(8,1) || !take(result.palette,result.paletteVectors))) ||
        (options.positionConversion && !take(result.position,2)) ||
        (options.textureConversion && !take(result.texture,2)) ||
        (options.textureMatrix && !take(result.matrix,4))) return false;
    output = result; return true;
}

EngineVertexShaderD3D11::EngineVertexShaderD3D11(ID3D11Device* device, EngineVertexShaderOptions selected) : options(selected) {
    if (!device || options.weights > 8) throw std::invalid_argument("Unsupported original vertex-template conditions");
    const auto weights = std::to_string(options.weights);
    const D3D_SHADER_MACRO macros[] = {
        {"MWCOMP", weights.c_str()}, {"POSITION_TRANS", options.positionConversion ? "1" : "0"},
        {"TEXTURE_TRANS", options.textureConversion ? "1" : "0"},
        {"TEXTURE_MATRIX", options.textureMatrix ? "1" : "0"},
        {"VERTEX_COLOR", options.vertexColor ? "1" : "0"}, {nullptr, nullptr}};
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT compiled = D3DCompile(engineVertexTemplateSource, std::strlen(engineVertexTemplateSource),
        "original_VP_template", macros, nullptr, "vertexMain", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
    if (FAILED(compiled)) throw std::runtime_error(errors ?
        std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "Vertex template compilation failed");
    check(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "Vertex shader creation");
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,position)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,uv)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,color)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,indices)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,weights)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDINDICES",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,indices2)),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offsetof(EngineShaderVertex,weights2)),D3D11_INPUT_PER_VERTEX_DATA,0}};
    check(device->CreateInputLayout(elements, UINT(std::size(elements)), code->GetBufferPointer(), code->GetBufferSize(), &layout), "Vertex input layout");
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = 256 * 16; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&desc, nullptr, &constantsBuffer), "Vertex constant buffer");
    desc.ByteWidth = 32;
    check(device->CreateBuffer(&desc, nullptr, &referencesBuffer), "Vertex reference buffer");
}

bool EngineVertexShaderD3D11::bind(ID3D11DeviceContext* context, const EngineVertexShaderConstants& constants,
                                 std::span<const EngineShaderVertex> vertices) {
    if (!context || !range(constants.color, 1) ||
        (options.positionConversion && !range(constants.position, 2)) ||
        (options.textureConversion && !range(constants.texture, 2)) ||
        (options.textureMatrix && !range(constants.matrix, 4))) return false;
    if (options.weights && (constants.paletteVectors < 3 || constants.paletteVectors > 156 ||
        constants.paletteVectors % 3 || !range(constants.palette, constants.paletteVectors))) return false;
    for (const auto& vector : constants.vectors) if (!finite(vector)) return false;
    for (const auto& vertex : vertices) {
        if (!finite(vertex.position) || !finite(vertex.uv) || (options.vertexColor && !finite(vertex.color))) return false;
        for (uint32_t w = 0; w < options.weights; ++w) {
            const float index = w < 4 ? vertex.indices[w] : vertex.indices2[w-4];
            const float weight = w < 4 ? vertex.weights[w] : vertex.weights2[w-4];
            const float address = std::floor(index * constants.vectors[8][3]);
            if (!std::isfinite(index) || !std::isfinite(weight) || !std::isfinite(address) ||
                address < 0 || address > float(constants.paletteVectors - 3)) return false;
        }
    }
    const std::array<uint32_t,8> references{constants.palette, constants.position, constants.color,
                                         constants.texture, constants.matrix, 0, 0, 0};
    context->UpdateSubresource(constantsBuffer.Get(), 0, nullptr, constants.vectors.data(), 0, 0);
    context->UpdateSubresource(referencesBuffer.Get(), 0, nullptr, references.data(), 0, 0);
    ID3D11Buffer* buffers[]{constantsBuffer.Get(), referencesBuffer.Get()};
    context->VSSetConstantBuffers(0, 2, buffers);
    context->IASetInputLayout(layout.Get());
    context->VSSetShader(shader.Get(), nullptr, 0);
    return true;
}
}
