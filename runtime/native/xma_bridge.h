#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace DarkRecomp::Native {
class Memory;
class XmaBridge {
public:
    XmaBridge();
    ~XmaBridge();
    const char* decode(Memory& memory, uint32_t context, uint32_t slot);
    void reset(uint32_t slot);
private:
    friend class Memory;
    struct ValidationCache;
    // Memory holds its mutex for the entire batch; cache lifetime ends here.
    const char* decodeBatch(Memory& memory, std::span<const uint32_t> contexts,
                            uint32_t& failedContext);
    const char* decodeRecords(Memory& memory, uint32_t records, uint32_t count,
                              uint32_t firstContext, uint32_t& failedContext);
    template<class ContextAt>
    const char* decodeSequence(Memory& memory, std::size_t count, ContextAt contextAt,
                               uint32_t& failedContext);
    const char* decodeValidated(Memory& memory, uint32_t context, uint32_t slot,
                                ValidationCache& validation);
    struct Stream;
    std::array<std::unique_ptr<Stream>, 320> streams_;
};
}
