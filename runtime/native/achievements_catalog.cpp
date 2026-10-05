#include "achievements_catalog.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string_view>

namespace DarkRecomp::Native::Achievements {
namespace {
constexpr uint32_t xdbfMagic = 0x58444246, xachMagic = 0x58414348, xstrMagic = 0x58535452;
constexpr size_t xdbfHeaderBytes = 24, resourceEntryBytes = 18, achievementEntryBytes = 36;
constexpr size_t maximumAchievements = 4096;
using Bytes = std::span<const uint8_t>;

uint16_t word16(Bytes bytes, size_t offset) {
    return uint16_t(uint16_t(bytes[offset]) << 8 | bytes[offset + 1]);
}
uint32_t word32(Bytes bytes, size_t offset) {
    return uint32_t(word16(bytes, offset)) << 16 | word16(bytes, offset + 2);
}
uint64_t word64(Bytes bytes, size_t offset) {
    return uint64_t(word32(bytes, offset)) << 32 | word32(bytes, offset + 4);
}
bool contains(Bytes bytes, size_t offset, size_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
}
// The SPA Size field excludes the four-byte signature, including for XACH/XSTR.
bool metadataHeader(Bytes bytes, uint32_t magic) {
    return bytes.size() >= 14 && word32(bytes, 0) == magic && word32(bytes, 4) == 1 &&
           uint64_t(word32(bytes, 8)) + 4 == bytes.size();
}

// Keep valid UTF-8 from the catalog, replacing invalid sequences so UI consumers
// never receive malformed text. Several approved localized dumps contain them.
uint32_t cp1251(uint8_t character) {
    static constexpr std::array<uint16_t, 64> extended{
        0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021,
        0x20ac, 0x2030, 0x0409, 0x2039, 0x040a, 0x040c, 0x040b, 0x040f,
        0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0xfffd, 0x2122, 0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f,
        0x00a0, 0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6, 0x00a7,
        0x0401, 0x00a9, 0x0404, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x0407,
        0x00b0, 0x00b1, 0x0406, 0x0456, 0x0491, 0x00b5, 0x00b6, 0x00b7,
        0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457};
    return character < 0x80 ? character : character < 0xc0 ? extended[character - 0x80] :
           0x0410 + character - 0xc0;
}
void appendUnicode(std::string& result, uint32_t character) {
    if (character < 0x80) result += char(character);
    else if (character < 0x800) {
        result += char(0xc0 | character >> 6);
        result += char(0x80 | (character & 0x3f));
    } else {
        result += char(0xe0 | character >> 12);
        result += char(0x80 | ((character >> 6) & 0x3f));
        result += char(0x80 | (character & 0x3f));
    }
}
std::string cleanText(Bytes bytes, bool russianEnglishSlot) {
    std::string result;
    result.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        const uint8_t first = bytes[i];
        size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                        first >= 0xe0 && first <= 0xef ? 3 :
                        first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = length && contains(bytes, i, length);
        for (size_t j = 1; valid && j < length; ++j)
            valid = (bytes[i + j] & 0xc0) == 0x80;
        if (valid && length >= 3)
            valid = !(first == 0xe0 && bytes[i + 1] < 0xa0) &&
                    !(first == 0xed && bytes[i + 1] >= 0xa0) &&
                    !(first == 0xf0 && bytes[i + 1] < 0x90) &&
                    !(first == 0xf4 && bytes[i + 1] >= 0x90);
        if (!valid) {
            result += "\xef\xbf\xbd";
            ++i;
        } else {
            // Embedded NULs and controls cannot be rendered as achievement text.
            if (length == 1 && first < 0x20 && first != '\n' && first != '\r' && first != '\t')
                result += ' ';
            else if (russianEnglishSlot && length == 2 && first <= 0xc3)
                // The approved Russian ZIP encodes its CP1251 game glyph bytes
                // as U+0080..U+00FF in the English SPA slot. The loader supplies
                // this hint from the verified revision; other languages retain
                // their authored Unicode, and ASCII remains identical.
                appendUnicode(result, cp1251(uint8_t((first & 0x1f) << 6 | (bytes[i + 1] & 0x3f))));
            else
                result.append(reinterpret_cast<const char*>(bytes.data() + i), length);
            i += length;
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '\r' ||
                               result.back() == '\n' || result.back() == '\t'))
        result.pop_back();
    return result;
}

using StringTable = std::map<uint16_t, std::string>;
bool readStrings(Bytes bytes, StringTable& table, bool russianEnglishSlot = false) {
    if (!metadataHeader(bytes, xstrMagic)) return false;
    const size_t count = word16(bytes, 12);
    if (count > (bytes.size() - 14) / 4) return false;
    size_t position = 14;
    for (size_t i = 0; i < count; ++i) {
        if (!contains(bytes, position, 4)) return false;
        const auto id = word16(bytes, position), length = word16(bytes, position + 2);
        position += 4;
        if (!contains(bytes, position, length) || table.contains(id)) return false;
        table.emplace(id, cleanText(bytes.subspan(position, length), russianEnglishSlot));
        position += length;
    }
    return position == bytes.size();
}
}

std::vector<Entry> parseAchievementCatalog(Bytes bytes, uint32_t consoleLanguage, bool russianEnglishSlot) {
    if (bytes.size() < xdbfHeaderBytes || word32(bytes, 0) != xdbfMagic ||
        word32(bytes, 4) != 0x10000) return {};
    const size_t tableCapacity = word32(bytes, 8), entryCount = word32(bytes, 12),
                 freeCapacity = word32(bytes, 16), freeCount = word32(bytes, 20);
    if (entryCount > tableCapacity || freeCount > freeCapacity ||
        tableCapacity > (bytes.size() - xdbfHeaderBytes) / resourceEntryBytes) return {};
    const size_t freeBase = xdbfHeaderBytes + tableCapacity * resourceEntryBytes;
    if (freeCapacity > (bytes.size() - freeBase) / 8) return {};
    const size_t dataBase = freeBase + freeCapacity * 8;
    std::map<std::pair<uint16_t, uint64_t>, Bytes> resources;
    for (size_t i = 0; i < entryCount; ++i) {
        const size_t position = xdbfHeaderBytes + i * resourceEntryBytes;
        const size_t offset = word32(bytes, position + 10), length = word32(bytes, position + 14);
        if (!contains(bytes.subspan(dataBase), offset, length)) return {};
        const auto key = std::make_pair(word16(bytes, position), word64(bytes, position + 2));
        if (!resources.emplace(key, bytes.subspan(dataBase + offset, length)).second) return {};
    }
    const auto achievements = resources.find({1, xachMagic});
    if (achievements == resources.end() || !metadataHeader(achievements->second, xachMagic)) return {};
    const auto records = achievements->second;
    const size_t count = word16(records, 12);
    if (!count || count > maximumAchievements || (records.size() - 14) / achievementEntryBytes != count ||
        (records.size() - 14) % achievementEntryBytes) return {};

    StringTable english, selected;
    if (auto table = resources.find({3, 1}); table != resources.end() &&
        !readStrings(table->second, english, russianEnglishSlot))
        return {};
    if (consoleLanguage != 1) {
        if (auto table = resources.find({3, consoleLanguage}); table != resources.end() &&
            !readStrings(table->second, selected)) return {};
    }
    if (english.empty() && selected.empty()) return {};
    auto string = [&](uint16_t id) -> std::string {
        if (auto found = selected.find(id); found != selected.end() && !found->second.empty()) return found->second;
        if (auto found = english.find(id); found != english.end()) return found->second;
        return {};
    };
    std::vector<Entry> result;
    result.reserve(count);
    std::set<uint32_t> ids;
    for (size_t i = 0; i < count; ++i) {
        const size_t position = 14 + i * achievementEntryBytes;
        Entry entry{};
        entry.id = word16(records, position);
        if (!ids.insert(entry.id).second) return {};
        entry.name = string(word16(records, position + 2));
        if (entry.name.empty()) entry.name = "Achievement " + std::to_string(entry.id);
        entry.description = string(word16(records, position + 4));
        const auto lockedId = word16(records, position + 6);
        entry.lockedDescription = lockedId == 0xffff ? "Secret achievement" : string(lockedId);
        entry.imageId = word32(records, position + 8);
        entry.gamerscore = word16(records, position + 12);
        entry.flags = word32(records, position + 16);
        result.push_back(std::move(entry));
    }
    return result;
}
}
