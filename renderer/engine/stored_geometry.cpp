#include "stored_geometry.h"
#include "simple_mesh.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00;
constexpr uint32_t maxVertices = 65535, maxIndices = 65535 * 3;
constexpr size_t maxVertexBytes = 4 * 1024 * 1024;
// Original byte-size table 82A3D31C, used by 82760538 and 82762328.
constexpr std::array<uint8_t, 27> formatSizes{0,4,8,12,16,2,4,6,8,2,4,6,8,6,4,4,4,4,4,4,2,4,8,2,4,6,8};
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool read(uint8_t* base, uint32_t address, uint32_t& result) {
    uint8_t bytes[4];
    if (!copyRenderMemory(base, address, bytes, sizeof(bytes))) return false;
    result = be32(bytes); return true;
}
bool live(uint8_t* base, uint32_t address, uint32_t id, std::array<uint8_t, 108>& resource) {
    uint32_t table = 0, resolved = 0, manager = 0, validity = 0;
    uint8_t flags = 0;
    return id && id < 65536 && copyRenderMemory(base, address, resource.data(), resource.size()) &&
        be16(resource.data() + 92) == id && read(base, context + 16636, table) && table &&
        uint64_t(table) + (uint64_t(id) + 1) * 4 + 4 <= 0x100000000ull &&
        read(base, table + (id + 1) * 4, resolved) && resolved == address &&
        read(base, context + 160, manager) && manager && uint64_t(manager) + 28 <= 0x100000000ull &&
        read(base, manager + 24, validity) && validity && uint64_t(validity) + id < 0x100000000ull &&
        copyRenderMemory(base, validity + id, &flags, 1) && (flags & 1);
}
uint32_t holder(const StoredGeometry& geometry, const uint8_t* resource, unsigned offset) {
    const uint32_t pointer = be32(resource + offset + 36);
    return pointer ? pointer : (be32(resource + offset) ? geometry.address + offset + 4 : 0);
}
bool sameBinding(const StoredGeometry& geometry, const uint8_t* current, bool vertices) {
    const auto* saved = geometry.resource.data();
    if (vertices)
        return current[96] == geometry.stride && be32(current + 8) == be32(saved + 8) &&
            be32(current + 100) == be32(saved + 100) &&
            be32(current + 104) == be32(saved + 104) && holder(geometry, current, 12) &&
            holder(geometry, current, 12) == holder(geometry, saved, 12);
    return be16(current + 94) * 3u == geometry.indices.size() && holder(geometry, current, 52) &&
        holder(geometry, current, 52) == holder(geometry, saved, 52);
}
}

bool decodeStoredAttribute(const StoredGeometry& geometry, uint32_t vertex, uint32_t slot,
                           std::array<float, 4>& out) noexcept {
    if (slot >= 16 || vertex >= geometry.vertexCount || !geometry.stride ||
        (geometry.conversionMask & ~31u) || uint64_t(geometry.vertexCount) * geometry.stride != geometry.vertices.size())
        return false;
    uint32_t stride = 0, offset = 0;
    for (size_t i = 0; i < geometry.formats.size(); ++i) {
        const auto format = geometry.formats[i];
        if (format >= formatSizes.size()) return false;
        if (i == slot) offset = stride;
        stride += formatSizes[format];
    }
    if (stride != geometry.stride) return false;
    const auto format = geometry.formats[slot];
    if (!format) return false;
    const uint8_t* p = geometry.vertices.data() + size_t(vertex) * stride + offset;
    std::array<float, 4> decoded{0, 0, 0, 1};
    if (format <= 4) {
        for (unsigned i = 0; i < format; ++i) decoded[i] = std::bit_cast<float>(be32(p + i * 4));
    } else if (format >= 9 && format <= 12) {
        for (unsigned i = 0; i < format - 8u; ++i) decoded[i] = float(be16(p + i * 2));
    } else if (format == 14 || format == 15) {
        // Original float constants: 8209DF0C (1/2047), 8209DF08
        // (1/1023), 8209DFE0 (1/511). These are unsigned 11:11:10
        // fields. Format 14 is not a signed DXGI normalized format.
        constexpr float s2047 = std::bit_cast<float>(0x3A001002u);
        constexpr float s1023 = std::bit_cast<float>(0x3A802008u);
        constexpr float s511 = std::bit_cast<float>(0x3B004020u);
        const uint32_t packed = be32(p);
        const float xy = format == 15 ? s2047 : s1023, z = format == 15 ? s1023 : s511;
        decoded[0] = float(packed & 2047u) * xy;
        decoded[1] = float((packed >> 11) & 2047u) * xy;
        decoded[2] = float(packed >> 22) * z;
    } else if (format >= 16 && format <= 18) {
        const uint32_t packed = be32(p);
        constexpr float s255 = std::bit_cast<float>(0x3B808081u); // 8209DCCC.
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned component = format == 18 && (i == 0 || i == 2) ? 2 - i : i;
            decoded[i] = float((packed >> (component * 8)) & 255u);
            if (format != 17) decoded[i] *= s255;
        }
    } else return false;
    if (slot < 5 && (geometry.conversionMask & (1u << slot))) {
        const uint8_t* conversion = geometry.conversionConstants.data() + slot * 32;
        for (unsigned i = 0; i < 4; ++i) {
            const float scale = std::bit_cast<float>(be32(conversion + i * 4));
            const float offsetValue = std::bit_cast<float>(be32(conversion + 16 + i * 4));
            if (!std::isfinite(scale) || !std::isfinite(offsetValue)) return false;
            // Match the original single-round multiply-add in the AOT CPU
            // converter; do not round the product to float before addition.
            decoded[i] = float(double(scale) * double(decoded[i]) + double(offsetValue));
        }
    }
    if (!std::all_of(decoded.begin(), decoded.end(), [](float value) { return std::isfinite(value); })) return false;
    out = decoded;
    return true;
}

StoredGeometryCache::StoredGeometryCache(size_t byteLimit, size_t entryLimit)
    : byId_(65536), generations_(65536), byteLimit_(byteLimit), entryLimit_(entryLimit) {}

StoredGeometryCache::Entries::iterator StoredGeometryCache::eraseEntry(Entries::iterator entry) {
    byId_[entry->first]=nullptr;
    const uint32_t address = entry->second.geometry->address, id = entry->first;
    if (auto range = byAddress_.equal_range(address); range.first != range.second)
        for (auto it = range.first; it != range.second; ++it)
            if (it->second == id) { byAddress_.erase(it); break; }
    stats_.cachedBytes-=entry->second.geometry->bytes();
    return entries_.erase(entry);
}

bool StoredDrawRequest::matches(uint32_t returnAddress, uint32_t primitive, uint32_t baseVertex,
                                uint32_t first, uint32_t count) const noexcept {
    if (returnAddress != caller || primitive != 4 || baseVertex || first != firstIndex) return false;
    // 8225E218 reads the complete triangle count from resource+94 (BE16)
    // after preparation. 8225E2F0 instead receives an explicit subset.
    if (caller == 0x8225E2E0) return count && count % 3 == 0 && count <= 3u * 65535u;
    return caller == 0x8225E3B8 && uint64_t(triangles) * 3 == count;
}

StoredDraw StoredDrawRequest::resolve(StoredGeometryCache& cache, uint8_t* base, uint32_t returnAddress,
                                     uint32_t primitive, uint32_t baseVertex, uint32_t first, uint32_t count,
                                     uint32_t boundVertexId, uint32_t boundIndexBuffer) const noexcept {
    if (boundVertexId != vertices || !matches(returnAddress,primitive,baseVertex,first,count)) return {};
    auto owned = cache.draw(base,vertices,indices,first,count,boundIndexBuffer);
    if (owned && caller == 0x8225E2E0 && count != owned.indices->indices.size()) return {};
    return owned;
}

StoredUpload StoredGeometryCache::begin(uint8_t* base, uint32_t address) noexcept {
    StoredUpload upload;
    try {
        std::lock_guard lock(mutex_);
        ++stats_.started;
        // Invalidate before calling the original provider, including address
        // reuse under a new ID and failed replacements. An old in-flight
        // upload must not resurrect a newer generation. The address index
        // finds same-address entries directly instead of scanning all entries.
        if (auto range = byAddress_.equal_range(address); range.first != range.second) {
            std::vector<uint32_t> doomed;
            for (auto it = range.first; it != range.second; ++it) doomed.push_back(it->second);
            for (uint32_t id : doomed) {
                ++generations_[id];
                if (auto it = entries_.find(id); it != entries_.end()) eraseEntry(it);
            }
        }
        std::array<uint8_t, 108> resource;
        if (!copyRenderMemory(base, address, resource.data(), resource.size())) return upload;
        const uint32_t id = be16(resource.data() + 92);
        if (!id) return upload;
        upload.generation = ++generations_[id];
        if (auto it = entries_.find(id); it != entries_.end()) {
            eraseEntry(it);
        }
        upload.geometry = std::make_unique<StoredGeometry>();
        upload.geometry->address = address; upload.geometry->id = id;
    } catch (...) { upload.failed = true; }
    return upload;
}

void StoredGeometryCache::vertices(StoredUpload& upload, uint8_t* base, uint32_t descriptor,
    uint32_t destination, uint32_t formats, uint32_t constants, uint32_t mask) noexcept {
    if (!upload.geometry || upload.failed) return;
    try {
        auto& geometry = *upload.geometry;
        uint8_t description[448];
        if (!geometry.vertices.empty() || (mask & ~31u) ||
            !copyRenderMemory(base, descriptor, description, sizeof(description)) ||
            !copyRenderMemory(base, formats, geometry.formats.data(), geometry.formats.size())) {
            upload.failed = true; return;
        }
        // 82762328 does not consume incoming r8. At 827623E4 it loads
        // descriptor+424 and passes that count on the stack to 82760598.
        const uint32_t count = be32(description + 424);
        if (!count || count > maxVertices) { upload.failed = true; return; }
        uint32_t stride = 0;
        for (size_t slot = 0; slot < geometry.formats.size(); ++slot) {
            const auto format = geometry.formats[slot];
            const auto source = description[392 + slot];
            // 82762328 skips absent source slots without advancing the
            // output cursor. Both layouts must therefore have the same
            // occupied slots; otherwise cached semantic offsets are wrong.
            if (format >= formatSizes.size() || source >= formatSizes.size() ||
                bool(format) != bool(source) || (source && !be32(description + slot * 4))) {
                upload.failed = true; return;
            }
            stride += formatSizes[format];
        }
        const uint64_t bytes = uint64_t(count) * stride;
        if (!stride || stride > 255 || bytes > maxVertexBytes || bytes > byteLimit_ ||
            (mask && !constants) || (constants && !copyRenderMemory(base, constants,
                geometry.conversionConstants.data(), geometry.conversionConstants.size()))) {
            upload.failed = true; return;
        }
        geometry.vertices.resize(size_t(bytes));
        if (!copyRenderMemory(base, destination, geometry.vertices.data(), geometry.vertices.size())) {
            upload.failed = true; return;
        }
        geometry.vertexCount = count; geometry.stride = stride; geometry.conversionMask = mask;
    } catch (...) { upload.failed = true; }
}

void StoredGeometryCache::indices(StoredUpload& upload, uint8_t* base, uint32_t destination,
                                 uint32_t capacity, uint32_t produced) noexcept {
    if (!upload.geometry || upload.failed) return;
    try {
        auto& geometry = *upload.geometry;
        if (!geometry.indices.empty() || !capacity || capacity > maxIndices || produced != capacity ||
            !produced || produced % 3 || uint64_t(produced) * 2 > byteLimit_) {
            upload.failed = true; return;
        }
        // The stored expansion allocates the total count from 82764320. A
        // short result is not a complete stored IB, unlike immediate chunks.
        geometry.indices.resize(produced);
        if (!copyRenderMemory(base, destination, geometry.indices.data(), size_t(produced) * 2)) {
            upload.failed = true; return;
        }
        for (auto& index : geometry.indices) {
            index = be16(reinterpret_cast<const uint8_t*>(&index));
            geometry.maximumIndex = (std::max)(geometry.maximumIndex, index);
        }
    } catch (...) { upload.failed = true; }
}

bool StoredGeometryCache::finish(StoredUpload&& upload, uint8_t* base) noexcept {
    try {
        std::lock_guard lock(mutex_);
        auto& geometry = upload.geometry;
        if (upload.failed || !geometry || !geometry->id || geometry->id>=byId_.size() ||
            !geometry->bytes() || geometry->bytes() > byteLimit_ || !entryLimit_ ||
            upload.generation != generations_[geometry->id] ||
            !live(base, geometry->address, geometry->id, geometry->resource) ||
            (!geometry->vertices.empty() && !sameBinding(*geometry, geometry->resource.data(), true)) ||
            (!geometry->indices.empty() && !sameBinding(*geometry, geometry->resource.data(), false))) {
            ++stats_.rejectedUploads; return false;
        }
        while (!entries_.empty() && (entries_.size() >= entryLimit_ || stats_.cachedBytes + geometry->bytes() > byteLimit_)) {
            auto oldest = std::min_element(entries_.begin(), entries_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            eraseEntry(oldest); ++stats_.evicted;
        }
        const auto id = geometry->id;
        const auto bytes = geometry->bytes();
        auto [entry,inserted]=entries_.emplace(id, Entry{std::shared_ptr<const StoredGeometry>(std::move(geometry)), ++clock_});
        if(!inserted) {++stats_.rejectedUploads;return false;}
        byId_[id]=&entry->second;
        byAddress_.emplace(entry->second.geometry->address, id);
        stats_.cachedBytes += bytes; ++stats_.published; return true;
    } catch (...) { return false; }
}

StoredDraw StoredGeometryCache::draw(uint8_t* base, uint32_t vertexId, uint32_t indexId,
                                     uint32_t firstIndex, uint32_t indexCount, uint32_t boundIndexBuffer) noexcept {
    // Phase 1: snapshot owned geometry under the lock. Guest validation below
    // performs a dozen SEH-guarded guest reads; holding the lock across them
    // would serialize any concurrent hook on memory latency.
    std::shared_ptr<const StoredGeometry> vertices, indices;
    uint32_t vertexAddress = 0, indexAddress = 0;
    uint64_t vertexGeneration = 0, indexGeneration = 0;
    const void* vertexEntry = nullptr;
    const void* indexEntry = nullptr;
    bool combined = false;
    try {
        std::lock_guard lock(mutex_);
        auto* v=vertexId<byId_.size()?byId_[vertexId]:nullptr;
        auto* i=indexId<byId_.size()?byId_[indexId]:nullptr;
        if (!v || !i) { ++stats_.missingDraws; return {}; }
        vertices = v->geometry; indices = i->geometry;
        vertexAddress = vertices->address; indexAddress = indices->address;
        vertexGeneration = generations_[vertexId]; indexGeneration = generations_[indexId];
        vertexEntry = v; indexEntry = i; combined = (v == i);
    } catch (...) { return {}; }
    // Phase 2: lock-free validation against the snapshots. Owned vectors stay
    // alive through the snapshot references even if evicted concurrently.
    enum class Verdict { kMatch, kMissing, kInvalid };
    Verdict verdict = Verdict::kMatch;
    try {
        std::array<uint8_t, 108> currentV, currentI;
        // A combined vertex/index resource has one identity and one descriptor.
        // Validate that live descriptor once, then check BOTH bindings against
        // the same owned bytes. Separate IDs retain independent validation.
        if (!live(base, vertexAddress, vertexId, currentV) ||
            (!combined && !live(base, indexAddress, indexId, currentI))) {
            verdict = Verdict::kMissing;
        } else {
            const auto* currentIndex = combined ? currentV.data() : currentI.data();
            if (!sameBinding(*vertices, currentV.data(), true) || !sameBinding(*indices, currentIndex, false) ||
                vertices->vertices.empty() || indices->indices.empty() || !boundIndexBuffer ||
                boundIndexBuffer != holder(*indices, currentIndex, 52)) {
                verdict = Verdict::kMissing;
            } else if (!indexCount || indexCount % 3 || uint64_t(firstIndex) + indexCount > indices->indices.size()) {
                verdict = Verdict::kInvalid;
            } else if (indices->maximumIndex >= vertices->vertexCount &&
                std::any_of(indices->indices.begin() + firstIndex, indices->indices.begin() + firstIndex + indexCount,
                    [&](uint16_t index) { return index >= vertices->vertexCount; })) {
                // The common same-resource case is O(1). Separate IB/VB IDs may use
                // a valid subset even when another part of that IB exceeds this VB.
                verdict = Verdict::kInvalid;
            }
        }
    } catch (...) { return {}; }
    // Phase 3: confirm neither entry was replaced during validation, then
    // publish LRU/statistics. A replacement is a safe miss, never stale data.
    try {
        std::lock_guard lock(mutex_);
        auto* v=vertexId<byId_.size()?byId_[vertexId]:nullptr;
        auto* i=indexId<byId_.size()?byId_[indexId]:nullptr;
        if (!v || !i || v != vertexEntry || i != indexEntry ||
            generations_[vertexId] != vertexGeneration || generations_[indexId] != indexGeneration) {
            ++stats_.missingDraws; return {};
        }
        if (verdict == Verdict::kMissing) { ++stats_.missingDraws; return {}; }
        if (verdict == Verdict::kInvalid) { ++stats_.invalidDraws; return {}; }
        v->used = i->used = ++clock_;
        ++stats_.matchedDraws;
        return {vertices, indices, firstIndex, indexCount};
    } catch (...) { return {}; }
}
std::shared_ptr<const StoredGeometry> StoredGeometryCache::vertexStream(uint8_t* base,uint32_t id,uint32_t bound) noexcept {
    std::shared_ptr<const StoredGeometry> geometry;
    uint32_t address = 0;
    uint64_t generation = 0;
    const void* entry = nullptr;
    try {
        std::lock_guard lock(mutex_);
        auto* found=id<byId_.size()?byId_[id]:nullptr;
        if (!found || !bound) return {};
        geometry = found->geometry; address = geometry->address;
        generation = generations_[id]; entry = found;
    } catch (...) {return {};}
    std::array<uint8_t,108> current;
    bool valid = false;
    try {
        const auto& g=*geometry;
        valid = !g.vertices.empty() && live(base,address,id,current) && sameBinding(g,current.data(),true) &&
            bound==holder(g,current.data(),12);
    } catch (...) {return {};}
    if (!valid) return {};
    try {
        std::lock_guard lock(mutex_);
        auto* found=id<byId_.size()?byId_[id]:nullptr;
        if (!found || found != entry || generations_[id] != generation) return {};
        found->used=++clock_;return found->geometry;
    } catch (...) {return {};}
}
StoredGeometryStats StoredGeometryCache::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_; result.cachedResources = entries_.size(); return result;
}
StoredGeometryCache& storedGeometryCache() { static StoredGeometryCache cache; return cache; }
void printStoredGeometryCounters() {
    const auto s = storedGeometryCache().stats();
    std::fprintf(stderr, "[StoredGeometry] started=%llu published=%llu rejectedUploads=%llu evicted=%llu cachedResources=%zu cachedBytes=%zu matchedDraws=%llu missingDraws=%llu invalidDraws=%llu\n",
        s.started, s.published, s.rejectedUploads, s.evicted, s.cachedResources, s.cachedBytes,
        s.matchedDraws, s.missingDraws, s.invalidDraws);
    printEngineTransformCounters();
}
}
