#include "renderer/engine/frame_resource_set.h"
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

using DarkRecomp::Native::FrameResourceSet;
static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {
    try {
        // Real, densely spaced object addresses exercise collisions and growth.
        std::vector<unsigned> resources(65536);
        FrameResourceSet<unsigned> actual;
        std::unordered_set<const unsigned*> expected;
        require(!actual.contains(nullptr) && actual.size()==0,"Empty set contains an identity");
        for(const auto& resource:resources) {
            require(actual.insert(&resource),"Growth lost a new identity");
            require(!actual.insert(&resource),"Duplicate identity was charged twice");
        }
        require(actual.insert(nullptr) && !actual.insert(nullptr),"Null identity occupancy is inconsistent");
        require(actual.size()==resources.size()+1,"Unique resource count differs");
        for(const auto& resource:resources)require(actual.contains(&resource),"Rehash lost an identity");
        const auto capacity=actual.capacity();
        uint32_t random=0x3571ABCD;
        auto next=[&] {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
        for(unsigned frame=0;frame<128;++frame) {
            actual.clear();expected.clear();
            require(actual.size()==0 && !actual.contains(nullptr),"Clear retained an identity");
            for(const auto& resource:resources)require(!actual.contains(&resource),"Previous frame leaked into membership");
            for(unsigned i=0;i<4096;++i) {
                const auto* value=i%17?&resources[next()%resources.size()]:nullptr;
                require(actual.contains(value)==expected.contains(value),"Membership differs from unordered_set");
                require(actual.insert(value)==expected.insert(value).second,"Insertion differs from unordered_set");
            }
            require(actual.size()==expected.size(),"Frame accounting differs from unordered_set");
            for(const auto* value:expected)require(actual.contains(value),"Frame identity disappeared");
            require(actual.capacity()==capacity,"Clear failed to reuse allocated bucket storage");
        }
        std::puts("FrameResourceSetContract passed: 65,537 identities, duplicate charging, growth, null, and 128 reused batches checked against unordered_set.");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FrameResourceSetContract failed: %s\n",error.what());return 1;
    }
}
