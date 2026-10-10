#include "runtime/native/native_menu_assets.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DarkRecomp::Native;
using Bytes = std::vector<uint8_t>;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::string hash(std::span<const uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 unavailable");
    std::array<uint8_t, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
        ULONG(bytes.size()), digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    check(status >= 0, "SHA256 failed");
    std::string result;
    for (auto byte : digest) { char hex[3]{}; sprintf_s(hex, "%02x", byte); result += hex; }
    return result;
}
static Bytes bytes(std::string_view text) { return Bytes(text.begin(), text.end()); }
static void append(Bytes& data, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.push_back(uint8_t(value >> (8 * i)));
}
static void word(Bytes& data, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(at + i) = uint8_t(value >> (8 * i));
}
static void save(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(bool(output.write(reinterpret_cast<const char*>(data.data()), data.size())), "cannot write menu fixture");
}
static Bytes read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(bool(input), "cannot read menu fixture");
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
static Bytes storedZlib(const Bytes& payload) {
    check(payload.size() < 65536, "fixture zlib block too large");
    const uint16_t size = uint16_t(payload.size()), inverse = uint16_t(~size);
    Bytes result{0x78, 0x01, 0x01, uint8_t(size), uint8_t(size >> 8), uint8_t(inverse), uint8_t(inverse >> 8)};
    result.insert(result.end(), payload.begin(), payload.end());
    uint32_t a = 1, b = 0;
    for (const auto value : payload) { a = (a + value) % 65521; b = (b + a) % 65521; }
    const uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) result.push_back(uint8_t(adler >> (8 * i)));
    return result;
}
struct ArchiveFixture {
    Bytes data;
    size_t files = 0, blocks = 0, stream = 0;
};
static ArchiveFixture archive(const Bytes& menu, const Bytes& localized,
                              std::string_view menuName = "gui\\cubewnd.xcr") {
    ArchiveFixture result;
    auto& data = result.data;
    Bytes names = bytes(menuName); names.push_back(0);
    const size_t textName = names.size();
    const auto tail = bytes("registry\\stringtable_eng.xcr");
    names.insert(names.end(), tail.begin(), tail.end()); names.push_back(0);
    append(data, 0x101); append(data, uint32_t(names.size()));
    data.insert(data.end(), names.begin(), names.end());
    append(data, 2); result.files = data.size();
    for (const uint32_t field : {0u, 0u, 0u, uint32_t(menu.size()), 0x01234567u, 0x89abcdefu}) append(data, field);
    for (const uint32_t field : {uint32_t(textName), 1u, 1u, uint32_t(localized.size()), 0xfedcba98u, 0x76543210u}) append(data, field);
    append(data, 2); result.blocks = data.size();
    for (const uint32_t field : {0xffffffffu, 0u, uint32_t(menu.size()), 0u, 0u}) append(data, field);
    for (const uint32_t field : {0xffffffffu, 1u, uint32_t(localized.size()), 0u, uint32_t(menu.size())}) append(data, field);
    result.stream = data.size();
    Bytes payload = menu; payload.insert(payload.end(), localized.begin(), localized.end());
    const auto compressed = storedZlib(payload);
    data.insert(data.end(), compressed.begin(), compressed.end());
    return result;
}
struct Fixture {
    std::filesystem::path root, game, bundle;
    std::vector<std::filesystem::path> privateFiles;
    Fixture() {
        root = std::filesystem::temp_directory_path() /
            (L"DarkRecomp-menu-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        check(!std::filesystem::exists(root), "menu fixture root already exists");
        game = root / L"game"; bundle = root / L"bundle";
        std::filesystem::create_directories(game / L"Content/Gui");
        std::filesystem::create_directories(game / L"Content/Xdf");
        std::filesystem::create_directory(bundle);
    }
    void retain(const NativeMenuAssets& assets) {
        if (!assets.archive.empty() && assets.archive.parent_path() != bundle &&
            std::find(privateFiles.begin(), privateFiles.end(), assets.archive) == privateFiles.end())
            privateFiles.push_back(assets.archive);
    }
    ~Fixture() {
        std::error_code error;
        for (const auto& path : privateFiles) {
            std::filesystem::remove(path, error);
            std::filesystem::remove(path.parent_path(), error);
        }
        for (const auto* name : {L"CubeWnd.pc.xcr", L"CubeWnd.pc.ru.xcr", L"GameContext_Create.pc.xdf", L"CubeWnd.pc.xcr.source.sha256"})
            std::filesystem::remove(bundle / name, error);
        std::filesystem::remove(game / L"Content/Gui/CubeWnd.xcr", error);
        std::filesystem::remove(game / L"Content/Xdf/GameContext_Create.XDF", error);
        for (const auto& path : {root / L"build_native/run/native-menu", root / L"build_native/run",
             root / L"build_native", game / L"Content/Gui", game / L"Content/Xdf", game / L"Content", game, bundle, root})
            std::filesystem::remove(path, error);
    }
};

int main() {
    try {
        Fixture fixture;
        const auto sourceMenu = bytes("original stock menu source");
        const auto baseline = archive(sourceMenu, bytes("original strings and fonts"));
        const auto localized = archive(sourceMenu, Bytes{0xc0, 0xe1, 0xf0, 0xa8, 0xb8, 0xde, 0xfe, 0xff});
        const auto menuPath = fixture.game / L"Content/Gui/CubeWnd.xcr";
        const auto archivePath = fixture.game / L"Content/Xdf/GameContext_Create.XDF";
        const auto metadataPath = fixture.bundle / L"CubeWnd.pc.xcr.source.sha256";
        const auto metadata = bytes(hash(sourceMenu) + "\n" + hash(baseline.data) + "\n");
        save(menuPath, sourceMenu); save(archivePath, baseline.data);
        save(fixture.bundle / L"CubeWnd.pc.xcr", bytes("bundled PC menu"));
        save(fixture.bundle / L"GameContext_Create.pc.xdf", bytes("bundled PC menu cache"));
        save(metadataPath, metadata);
        auto prepared = prepareNativeMenuAssets(fixture.game, fixture.bundle);
        check(prepared.menu == fixture.bundle / L"CubeWnd.pc.xcr" &&
              prepared.archive == fixture.bundle / L"GameContext_Create.pc.xdf", "stock dump stopped using bundled menu cache");
        check(prepareNativeMenuAssets(fixture.game, fixture.bundle, true).menu.empty(),
              "missing Russian menu silently selected an English override");
        save(fixture.bundle / L"CubeWnd.pc.ru.xcr", bytes("Russian PC menu"));
        auto russian = prepareNativeMenuAssets(fixture.game, fixture.bundle, true); fixture.retain(russian);
        check(russian.menu == fixture.bundle / L"CubeWnd.pc.ru.xcr" && russian.archive != prepared.archive,
              "Russian menu reused the English prefetched menu");
        check(read(archivePath) == baseline.data, "Russian menu selection changed the source startup assets");

        save(archivePath, localized.data);
        prepared = prepareNativeMenuAssets(fixture.game, fixture.bundle); fixture.retain(prepared);
        check(!prepared.menu.empty() && !prepared.archive.empty() && prepared.archive.parent_path() != fixture.bundle,
              "localized dump did not get a private startup cache");
        const auto privateArchive = read(prepared.archive);
        auto expected = localized.data;
        const auto sentinel = bytes("gui\\pcmenu_.xcr");
        std::copy(sentinel.begin(), sentinel.end(), expected.begin() + 8);
        check(privateArchive == expected, "localized strings/fonts, file metadata, read links or compressed payload changed");
        check(read(archivePath) == localized.data && read(menuPath) == sourceMenu,
              "private menu preparation changed the user's original assets");
        check(prepareNativeMenuAssets(fixture.game, fixture.bundle).archive == prepared.archive,
              "identical localized cache was not reused");
        auto damagedPrivate = privateArchive; damagedPrivate.back() ^= 1;
        save(prepared.archive, damagedPrivate);
        const auto conflict = prepareNativeMenuAssets(fixture.game, fixture.bundle);
        check(conflict.menu.empty() && conflict.archive.empty() && read(prepared.archive) == damagedPrivate,
              "existing conflicting cache was overwritten or used");
        save(prepared.archive, privateArchive);

        auto upper = archive(sourceMenu, bytes("localized content"), "GUI\\CUBEWND.XCR");
        save(archivePath, upper.data);
        prepared = prepareNativeMenuAssets(fixture.game, fixture.bundle); fixture.retain(prepared);
        check(!prepared.archive.empty(), "case-insensitive cached menu filename was rejected");

        save(menuPath, bytes("customized menu source"));
        const auto custom = prepareNativeMenuAssets(fixture.game, fixture.bundle);
        check(custom.menu.empty() && custom.archive.empty(), "customized user menu was replaced with the bundled menu");
        save(menuPath, sourceMenu);
        for (unsigned mutation = 0; mutation < 11; ++mutation) {
            auto bad = localized.data;
            switch (mutation) {
            case 0: word(bad, 0, 0x102); break;
            case 1: word(bad, 4, uint32_t(bad.size())); break;
            case 2: word(bad, localized.files, 0xffffffff); break;
            case 3: word(bad, localized.files + 4, 3); break;
            case 4: word(bad, localized.blocks, 0); break;
            case 5: word(bad, localized.blocks + 4, 1); break;
            case 6: word(bad, localized.blocks + 12, uint32_t(sourceMenu.size())); break;
            case 7: word(bad, localized.blocks + 16, 1); break;
            case 8: bad[localized.stream] = 0; break;
            case 9: word(bad, localized.files + 12, uint32_t(sourceMenu.size() + 1)); break;
            case 10: bad.resize(localized.stream - 1); break;
            }
            save(archivePath, bad);
            const auto rejected = prepareNativeMenuAssets(fixture.game, fixture.bundle);
            check(rejected.menu.empty() && rejected.archive.empty(), "malformed startup cache was accepted");
            check(read(archivePath) == bad, "rejected startup cache was changed");
        }
        save(archivePath, baseline.data);
        save(metadataPath, bytes(hash(sourceMenu) + "\nnot-a-hash\n"));
        check(prepareNativeMenuAssets(fixture.game, fixture.bundle).menu.empty(), "malformed source metadata was accepted");
        std::filesystem::remove(metadataPath);
        check(prepareNativeMenuAssets(fixture.game, fixture.bundle).menu.empty(), "missing source metadata was accepted");
        std::puts("Native menu assets: stock cache reuse, localized payload preservation, source guards and malformed cache rejection passed.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
