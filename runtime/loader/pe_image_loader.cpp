#include "pe_image_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace DarkRecomp {

#pragma pack(push, 1)
struct DOSHeader {
    uint16_t e_magic;
    uint8_t  e_cblp[58];
    uint32_t e_lfanew;
};

struct FileHeader {
    uint16_t machine;
    uint16_t numberOfSections;
    uint32_t timeDateStamp;
    uint32_t pointerToSymbolTable;
    uint32_t numberOfSymbols;
    uint16_t sizeOfOptionalHeader;
    uint16_t characteristics;
};

struct SectionHeader {
    char     name[8];
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t sizeOfRawData;
    uint32_t pointerToRawData;
    uint32_t pointerToRelocations;
    uint32_t pointerToLinenumbers;
    uint16_t numberOfRelocations;
    uint16_t numberOfLinenumbers;
    uint32_t characteristics;
};
#pragma pack(pop)

bool PEImageLoader::Load(const std::string& path, GuestMemory& memory) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[Loader] Error: Unable to open binary: " << path << std::endl;
        return false;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        std::cerr << "[Loader] Error: Failed to read binary data." << std::endl;
        return false;
    }

    auto* dosHeader = reinterpret_cast<const DOSHeader*>(buffer.data());
    if (dosHeader->e_magic != 0x5A4D) { // 'MZ'
        std::cerr << "[Loader] Error: Invalid DOS header magic." << std::endl;
        return false;
    }

    auto* peSig = reinterpret_cast<const uint32_t*>(buffer.data() + dosHeader->e_lfanew);
    if (*peSig != 0x00004550) { // 'PE\0\0'
        std::cerr << "[Loader] Error: Invalid PE signature." << std::endl;
        return false;
    }

    auto* fileHeader = reinterpret_cast<const FileHeader*>(buffer.data() + dosHeader->e_lfanew + 4);
    const uint8_t* optHeader = buffer.data() + dosHeader->e_lfanew + 4 + sizeof(FileHeader);
    const uint8_t* secHeaderPtr = optHeader + fileHeader->sizeOfOptionalHeader;

    uint8_t* guestBase = memory.GetBase();
    uint8_t* targetBase = guestBase + m_imageBase;

    std::cout << "[Loader] Loading " << fileHeader->numberOfSections << " PE sections into guest RAM [0x"
              << std::hex << m_imageBase << " - 0x" << (m_imageBase + m_imageSize) << std::dec << "]..." << std::endl;

    // XexTool basefile dumps are raw memory images loaded at 0x82000000:
    // file offset == RVA (guest addr - image base), NOT PointerToRawData.
    // A true on-disk PE uses PointerToRawData as file offset. Detect which
    // layout this file uses by checking the .text section: in a raw dump the
    // bytes at file offset VA decode to valid PPC, while PointerToRawData
    // points elsewhere. Heuristic: if any section has VA != rawPtr and the
    // file size matches the memory image size (~0xB0FE00), treat as raw dump.
    bool isRawDump = false;
    {
        // basefile.exe from xextool is 11599872 bytes (0xB0FE00) for a
        // 0xB10000 image; a linked PE would be smaller and have distinct
        // raw pointers. Check .text specifically.
        for (uint16_t i = 0; i < fileHeader->numberOfSections; i++) {
            auto* sec = reinterpret_cast<const SectionHeader*>(secHeaderPtr + i * sizeof(SectionHeader));
            std::string nm(sec->name, strnlen(sec->name, 8));
            if (nm == ".text" && sec->virtualAddress != sec->pointerToRawData) {
                // In a raw dump, file offset VA contains code; in a PE file,
                // file offset rawPtr contains code. Both exist in file, but
                // only one is correct. The dump size matches the image size.
                if ((size_t)fileSize >= (size_t)(sec->virtualAddress + sec->virtualSize) &&
                    (size_t)fileSize < (size_t)(m_imageSize + 0x20000)) {
                    isRawDump = true;
                }
                break;
            }
        }
    }
    if (isRawDump) {
        std::cout << "[Loader] Detected xextool basefile memory dump (file offset == RVA). "
                  << "Mapping image directly to 0x82000000." << std::endl;
        size_t copySize = std::min<size_t>((size_t)fileSize, (size_t)m_imageSize);
        memcpy(targetBase, buffer.data(), copySize);
        // Record section info for diagnostics (no per-section copy needed).
        for (uint16_t i = 0; i < fileHeader->numberOfSections; i++) {
            auto* sec = reinterpret_cast<const SectionHeader*>(secHeaderPtr + i * sizeof(SectionHeader));
            std::string secName(sec->name, strnlen(sec->name, 8));
            SectionInfo info;
            info.name = secName;
            info.virtualAddress = sec->virtualAddress;
            info.virtualSize = sec->virtualSize;
            info.rawOffset = sec->pointerToRawData;
            info.rawSize = sec->sizeOfRawData;
            m_sections.push_back(info);
        }
        // Zero BSS tails where virtualSize > rawSize is not applicable to a
        // full dump (dump already contains zeros), so skip.
    } else for (uint16_t i = 0; i < fileHeader->numberOfSections; i++) {
        auto* sec = reinterpret_cast<const SectionHeader*>(secHeaderPtr + i * sizeof(SectionHeader));
        std::string secName(sec->name, strnlen(sec->name, 8));

        SectionInfo info;
        info.name = secName;
        info.virtualAddress = sec->virtualAddress;
        info.virtualSize = sec->virtualSize;
        info.rawOffset = sec->pointerToRawData;
        info.rawSize = sec->sizeOfRawData;
        m_sections.push_back(info);

        uint8_t* dest = targetBase + sec->virtualAddress;
        if (sec->pointerToRawData < buffer.size() && sec->sizeOfRawData > 0) {
            uint32_t copySize = std::min<uint32_t>(sec->sizeOfRawData, (uint32_t)(buffer.size() - sec->pointerToRawData));
            memcpy(dest, buffer.data() + sec->pointerToRawData, copySize);
        }

        // Zero out uninitialized BSS space in section
        if (sec->virtualSize > sec->sizeOfRawData) {
            memset(dest + sec->sizeOfRawData, 0, sec->virtualSize - sec->sizeOfRawData);
        }
    }

    std::cout << "[Loader] Sections mapped successfully." << std::endl;
    return true;
}

void PEImageLoader::SetupLookupTable(uint8_t* base) {
    std::cout << "[Loader] Initializing PowerPC indirect function lookup table..." << std::endl;
    size_t count = 0;
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++) {
        uint32_t guestAddr = (uint32_t)PPCFuncMappings[i].guest;
        PPCFunc* hostFunc = PPCFuncMappings[i].host;
        PPC_LOOKUP_FUNC(base, guestAddr) = hostFunc;
        count++;
    }
    std::cout << "[Loader] Populated " << count << " recompiled function entries in dispatch table." << std::endl;
}

void PEImageLoader::SetupThreadContext(PPCContext& ctx, uint8_t* base) {
    memset(&ctx, 0, sizeof(ctx));

    // 1. Stack setup (top of stack at 0x70030000, 256KB stack)
    constexpr uint32_t GUEST_STACK_TOP = 0x70030000;
    ctx.r1.u64 = GUEST_STACK_TOP;

    // 2. PCR / TLS / TEB setup at 0x70040000
    constexpr uint32_t GUEST_PCR_ADDR = 0x70040000;
    constexpr uint32_t GUEST_TLS_ADDR = 0x70040AB0;
    constexpr uint32_t GUEST_TEB_ADDR = 0x70040BB0;

    uint8_t* pcr = base + GUEST_PCR_ADDR;
    memset(pcr, 0, 0x1000);

    // TLS pointer at PCR + 0x0
    *reinterpret_cast<uint32_t*>(pcr) = _byteswap_ulong(GUEST_TLS_ADDR);
    // TEB pointer at PCR + 0x100
    *reinterpret_cast<uint32_t*>(pcr + 0x100) = _byteswap_ulong(GUEST_TEB_ADDR);
    // CPU Number 0 at PCR + 0x10C
    pcr[0x10C] = 0;

    // Thread ID in TEB + 0x14C
    *reinterpret_cast<uint32_t*>(base + GUEST_TEB_ADDR + 0x14C) = _byteswap_ulong(1);

    ctx.r13.u64 = GUEST_PCR_ADDR;
    std::cout << "[Loader] Initialized Guest Thread Context (SP=0x" << std::hex << GUEST_STACK_TOP
              << ", PCR=0x" << GUEST_PCR_ADDR << std::dec << ")." << std::endl;
}

} // namespace DarkRecomp
