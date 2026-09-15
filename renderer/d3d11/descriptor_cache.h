#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace DarkRecomp {
template<size_t Size> struct DescriptorKey {
    std::array<unsigned char,Size> bytes;
    bool operator==(const DescriptorKey& other) const noexcept {
        return std::memcmp(bytes.data(),other.bytes.data(),Size)==0;
    }
};

// State descriptors used to run through the string library's byte-at-a-time
// hash on every draw (264 bytes for blend state alone). Mix complete words;
// equality still checks every original byte, including signed zero/padding.
template<size_t Size> struct DescriptorHash {
    size_t operator()(const DescriptorKey<Size>& key) const noexcept {
        uint64_t hash=0x9e3779b185ebca87ull ^ Size;
        size_t i=0;
        for(;i+8<=Size;i+=8) {
            uint64_t word;
            std::memcpy(&word,key.bytes.data()+i,8);
            hash=(hash^word)*0x9e3779b185ebca87ull;
        }
        if constexpr(Size%8) {
            uint64_t tail=0;
            std::memcpy(&tail,key.bytes.data()+i,Size%8);
            hash=(hash^tail)*0x9e3779b185ebca87ull;
        }
        hash^=hash>>33;hash*=0xff51afd7ed558ccdull;hash^=hash>>33;
        return size_t(hash);
    }
};

// One renderer thread owns each cache. The last entry can be compared without
// hashing. Map nodes keep it valid across rehash; clear invalidates it first.
// Copy bytes explicitly instead of copying a descriptor's padding as fields.
template<class Descriptor,class Value,class Hash=DescriptorHash<sizeof(Descriptor)>>
class DescriptorCache {
    static_assert(std::is_trivially_copyable_v<Descriptor>);
    using Key=DescriptorKey<sizeof(Descriptor)>;
    using Map=std::unordered_map<Key,Value,Hash>;
    Map entries_;
    typename Map::value_type* last_=nullptr;
public:
    DescriptorCache()=default;
    DescriptorCache(const DescriptorCache&)=delete;
    DescriptorCache& operator=(const DescriptorCache&)=delete;
    Value& entry(const Descriptor& descriptor) {
        if(last_ && std::memcmp(last_->first.bytes.data(),&descriptor,sizeof(descriptor))==0)
            return last_->second;
        Key key;
        std::memcpy(key.bytes.data(),&descriptor,sizeof(descriptor));
        last_=&*entries_.try_emplace(key).first;
        return last_->second;
    }
    size_t size() const noexcept {return entries_.size();}
    void clear() noexcept {last_=nullptr;entries_.clear();}
};
}
