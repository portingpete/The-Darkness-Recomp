#pragma once
#include "renderer/engine/world_mesh.h"
#include <emmintrin.h>
#include <span>

namespace DarkRecomp {
// Uploaded vertex buffers only need a count and skinning-index extrema on the
// CPU. Keep the immutable packed source for diagnostics; the expanded fetch
// values can be released as soon as CreateBuffer/Unmap copies them to the GPU.
struct WorldVertexMetadata {
    uint32_t count=0;
    std::array<std::array<float,2>,8> indexBounds{};
};

inline WorldVertexMetadata worldVertexMetadata(std::span<const Native::WorldVertex> vertices,
                                              const std::array<uint8_t,16>& formats) noexcept {
    WorldVertexMetadata result;
    result.count=uint32_t(vertices.size());
    if(vertices.empty())return result;
    const auto& first=vertices.front();
    for(unsigned lane=0;lane<4;++lane) {
        result.indexBounds[lane].fill(first.indices[lane]);
        result.indexBounds[lane+4].fill(first.indices2[lane]);
    }
    // Absent streams decode to the same (0,0,0,1) for every vertex. Scan
    // present streams together, rather than revisiting a 240-byte vertex
    // eight times. Unskinned meshes need no scan beyond the first vertex.
    auto firstMin=_mm_loadu_ps(first.indices.data()),firstMax=firstMin;
    auto secondMin=_mm_loadu_ps(first.indices2.data()),secondMax=secondMin;
    // Compare and select raw bits, as std::min/std::max do for these finite
    // values. This keeps prior signed-zero ties and subnormal operand bits
    // even when the calling thread enables DAZ/FTZ. SSE2 is guaranteed by
    // the Windows x64 target.
    auto include=[](const Native::EngineVector& values,__m128& minimum,__m128& maximum) {
        const auto value=_mm_loadu_ps(values.data());
        const auto lower=_mm_cmplt_ps(value,minimum),higher=_mm_cmpgt_ps(value,maximum);
        minimum=_mm_or_ps(_mm_and_ps(lower,value),_mm_andnot_ps(lower,minimum));
        maximum=_mm_or_ps(_mm_and_ps(higher,value),_mm_andnot_ps(higher,maximum));
    };
    if(formats[12] && formats[14])for(const auto& vertex:vertices) {
        include(vertex.indices,firstMin,firstMax);include(vertex.indices2,secondMin,secondMax);
    }
    else if(formats[12])for(const auto& vertex:vertices)include(vertex.indices,firstMin,firstMax);
    else if(formats[14])for(const auto& vertex:vertices)include(vertex.indices2,secondMin,secondMax);
    Native::EngineVector firstLow,firstHigh,secondLow,secondHigh;
    _mm_storeu_ps(firstLow.data(),firstMin);_mm_storeu_ps(firstHigh.data(),firstMax);
    _mm_storeu_ps(secondLow.data(),secondMin);_mm_storeu_ps(secondHigh.data(),secondMax);
    for(unsigned lane=0;lane<4;++lane) {
        result.indexBounds[lane]={firstLow[lane],firstHigh[lane]};
        result.indexBounds[lane+4]={secondLow[lane],secondHigh[lane]};
    }
    return result;
}

// Inspection-only reconstruction reads just the selected immutable vertices.
// Use the actual fetch decoder so packed/half formats and recovered finite
// holes have exactly the same values as the uploaded vertex buffer.
inline bool sampleWorldVertices(const Native::StoredGeometry& source,std::span<const uint16_t> indices,
                                std::vector<Native::WorldVertex>& output) noexcept {
    try {
        if(indices.empty() || indices.size()>65535 || !source.vertexCount || source.vertexCount>65535 ||
           source.vertices.size()!=uint64_t(source.vertexCount)*source.stride)return false;
        Native::StoredGeometry sample;
        sample.vertexCount=uint32_t(indices.size());sample.stride=source.stride;sample.formats=source.formats;
        sample.vertices.reserve(indices.size()*source.stride);
        for(auto index:indices) {
            if(index>=source.vertexCount)return false;
            const auto first=source.vertices.begin()+size_t(index)*source.stride;
            sample.vertices.insert(sample.vertices.end(),first,first+source.stride);
        }
        return Native::decodeWorldVertices(sample,output);
    } catch(...) {return false;}
}
}
