#pragma once
#include "engine_transforms.h"
#include "engine_vertex_program.h"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DarkRecomp::Native {
// Owned original conversion output. Formats retain engine slot semantics;
// in particular slot 9 is NORMAL, not COLOR. No shader meaning is inferred.
struct StoredGeometry {
    uint32_t address = 0, id = 0, vertexCount = 0, stride = 0, conversionMask = 0;
    uint16_t maximumIndex = 0;
    std::array<uint8_t, 16> formats{};
    std::array<uint8_t, 160> conversionConstants{}; // Original big-endian bytes.
    std::array<uint8_t, 108> resource{};
    std::vector<uint8_t> vertices;
    std::vector<uint16_t> indices;
    size_t bytes() const { return vertices.size() + indices.size() * sizeof(uint16_t); }
};
struct StoredUpload {
    uint64_t generation = 0;
    bool failed = false;
    std::unique_ptr<StoredGeometry> geometry;
};
struct StoredDraw {
    std::shared_ptr<const StoredGeometry> vertices, indices;
    uint32_t firstIndex = 0, indexCount = 0;
    std::optional<EngineTransformSnapshot> transforms;
    std::optional<EngineVertexBindingSnapshot> vertexBindings;
    explicit operator bool() const { return vertices && indices; }
};
struct StoredGeometryStats {
    uint64_t started = 0, published = 0, rejectedUploads = 0, evicted = 0;
    uint64_t matchedDraws = 0, missingDraws = 0, invalidDraws = 0;
    size_t cachedBytes = 0, cachedResources = 0;
};

// Recover one attribute using the original CPU conversion semantics from
// 82760598. The result has default (0,0,0,1) components and applies the stored
// scale/offset for slots 0..4. This does not assign GPU/shader semantics to
// packed data, skin vertices or apply a model transform. Failure leaves out
// unchanged; only owned bytes are read.
bool decodeStoredAttribute(const StoredGeometry& geometry, uint32_t vertex, uint32_t slot,
                           std::array<float, 4>& out) noexcept;

class StoredGeometryCache {
public:
    // The first level uploads over 6,000 resources before drawing them. Keep
    // those original bytes through the load; a menu-sized entry cap discards
    // live geometry which the title will not upload again.
    explicit StoredGeometryCache(size_t byteLimit = 256 * 1024 * 1024, size_t entryLimit = 16384);
    StoredUpload begin(uint8_t* base, uint32_t resource) noexcept;
    void vertices(StoredUpload& upload, uint8_t* base, uint32_t descriptor, uint32_t destination,
                  uint32_t formats, uint32_t constants, uint32_t mask) noexcept;
    void indices(StoredUpload& upload, uint8_t* base, uint32_t destination,
                 uint32_t capacity, uint32_t produced) noexcept;
    bool finish(StoredUpload&& upload, uint8_t* base) noexcept;
    StoredDraw draw(uint8_t* base, uint32_t vertexId, uint32_t indexId,
                    uint32_t firstIndex, uint32_t indexCount, uint32_t boundIndexBuffer) noexcept;
    std::shared_ptr<const StoredGeometry> vertexStream(uint8_t* base,uint32_t vertexId,uint32_t boundVertexBuffer) noexcept;
    StoredGeometryStats stats() const;
private:
    struct Entry { std::shared_ptr<const StoredGeometry> geometry; uint64_t used; };
    using Entries=std::unordered_map<uint32_t,Entry>;
    Entries::iterator eraseEntry(Entries::iterator);
    mutable std::mutex mutex_;
    Entries entries_;
    // Original IDs are BE16. Map entries retain stable addresses on rehash;
    // every erase clears this non-owning index while holding mutex_.
    std::vector<Entry*> byId_;
    // Address-to-ID index for upload invalidation. begin() previously scanned
    // all entries per upload (O(entryLimit) each, up to 16384); the first level
    // uploads 6,000+ resources before drawing. The mutex stays exclusive: all
    // callers are engine-thread hooks (render_trace/simple_mesh interception),
    // so a shared_mutex would only pessimize the uncontended path. Concurrency
    // safety instead comes from this index plus generation-rechecked,
    // lock-free guest validation in draw()/vertexStream().
    std::unordered_multimap<uint32_t, uint32_t> byAddress_;
    std::vector<uint64_t> generations_;
    uint64_t clock_ = 0;
    size_t byteLimit_, entryLimit_;
    StoredGeometryStats stats_;
};
StoredGeometryCache& storedGeometryCache();
void printStoredGeometryCounters();

// Arguments retained across the original preparation call. The two engine
// entries reach different indexed callers; an immediate draw cannot consume
// either request, even when nested inside the original preparation.
struct StoredDrawRequest {
    uint32_t vertices, indices, triangles, firstIndex;
    uint32_t caller = 0x8225E3B8;
    static StoredDrawRequest whole(uint32_t id) noexcept { return {id,id,0,0,0x8225E2E0}; }
    bool matches(uint32_t returnAddress, uint32_t primitive, uint32_t baseVertex,
                 uint32_t first, uint32_t count) const noexcept;
    StoredDraw resolve(StoredGeometryCache& cache, uint8_t* base, uint32_t returnAddress,
                       uint32_t primitive, uint32_t baseVertex, uint32_t first, uint32_t count,
                       uint32_t boundVertexId, uint32_t boundIndexBuffer) const noexcept;
};
}
