#include "runtime/native/xex_image.h"
#include "ppc_image_metadata.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
using namespace DarkRecomp::Native;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::string sha256(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 unavailable");
    std::array<uint8_t, 32> hash{};
    const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), hash.data(), ULONG(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    check(status >= 0, "SHA256 failed");
    std::string result;
    for (auto byte : hash) { char hex[3]; sprintf_s(hex, "%02x", byte); result += hex; }
    return result;
}
static void word(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.at(offset + i) = uint8_t(value >> (24 - i * 8));
}
static void rejects(const std::vector<uint8_t>& bytes) {
    try { decodeXex(bytes); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Malformed XEX was accepted");
}
static std::vector<uint8_t> basicFixture() {
    std::vector<uint8_t> bytes(0x200 + 12);
    word(bytes, 0, 0x58455832); word(bytes, 8, 0x200); word(bytes, 16, 0x80); word(bytes, 20, 1);
    word(bytes, 24, 0x3ff); word(bytes, 28, 0x30);
    word(bytes, 0x84, 32); word(bytes, 0x190, 0x82000000);
    word(bytes, 0x30, 24); word(bytes, 0x34, 1);
    word(bytes, 0x38, 8); word(bytes, 0x3c, 8);
    word(bytes, 0x40, 4); word(bytes, 0x44, 0);
    bytes[0x200] = 'M'; bytes[0x201] = 'Z'; bytes[0x208] = 0x12;
    return bytes;
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Supply original default.xex");
        std::ifstream file(argv[1], std::ios::binary);
        check(bool(file), "Cannot open original default.xex");
        const std::vector<uint8_t> original((std::istreambuf_iterator<char>(file)), {});
        check(sha256(original) == kGameXexSha256, "Original revision hash mismatch");
        const auto decoded = decodeXex(original);
        check(sha256(decoded.image) == kGameImageSha256, "AES decoder changed original memory-image bytes");
        check(decoded.header.size() == 0x3000 && decoded.image.size() == 11599872, "Unexpected supported image sizes");
        auto fixture = basicFixture();
        const auto basic = decodeXex(fixture);
        check(basic.image.size() == 32 && basic.image[0] == 'M' && basic.image[16] == 0x12,
              "Basic blocks were not placed at their original offsets");
        for (size_t i = 8; i < 16; ++i) check(basic.image[i] == 0, "Block zero fill changed");
        for (size_t i = 20; i < 32; ++i) check(basic.image[i] == 0, "Final image zero fill changed");
        rejects({});
        for (const auto [offset, value] : std::array<std::pair<size_t, uint32_t>, 8>{{
                {0, 0}, {8, 0xffffffff}, {16, 0x1ff}, {20, 0xffffffff},
                {28, 0x1ff}, {0x30, 23}, {0x34, 2}, {0x38, 0xffffffff}}}) {
            auto invalid = fixture; word(invalid, offset, value); rejects(invalid);
        }
        auto shortPayload = fixture; shortPayload.resize(0x204); rejects(shortPayload);
        auto encrypted = fixture; word(encrypted, 0x34, 0x10001); rejects(encrypted);
        auto plain = fixture;
        plain.resize(0x200 + 32); word(plain, 0x30, 8); word(plain, 0x34, 0);
        check(decodeXex(plain).image[0] == 'M', "Uncompressed image decode failed");
        puts("Original AES image matches AOT SHA256; block placement, zero fill and malformed XEX checks passed.");
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
