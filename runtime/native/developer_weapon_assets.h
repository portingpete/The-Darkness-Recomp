#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct PPCContext;

namespace DarkRecomp::Native {
struct CachedWeaponFile {
    std::filesystem::path path;
    uint64_t size = 0;
    // Sorted, disjoint cached intervals. Adjacent intervals are coalesced.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    bool covers(uint64_t offset, uint32_t length) const noexcept;
};
using DeveloperWeaponAssets = std::map<std::string, std::shared_ptr<const CachedWeaponFile>>;

// Extract only the developer weapons' cached reads and metadata-matched
// streamed texture bytes from this game's own files into private sparse
// files. Uncached holes are not asset data: callers must enforce covers()
// before every read. Keys are lowercase
// Content-relative paths, including "content\\". Throws on invalid sources.
DeveloperWeaponAssets prepareDeveloperWeaponAssets(PPCContext& ctx, uint8_t* base,
    const std::filesystem::path& gameDirectory);

namespace DeveloperWeaponAssetDetail {
struct Segment { uint32_t offset = 0; std::vector<uint8_t> bytes; };
struct File {
    std::string name;
    uint32_t size = 0;
    std::vector<Segment> segments;
};
// Pure format/range helpers shared by production preparation and its tests.
// The payload must be the archive's complete, verified decompressed stream.
std::vector<File> parseArchive(std::span<const uint8_t> archive,
                              std::span<const uint8_t> payload);
void mergeFile(File& destination, const File& source);
// Validate the physical streaming file's LE image-index/offset table and the
// selected record's bounds, including the next physical record.
void validateStreamedMipTable(std::span<const uint8_t> table, uint64_t fileSize,
    uint32_t imageIndex, uint32_t expectedOffset, uint32_t serializedSize);
// The embedded and streamed 0x300 mip formats have the same 28-byte metadata.
// Publish only a complete, metadata-matched physical payload, including its
// opaque GPU prefix. Cached overlap must agree byte-for-byte.
void mergeStreamedMip(File& destination, uint32_t imageStart, uint32_t imageEnd,
    std::span<const uint8_t> serializedMip);
}
}
