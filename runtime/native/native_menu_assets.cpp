#include "native_menu_assets.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace DarkRecomp::Native {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t absent = 0xffffffff;
constexpr std::string_view menuName = "gui\\cubewnd.xcr";
constexpr std::string_view privateName = "gui\\pcmenu_.xcr";
static_assert(menuName.size() == privateName.size());

Bytes read(const std::filesystem::path& path, size_t maximum = 128 * 1024 * 1024) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    if (!input || length <= 0 || uint64_t(length) > maximum)
        throw std::runtime_error("missing or invalid menu asset");
    Bytes bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("cannot read menu asset");
    return bytes;
}

std::string hash(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 unavailable");
    std::array<uint8_t, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
        ULONG(bytes.size()), digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

uint32_t word(std::span<const uint8_t> data, size_t position) {
    if (position > data.size() || data.size() - position < 4)
        throw std::runtime_error("truncated startup cache header");
    return uint32_t(data[position]) | uint32_t(data[position + 1]) << 8 |
           uint32_t(data[position + 2]) << 16 | uint32_t(data[position + 3]) << 24;
}

bool equals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const char c = left[index] >= 'A' && left[index] <= 'Z' ? left[index] + ('a' - 'A') : left[index];
        if (c != right[index]) return false;
    }
    return true;
}

void uncachedMenu(Bytes& bytes, size_t originalMenuSize) {
    // XDF 0x101 contains filename strings, 24-byte file records, 20-byte
    // linked read records and a zlib stream. The guest builds its filename
    // lookup from these strings; there are no serialized filename hashes.
    // Renaming just the cached menu makes its reads use the loose PC menu.
    // Keep every cached localization/font byte and all stream offsets intact.
    const std::span<const uint8_t> data(bytes);
    if (word(data, 0) != 0x101) throw std::runtime_error("unsupported startup cache version");
    const size_t stringSize = word(data, 4);
    if (stringSize > data.size() - 8) throw std::runtime_error("invalid startup cache string table");
    const size_t files = 8 + stringSize + 4;
    const size_t fileCount = word(data, files - 4);
    if (!fileCount || fileCount > 65536 || fileCount > (data.size() - files) / 24)
        throw std::runtime_error("invalid startup cache file table");
    const size_t blocks = files + fileCount * 24 + 4;
    const size_t blockCount = word(data, blocks - 4);
    if (!blockCount || blockCount > 1048576 || blockCount > (data.size() - blocks) / 20)
        throw std::runtime_error("invalid startup cache read table");
    const size_t stream = blocks + blockCount * 20;
    if (data.size() - stream < 6 || (data[stream] & 15) != 8 || data[stream] >> 4 > 7 ||
        ((unsigned(data[stream]) << 8) | data[stream + 1]) % 31 || (data[stream + 1] & 32))
        throw std::runtime_error("invalid startup cache zlib header");
    std::vector<std::pair<size_t, size_t>> names;
    std::vector<bool> seen(blockCount, false);
    size_t menuAt = 0, menuCount = 0;
    for (size_t index = 0; index < fileCount; ++index) {
        const size_t record = files + index * 24;
        const size_t nameAt = word(data, record);
        if (nameAt >= stringSize) throw std::runtime_error("invalid startup cache filename offset");
        const auto start = bytes.begin() + 8 + nameAt;
        const auto end = std::find(start, bytes.begin() + 8 + stringSize, uint8_t(0));
        if (end == bytes.begin() + 8 + stringSize)
            throw std::runtime_error("unterminated startup cache filename");
        const size_t nameLength = size_t(end - start);
        const std::string_view name(reinterpret_cast<const char*>(bytes.data() + 8 + nameAt), nameLength);
        names.emplace_back(nameAt, nameAt + nameLength);
        if (equals(name, privateName)) throw std::runtime_error("startup cache contains reserved menu filename");
        if (equals(name, menuName)) {
            ++menuCount; menuAt = nameAt;
            if (word(data, record + 12) != originalMenuSize)
                throw std::runtime_error("cached menu size differs from loose menu");
        }
        uint32_t block = word(data, record + 4);
        const uint32_t last = word(data, record + 8), size = word(data, record + 12);
        if (block == absent) {
            if (last != absent) throw std::runtime_error("invalid empty startup cache read list");
            continue;
        }
        for (;;) {
            if (block >= blockCount || seen[block]) throw std::runtime_error("invalid startup cache read link");
            seen[block] = true;
            const size_t at = blocks + size_t(block) * 20;
            const uint32_t next = word(data, at), owner = word(data, at + 4);
            const uint32_t length = word(data, at + 8), offset = word(data, at + 12);
            if (owner != index || offset > size || length > size - offset)
                throw std::runtime_error("invalid startup cache read range");
            if (next == absent) {
                if (block != last) throw std::runtime_error("invalid startup cache last read");
                break;
            }
            block = next;
        }
    }
    if (menuCount != 1 || std::find(seen.begin(), seen.end(), false) != seen.end())
        throw std::runtime_error("startup cache menu/read records are ambiguous");
    uint64_t payloadSize = 0;
    for (size_t index = 0; index < blockCount; ++index) {
        const size_t at = blocks + index * 20;
        if (word(data, at + 16) != payloadSize)
            throw std::runtime_error("invalid startup cache stream offset");
        payloadSize += word(data, at + 8);
        if (payloadSize > 512 * 1024 * 1024) throw std::runtime_error("startup cache payload is too large");
    }
    for (const auto [first, last] : names) {
        if (first != menuAt && first < menuAt + menuName.size() && last > menuAt)
            throw std::runtime_error("startup cache shares the menu filename storage");
    }
    std::copy(privateName.begin(), privateName.end(), bytes.begin() + 8 + menuAt);
}

std::pair<std::string, std::string> sourceHashes(const std::filesystem::path& path) {
    const auto bytes = read(path, 132);
    std::string text(bytes.begin(), bytes.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.size() != 130 || text[64] != '\n' || text[129] != '\n')
        throw std::runtime_error("invalid native menu source hashes");
    auto valid = [](std::string_view value) {
        return std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    const auto menu = text.substr(0, 64), archive = text.substr(65, 64);
    if (!valid(menu) || !valid(archive)) throw std::runtime_error("invalid native menu source hashes");
    return {menu, archive};
}

void store(const std::filesystem::path& path, const Bytes& bytes) {
    if (std::filesystem::exists(path)) {
        if (read(path) != bytes) throw std::runtime_error("generated menu cache differs from its content hash");
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()) +
                           L"." + std::to_wstring(GetTickCount64());
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create private menu cache");
    DWORD written = 0;
    const bool saved = WriteFile(output, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
                       written == bytes.size();
    CloseHandle(output);
    if (!saved || !MoveFileExW(temporary.c_str(), path.c_str(), 0)) {
        DeleteFileW(temporary.c_str());
        if (!saved || !std::filesystem::exists(path) || read(path) != bytes)
            throw std::runtime_error("cannot publish private menu cache");
    }
}
}

NativeMenuAssets prepareNativeMenuAssets(const std::filesystem::path& gameDirectory,
                                        const std::filesystem::path& bundledAssetsDirectory) noexcept {
    try {
        const auto menuPath = bundledAssetsDirectory / L"CubeWnd.pc.xcr";
        const auto archivePath = bundledAssetsDirectory / L"GameContext_Create.pc.xdf";
        const auto [menuHash, archiveHash] = sourceHashes(bundledAssetsDirectory / L"CubeWnd.pc.xcr.source.sha256");
        const auto sourceMenu = read(gameDirectory / L"Content/Gui/CubeWnd.xcr");
        if (hash(sourceMenu) != menuHash) {
            std::fputs("[Menu] Dump menu differs from the PC menu source; using original menus.\n", stderr);
            return {};
        }
        read(menuPath);
        auto sourceArchive = read(gameDirectory / L"Content/Xdf/GameContext_Create.XDF");
        if (hash(sourceArchive) == archiveHash) {
            read(archivePath);
            return {menuPath, archivePath};
        }
        uncachedMenu(sourceArchive, sourceMenu.size());
        const auto privateArchive = gameDirectory.parent_path() / L"build_native/run/native-menu" /
            hash(sourceArchive) / L"GameContext_Create.pc.xdf";
        store(privateArchive, sourceArchive);
        std::fputs("[Menu] Preserving this dump's startup assets; PC menu reads use the loose override.\n", stderr);
        return {menuPath, privateArchive};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[Menu] Using original menus: %s.\n", error.what());
        return {};
    }
}
}
