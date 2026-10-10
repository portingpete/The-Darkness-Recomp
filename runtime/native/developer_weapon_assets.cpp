#include "developer_weapon_assets.h"
#include "runtime.h"
#include "objects.h"
#include "ppc_recomp_shared.h"
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace DarkRecomp::Native {
namespace {
using Bytes = std::vector<uint8_t>;
using DeveloperWeaponAssetDetail::File;
using DeveloperWeaponAssetDetail::Segment;
constexpr uint32_t absent = 0xffffffff;
constexpr uint64_t maximumPayload = 512ull * 1024 * 1024;
constexpr uint32_t mipMetadataSize = 28;
constexpr uint32_t maximumMipPayload = 1024 * 1024;
constexpr std::array<std::string_view, 6> requiredNames{
    "anim\\animgraphs\\weapons\\ag2weapon_darkness01.xah",
    "anim\\animgraphs\\weapons\\ag2weapon_darkness02.xah",
    "anim\\main\\gun_darkness_01.xsa", "anim\\main\\gun_darkness_02.xsa",
    "models\\weapons\\gun_darkness_01.xmd", "models\\weapons\\gun_darkness_02.xmd"};
constexpr std::array<std::string_view, 3> donors{
    "GameContext_Create.XDF", "NY1_Chinatown_Common.XDF", "NY1_Chinatown_Precache.XDF"};
// The original XTC image directories identify these Ancient material/effect
// images. Import only their recorded header/lower-mip reads, plus the global
// container header, directory and footer. Missing resident mip payloads are
// copied from the game's .xt0/.xt1 files after exact metadata validation.
// Every approved archive interval must be supplied by the donors.
constexpr std::string_view texture0 = "textures\\alltextures.000.xtc";
constexpr std::string_view texture1 = "textures\\alltextures.001.xtc";
constexpr std::array<std::pair<uint32_t, uint32_t>, 39> texture0Ranges{{
    {0, 48}, {1076924517, 868132}, {1077792649, 96},
    {2338224, 60}, {2346492, 3052}, // cube_alley_00
    {47389952, 64}, {47455568, 22224}, // muzzleancientgun01_003
    {48864672, 56}, {48868840, 1656}, // impactspark0001
    {50052616, 68}, {50183772, 44124}, // impactsmoke0003
    {52359272, 68}, {52490428, 44124}, // muzzleshotgunsmoke_02
    {55084664, 64}, {55150280, 22224}, // muzzlegatlin001
    {55216640, 64}, {55282256, 22224}, // muzzleancientgun02_002
    {55439992, 64}, {55472840, 11288}, // muzzleancientgun01_004
    {55583328, 64}, {55607984, 8568}, // muzzleanbeam02_001
    {55616552, 64}, {55649400, 11288}, // muzzleancientgun01_005
    {56514544, 60}, {56518716, 1692}, // electric01
    {55304480, 68}, {55370100, 22300}, // anweaponhit04
    {55484128, 68}, {55549748, 22260}, // anweaponhit05
    {52051616, 64}, {52084464, 11288}, // draintest2
    {55660688, 56}, {55662808, 960}, // holeconcreteangun02_a
    {55392400, 60}, {55408860, 5796}, // holeconcreteangun02_c
    {55414656, 60}, {55431116, 5796}, // holeconcreteangun02_n
    {55572008, 60}, {55580276, 3052} // holeconcreteangun02_o
}};
constexpr std::array<std::pair<uint32_t, uint32_t>, 23> texture1Ranges{{
    {0, 48}, {147500436, 64992}, {147565428, 96},
    {138602740, 64}, {138668356, 22224}, // gun_darkness_01_d
    {138690580, 64}, {138756196, 22224}, // gun_darkness_01_s
    {138778420, 64}, {138844020, 22096}, // gun_darkness_01_n
    {138866116, 92}, // gun_darkness_01_ani
    {138869444, 60}, {138885888, 5684}, // gun_darkness_01b_n
    {138891572, 60}, {138908032, 5796}, // gun_darkness_01b_s
    {138913828, 60}, {138930288, 5796}, // gun_darkness_01b_d
    {138936084, 608}, // gun_darkness_02_ani
    {138936692, 64}, {139002308, 22224}, // gun_darkness_02_d
    {139024532, 64}, {139090148, 22224}, // gun_darkness_02_s
    {139112372, 64}, {139177972, 22096} // gun_darkness_02_n
}};

struct StreamedMip {
    std::string_view image;
    bool textureOne, xtOne;
    uint32_t index, imageStart, imageEnd, physicalOffset;
};
// These physical records contain the exact metadata of the embedded resident
// mip. A directory's streaming offset can refer to an earlier, larger mip;
// it must not replace this independently verified physical mapping.
constexpr std::array<StreamedMip, 28> streamedMips{{
    {"cube_alley_00", false, true, 93, 2338224, 2349544, 89050188},
    {"electric01", false, true, 728, 56514544, 56520408, 93904380},
    {"impactsmoke0003", false, false, 492, 50052616, 50227896, 496595772},
    {"impactspark0001", false, true, 402, 48864672, 48870496, 95034600},
    {"muzzleanbeam02_001", false, true, 716, 55583328, 55616552, 57038040},
    {"muzzleancientgun01_003", false, false, 320, 47389952, 47477792, 577998400},
    {"muzzleancientgun01_004", false, true, 713, 55439992, 55484128, 30625256},
    {"muzzleancientgun01_005", false, true, 717, 55616552, 55660688, 30887752},
    {"muzzleancientgun02_002", false, false, 708, 55216640, 55304480, 583441540},
    {"muzzlegatlin001", false, false, 706, 55084664, 55172504, 583375960},
    {"muzzleshotgunsmoke_02", false, false, 555, 52359272, 52534552, 491875596},
    {"anweaponhit04", false, false, 709, 55304480, 55392400, 582785740},
    {"anweaponhit05", false, false, 714, 55484128, 55572008, 569276260},
    {"draintest2", false, true, 551, 52051616, 52095752, 27869048},
    {"holeconcreteangun02_a", false, true, 718, 55660688, 55663768, 97525348},
    {"holeconcreteangun02_c", false, true, 710, 55392400, 55414656, 72214416},
    {"holeconcreteangun02_n", false, true, 711, 55414656, 55436912, 72197988},
    {"holeconcreteangun02_o", false, true, 715, 55572008, 55583328, 91422156},
    {"gun_darkness_01_d", true, false, 642, 138602740, 138690580, 97073380},
    {"gun_darkness_01_n", true, false, 644, 138778420, 138866116, 103761980},
    {"gun_darkness_01_s", true, false, 643, 138690580, 138778420, 97007800},
    {"gun_darkness_01b_d", true, true, 649, 138913828, 138936084, 3842848},
    {"gun_darkness_01b_n", true, true, 647, 138869444, 138891572, 5091264},
    {"gun_darkness_01b_s", true, true, 648, 138891572, 138913828, 3892132},
    {"gun_darkness_02_ani", true, true, 650, 138936084, 138936692, 5903436},
    {"gun_darkness_02_d", true, false, 651, 138936692, 139024532, 97204540},
    {"gun_darkness_02_n", true, false, 653, 139112372, 139200068, 103696416},
    {"gun_darkness_02_s", true, false, 652, 139024532, 139112372, 97729180}
}};

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }
uint32_t word(std::span<const uint8_t> bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) invalid("Truncated developer weapon cache.");
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 |
        uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
}
std::span<const uint8_t> cachedBytes(const File& file, uint32_t offset, uint32_t length) {
    if (offset > file.size || length > file.size - offset)
        invalid("Developer weapon mip metadata is outside its container.");
    for (const auto& segment : file.segments) {
        if (segment.offset <= offset && uint64_t(offset) + length <=
            uint64_t(segment.offset) + segment.bytes.size())
            return std::span(segment.bytes).subspan(offset - segment.offset, length);
    }
    invalid("Developer weapon mip metadata is not cached.");
}
uint32_t mipPayloadSize(std::span<const uint8_t> metadata) {
    if (metadata.size() < mipMetadataSize || word(metadata, 0) != 0x300 ||
        !(word(metadata, 4) & 0x1000)) invalid("Unsupported developer weapon streamed mip format.");
    const auto size = word(metadata, 8), width = word(metadata, 12), height = word(metadata, 16);
    if (!size || size > maximumMipPayload || !width || width > 16384 || !height || height > 16384)
        invalid("Invalid developer weapon streamed mip dimensions or size.");
    return size;
}
std::string normalized(std::string_view input) {
    if (input.empty() || input.front() == '/' || input.front() == '\\')
        invalid("Invalid developer weapon cache filename.");
    std::string name(input);
    for (auto& c : name) {
        if (!c || static_cast<unsigned char>(c) >= 128 || c == ':')
            invalid("Invalid developer weapon cache filename.");
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    size_t begin = 0;
    while (begin < name.size()) {
        const auto end = name.find('\\', begin);
        const auto part = std::string_view(name).substr(begin,
            (end == std::string::npos ? name.size() : end) - begin);
        if (part.empty() || part == "." || part == "..")
            invalid("Invalid developer weapon cache filename.");
        if (end == std::string::npos) return name;
        begin = end + 1;
    }
    invalid("Invalid developer weapon cache filename.");
}
bool wanted(std::string_view name) {
    return std::find(requiredNames.begin(), requiredNames.end(), name) != requiredNames.end() ||
        name == texture0 || name == texture1;
}
std::span<const std::pair<uint32_t, uint32_t>> textureRanges(std::string_view name) {
    if (name == texture0) return texture0Ranges;
    if (name == texture1) return texture1Ranges;
    return {};
}
bool wantedRange(std::string_view name, uint32_t offset, uint32_t length) {
    if (name != texture0 && name != texture1) return wanted(name);
    for (const auto [first, count] : textureRanges(name))
        if (offset >= first && uint64_t(offset) + length <= uint64_t(first) + count) return true;
    return false;
}
struct Header {
    size_t strings = 0, files = 0, blocks = 0, stream = 0;
    uint32_t fileCount = 0, blockCount = 0;
    uint64_t payloadSize = 0;
};
Header header(std::span<const uint8_t> bytes) {
    if (word(bytes, 0) != 0x101) invalid("Unsupported developer weapon cache version.");
    Header result;
    result.strings = word(bytes, 4);
    if (bytes.size() < 8 || result.strings > bytes.size() - 8)
        invalid("Invalid developer weapon cache string table.");
    result.files = 8 + result.strings + 4;
    result.fileCount = word(bytes, result.files - 4);
    if (!result.fileCount || result.fileCount > 65536 || result.files > bytes.size() ||
        result.fileCount > (bytes.size() - result.files) / 24)
        invalid("Invalid developer weapon cache file table.");
    result.blocks = result.files + size_t(result.fileCount) * 24 + 4;
    result.blockCount = word(bytes, result.blocks - 4);
    if (!result.blockCount || result.blockCount > 1048576 || result.blocks > bytes.size() ||
        result.blockCount > (bytes.size() - result.blocks) / 20)
        invalid("Invalid developer weapon cache read table.");
    result.stream = result.blocks + size_t(result.blockCount) * 20;
    if (bytes.size() - result.stream < 6 || (bytes[result.stream] & 15) != 8 ||
        bytes[result.stream] >> 4 > 7 ||
        ((unsigned(bytes[result.stream]) << 8) | bytes[result.stream + 1]) % 31 ||
        (bytes[result.stream + 1] & 32)) invalid("Invalid developer weapon cache zlib header.");
    for (uint32_t index = 0; index < result.blockCount; ++index) {
        const auto at = result.blocks + size_t(index) * 20;
        const auto length = word(bytes, at + 8);
        if (!length || word(bytes, at + 16) != result.payloadSize)
            invalid("Invalid developer weapon cache stream offset.");
        result.payloadSize += length;
        if (result.payloadSize > maximumPayload) invalid("Developer weapon cache payload is too large.");
    }
    return result;
}
Bytes read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size <= 0 || uint64_t(size) > 128ull * 1024 * 1024)
        invalid("Missing or invalid developer weapon donor archive.");
    Bytes bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        invalid("Cannot read developer weapon donor archive.");
    return bytes;
}
class SourceHash {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
public:
    SourceHash() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            invalid("Developer weapon SHA256 is unavailable.");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
            invalid("Cannot initialize developer weapon SHA256.");
        }
    }
    ~SourceHash() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void add(std::span<const uint8_t> bytes) {
        if (bytes.size() > ULONG_MAX ||
            BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), 0) < 0)
            invalid("Cannot hash developer weapon source.");
    }
    void add(std::string_view text) {
        add(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    }
    std::string finish() {
        std::array<uint8_t, 32> bytes{};
        if (BCryptFinishHash(hash_, bytes.data(), ULONG(bytes.size()), 0) < 0)
            invalid("Cannot finish developer weapon SHA256.");
        std::string value;
        constexpr char hex[] = "0123456789abcdef";
        for (auto byte : bytes) { value += hex[byte >> 4]; value += hex[byte & 15]; }
        return value;
    }
};
class StreamedFile {
    std::ifstream input_;
public:
    uint64_t size = 0;
    Bytes table;
    explicit StreamedFile(const std::filesystem::path& path) : input_(path, std::ios::binary | std::ios::ate) {
        const auto length = input_.tellg();
        if (!input_ || length < 4 || uint64_t(length) > UINT32_MAX)
            invalid("Missing or invalid developer weapon streamed texture file.");
        size = uint64_t(length);
        const auto prefix = readAt(0, 4);
        const auto count = word(prefix, 0);
        if (!count || count > 65536) invalid("Invalid developer weapon streamed texture table.");
        table = readAt(0, 4 + count * 8);
    }
    Bytes readAt(uint32_t offset, uint32_t length) {
        if (!length || length > maximumMipPayload + mipMetadataSize ||
            offset > size || length > size - offset)
            invalid("Developer weapon streamed texture read is outside its source.");
        Bytes bytes(length);
        input_.seekg(offset);
        if (!input_.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            invalid("Cannot read complete developer weapon streamed texture bytes.");
        return bytes;
    }
};
void materializeStreamedMips(std::map<std::string, File>& files,
    const std::filesystem::path& gameDirectory, SourceHash& sourceHash) {
    std::map<std::string, StreamedFile> sources;
    for (const auto& mip : streamedMips) {
        const auto container = mip.textureOne ? texture1 : texture0;
        const auto filename = std::string(container) + (mip.xtOne ? ".xt1" : ".xt0");
        auto [source, inserted] = sources.try_emplace(filename, gameDirectory / L"Content" / filename);
        if (inserted) {
            sourceHash.add(filename); sourceHash.add("\n");
            sourceHash.add(std::to_string(source->second.size)); sourceHash.add("\n");
            sourceHash.add(source->second.table);
        }
        auto& physical = source->second;
        DeveloperWeaponAssetDetail::validateStreamedMipTable(physical.table, physical.size,
            mip.index, mip.physicalOffset, mipMetadataSize);
        const auto metadata = physical.readAt(mip.physicalOffset, mipMetadataSize);
        const auto length = mipPayloadSize(metadata);
        if (mip.imageStart >= mip.imageEnd || length > mip.imageEnd - mip.imageStart)
            invalid("Developer weapon streamed mip exceeds its selected image.");
        DeveloperWeaponAssetDetail::validateStreamedMipTable(physical.table, physical.size,
            mip.index, mip.physicalOffset, mipMetadataSize + length);
        const auto serialized = physical.readAt(mip.physicalOffset, mipMetadataSize + length);
        // Validate both reads of the immutable source, too: publication must
        // not mix a metadata revision with a later payload revision.
        if (!std::equal(metadata.begin(), metadata.end(), serialized.begin()))
            invalid("Developer weapon streamed mip changed while being read.");
        auto& destination = files.at(std::string(container));
        DeveloperWeaponAssetDetail::mergeStreamedMip(destination, mip.imageStart, mip.imageEnd, serialized);
        CachedWeaponFile coverage; coverage.size = destination.size;
        for (const auto& segment : destination.segments)
            coverage.ranges.emplace_back(segment.offset, uint32_t(segment.bytes.size()));
        if (!coverage.covers(mip.imageStart, mip.imageEnd - mip.imageStart))
            invalid("Developer weapon resident image still contains an uncached hole.");
        sourceHash.add(filename); sourceHash.add("\n");
        sourceHash.add(mip.image); sourceHash.add("\n");
        for (const auto value : {mip.index, mip.imageStart, mip.imageEnd, mip.physicalOffset}) {
            sourceHash.add(std::to_string(value)); sourceHash.add(":");
        }
        sourceHash.add("\n"); sourceHash.add(serialized);
    }
    // gun_darkness_01_ani has no physical streamed record. Its authored
    // resident 8x8 mip is already fully cached; lower uncached mips stay holes.
}
struct GuestBuffer {
    PPCContext& call;
    uint8_t* base;
    uint32_t address = 0;
    GuestBuffer(PPCContext& context, uint8_t* memoryBase, uint32_t size) : call(context), base(memoryBase) {
        // Use the game's live heap. The native Memory allocation budget also
        // includes the loaded world and cannot accommodate entire archives.
        call.r3.u64 = 0; call.r4.u64 = 1; call.r5.u64 = size;
        PPCSafeIndirect(call, base, 0x822323C8);
        address = call.r3.u32;
        if (!address) invalid("Cannot allocate developer weapon decompression buffer.");
    }
    ~GuestBuffer() {
        if (address) {
            call.r3.u64 = 0; call.r4.u64 = address;
            PPCSafeIndirect(call, base, 0x822323E8);
        }
    }
};
Bytes inflate(PPCContext& context, uint8_t* base, std::span<const uint8_t> compressed, uint32_t size) {
    if (!memory || base != memory->base() || compressed.empty() || compressed.size() > UINT32_MAX ||
        size >= UINT32_MAX) invalid("Invalid developer weapon decompression context.");
    constexpr uint32_t chunk = 65536;
    auto call = context;
    GuestBuffer stream(call, base, 4096), input(call, base, chunk), output(call, base, chunk);
    std::memset(base + stream.address, 0, 4096);
    call.r3.u64 = stream.address;
    sub_8222B248(call, base);
    if (call.r3.s32 != 0) invalid("Cannot initialize original developer weapon decompressor.");
    struct Finish {
        PPCContext& call; uint8_t* base; uint32_t stream;
        ~Finish() { call.r3.u64 = stream; sub_8222CBB0(call, base); }
    } finish{call, base, stream.address};
    Bytes payload(size);
    size_t supplied = 0, produced = 0;
    int status = 0;
    // Every successful step consumes input or produces output. The original
    // decoder can retain buffered bits between chunks, including its trailer.
    const auto maximumSteps = (compressed.size() + size) / chunk * 2 + 1024;
    for (size_t iteration = 0; iteration < maximumSteps && status != 1; ++iteration) {
        if (!PPC_LOAD_U32(stream.address + 4) && supplied < compressed.size()) {
            const auto count = uint32_t((std::min)(size_t(chunk), compressed.size() - supplied));
            std::memcpy(base + input.address, compressed.data() + supplied, count);
            supplied += count;
            PPC_STORE_U32(stream.address, input.address);
            PPC_STORE_U32(stream.address + 4, count);
        }
        // One extra byte of capacity detects an overlong stream while still
        // allowing the decoder to consume its checksum at the expected end.
        const auto offered = uint32_t((std::min)(size_t(chunk), size_t(size) - produced + 1));
        PPC_STORE_U32(stream.address + 12, output.address);
        PPC_STORE_U32(stream.address + 16, offered);
        const auto oldInput = PPC_LOAD_U32(stream.address + 8);
        const auto oldOutput = PPC_LOAD_U32(stream.address + 20);
        call.r3.u64 = stream.address; call.r4.u64 = 0;
        sub_8222B468(call, base);
        status = call.r3.s32;
        if (status != 0 && status != 1) invalid("Invalid compressed developer weapon archive.");
        const auto remaining = PPC_LOAD_U32(stream.address + 16);
        if (remaining > offered || offered - remaining > size_t(size) - produced)
            invalid("Developer weapon decompression exceeded its cache records.");
        const auto count = offered - remaining;
        std::memcpy(payload.data() + produced, base + output.address, count);
        produced += count;
        if (!status && oldInput == PPC_LOAD_U32(stream.address + 8) &&
            oldOutput == PPC_LOAD_U32(stream.address + 20))
            invalid("Developer weapon decompression did not progress.");
    }
    if (status != 1 || supplied != compressed.size() || PPC_LOAD_U32(stream.address + 4) != 0 ||
        PPC_LOAD_U32(stream.address + 8) != compressed.size() ||
        PPC_LOAD_U32(stream.address + 20) != size || produced != size)
        invalid("Developer weapon decompression length differs from its cache records.");
    return payload;
}
bool sameFile(const std::filesystem::path& path, const File& file) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || uint64_t(input.tellg()) != file.size) return false;
    for (const auto& segment : file.segments) {
        Bytes bytes(segment.bytes.size());
        input.seekg(segment.offset);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || bytes != segment.bytes)
            return false;
    }
    return true;
}
void store(const std::filesystem::path& path, const File& file) {
    if (std::filesystem::exists(path)) {
        if (!sameFile(path, file)) invalid("Private developer weapon cache differs from its source.");
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = path.wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) invalid("Cannot create private developer weapon cache.");
    struct Temporary {
        HANDLE handle; std::filesystem::path path;
        ~Temporary() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            if (!path.empty()) DeleteFileW(path.c_str());
        }
    } pending{output, temporary};
    DWORD bytes = 0;
    if (!DeviceIoControl(output, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr))
        invalid("Private developer weapon cache requires sparse-file support.");
    LARGE_INTEGER position{}; position.QuadPart = file.size;
    if (!SetFilePointerEx(output, position, nullptr, FILE_BEGIN) || !SetEndOfFile(output))
        invalid("Cannot size private developer weapon cache.");
    for (const auto& segment : file.segments) {
        position.QuadPart = segment.offset;
        DWORD written = 0;
        if (!SetFilePointerEx(output, position, nullptr, FILE_BEGIN) ||
            !WriteFile(output, segment.bytes.data(), DWORD(segment.bytes.size()), &written, nullptr) ||
            written != segment.bytes.size()) invalid("Cannot write private developer weapon cache.");
    }
    if (!FlushFileBuffers(output)) invalid("Cannot flush private developer weapon cache.");
    CloseHandle(output); pending.handle = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.c_str(), path.c_str(), 0)) {
        if (!sameFile(path, file)) invalid("Cannot publish private developer weapon cache.");
    } else pending.path.clear();
}
bool guestSpan(uint8_t* base, uint32_t address, uint32_t size, bool writable) {
    if (!memory || base != memory->base() || !address || !size ||
        uint64_t(address) + size > PPC_MEMORY_SIZE) return false;
    auto* cursor = base + address;
    const auto* end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = info.Protect & 0xff;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool canRead = canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if (!canRead || (writable && !canWrite)) return false;
        auto* next = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
bool cachedLogicalRead(PPCContext& ctx, uint8_t* base) {
    const auto context = ctx.r3.u32, destination = ctx.r4.u32, requested = ctx.r5.u32;
    // CFile's I/O context owns a native descriptor wrapper at +20. Do not
    // intercept ordinary files, including real loose versions of these assets.
    if ((context & 3) || !guestSpan(base, context, 24, false)) return false;
    const auto wrapper = PPC_LOAD_U32(context + 20);
    if ((wrapper & 3) || !guestSpan(base, wrapper, 4, false)) return false;
    const auto file = object(PPC_LOAD_U32(wrapper));
    if (!file || !file->isFile || !file->cachedWeaponFile) return false;
    if (!requested) return true; // Retail zero-byte reads leave the state alone.
    uint64_t position = 0;
    auto fail = [&](const char* reason) {
        if (guestSpan(base, context + 88, 1, true)) PPC_STORE_U8(context + 88, 0);
        ctx.r3.u64 = 0;
        std::fprintf(stderr, "[Weapons] Logical read failed '%ls' offset=%llu bytes=%u: %s.\n",
            file->path.filename().c_str(), position, requested, reason);
    };
    // The original logical reader updates these counters only after memcpy,
    // and sets byte +88 to zero if its read buffer cannot supply the bytes.
    if (!guestSpan(base, context, 89, true)) { fail("invalid guest file context"); return true; }
    position = PPC_LOAD_U64(context + 32);
    const auto length = PPC_LOAD_U64(context + 48);
    const auto nominal = int64_t(length) < 0 ? file->cachedWeaponFile->size : length;
    if (int64_t(position) < 0) { fail("invalid file position"); return true; }
    if (position >= nominal) {
        PPC_STORE_U8(context + 88, 0); ctx.r3.u64 = nominal; return true;
    }
    const auto count = uint32_t((std::min)(uint64_t(requested), nominal - position));
    if (!file->cachedWeaponFile->covers(position, count)) { fail("uncached range"); return true; }
    if (!guestSpan(base, destination, count, true)) { fail("invalid guest destination"); return true; }
    try {
        // Read only the logical request. The ordinary loose-file reader would
        // prefetch aligned 64 KiB blocks spanning bytes never shipped in XDF.
        // An independent buffered handle avoids that read-ahead and preserves
        // the original native handle's asynchronous/unbuffered I/O contract.
        auto lock = std::lock_guard(file->ioMutex);
        Bytes bytes(count);
        std::ifstream input(file->cachedWeaponFile->path, std::ios::binary);
        input.seekg(std::streamoff(position));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
            fail("cannot read complete cached bytes"); return true;
        }
        std::memcpy(base + destination, bytes.data(), bytes.size());
        PPC_STORE_U64(context + 32, position + count);
        PPC_STORE_U64(context + 40, PPC_LOAD_U64(context + 40) + count);
        if (count != requested) PPC_STORE_U8(context + 88, 0);
        ctx.r3.u64 = destination; // Retail's last copy returns its destination.
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("host read error");
    }
    return true;
}

}

bool CachedWeaponFile::covers(uint64_t offset, uint32_t length) const noexcept {
    if (offset > size || length > size - offset) return false;
    if (!length) return true;
    for (const auto [first, count] : ranges) {
        if (first > offset) return false;
        const uint64_t end = uint64_t(first) + count;
        if (offset < end) return uint64_t(length) <= end - offset;
    }
    return false;
}

namespace DeveloperWeaponAssetDetail {
void mergeFile(File& destination, const File& source) {
    if (destination.name != source.name || destination.size != source.size)
        invalid("Developer weapon donor file metadata differs.");
    for (const auto& added : source.segments) {
        if (added.bytes.empty() || added.offset > destination.size ||
            added.bytes.size() > destination.size - added.offset)
            invalid("Invalid developer weapon cached range.");
        for (const auto& current : destination.segments) {
            const uint64_t first = (std::max)(uint64_t(added.offset), uint64_t(current.offset));
            const uint64_t last = (std::min)(uint64_t(added.offset) + added.bytes.size(),
                uint64_t(current.offset) + current.bytes.size());
            if (first < last && std::memcmp(added.bytes.data() + first - added.offset,
                current.bytes.data() + first - current.offset, size_t(last - first)))
                invalid("Developer weapon donor cached bytes conflict.");
        }
        destination.segments.push_back(added);
    }
    std::sort(destination.segments.begin(), destination.segments.end(),
        [](const auto& left, const auto& right) { return left.offset < right.offset; });
    std::vector<Segment> merged;
    for (const auto& segment : destination.segments) {
        if (merged.empty() || uint64_t(merged.back().offset) + merged.back().bytes.size() < segment.offset) {
            merged.push_back(segment); continue;
        }
        auto& previous = merged.back();
        const auto end = uint64_t(previous.offset) + previous.bytes.size();
        const auto skip = (std::min)(uint64_t(segment.bytes.size()), end - segment.offset);
        previous.bytes.insert(previous.bytes.end(), segment.bytes.begin() + size_t(skip), segment.bytes.end());
    }
    destination.segments = std::move(merged);
}
void validateStreamedMipTable(std::span<const uint8_t> table, uint64_t fileSize,
    uint32_t imageIndex, uint32_t expectedOffset, uint32_t serializedSize) {
    const auto count = word(table, 0);
    if (!count || count > 65536 || table.size() != 4ull + uint64_t(count) * 8 ||
        table.size() > fileSize || serializedSize < mipMetadataSize)
        invalid("Invalid developer weapon streamed texture table.");
    std::map<uint32_t, bool> indices, offsets;
    bool found = false;
    uint64_t end = fileSize;
    for (uint32_t index = 0; index < count; ++index) {
        const auto at = 4 + size_t(index) * 8;
        const auto image = word(table, at), offset = word(table, at + 4);
        if (image >= 65536 || offset < table.size() || offset > fileSize ||
            mipMetadataSize > fileSize - offset || !indices.emplace(image, true).second ||
            !offsets.emplace(offset, true).second)
            invalid("Invalid developer weapon streamed texture record.");
        if (image == imageIndex) {
            if (offset != expectedOffset) invalid("Developer weapon streamed mip mapping differs.");
            found = true;
        }
        if (offset > expectedOffset) end = (std::min)(end, uint64_t(offset));
    }
    if (!found || expectedOffset > end || serializedSize > end - expectedOffset)
        invalid("Developer weapon streamed mip is outside its physical record.");
}
void mergeStreamedMip(File& destination, uint32_t imageStart, uint32_t imageEnd,
    std::span<const uint8_t> serializedMip) {
    if (imageStart >= imageEnd || imageEnd > destination.size || imageEnd - imageStart < 4 + mipMetadataSize)
        invalid("Invalid developer weapon embedded image bounds.");
    const auto payloadSize = mipPayloadSize(serializedMip);
    if (serializedMip.size() != uint64_t(mipMetadataSize) + payloadSize)
        invalid("Developer weapon streamed mip length differs from its metadata.");
    const auto embedded = word(cachedBytes(destination, imageStart, 4), 0);
    if (embedded < uint64_t(imageStart) + 4 || uint64_t(embedded) + mipMetadataSize + payloadSize > imageEnd)
        invalid("Developer weapon streamed mip exceeds its embedded image.");
    const auto metadata = cachedBytes(destination, embedded, mipMetadataSize);
    if (!std::equal(metadata.begin(), metadata.end(), serializedMip.begin()))
        invalid("Developer weapon embedded and streamed mip metadata differs.");
    // Both raw-copy formats use this serialized body unchanged. Its opaque
    // GPU data can include a prefix; no guessed header is stripped or rebuilt.
    const File addition{destination.name, destination.size, {{embedded + mipMetadataSize,
        Bytes(serializedMip.begin() + mipMetadataSize, serializedMip.end())}}};
    mergeFile(destination, addition);
}
std::vector<File> parseArchive(std::span<const uint8_t> archive, std::span<const uint8_t> payload) {
    const auto info = header(archive);
    if (payload.size() != info.payloadSize) invalid("Invalid developer weapon cache payload length.");
    std::vector<bool> seen(info.blockCount, false);
    std::map<std::string, bool> names;
    std::vector<File> result;
    for (uint32_t index = 0; index < info.fileCount; ++index) {
        const auto record = info.files + size_t(index) * 24;
        const auto nameAt = word(archive, record);
        if (nameAt >= info.strings) invalid("Invalid developer weapon cache filename offset.");
        const auto first = archive.begin() + 8 + nameAt;
        const auto end = std::find(first, archive.begin() + 8 + info.strings, uint8_t(0));
        if (end == archive.begin() + 8 + info.strings)
            invalid("Unterminated developer weapon cache filename.");
        const auto name = normalized(std::string_view(reinterpret_cast<const char*>(archive.data() + 8 + nameAt),
            size_t(end - first)));
        if (!names.emplace(name, true).second) invalid("Duplicate developer weapon cache filename.");
        File file{name, word(archive, record + 12), {}};
        auto block = word(archive, record + 4);
        const auto last = word(archive, record + 8);
        if (block == absent) {
            if (last != absent) invalid("Invalid empty developer weapon cache read list.");
        } else for (;;) {
            if (block >= info.blockCount || seen[block]) invalid("Invalid developer weapon cache read link.");
            seen[block] = true;
            const auto at = info.blocks + size_t(block) * 20;
            const auto next = word(archive, at), owner = word(archive, at + 4);
            const auto length = word(archive, at + 8), offset = word(archive, at + 12);
            const auto stream = word(archive, at + 16);
            if (owner != index || offset > file.size || length > file.size - offset)
                invalid("Invalid developer weapon cache read range.");
            if (wantedRange(name, offset, length)) {
                File addition{name, file.size, {{offset, Bytes(payload.begin() + stream,
                    payload.begin() + stream + length)}}};
                mergeFile(file, addition);
            }
            if (next == absent) {
                if (block != last) invalid("Invalid developer weapon cache last read.");
                break;
            }
            block = next;
        }
        if (wanted(name) && !file.segments.empty()) result.push_back(std::move(file));
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
        invalid("Unowned developer weapon cache read records.");
    return result;
}
}

DeveloperWeaponAssets prepareDeveloperWeaponAssets(PPCContext& ctx, uint8_t* base,
    const std::filesystem::path& gameDirectory) {
    std::map<std::string, File> files;
    SourceHash sourceHash;
    sourceHash.add("DarkRecomp developer weapon cached reads and matched streamed mips v2\n");
    for (const auto name : requiredNames) { sourceHash.add(name); sourceHash.add("\n"); }
    for (const auto name : {texture0, texture1}) {
        sourceHash.add(name); sourceHash.add("\n");
        for (const auto [first, count] : textureRanges(name)) {
            sourceHash.add(std::to_string(first)); sourceHash.add(":");
            sourceHash.add(std::to_string(count)); sourceHash.add("\n");
        }
    }
    for (const auto donor : donors) {
        const auto archive = read(gameDirectory / L"Content/Xdf" / std::string(donor));
        const auto info = header(archive);
        sourceHash.add(donor); sourceHash.add("\n"); sourceHash.add(archive);
        const auto payload = inflate(ctx, base, std::span(archive).subspan(info.stream), uint32_t(info.payloadSize));
        for (const auto& file : DeveloperWeaponAssetDetail::parseArchive(archive, payload)) {
            auto [where, inserted] = files.try_emplace(file.name, File{file.name, file.size, {}});
            DeveloperWeaponAssetDetail::mergeFile(where->second, file);
        }
    }
    for (const auto name : requiredNames) if (!files.contains(std::string(name)))
        invalid("Developer weapon resources are missing from the donor archives.");
    for (const auto name : {texture0, texture1}) {
        const auto found = files.find(std::string(name));
        if (found == files.end() || found->second.size != (name == texture0 ? 1077792745u : 147565524u))
            invalid("Unsupported developer weapon texture container metadata.");
        CachedWeaponFile coverage; coverage.size = found->second.size;
        for (const auto& segment : found->second.segments)
            coverage.ranges.emplace_back(segment.offset, uint32_t(segment.bytes.size()));
        for (const auto [first, count] : textureRanges(name)) if (!coverage.covers(first, count))
            invalid("Developer weapon texture reads are missing from the donor archives.");
    }
    materializeStreamedMips(files, gameDirectory, sourceHash);
    const auto directory = gameDirectory.parent_path() / L"build_native/run/developer-weapons" / sourceHash.finish();
    DeveloperWeaponAssets result;
    for (const auto& [name, file] : files) {
        const auto path = directory / std::filesystem::path(name);
        store(path, file);
        auto prepared = std::make_shared<CachedWeaponFile>();
        prepared->path = path; prepared->size = file.size;
        for (const auto& segment : file.segments)
            prepared->ranges.emplace_back(segment.offset, uint32_t(segment.bytes.size()));
        result.emplace("content\\" + name, std::move(prepared));
    }
    return result;
}
}

// Keep the original CFile reader for every ordinary native file. Only private
// developer cached resources need logical reads rather than loose read-ahead.
extern "C" PPC_FUNC(__imp__sub_82209560);
PPC_FUNC(sub_82209560) {
    if (!DarkRecomp::Native::cachedLogicalRead(ctx, base)) __imp__sub_82209560(ctx, base);
}
