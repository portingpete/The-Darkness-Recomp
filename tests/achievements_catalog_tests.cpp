#include "runtime/native/achievements_catalog.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>

using namespace DarkRecomp::Native::Achievements;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void word16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes.at(offset) = uint8_t(value >> 8); bytes.at(offset + 1) = uint8_t(value);
}
void word32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    word16(bytes, offset, uint16_t(value >> 16)); word16(bytes, offset + 2, uint16_t(value));
}
std::vector<uint8_t> strings(std::initializer_list<std::pair<uint16_t, std::string_view>> entries) {
    std::vector<uint8_t> bytes(14);
    word32(bytes, 0, 0x58535452); word32(bytes, 4, 1); word16(bytes, 12, uint16_t(entries.size()));
    for (auto [id, text] : entries) {
        const size_t position = bytes.size(); bytes.resize(position + 4 + text.size());
        word16(bytes, position, id); word16(bytes, position + 2, uint16_t(text.size()));
        std::copy(text.begin(), text.end(), bytes.begin() + position + 4);
    }
    word32(bytes, 8, uint32_t(bytes.size() - 4));
    return bytes;
}
struct Fixture {
    std::vector<uint8_t> bytes;
    size_t recordBase, englishBase, germanBase;
};
Fixture fixture(std::string_view firstName = "First award\r\n",
                std::string_view firstGermanName = "Erste Auszeichnung") {
    std::vector<uint8_t> records(14 + 2 * 36);
    word32(records, 0, 0x58414348); word32(records, 4, 1);
    word32(records, 8, uint32_t(records.size() - 4)); word16(records, 12, 2);
    for (size_t i = 0; i < 2; ++i) {
        const size_t position = 14 + i * 36;
        word16(records, position, uint16_t(i)); word16(records, position + 2, uint16_t(10 + i));
        word16(records, position + 4, 20); word16(records, position + 6, i ? 0xffff : 21);
        word32(records, position + 8, uint32_t(100 + i)); word16(records, position + 12, uint16_t(5 + i * 5));
        word32(records, position + 16, uint32_t(4 + i));
    }
    const auto english = strings({{10, firstName}, {11, "Hidden award"},
                                 {20, "Completed requirement\r\n"}, {21, "Meet requirement"}});
    const auto german = strings({{10, firstGermanName}, {20, "Geschafft"}});
    constexpr size_t dataBase = 24 + 5 * 18 + 3 * 8;
    Fixture fixture{{}, dataBase, dataBase + records.size(), dataBase + records.size() + english.size()};
    fixture.bytes.resize(dataBase + records.size() + english.size() + german.size());
    auto& bytes = fixture.bytes;
    word32(bytes, 0, 0x58444246); word32(bytes, 4, 0x10000);
    // Table capacities exceed occupied counts: content must follow capacities.
    word32(bytes, 8, 5); word32(bytes, 12, 3); word32(bytes, 16, 3); word32(bytes, 20, 1);
    const std::array<std::vector<uint8_t>, 3> resources{records, english, german};
    const std::array<uint32_t, 3> ids{0x58414348, 1, 3};
    size_t offset = 0;
    for (size_t i = 0; i < resources.size(); ++i) {
        const size_t position = 24 + i * 18;
        word16(bytes, position, i ? 3 : 1); word32(bytes, position + 6, ids[i]);
        word32(bytes, position + 10, uint32_t(offset)); word32(bytes, position + 14, uint32_t(resources[i].size()));
        std::copy(resources[i].begin(), resources[i].end(), bytes.begin() + dataBase + offset);
        offset += resources[i].size();
    }
    return fixture;
}
void syntheticTests() {
    const auto source = fixture();
    const auto english = parseAchievementCatalog(source.bytes, 1);
    check(english.size() == 2, "Synthetic catalog count");
    check(english[0].id == 0 && english[1].id == 1, "Zero-based IDs changed");
    check(english[0].gamerscore == 5 && english[1].gamerscore == 10 &&
          english[0].imageId == 100 && english[1].flags == 5, "Achievement numeric fields changed");
    check(english[0].name == "First award" && english[0].description == "Completed requirement" &&
          english[0].lockedDescription == "Meet requirement", "UTF-8 text or trailing CRLF changed");
    check(english[1].lockedDescription == "Secret achievement" && english[1].unlockedAt == 0,
          "Secret description exposed or achievements initialized unlocked");
    const auto german = parseAchievementCatalog(source.bytes, 3);
    check(german.size() == 2 && german[0].name == "Erste Auszeichnung" && german[0].description == "Geschafft" &&
          german[1].name == "Hidden award" && german[0].lockedDescription == "Meet requirement",
          "Selected language or per-string English fallback changed");
    check(parseAchievementCatalog(source.bytes, 12)[0].name == english[0].name, "Missing language did not fall back");
    for (size_t size = 0; size < source.bytes.size(); ++size)
        check(parseAchievementCatalog(std::span(source.bytes).first(size), 1).empty(), "Truncated resource accepted");
    for (const auto [offset, value] : std::array<std::pair<size_t, uint32_t>, 14>{{
             {0, 0}, {4, 0}, {8, 0xffffffff}, {12, 6}, {16, 0xffffffff}, {20, 4},
             {24 + 10, 0xffffffff}, {24 + 14, 0xffffffff},
             {source.recordBase, 0}, {source.recordBase + 4, 2}, {source.recordBase + 8, 1},
             {source.englishBase, 0}, {source.englishBase + 4, 2}, {source.englishBase + 8, 1}}}) {
        auto invalid = source.bytes; word32(invalid, offset, value);
        check(parseAchievementCatalog(invalid, 1).empty(), "Malformed table/header accepted");
    }
    auto invalid = source.bytes;
    word16(invalid, source.recordBase + 12, 3);
    check(parseAchievementCatalog(invalid, 1).empty(), "Achievement count overrun accepted");
    invalid = source.bytes; word16(invalid, source.recordBase + 14 + 36, 0);
    check(parseAchievementCatalog(invalid, 1).empty(), "Duplicate achievement IDs accepted");
    invalid = source.bytes; word16(invalid, source.englishBase + 16, 0xffff);
    check(parseAchievementCatalog(invalid, 1).empty(), "String overrun accepted");
    invalid = source.bytes; word16(invalid, source.englishBase + 12, 0xffff);
    check(parseAchievementCatalog(invalid, 1).empty(), "String count overrun accepted");
    invalid = source.bytes; word32(invalid, 24 + 2 * 18 + 6, 1);
    check(parseAchievementCatalog(invalid, 1).empty(), "Duplicate resource accepted");
    invalid = source.bytes;
    std::fill(invalid.begin() + 24 + 3 * 18, invalid.begin() + 24 + 4 * 18, 0xff);
    check(parseAchievementCatalog(invalid, 1).size() == 2, "Unoccupied table slot interpreted as a resource");
    // Bounds-check claimed resources even when achievement loading never needs
    // them, rather than permitting an out-of-range image/metadata pointer.
    invalid = source.bytes; word32(invalid, 12, 4);
    word16(invalid, 24 + 3 * 18, 2); word32(invalid, 24 + 3 * 18 + 6, 999);
    word32(invalid, 24 + 3 * 18 + 10, 0xffffffff); word32(invalid, 24 + 3 * 18 + 14, 1);
    check(parseAchievementCatalog(invalid, 1).empty(), "Unreferenced resource pointer escaped bounds checking");
    auto unicode = source.bytes;
    unicode[source.englishBase + 18] = 0xff;
    check(parseAchievementCatalog(unicode, 1)[0].name.starts_with("\xef\xbf\xbd"), "Invalid UTF-8 not replaced");
    unicode = source.bytes;
    unicode[source.englishBase + 18] = 0xc3; unicode[source.englishBase + 19] = 0xa9;
    check(parseAchievementCatalog(unicode, 1)[0].name.starts_with("\xc3\xa9"), "Valid UTF-8 not preserved");
    const auto legacy = fixture("\xc3\x80\xc3\x81\xc3\x82\xc2\xa8\xc2\xb8\r\n", "\xc3\xa9");
    const auto nativeRussian = parseAchievementCatalog(legacy.bytes, 1, true);
    check(nativeRussian[0].name == "\xd0\x90\xd0\x91\xd0\x92\xd0\x81\xd1\x91",
          "Explicit Russian slot hint did not decode CP1251 glyphs");
    check(parseAchievementCatalog(legacy.bytes, 1)[0].name == "\xc3\x80\xc3\x81\xc3\x82\xc2\xa8\xc2\xb8",
          "Retail catalog was heuristically recoded");
    const auto localized = parseAchievementCatalog(legacy.bytes, 3, true);
    check(localized[0].name == "\xc3\xa9" && localized[1].name == "Hidden award",
          "Russian hint changed other language Unicode or English fallback");
}
// Optional local-only XDBF files extracted from the user's supported executable.
// No copyrighted catalog fixture or display text is checked into this test.
void retailTest(const char* path) {
    std::ifstream file(path, std::ios::binary);
    check(bool(file), "Cannot read supplied local XDBF fixture");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    for (const auto language : {1u, 3u, 4u, 5u, 6u, 12u}) {
        const auto entries = parseAchievementCatalog(bytes, language);
        check(entries.size() == 50, "Retail catalog did not contain 50 achievements");
        uint32_t score = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            check(entry.id == i && !entry.name.empty() && !entry.description.empty() && !entry.lockedDescription.empty(),
                  "Retail catalog lost IDs, names or descriptions");
            check(entry.name.back() != '\n' && entry.name.back() != '\r', "Retail name includes trailing CRLF");
            score += entry.gamerscore;
        }
        check(score == 1000, "Retail gamerscore total changed");
        const auto russian = parseAchievementCatalog(bytes, language, true);
        check(russian.size() == entries.size(), "Russian hint lost achievements");
        if (language == 3 || language == 4 || language == 5 || language == 6) {
            for (size_t i = 0; i < entries.size(); ++i)
                check(russian[i].name == entries[i].name && russian[i].description == entries[i].description &&
                      russian[i].lockedDescription == entries[i].lockedDescription,
                      "Russian hint changed a non-English retail language table");
        }
        if (language == 1) {
            if (entries[0].name.starts_with("\xc3\x81"))
                check(russian[0].name.starts_with("\xd0\x91") && russian[49].name.starts_with("\xd0\x9b"),
                      "Approved Russian catalog failed to produce meaningful Cyrillic");
            else check(russian[0].name == entries[0].name, "Russian hint changed an ASCII catalog");
        }
    }
    printf("Validated local retail catalog: %s\n", path);
}
}
int main(int argc, char** argv) {
    try {
        syntheticTests();
        for (int i = 1; i < argc; ++i) retailTest(argv[i]);
        puts("Achievement catalog parsing, language fallback, secret descriptions and malformed bounds checks passed.");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
