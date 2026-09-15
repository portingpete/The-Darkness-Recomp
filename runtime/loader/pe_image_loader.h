#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "runtime/guest/guest_memory.h"
#include "runtime/guest/guest_types.h"

namespace DarkRecomp {

struct SectionInfo {
    std::string name;
    uint32_t virtualAddress;
    uint32_t virtualSize;
    uint32_t rawOffset;
    uint32_t rawSize;
};

class PEImageLoader {
public:
    PEImageLoader() = default;
    ~PEImageLoader() = default;

    bool Load(const std::string& path, GuestMemory& memory);
    void SetupLookupTable(uint8_t* base);
    void SetupThreadContext(PPCContext& ctx, uint8_t* base);

    uint32_t GetEntryPoint() const { return m_entryPoint; }
    uint32_t GetImageBase() const { return m_imageBase; }
    uint32_t GetImageSize() const { return m_imageSize; }

    const std::vector<SectionInfo>& GetSections() const { return m_sections; }

private:
    uint32_t m_entryPoint = 0x828AA3E8;
    uint32_t m_imageBase = 0x82000000;
    uint32_t m_imageSize = 0x00B10000;
    std::vector<SectionInfo> m_sections;
};

} // namespace DarkRecomp
