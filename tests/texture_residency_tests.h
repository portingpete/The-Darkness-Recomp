// Original sampler residency oracle, run by NativeTextureUploadContract in
// a disposable offline guest fixture with the original image loaded.
// base is the existing 4GiB guest mapping; scratch is a committed, unused 64KiB
// region. Never call in a live game. This seeds upload-tail outputs, then calls
// the real original sampler helper; no reimplementation of the clamp is used.
#include "ppc_recomp_shared.h"
#include "renderer/engine/world_mesh.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_822569E0);

namespace OriginalResidencyFixture {
static void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
static uint32_t get32(const uint8_t* b, uint32_t a) {
    return uint32_t(b[a])<<24 | uint32_t(b[a+1])<<16 |
           uint32_t(b[a+2])<<8 | b[a+3];
}
static void put32(uint8_t* b, uint32_t a, uint32_t v) {
    for (unsigned i=0;i<4;++i) b[a+i]=uint8_t(v>>((3-i)*8));
}
static void put16(uint8_t* b, uint32_t a, uint16_t v) {
    b[a]=uint8_t(v>>8); b[a+1]=uint8_t(v);
}
struct RestoreRegion {
    uint8_t* start;
    std::vector<uint8_t> bytes;
    RestoreRegion(uint8_t* p, size_t size):start(p),bytes(p,p+size) {}
    ~RestoreRegion() { std::memcpy(start,bytes.data(),bytes.size()); }
};

void originalSamplerFirstResident2(uint8_t* base, const PPCContext& initial,
                                  uint32_t scratch) {
    constexpr uint32_t context=0x82A69B00;
    require(uint64_t(scratch)+65536<=0x100000000ull,"Scratch extent wraps");
    // Caller owns/commits scratch and supplies a valid original-image context.
    RestoreRegion saveContext(base+context,18200), saveScratch(base+scratch,65536);
    RestoreRegion saveStack(base+initial.r1.u32-1024,1024);
    const unsigned savedMxcsr=_mm_getcsr();
    struct RestoreFp { unsigned value; ~RestoreFp(){_mm_setcsr(value);} } fp{savedMxcsr};
    require(get32(base,0x8209DCBC)==0,"Expected original zero LOD constant");

    const uint32_t ids=scratch+0x100,table=scratch+0x200;
    const uint32_t metadata=scratch+0x300,validBits=scratch+0x400;
    const uint32_t resource=scratch+0x800,external=scratch+0x1000;
    const uint32_t device=scratch+0x4000;
    constexpr uint32_t firstResident=2,headerMax=5;
    for (unsigned variant=0;variant<4;++variant) {
        std::memset(base+context,0,18200);
        std::memset(base+scratch,0,65536);
        put32(base,context+15748,device);
        put32(base,context+17964,table);
        put32(base,context+144,metadata);
        put32(base,metadata+8,validBits);
        put16(base,validBits+2,1); // ID1 valid; avoid lazy preparation helper.
        put16(base,ids,1);        // Requested ID1, all other slots ID0.
        put32(base,table+8,resource); // table[(id+1)]
        const bool alternate=variant&1,inlineHeader=variant&2;
        const uint32_t selected=resource+(alternate?88:8);
        const uint32_t texture=inlineHeader?selected+4:external;
        put32(base,resource+172,0x10000000u|(alternate?0x02000000u:0u)|(3u<<13));
        if (inlineHeader) put32(base,selected,1);
        else put32(base,selected+76,texture);
        // Full 32x32 dimensions, six original levels 0..5; no mip rebasing.
        put32(base,texture+28,2);
        put32(base,texture+32,0x01000006);
        put32(base,texture+36,31u|(31u<<13));
        put32(base,texture+44,headerMax<<6); // Historical headerMin=0.
        put32(base,resource+176,std::bit_cast<uint32_t>(float(firstResident)));
        put32(base,resource+180,firstResident);
        put32(base,resource+184,get32(base,0x82A8C560)); // Current frame.
        base[device+11968]=15; // Device maximum; required nonzero initialization.
        // Binding preserves device filter bits; select mip-linear, not baseOnly.
        put32(base,device+1152+12,(1u<<19)|(1u<<21)|(1u<<23));
        auto callOriginal=[&] {
            PPCContext guest; std::memcpy(&guest,&initial,sizeof guest);
            guest.r3.u64=ids; guest.r4.u64=0;
            __imp__sub_822569E0(guest,base);
            require(guest.r1.u32==initial.r1.u32,"Original sampler stack imbalance");
        };
        auto check=[&](uint32_t expectedMin) {
            const auto packed=get32(base,device+1168);
            require(((packed>>2)&15)==expectedMin,"Original packed sampler MinLOD differs");
            require(((packed>>6)&15)==headerMax,"Original packed sampler MaxLOD differs");
            require(base[device+11942]==expectedMin,"Device cached minimum differs");
            require(get32(base,device+12536)==texture,"Original selected wrong header");
            require((get32(base,device+28)&0x80000000u)!=0,"Slot0 sampler dirty bit missing");
            require(get32(base,texture+44)==headerMax<<6,"Original mutated header LODs");
            require(get32(base,texture+36)==(31u|(31u<<13)),"Original rebased full dimensions");
            std::array<uint32_t,6> words{};
            for (unsigned w=0;w<6;++w) words[w]=get32(base,device+1152+w*4);
            const auto decoded=DarkRecomp::Native::decodeWorldSampler(words);
            require(decoded.valid && !decoded.baseOnly && decoded.minLevel==expectedMin &&
                    decoded.maxLevel==headerMax,"Native decoder lost original LOD state");
        };
        for(int exponent=-32;exponent<=31;++exponent) {
            put32(base,texture+40,0xd10u|((uint32_t(exponent)&63u)<<13));
            put32(base,resource+176,std::bit_cast<uint32_t>(2.0f));put32(base,resource+180,2);
            put16(base,context+17968,0xFFFF);
            callOriginal(); check(2);
            auto checkExponent=[&] {
                const unsigned encoded=(get32(base,device+1164)>>13)&63u;
                require(int(encoded^32)-32==exponent,"Original texture bind discarded signed fetch exponent");
                DarkRecomp::Native::WorldTexture snapshot;
                require(DarkRecomp::Native::snapshotWorldTexture(base,texture,snapshot) && snapshot.exponent==exponent,
                        "Native resource snapshot differs from original bound exponent");
            };
            checkExponent();
            // A completed prefix retains the same exponent while removing its
            // residency clamp. Force a changed-ID bind for the original reset.
            put32(base,resource+176,0);put32(base,resource+180,0);
            put16(base,context+17968,0xFFFF);
            callOriginal();check(0);checkExponent();
        }
    }
}
} // namespace OriginalResidencyFixture

static void testTextureResidency(PPCContext& ctx) {
    const auto scratch=memory->allocate(65536);
    check(scratch!=0,"Residency fixture allocation failed");
    struct Release {uint32_t address;~Release(){memory->release(address);}} release{scratch};
    OriginalResidencyFixture::originalSamplerFirstResident2(memory->base(),ctx,scratch);
    puts("Original fetch exponent: 512 bindings preserve all 64 signed values across four selected header forms.");
    puts("Original sampler residency: primary/alternate/inline headers preserve logical dimensions and clamp 2..5, then restore 0..5.");
}
