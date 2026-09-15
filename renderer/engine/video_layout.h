#pragma once
#include "simple_mesh.h"
#include <cmath>

namespace DarkRecomp {
// Only the front-end's full-screen video quad gets aspect fitting. Embedded
// video rectangles and world-space television materials keep their geometry.
inline bool isFullscreenVideo(const SimpleMesh& mesh) {
    if (!mesh.video || mesh.vertices.size() != 4 || mesh.indices.size() != 6) return false;
    unsigned corners = 0;
    for (const auto& vertex : mesh.vertices) {
        const auto& p = mesh.projection;
        const auto* v = vertex.position;
        const float w = v[0]*p[3] + v[1]*p[7] + v[2]*p[11] + p[15];
        if (!std::isfinite(w) || w <= 0) return false;
        const float x = (v[0]*p[0] + v[1]*p[4] + v[2]*p[8] + p[12]) / w;
        const float y = (v[0]*p[1] + v[1]*p[5] + v[2]*p[9] + p[13]) / w;
        if (!std::isfinite(x) || !std::isfinite(y) ||
            std::abs(std::abs(x)-1) > .02f || std::abs(std::abs(y)-1) > .02f) return false;
        corners |= 1u << ((x > 0 ? 1 : 0) | (y > 0 ? 2 : 0));
    }
    return corners == 15;
}
}
