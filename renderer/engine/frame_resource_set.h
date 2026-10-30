#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace DarkRecomp::Native {
// Non-owning identities charged once per queued batch. The caller holds the
// queue mutex and owns every pointed-to resource. Reuse bucket storage across
// batches instead of allocating/freeing an unordered_set node for each entry.
template<class T> class FrameResourceSet {
    struct Bucket {const T* value=nullptr;uint64_t epoch=0;};
    std::vector<Bucket> buckets_;
    size_t size_=0;
    uint64_t epoch_=1;
    const T* last0_=nullptr;
    const T* last1_=nullptr;
    static size_t hash(const T* value) noexcept {
        uint64_t bits=reinterpret_cast<uintptr_t>(value);
        bits^=bits>>33;bits*=0xff51afd7ed558ccdull;bits^=bits>>33;
        return size_t(bits);
    }
    static size_t slot(const std::vector<Bucket>& buckets,const T* value,uint64_t epoch) noexcept {
        size_t i=hash(value)&(buckets.size()-1);
        while(buckets[i].epoch==epoch && buckets[i].value!=value)i=(i+1)&(buckets.size()-1);
        return i;
    }
    void grow() {
        if(buckets_.size()>buckets_.max_size()/2)throw std::length_error("Frame resource set capacity");
        std::vector<Bucket> next(buckets_.empty()?128:buckets_.size()*2);
        for(const auto& bucket:buckets_)if(bucket.epoch==epoch_)
            next[slot(next,bucket.value,epoch_)]=bucket;
        buckets_.swap(next);
    }
public:
    size_t size() const noexcept {return size_;}
    size_t capacity() const noexcept {return buckets_.size();}
    bool contains(const T* value) const noexcept {
        if(value && (value==last0_ || value==last1_))return true;
        return !buckets_.empty() && buckets_[slot(buckets_,value,epoch_)].epoch==epoch_;
    }
    bool insert(const T* value) {
        if(value && (value==last0_ || value==last1_))return false;
        if(buckets_.empty())grow();
        size_t i=slot(buckets_,value,epoch_);
        if(buckets_[i].epoch==epoch_) {
            last1_=last0_;last0_=value;
            return false;
        }
        // At most half full, so probes always reach an empty bucket.
        if(size_==buckets_.size()/2) {grow();i=slot(buckets_,value,epoch_);}
        buckets_[i]={value,epoch_};++size_;
        last1_=last0_;last0_=value;
        return true;
    }
    void clear() noexcept {
        size_=0;
        last0_=nullptr;
        last1_=nullptr;
        if(++epoch_==0) {
            for(auto& bucket:buckets_)bucket.epoch=0;
            epoch_=1;
        }
    }
};
}
