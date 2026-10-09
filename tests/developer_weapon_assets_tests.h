#pragma once
#include "runtime/native/developer_weapon_assets.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace DeveloperWeaponAssetFixture {
using Bytes = std::vector<uint8_t>;
using DarkRecomp::Native::DeveloperWeaponAssetDetail::File;
using DarkRecomp::Native::DeveloperWeaponAssetDetail::mergeFile;
using DarkRecomp::Native::DeveloperWeaponAssetDetail::mergeStreamedMip;
using DarkRecomp::Native::DeveloperWeaponAssetDetail::parseArchive;
using DarkRecomp::Native::DeveloperWeaponAssetDetail::validateStreamedMipTable;
static void append(Bytes& bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}
static void word(Bytes& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = uint8_t(value >> (i * 8));
}
struct Archive {
    Bytes bytes, payload;
    size_t files, blocks, stream;
    Archive() {
        const std::string name = "MODELS/WEAPONS/GUN_DARKNESS_01.XMD";
        append(bytes, 0x101); append(bytes, uint32_t(name.size() + 1));
        bytes.insert(bytes.end(), name.begin(), name.end()); bytes.push_back(0);
        append(bytes, 1); files = bytes.size();
        for (const auto value : {0u, 0u, 2u, 32u, 0x01234567u, 0x89abcdefu}) append(bytes, value);
        append(bytes, 3); blocks = bytes.size();
        for (const auto value : {1u, 0u, 4u, 0u, 0u}) append(bytes, value);
        for (const auto value : {2u, 0u, 4u, 2u, 4u}) append(bytes, value);
        for (const auto value : {0xffffffffu, 0u, 2u, 10u, 8u}) append(bytes, value);
        stream = bytes.size();
        // Pure parser tests supply a verified payload separately. Only the
        // zlib framing is inspected here; real inflation is exercised below.
        bytes.insert(bytes.end(), {0x78, 0x01, 0x03, 0x00, 0x00, 0x00});
        payload = {'A', 'B', 'C', 'D', 'C', 'D', 'E', 'F', 'G', 'H'};
    }
};
template<class Callback> static void rejected(Callback callback, const char* message) {
    bool threw = false;
    try { callback(); } catch (const std::runtime_error&) { threw = true; }
    check(threw, message);
}
static void parserTests() {
    const Archive fixture;
    const auto parsed = parseArchive(fixture.bytes, fixture.payload);
    check(parsed.size() == 1 && parsed[0].name == "models\\weapons\\gun_darkness_01.xmd" && parsed[0].size == 32,
          "Developer cache parser lost normalized filenames or nominal sizes");
    check(parsed[0].segments.size() == 2 && parsed[0].segments[0].offset == 0 &&
          parsed[0].segments[0].bytes == Bytes({'A', 'B', 'C', 'D', 'E', 'F'}) &&
          parsed[0].segments[1].offset == 10 && parsed[0].segments[1].bytes == Bytes({'G', 'H'}),
          "Developer cache parser failed to merge matching overlap without filling holes");
    CachedWeaponFile ranges; ranges.size = 32; ranges.ranges = {{0, 6}, {10, 2}};
    check(ranges.covers(0, 6) && ranges.covers(2, 4) && ranges.covers(10, 2) && ranges.covers(32, 0),
          "Covered developer cache reads were rejected");
    check(!ranges.covers(5, 2) && !ranges.covers(6, 1) && !ranges.covers(9, 2) &&
          !ranges.covers(12, 1) && !ranges.covers(32, 1) && !ranges.covers(UINT64_MAX, 1) &&
          !ranges.covers(33, 0), "Developer cache accepted a hole, boundary crossing or overflowing read");
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
        auto bytes = fixture.bytes;
        switch (mutation) {
        case 0: word(bytes, 0, 0x102); break;
        case 1: word(bytes, 4, uint32_t(bytes.size())); break;
        case 2: word(bytes, fixture.files, 0xffffffff); break;
        case 3: word(bytes, fixture.files + 8, 1); break;
        case 4: word(bytes, fixture.blocks, 0); break;
        case 5: word(bytes, fixture.blocks + 4, 1); break;
        case 6: word(bytes, fixture.blocks + 12, 31); break;
        case 7: word(bytes, fixture.blocks + 16, 1); break;
        case 8: bytes[fixture.stream] = 0; break;
        case 9: word(bytes, fixture.files + 4, 0xffffffff); break;
        case 10: bytes.resize(fixture.stream - 1); break;
        case 11: bytes[8] = '\\'; break;
        }
        rejected([&] { parseArchive(bytes, fixture.payload); }, "Malformed developer weapon cache was accepted");
    }
    auto shorter = fixture.payload; shorter.pop_back();
    rejected([&] { parseArchive(fixture.bytes, shorter); }, "Developer cache accepted truncated decompressed data");
    auto conflicting = fixture.payload; conflicting[4] = 'X';
    rejected([&] { parseArchive(fixture.bytes, conflicting); }, "Developer cache accepted conflicting overlapping reads");
    auto merged = parsed[0];
    const File adjacent{merged.name, merged.size, {{6, {'I', 'J', 'K', 'L'}}}};
    mergeFile(merged, adjacent);
    check(merged.segments.size() == 1 && merged.segments[0].bytes.size() == 12,
          "Cross-archive adjacent developer ranges were not coalesced");
    const File bad{merged.name, merged.size, {{5, {'X'}}}};
    rejected([&] { mergeFile(merged, bad); }, "Cross-archive conflicting developer bytes were accepted");
    const File wrongSize{merged.name, 31, {{0, {'A'}}}};
    rejected([&] { mergeFile(merged, wrongSize); }, "Cross-archive developer file sizes were allowed to differ");
}
static bool sameSegments(const File& left, const File& right) {
    if (left.name != right.name || left.size != right.size || left.segments.size() != right.segments.size()) return false;
    for (size_t i = 0; i < left.segments.size(); ++i)
        if (left.segments[i].offset != right.segments[i].offset || left.segments[i].bytes != right.segments[i].bytes)
            return false;
    return true;
}
static void streamedMipTests() {
    Bytes serialized;
    for (const auto value : {0x300u, 0x5000u, 24u, 4u, 4u, 0x800u, 0u}) append(serialized, value);
    // This opaque body deliberately has a distinct first 16 bytes. They are
    // part of the authentic GPU payload and must survive without stripping.
    for (unsigned i = 0; i < 24; ++i) serialized.push_back(uint8_t(0x80 + i));
    auto destination = [&](const Bytes& source) {
        Bytes prefix(36); word(prefix, 0, 16);
        std::copy(source.begin(), source.begin() + 28, prefix.begin() + 8);
        return File{"textures\\alltextures.001.xtc", 112,
            {{0, {'S', 'A', 'F', 'E'}}, {8, prefix}, {68, Bytes(16, 0xAD)}}};
    };
    const auto fixture = destination(serialized);
    for (const auto flags : {0x5000u, 0x11000u}) {
        auto source = serialized; word(source, 4, flags);
        auto merged = destination(source);
        mergeStreamedMip(merged, 8, 84, source);
        check(merged.segments.size() == 2 && merged.segments[1].offset == 8 &&
              merged.segments[1].bytes.size() == 76 &&
              !std::memcmp(merged.segments[1].bytes.data() + 36, source.data() + 28, 24),
              "Matched streamed mip lost its opaque prefix or did not bridge exactly the cached hole");
        CachedWeaponFile coverage; coverage.size = merged.size;
        for (const auto& segment : merged.segments) coverage.ranges.emplace_back(segment.offset, uint32_t(segment.bytes.size()));
        check(coverage.covers(8, 76) && !coverage.covers(4, 1) && !coverage.covers(84, 1),
              "Streamed mip publication filled unrelated sparse holes");
        const auto unchanged = merged;
        mergeStreamedMip(merged, 8, 84, source);
        check(sameSegments(merged, unchanged), "Identical already-cached streamed mip changed the file");
    }
    for (unsigned mutation = 0; mutation < 19; ++mutation) {
        auto file = fixture; auto source = serialized;
        uint32_t start = 8, end = 84;
        switch (mutation) {
        case 0: source.resize(27); break;
        case 1: source.pop_back(); break;
        case 2: source.push_back(0); break;
        case 3: word(source, 0, 0x200); break;
        case 4: word(source, 4, 0x4000); break;
        case 5: word(source, 8, 0); break;
        case 6: word(source, 8, UINT32_MAX); break;
        case 7: word(source, 12, 0); break;
        case 8: word(source, 16, 0); break;
        case 9: word(source, 12, 16385); break;
        case 10: word(source, 20, 0x20000); break;
        case 11: end = 67; break;
        case 12: start = 9; break;
        case 13: file.segments[1].bytes.pop_back(); break;
        case 14: word(file.segments[1].bytes, 0, UINT32_MAX); break;
        case 15: word(file.segments[1].bytes, 0, 8); break;
        case 16: mergeFile(file, File{file.name, file.size, {{44, {0x00}}}}); break;
        case 17: file.size = 63; break;
        case 18: end = 113; break;
        }
        const auto before = file;
        rejected([&] { mergeStreamedMip(file, start, end, source); },
                 "Invalid, mismatched or conflicting streamed mip was accepted");
        check(sameSegments(file, before), "Rejected streamed mip published partial bytes");
    }
    Bytes table;
    for (const auto value : {3u, 10u, 32u, 20u, 96u, 30u, 160u}) append(table, value);
    validateStreamedMipTable(table, 224, 20, 96, 52);
    for (unsigned mutation = 0; mutation < 15; ++mutation) {
        auto input = table;
        uint64_t size = 224;
        uint32_t index = 20, offset = 96, serializedSize = 52;
        switch (mutation) {
        case 0: word(input, 0, 0); break;
        case 1: word(input, 0, 65537); break;
        case 2: input.pop_back(); break;
        case 3: input.push_back(0); break;
        case 4: word(input, 4, 65536); break;
        case 5: word(input, 20, 20); break;
        case 6: word(input, 24, 96); break;
        case 7: word(input, 8, 27); break;
        case 8: word(input, 8, 225); break;
        case 9: word(input, 16, 97); break;
        case 10: index = 21; break;
        case 11: word(input, 24, 147); break;
        case 12: size = 175; break;
        case 13: serializedSize = 27; break;
        case 14: serializedSize = UINT32_MAX; break;
        }
        rejected([&] { validateStreamedMipTable(input, size, index, offset, serializedSize); },
                 "Malformed streaming index, wrong mapping or physical record crossing was accepted");
    }
}
static PPC_FUNC(allocate) {
    const auto size = uint64_t(ctx.r4.u32) * ctx.r5.u32;
    check(size <= 65536, "Developer decompression allocated an archive-sized guest buffer");
    ctx.r3.u64 = size && size <= UINT32_MAX ? memory->allocate(uint32_t(size)) : 0;
}
static PPC_FUNC(free) {
    if (ctx.r4.u32) check(memory->release(ctx.r4.u32), "Developer decompressor freed an unowned allocation");
}
static Bytes read(const std::filesystem::path& path, uint32_t offset, uint32_t length) {
    std::ifstream input(path, std::ios::binary); input.seekg(offset);
    Bytes bytes(length);
    check(bool(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())),
          "Cannot read developer asset source or private cache fixture");
    return bytes;
}
static void materializedTextureTests() {
    // These are the exact nine material and six impact reads that failed in
    // the real buffered loader. Compare the complete payload with the owned
    // physical source, not with sparse-file zeros or another extracted copy.
    struct Read {
        bool textureOne, xtOne;
        uint32_t logical, physical, length, imageStart, imageEnd;
    };
    constexpr std::array<Read, 15> reads{{
        {true, false, 138602804, 97073408, 65552, 138602740, 138690580},
        {true, false, 138690644, 97007828, 65552, 138690580, 138778420},
        {true, false, 138778484, 103762008, 65536, 138778420, 138866116},
        {true, true, 138913888, 3842876, 16400, 138913828, 138936084},
        {true, true, 138891632, 3892160, 16400, 138891572, 138913828},
        {true, true, 138869504, 5091292, 16384, 138869444, 138891572},
        {true, false, 138936756, 97204568, 65552, 138936692, 139024532},
        {true, false, 139024596, 97729208, 65552, 139024532, 139112372},
        {true, false, 139112436, 103696444, 65536, 139112372, 139200068},
        {false, false, 55484196, 569276288, 65552, 55484128, 55572008},
        {false, true, 55414716, 72198016, 16400, 55414656, 55436912},
        {false, true, 55392460, 72214444, 16400, 55392400, 55414656},
        {false, true, 55660744, 97525376, 2064, 55660688, 55663768},
        {false, true, 55572068, 91422184, 8208, 55572008, 55583328},
        {false, true, 52051680, 27869076, 32784, 52051616, 52095752}
    }};
    for (const auto& item : reads) {
        const std::string filename = item.textureOne ? "AllTextures.001.XTC" : "AllTextures.000.XTC";
        const auto original = memory->gameDirectory() / "Content/Textures" / filename;
        const auto prepared = memory->developerWeaponFile(original);
        check(prepared && prepared->covers(item.logical, item.length) &&
              prepared->covers(item.imageStart, item.imageEnd - item.imageStart),
              "Previously missing resident material/effect mip is not fully covered");
        const auto physical = std::filesystem::path(original.wstring() + (item.xtOne ? L".xt1" : L".xt0"));
        check(read(prepared->path, item.logical, item.length) == read(physical, item.physical, item.length) &&
              read(prepared->path, item.logical - 28, 28) == read(physical, item.physical - 28, 28),
              "Materialized resident mip differs from its exact physical metadata or authentic GPU payload");
    }
    const auto texture0 = memory->developerWeaponFile(memory->gameDirectory() / "Content/Textures/AllTextures.000.XTC");
    const auto texture1 = memory->developerWeaponFile(memory->gameDirectory() / "Content/Textures/AllTextures.001.XTC");
    const auto physical = memory->gameDirectory() / "Content/Textures/AllTextures.001.XTC.xt1";
    check(texture1->covers(138936084, 608) &&
          read(texture1->path, 138936132, 272) == read(physical, 5903464, 272),
          "Already-cached animation mip disagrees with the matching physical control");
    check(!texture0->covers(48, 1) && !texture1->covers(48, 1) && texture1->covers(138866116, 92) &&
          !texture1->covers(138866208, 1) && !texture1->covers(138866208, 156),
          "Materialization invented unrelated data or unauthenticated lower animation mips");
}
static void filesystemTests(PPCContext& context) {
    auto* base = memory->base();
    const auto original = memory->gameDirectory() / "Content/Anim/AnimGraphs/Weapons/AG2Weapon_Darkness01.XAH";
    const auto prepared = memory->developerWeaponFile(original);
    check(prepared && prepared->size == 46225 && prepared->ranges ==
          std::vector<std::pair<uint32_t, uint32_t>>{{0, 2114}},
          "Developer graph cache lost its exact cached extent or nominal metadata");
    check(!memory->developerWeaponFile(memory->gameDirectory() / "Content/Anim/Unknown.XAH"),
          "Developer asset overlay exposed an unrelated resource");
    const uint32_t block = memory->allocate(8192);
    check(block != 0, "Cannot allocate developer filesystem test fixture");
    struct Release { uint32_t block; ~Release() { memory->release(block); } } release{block};
    std::memset(base + block, 0, 8192);
    const char* name = "game:\\Content\\Anim\\AnimGraphs\\Weapons\\AG2Weapon_Darkness01.XAH";
    const auto nameLength = std::strlen(name);
    std::memcpy(base + block + 256, name, nameLength + 1);
    PPC_STORE_U32(block, 0xfffffffd); PPC_STORE_U32(block + 4, block + 16); PPC_STORE_U32(block + 8, 0x40);
    PPC_STORE_U32(block + 16, uint32_t(nameLength << 16) | uint32_t(nameLength + 1));
    PPC_STORE_U32(block + 20, block + 256);
    auto call = context;
    call.r3.u64 = block; call.r4.u64 = block + 64;
    __imp__NtQueryFullAttributesFile(call, base);
    check(call.r3.u32 == 0 && PPC_LOAD_U64(block + 64 + 40) == prepared->size,
          "Developer graph full-attribute query did not preserve the nominal file size");
    const auto oldOptions = PPC_LOAD_U32(call.r1.u32 + 84);
    struct RestoreOptions {
        uint8_t* base; uint32_t at, options;
        ~RestoreOptions() { PPC_STORE_U32(at, options); }
    } restoreOptions{base, call.r1.u32 + 84, oldOptions};
    PPC_STORE_U32(call.r1.u32 + 84, 0x60); // Synchronous, non-directory; buffered cached reads.
    call.r3.u64 = block + 160; call.r4.u64 = GENERIC_READ | SYNCHRONIZE;
    call.r5.u64 = block; call.r6.u64 = block + 128; call.r7.u64 = 0;
    call.r8.u64 = 0; call.r9.u64 = FILE_SHARE_READ; call.r10.u64 = 1;
    __imp__NtCreateFile(call, base);
    check(call.r3.u32 == 0, "Developer graph could not be opened through the native filesystem");
    const auto handle = PPC_LOAD_U32(block + 160);
    struct Close {
        PPCContext& call; uint8_t* base; uint32_t handle;
        ~Close() { call.r3.u64 = handle; __imp__NtClose(call, base); }
    } close{call, base, handle};
    check(object(handle) && object(handle)->path == prepared->path && !object(handle)->writable,
          "Developer graph handle was not bound to its private read-only asset");
    auto cachedRead = [&](uint64_t offset, uint32_t length) {
        PPC_STORE_U64(block + 168, offset);
        call.r3.u64 = handle; call.r4.u64 = 0; call.r5.u64 = 0; call.r6.u64 = 0;
        call.r7.u64 = block + 128; call.r8.u64 = block + 2048;
        call.r9.u64 = length; call.r10.u64 = block + 168;
        __imp__NtReadFile(call, base);
        return call.r3.u32;
    };
    const auto expected = read(prepared->path, 0, 2114);
    check(cachedRead(0, 48) == 0 && PPC_LOAD_U32(block + 132) == 48 &&
          !std::memcmp(base + block + 2048, expected.data(), 48),
          "Developer graph header read differed from its cached bytes");
    check(cachedRead(0, 2114) == 0 && PPC_LOAD_U32(block + 132) == 2114 &&
          !std::memcmp(base + block + 2048, expected.data(), expected.size()),
          "Developer graph body read differed from its cached bytes");
    std::memset(base + block + 2048, 0xAD, 64);
    check(cachedRead(2114, 1) == 0xc000003e && PPC_LOAD_U32(block + 128) == 0xc000003e &&
          PPC_LOAD_U32(block + 132) == 0 && base[block + 2048] == 0xAD,
          "Developer graph uncached hole returned invented bytes or changed the read destination");
    check(cachedRead(2110, 8) == 0xc000003e && PPC_LOAD_U32(block + 132) == 0 &&
          base[block + 2048] == 0xAD, "Developer graph boundary-crossing read returned partial invented data");

    // Exercise the original logical CFile read boundary. Loose read-ahead at
    // the native layer spans uncached bytes; logical requests remain exact.
    const auto io = block + 512, descriptor = block + 608, logicalBuffer = block + 4096;
    PPC_STORE_U32(descriptor, handle);
    auto initializeLogical = [&](uint64_t position, uint64_t nominal) {
        std::memset(base + io, 0, 89);
        PPC_STORE_U32(io + 20, descriptor);
        PPC_STORE_U64(io + 32, position); PPC_STORE_U64(io + 40, 7);
        PPC_STORE_U64(io + 48, nominal); PPC_STORE_U8(io + 88, 1);
        std::memset(base + logicalBuffer, 0xAD, 4096);
    };
    auto logicalRead = [&](uint32_t destination, uint32_t length) {
        auto logical = context;
        logical.r3.u64 = io; logical.r4.u64 = destination; logical.r5.u64 = length;
        logical.r31.u64 = 0xCAFEBABE; logical.r30.u64 = 0xABCDEF;
        sub_82209560(logical, base);
        check(logical.r31.u64 == 0xCAFEBABE && logical.r30.u64 == 0xABCDEF && logical.r1.u64 == context.r1.u64,
              "Developer logical read changed caller nonvolatile registers or stack");
    };
    initializeLogical(0, prepared->size);
    logicalRead(logicalBuffer, 2114);
    check(!std::memcmp(base + logicalBuffer, expected.data(), expected.size()) &&
          PPC_LOAD_U64(io + 32) == 2114 && PPC_LOAD_U64(io + 40) == 2121 && PPC_LOAD_U8(io + 88) == 1,
          "Developer logical read lost exact bytes, position, aggregate count or validity");
    initializeLogical(2114, prepared->size);
    logicalRead(logicalBuffer, 1);
    check(base[logicalBuffer] == 0xAD && PPC_LOAD_U64(io + 32) == 2114 &&
          PPC_LOAD_U64(io + 40) == 7 && !PPC_LOAD_U8(io + 88),
          "Developer logical read invented hole bytes or advanced failed counters");
    initializeLogical(0, prepared->size);
    logicalRead(0, 48);
    check(PPC_LOAD_U64(io + 32) == 0 && PPC_LOAD_U64(io + 40) == 7 && !PPC_LOAD_U8(io + 88),
          "Developer logical read accepted an unmapped destination");
    initializeLogical(2110, 2114);
    logicalRead(logicalBuffer, 8);
    check(!std::memcmp(base + logicalBuffer, expected.data() + 2110, 4) && base[logicalBuffer + 4] == 0xAD &&
          PPC_LOAD_U64(io + 32) == 2114 && PPC_LOAD_U64(io + 40) == 11 && !PPC_LOAD_U8(io + 88),
          "Developer logical read changed original EOF clipping semantics");
    initializeLogical(2114, 2114);
    logicalRead(logicalBuffer, 1);
    check(base[logicalBuffer] == 0xAD && PPC_LOAD_U64(io + 40) == 7 && !PPC_LOAD_U8(io + 88),
          "Developer logical read at EOF changed the destination or byte count");
    initializeLogical(123, prepared->size);
    logicalRead(0, 0);
    check(PPC_LOAD_U64(io + 32) == 123 && PPC_LOAD_U64(io + 40) == 7 && PPC_LOAD_U8(io + 88) == 1,
          "Developer zero-length logical read changed the original state");
    const auto record = object(handle);
    struct RestoreMarker {
        decltype(record) file; std::shared_ptr<const CachedWeaponFile> marker;
        ~RestoreMarker() { file->cachedWeaponFile = std::move(marker); }
    };
    {
        RestoreMarker restore{record, record->cachedWeaponFile};
        auto missing = std::make_shared<CachedWeaponFile>(*prepared);
        missing->path += L".missing-test";
        check(!std::filesystem::exists(missing->path), "Logical read missing-file fixture already exists");
        record->cachedWeaponFile = missing;
        initializeLogical(0, prepared->size);
        logicalRead(logicalBuffer, 48);
        check(base[logicalBuffer] == 0xAD && PPC_LOAD_U64(io + 32) == 0 &&
              PPC_LOAD_U64(io + 40) == 7 && !PPC_LOAD_U8(io + 88),
              "Developer host read failure copied partial data or advanced counters");
    }
    {
        // An ordinary handle must use the unchanged original reader, including
        // its existing engine buffer. Its bytes deliberately differ from AG.
        RestoreMarker restore{record, record->cachedWeaponFile};
        record->cachedWeaponFile.reset();
        initializeLogical(0, 16);
        const auto buffer = block + 768, data = block + 1024;
        PPC_STORE_U32(io + 72, buffer); PPC_STORE_U32(io + 76, 1);
        PPC_STORE_U32(buffer + 20, data); PPC_STORE_U64(buffer + 80, 0); PPC_STORE_U32(buffer + 88, 16);
        std::memcpy(base + data, "OriginalBuffer!!", 16);
        logicalRead(logicalBuffer, 16);
        check(!std::memcmp(base + logicalBuffer, "OriginalBuffer!!", 16) &&
              PPC_LOAD_U64(io + 32) == 16 && PPC_LOAD_U64(io + 40) == 23 && PPC_LOAD_U8(io + 88) == 1,
              "Developer logical read intercepted an ordinary file or changed original buffer behavior");
    }
    {
        const auto model = memory->developerWeaponFile(memory->gameDirectory() / "Content/Models/Weapons/gun_darkness_01.XMD");
        check(model && model->covers(128289, 60) && !model->covers(69632, 65536),
              "Model fixture does not distinguish logical reads from uncached read-ahead");
        RestoreMarker restore{record, record->cachedWeaponFile};
        record->cachedWeaponFile = model;
        initializeLogical(128289, model->size);
        const auto modelBytes = read(model->path, 128289, 60);
        logicalRead(logicalBuffer, 60);
        check(!std::memcmp(base + logicalBuffer, modelBytes.data(), modelBytes.size()) &&
              PPC_LOAD_U64(io + 32) == 128349 && PPC_LOAD_U64(io + 40) == 67 && PPC_LOAD_U8(io + 88) == 1,
              "Developer model logical read attempted an uncached aligned block instead of its exact slice");
    }
    {
        const auto texture = memory->developerWeaponFile(memory->gameDirectory() / "Content/Textures/AllTextures.001.XTC");
        const auto textureBuffer = memory->allocate(65552);
        check(texture && textureBuffer, "Cannot allocate materialized texture logical-read fixture");
        struct ReleaseTexture { uint32_t address; ~ReleaseTexture() { memory->release(address); } } releaseTexture{textureBuffer};
        RestoreMarker restore{record, record->cachedWeaponFile};
        record->cachedWeaponFile = texture;
        initializeLogical(138602804, texture->size);
        const auto expectedMip = read(memory->gameDirectory() / "Content/Textures/AllTextures.001.XTC.xt0", 97073408, 65552);
        logicalRead(textureBuffer, 65552);
        check(!std::memcmp(base + textureBuffer, expectedMip.data(), expectedMip.size()) &&
              PPC_LOAD_U64(io + 32) == 138668356 && PPC_LOAD_U64(io + 40) == 65559 && PPC_LOAD_U8(io + 88) == 1,
              "Materialized texture logical read lost physical bytes or advanced incorrect counters");
        initializeLogical(138866208, texture->size);
        logicalRead(logicalBuffer, 1);
        check(base[logicalBuffer] == 0xAD && PPC_LOAD_U64(io + 32) == 138866208 &&
              PPC_LOAD_U64(io + 40) == 7 && !PPC_LOAD_U8(io + 88),
              "Texture logical read filled an unauthenticated lower animation mip");
    }
    call.r3.u64 = block + 164; call.r4.u64 = GENERIC_WRITE | SYNCHRONIZE;
    call.r5.u64 = block; call.r6.u64 = block + 128; call.r7.u64 = 0;
    call.r8.u64 = 0; call.r9.u64 = FILE_SHARE_READ; call.r10.u64 = 1;
    __imp__NtCreateFile(call, base);
    check(call.r3.u32 == 0xc00000a2, "Developer asset overlay granted write access to cached game data");
}
}

static void testDeveloperWeaponAssets(PPCContext& context) {
    using namespace DeveloperWeaponAssetFixture;
    parserTests();
    streamedMipTests();
    auto* base = memory->base();
    const auto originalAllocate = PPC_LOOKUP_FUNC(base, 0x822323C8);
    const auto originalFree = PPC_LOOKUP_FUNC(base, 0x822323E8);
    struct Restore {
        uint8_t* base; PPCFunc* allocate; PPCFunc* free;
        ~Restore() {
            PPC_LOOKUP_FUNC(base, 0x822323C8) = allocate;
            PPC_LOOKUP_FUNC(base, 0x822323E8) = free;
        }
    } restore{base, originalAllocate, originalFree};
    PPC_LOOKUP_FUNC(base, 0x822323C8) = allocate;
    PPC_LOOKUP_FUNC(base, 0x822323E8) = free;
    const auto before = memory->allocatedBytes();
    memory->enableDeveloperWeaponAssets(context, base);
    check(memory->allocatedBytes() == before, "Developer asset preparation leaked guest decompression buffers");
    for (const auto* name : {
        "Anim/AnimGraphs/Weapons/AG2Weapon_Darkness01.XAH", "Anim/AnimGraphs/Weapons/AG2Weapon_Darkness02.XAH",
        "Anim/Main/gun_darkness_01.XSA", "Anim/Main/gun_darkness_02.XSA",
        "Models/Weapons/gun_darkness_01.XMD", "Models/Weapons/gun_darkness_02.XMD"}) {
        const auto prepared = memory->developerWeaponFile(memory->gameDirectory() / "Content" / name);
        check(prepared && prepared->covers(0, 48) && !prepared->path.empty() &&
              std::filesystem::file_size(prepared->path) == prepared->size,
              "Developer asset preparation omitted a required resource or changed nominal size");
    }
    const auto texture0 = memory->developerWeaponFile(memory->gameDirectory() / "Content/Textures/AllTextures.000.XTC");
    const auto texture1 = memory->developerWeaponFile(memory->gameDirectory() / "Content/Textures/AllTextures.001.XTC");
    check(texture0 && texture1 && texture0->size == 1077792745 && texture1->size == 147565524 &&
          texture0->covers(1076924517, 868132) && texture0->covers(1077792649, 96) &&
          texture0->covers(47389952, 64) && texture0->covers(47455568, 22224) &&
          texture1->covers(147500436, 64992) && texture1->covers(147565428, 96) &&
          texture1->covers(138602740, 64) && texture1->covers(138668356, 22224) &&
          !texture0->covers(48, 1) && texture1->covers(138602804, 65552) && !texture1->covers(138866208, 1),
          "Developer textures lost their directories, Ancient mip reads or sparse hole guards");
    materializedTextureTests();
    filesystemTests(context);
    std::puts("Developer weapon cache: strict parsing, original inflation, authenticated mip bodies, sparse read guards and native logical reads passed.");
}
