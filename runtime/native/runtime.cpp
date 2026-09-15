#include "runtime.h"
#include "audio_driver.h"
#include "xma_bridge.h"
#include "ppc_image_metadata.h"
#include <bcrypt.h>
#include <array>
#include <dbghelp.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace DarkRecomp::Native {
Memory* memory = nullptr;
thread_local PPCContext* currentContext = nullptr;

Memory::Memory() {
    // One owned physical store, with the CPU aliases used by the original
    // title. A/C map offset0; E maps offset4096. This is memory, not a GPU
    // command interpreter. Placeholders preserve the contiguous AOT base.
    auto module = GetModuleHandleW(L"kernelbase.dll");
    auto reserve = reinterpret_cast<decltype(&VirtualAlloc2)>(GetProcAddress(module, "VirtualAlloc2"));
    auto map = reinterpret_cast<decltype(&MapViewOfFile3)>(GetProcAddress(module, "MapViewOfFile3"));
    if (!reserve || !map) throw std::runtime_error("Native physical aliases require Windows VirtualAlloc2/MapViewOfFile3");
    constexpr auto boundaries = [] {
        std::array<uint64_t, (cAliasEnd-cAliasBegin)/cViewBytes + 5> result{};
        size_t n = 0;
        result[n++] = 0;
        result[n++] = 0xa0000000;
        for (uint64_t address = cAliasBegin; address <= cAliasEnd; address += cViewBytes)
            result[n++] = address;
        result[n++] = 0xfffff000;
        result[n] = PPC_MEMORY_SIZE;
        return result;
    }();
    // No bookkeeping allocation may throw after a placeholder is split or
    // replaced: every acquired region must be visible to failure cleanup.
    memoryViews_.reserve(boundaries.size()-1);
    base_ = static_cast<uint8_t*>(reserve(GetCurrentProcess(), nullptr, PPC_MEMORY_SIZE,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base_) throw std::runtime_error("Cannot reserve the 4 GB guest address space");
    HANDLE backing = nullptr;
    try {
        memoryViews_.emplace_back(0, false);
        for (size_t i=0;i+2<boundaries.size();++i) {
            if (!VirtualFree(base_+boundaries[i], size_t(boundaries[i+1]-boundaries[i]), MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
                throw std::runtime_error("Cannot split native address placeholders");
            memoryViews_.emplace_back(uint32_t(boundaries[i+1]), false);
        }
        backing=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,0x20000000,nullptr);
        if (!backing) throw std::runtime_error("Cannot create native physical memory backing");
        for (size_t i=0;i+1<boundaries.size();++i) {
            const auto address=boundaries[i], size=boundaries[i+1]-address;
            if(address>=0xa0000000 && address<0xfffff000) {
                const uint64_t backingOffset = address>=cAliasBegin && address<cAliasEnd ?
                    address-cAliasBegin : address==cAliasEnd ? 4096 : 0;
                if (!map(backing,GetCurrentProcess(),base_+address,backingOffset,size,
                         MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,nullptr,0))
                    throw std::runtime_error("Cannot map native physical alias");
                memoryViews_[i].second=true;
                DWORD previous;
                if(!VirtualProtect(base_+address,size,PAGE_NOACCESS,&previous))
                    throw std::runtime_error("Cannot guard unallocated physical memory");
            } else if(!reserve(GetCurrentProcess(),base_+address,size,MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,PAGE_NOACCESS,nullptr,0))
                throw std::runtime_error("Cannot reserve native virtual memory");
        }
        CloseHandle(backing);backing=nullptr;
    // Keep the Xenon null guard while mapping the first valid guest pages.
    if (!commit(0x10000, 0x0FFF0000)) throw std::runtime_error("Cannot map low guest memory");
    // Early title startup uses a separate 512 MiB low-memory arena before native allocation imports are reached.
    if (!commit(0x10000000, 0x20000000)) throw std::runtime_error("Cannot map the title guest arena");
    } catch(...) {
        if(backing)CloseHandle(backing);
        releaseAddressSpace();throw;
    }
}
Memory::~Memory() {
    stopTimestamp();
    audioDriverShutdownForMemory(this);
    releaseAddressSpace();
}
void Memory::releaseAddressSpace() {
    for(const auto& [offset,mapped]:memoryViews_) {
        if(mapped)UnmapViewOfFile(base_+offset);
        else VirtualFree(base_+offset,0,MEM_RELEASE);
    }
    memoryViews_.clear();base_=nullptr;
}
uint32_t Memory::physicalAddress(uint32_t address) {
    return (address & 0x1fffffffu) + (address>=0xe0000000u?0x1000u:0u);
}
bool Memory::protectPhysical(uint32_t offset,uint32_t size,DWORD protection) {
    if(!size || uint64_t(offset)+size>0x20000000)return false;
    DWORD previous;
    for(uint32_t alias:{0xa0000000u,0xc0000000u,0xe0000000u}) {
        uint32_t first=offset,last=offset+size;
        if(alias==0xe0000000u)first=(std::max)(first,0x1000u);
        if(first>=last)continue;
        uint32_t address=alias+first-(alias==0xe0000000u?0x1000u:0u);
        const uint32_t end=address+(last-first);
        while(address<end) {
            const uint32_t next=alias==cAliasBegin ? (std::min)(end,cViewEnd(address)) : end;
            if(!VirtualProtect(base_+address,next-address,protection,&previous))return false;
            address=next;
        }
    }
    return true;
}
uint32_t Memory::millisSince(uint64_t startQpc, uint64_t frequency, uint64_t nowQpc) {
    uint64_t elapsed = nowQpc - startQpc;
    uint64_t seconds = elapsed / frequency;
    uint64_t rest = elapsed % frequency;
    return uint32_t(seconds * 1000 + rest * 1000 / frequency);
}
void Memory::writeTimestamp(uint32_t storage, uint32_t millis) {
    LONG swapped = LONG(_byteswap_ulong(millis));
    InterlockedExchange(reinterpret_cast<volatile LONG*>(base_ + storage + 16), swapped);
}
void Memory::stopTimestampLocked() {
    timestampStop_.store(true, std::memory_order_relaxed);
    if (timestampThread_.joinable()) timestampThread_.join();
}
void Memory::stopTimestamp() {
    std::lock_guard lock(mutex_);
    stopTimestampLocked();
}
void Memory::startTimestamp(uint32_t storage) {
    std::lock_guard lock(mutex_);
    stopTimestampLocked();
    timestampStop_.store(false, std::memory_order_relaxed);
    if (!timestampArmed_) {
        LARGE_INTEGER frequency{}, start{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        timestampFreq_ = uint64_t(frequency.QuadPart);
        timestampEpochQpc_ = uint64_t(start.QuadPart);
        timestampArmed_ = true;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    writeTimestamp(storage, millisSince(timestampEpochQpc_, timestampFreq_, uint64_t(now.QuadPart)));
    timestampThread_ = std::thread([this, storage] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (timestampStop_.load(std::memory_order_relaxed)) break;
            LARGE_INTEGER tick{};
            QueryPerformanceCounter(&tick);
            writeTimestamp(storage, millisSince(timestampEpochQpc_, timestampFreq_, uint64_t(tick.QuadPart)));
        }
    });
}
bool Memory::commit(uint32_t address, uint32_t size) {
    std::lock_guard lock(mutex_);
    if (!size || uint64_t(address) + size > PPC_MEMORY_SIZE || address < 0x10000) return false;
    if(address>=0xa0000000u && address<0xfffff000u)
        return protectPhysical(physicalAddress(address),size,PAGE_READWRITE);
    return VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
uint32_t Memory::allocate(uint32_t size, uint32_t alignment, uint32_t regionBase, uint64_t regionEnd) {
    std::lock_guard lock(mutex_);
    if (!size || !alignment || (alignment & (alignment - 1))) return 0;
    alignment = (std::max)(4096u, alignment);
    uint64_t start = (uint64_t(regionBase) + alignment - 1) & ~uint64_t(alignment - 1);
    uint64_t length = (uint64_t(size) + 4095) & ~uint64_t(4095);
    uint64_t used = 0;
    for (const auto& [address, bytes] : allocations_) used += bytes;
    if (used + length + PPC_IMAGE_SIZE + 0x1000000 > 512ull * 1024 * 1024) return 0;
    if(regionBase>=0xa0000000u && regionEnd<=0xfffff000ull) {
        // Search common physical offsets so allocations in different aliases
        // cannot overlap. Keep the caller's virtual alignment, including E's
        // 4 KiB bias, and preserve exact-region fixture allocations.
        std::map<uint32_t,uint32_t> physical;
        for(const auto& [address,bytes]:allocations_)
            if(address>=0xa0000000u)physical.emplace(physicalAddress(address),bytes);
        const uint32_t alias=regionBase & 0xe0000000u;
        const uint32_t bias=alias==0xe0000000u?0x1000u:0u;
        uint64_t offset=start-alias+bias;
        for(const auto& [address,bytes]:physical) {
            if(uint64_t(address)+bytes<=offset)continue;
            if(offset+length<=address)break;
            offset=((uint64_t(address)+bytes-bias+alignment-1)&~uint64_t(alignment-1))+bias;
        }
        start=alias+offset-bias;
        if(start+length>regionEnd || offset+length>0x20000000 || !commit(uint32_t(start),uint32_t(length)))return 0;
        memset(base_+start,0,size_t(length));
        allocations_.emplace(uint32_t(start),uint32_t(length));
        return uint32_t(start);
    }
    for (const auto& [address, bytes] : allocations_) {
        if (uint64_t(address) + bytes <= start) continue;
        if (start + length <= address) break;
        start = (uint64_t(address) + bytes + alignment - 1) & ~uint64_t(alignment - 1);
    }
    if (start + length > regionEnd || !commit(uint32_t(start), uint32_t(length))) return 0;
    allocations_.emplace(uint32_t(start), uint32_t(length));
    return uint32_t(start);
}
void Memory::forgetThreadStorage(uint32_t address, uint32_t size) {
    // Allocation release holds mutex_, also used by TLS clearing and setup.
    for (auto it = threadTls_.lower_bound(address);
         it != threadTls_.end() && uint64_t(it->first) < uint64_t(address) + size;)
        it = threadTls_.erase(it);
}
bool Memory::releaseContaining(uint32_t address) {
    std::lock_guard lock(mutex_);
    auto it = allocations_.upper_bound(address);
    if (it == allocations_.begin()) return false;
    --it;
    if (uint64_t(address) >= uint64_t(it->first) + it->second) return false;
    uint32_t base = it->first;
    if (base>=0xa0000000u ? !protectPhysical(physicalAddress(base),it->second,PAGE_NOACCESS) :
        !VirtualFree(base_ + base, it->second, MEM_DECOMMIT)) return false;
    forgetThreadStorage(base, it->second);
    allocations_.erase(it);
    return true;
}
uint32_t Memory::allocationSize(uint32_t address) {
    std::lock_guard lock(mutex_);
    auto it = allocations_.find(address);
    return it == allocations_.end() ? 0 : it->second;
}
uint32_t Memory::allocatedBytes() {
    std::lock_guard lock(mutex_);
    uint32_t bytes = 0;
    for (const auto& entry : allocations_) bytes += entry.second;
    return bytes;
}
bool Memory::release(uint32_t address) {
    std::lock_guard lock(mutex_);
    auto it = allocations_.find(address);
    if (it == allocations_.end())return false;
    if(address>=0xa0000000u ? !protectPhysical(physicalAddress(address),it->second,PAGE_NOACCESS) :
       !VirtualFree(base_+address,it->second,MEM_DECOMMIT))return false;
    forgetThreadStorage(address, it->second);
    allocations_.erase(it);
    return true;
}
uint32_t Memory::read32(uint32_t address) const { return _byteswap_ulong(*reinterpret_cast<uint32_t*>(base_ + address)); }
void Memory::write32(uint32_t address, uint32_t value) { *reinterpret_cast<uint32_t*>(base_ + address) = _byteswap_ulong(value); }
namespace {
constexpr uint32_t kXmaContexts = 320;
constexpr uint32_t kXmaContextBytes = 64;
}
uint32_t Memory::xmaPoolBase() {
    std::lock_guard lock(mutex_);
    if (!xmaPool_) {
        // Keep the decoder contexts outside the precommitted title arena and
        // general heap. VirtualQuery walks the whole contiguous protection
        // region: a context in the 512 MiB arena cost ~1.35 ms per decode on
        // the test host, starving voices as their count grew. Reserved gaps
        // on both sides bound the existing permission checks to these 20 KiB.
        constexpr uint32_t poolBegin = 0x60010000u;
        xmaPool_ = allocate(kXmaContexts * kXmaContextBytes, 0x1000,
                            poolBegin, poolBegin + kXmaContexts * kXmaContextBytes);
        if (!xmaPool_) return 0;
        memset(base_ + xmaPool_, 0, kXmaContexts * kXmaContextBytes);
        memset(xmaUsed_, 0, sizeof(xmaUsed_));
    }
    return xmaPool_;
}
uint32_t Memory::xmaCreate() {
    std::lock_guard lock(mutex_);
    uint32_t pool = xmaPoolBase();
    if (!pool) return 0;
    for (uint32_t i = 0; i < kXmaContexts; ++i) {
        if (xmaUsed_[i]) continue;
        xmaUsed_[i] = true;
        uint32_t context = pool + i * kXmaContextBytes;
        memset(base_ + context, 0, kXmaContextBytes);
        return context;
    }
    return 0;
}
bool Memory::xmaFree(uint32_t context) {
    std::lock_guard lock(mutex_);
    if (!xmaPool_ || context < xmaPool_ || context >= xmaPool_ + kXmaContexts * kXmaContextBytes ||
        (context - xmaPool_) % kXmaContextBytes)
        return false;
    uint32_t slot = (context - xmaPool_) / kXmaContextBytes;
    if (!xmaUsed_[slot]) return false;
    if (xmaBridge_) xmaBridge_->reset(slot);
    xmaUsed_[slot] = false;
    memset(base_ + context, 0, kXmaContextBytes);
    return true;
}

bool Memory::xmaOwned(uint32_t context) {
    std::lock_guard lock(mutex_);
    return xmaPool_ && context >= xmaPool_ && context < xmaPool_ + kXmaContexts*kXmaContextBytes &&
        (context-xmaPool_) % kXmaContextBytes == 0 && xmaUsed_[(context-xmaPool_)/kXmaContextBytes];
}
const char* Memory::xmaDecode(uint32_t context) {
    std::lock_guard lock(mutex_);
    if (!xmaOwned(context))
        return "context is not owned by this address space";
    if (!xmaBridge_) xmaBridge_ = std::make_unique<XmaBridge>();
    return xmaBridge_->decode(*this, context, (context-xmaPool_)/kXmaContextBytes);
}
const char* Memory::xmaDecodeBatch(std::span<const uint32_t> contexts, uint32_t& failedContext) {
    std::lock_guard lock(mutex_);
    failedContext = 0;
    if (contexts.empty()) return nullptr;
    if (!xmaOwned(contexts.front())) {
        failedContext = contexts.front();
        return "context is not owned by this address space";
    }
    if (!xmaBridge_) xmaBridge_ = std::make_unique<XmaBridge>();
    return xmaBridge_->decodeBatch(*this, contexts, failedContext);
}
const char* Memory::xmaDecodeRecords(uint32_t records, uint32_t count, uint32_t& failedContext) {
    std::lock_guard lock(mutex_);
    failedContext = 0;
    if (!count) return nullptr;
    const uint32_t firstContext = read32(records + 64);
    if (!xmaOwned(firstContext)) {
        failedContext = firstContext;
        return "context is not owned by this address space";
    }
    if (!xmaBridge_) xmaBridge_ = std::make_unique<XmaBridge>();
    return xmaBridge_->decodeRecords(*this, records, count, firstContext, failedContext);
}
void Memory::xmaReset(uint32_t context) {
    std::lock_guard lock(mutex_);
    if (xmaBridge_ && xmaPool_ && context >= xmaPool_ && context < xmaPool_ + kXmaContexts*kXmaContextBytes &&
        (context-xmaPool_) % kXmaContextBytes == 0)
        xmaBridge_->reset((context-xmaPool_)/kXmaContextBytes);
}

static std::string digest(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE provider = nullptr;
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    uint8_t result[32];
    auto status = BCryptHash(provider, nullptr, 0, const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), result, sizeof(result));
    BCryptCloseAlgorithmProvider(provider, 0);
    if (status < 0) throw std::runtime_error("Cannot verify game image hash");
    char hex[65] = {};
    for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", result[i]);
    return hex;
}
void Memory::load(const std::filesystem::path& gameDir) {
    gameDir_ = std::filesystem::weakly_canonical(gameDir);
    std::ifstream file(gameDir / "basefile.exe", std::ios::binary);
    if (!file) throw std::runtime_error("Game directory must contain the original basefile.exe memory image");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (digest(bytes) != kGameImageSha256) throw std::runtime_error("Game image differs from the image used for AOT generation");
    if (bytes.size() > PPC_IMAGE_SIZE || !commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE))
        throw std::runtime_error("Cannot map the game image");
    memcpy(base_ + PPC_IMAGE_BASE, bytes.data(), bytes.size());
    std::ifstream xexFile(gameDir / "_uncrypted.xex", std::ios::binary);
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(xexFile)), {});
    if (digest(xex) != kGameXexSha256) throw std::runtime_error("XEX differs from the AOT input");
    headerSize_ = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(xex.data() + 8));
    if (headerSize_ > xex.size() || headerSize_ > 0x100000 || !commit(xexHeader, headerSize_))
        throw std::runtime_error("Invalid XEX header size");
    memcpy(base_ + xexHeader, xex.data(), headerSize_);
    uint32_t tlsHeader = headerField(0x20104);
    if (!tlsHeader || read32(tlsHeader) != 64) throw std::runtime_error("Unsupported XEX TLS slot count");
    uint32_t rawAddress = read32(tlsHeader + 4), size = read32(tlsHeader + 8), rawSize = read32(tlsHeader + 12);
    if (size > threadObjectOffset - 0xab0 - 256 || rawSize > size ||
        rawAddress < PPC_IMAGE_BASE || uint64_t(rawAddress) + rawSize > uint64_t(PPC_IMAGE_BASE) + PPC_IMAGE_SIZE)
        throw std::runtime_error("Invalid XEX static TLS template");
    tlsInitial_.resize(size);
    memcpy(tlsInitial_.data(), base_ + rawAddress, rawSize);
    if (!commit(PPC_IMAGE_BASE + PPC_IMAGE_SIZE, uint32_t(PPC_CODE_SIZE * 2)))
        throw std::runtime_error("Cannot map native function dispatch table");
    size_t count = 0;
    for (auto* entry = PPCFuncMappings; entry->host; ++entry) {
        if ((entry->guest & 3) || entry->guest < PPC_CODE_BASE || entry->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
            throw std::runtime_error("Invalid address in AOT function manifest");
        PPC_LOOKUP_FUNC(base_, entry->guest) = entry->host;
        ++count;
    }
    printf("[AOT] Verified game image; mapped %zu native functions.\n", count);
}
uint32_t Memory::headerField(uint32_t key) const {
    uint32_t count = read32(xexHeader + 20);
    if (uint64_t(count) * 8 + 24 > headerSize_) throw std::runtime_error("Invalid XEX optional header count");
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entry = xexHeader + 24 + i * 8;
        if (read32(entry) != key) continue;
        if ((key & 0xff) <= 1) return entry + 4;
        uint32_t offset = read32(entry + 4);
        if (offset >= headerSize_) throw std::runtime_error("XEX field lies outside its header");
        return xexHeader + offset;
    }
    return 0;
}
void Memory::initThread(PPCContext& ctx) {
    ctx = PPCContext{};
    // Guarded guest stack and a separate PCR/TLS/TEB allocation.
    if (!commit(0x70000000, 0x01000000) || !commit(0x7ff00000, 0x10000))
        throw std::runtime_error("Cannot allocate the main guest thread");
    ctx.r1.u64 = 0x70ffff00;
    ctx.r13.u64 = 0x7ff00000;
    initThreadStorage(0x7ff00000, 0x70000000, 0x1000000, GetCurrentThreadId());
    ctx.fpscr.loadFromHost();
    currentContext = &ctx;
}
void Memory::initThreadStorage(uint32_t pcr, uint32_t stackLow, uint32_t stackSize, uint32_t id) {
    std::lock_guard lock(mutex_);
    uint32_t tls = pcr + 0xab0, thread = pcr + threadObjectOffset;
    memset(base_ + pcr, 0, 0x1000);
    memcpy(base_ + tls, tlsInitial_.data(), tlsInitial_.size());
    write32(pcr, tls);
    write32(pcr + 0x100, thread);
    write32(thread + 0x5c, stackLow + stackSize);
    write32(thread + 0x60, stackLow);
    write32(thread + 0x68, tls);
    write32(thread + 0x14c, id);
    threadTls_[pcr] = tls + uint32_t(tlsInitial_.size());
}
uint32_t Memory::dynamicTls(const PPCContext& ctx) const {
    return read32(ctx.r13.u32) + uint32_t(tlsInitial_.size());
}
void Memory::clearDynamicTls(uint32_t index) {
    if (index >= 64) throw std::out_of_range("Invalid dynamic TLS slot");
    std::lock_guard lock(mutex_);
    // A recycled index belongs to a new owner in every guest thread, including
    // code that reads the slot directly instead of calling KeTlsGetValue.
    for (const auto& [pcr, tls] : threadTls_) write32(tls + index * 4, 0);
}
void printContext(const PPCContext& ctx) {
    fprintf(stderr, "guest function=0x%08X lr=0x%08X sp=0x%08X r3=0x%08X r4=0x%08X\n",
            ctx.lastFunction, uint32_t(ctx.lr), ctx.r1.u32, ctx.r3.u32, ctx.r4.u32);
    fprintf(stderr, "r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X\n",
            ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
    fprintf(stderr, "r11=0x%08X r13=0x%08X r28=0x%08X r29=0x%08X r30=0x%08X r31=0x%08X\n",
            ctx.r11.u32, ctx.r13.u32, ctx.r28.u32, ctx.r29.u32, ctx.r30.u32, ctx.r31.u32);
    uint32_t count = (std::min)(ctx.traceIndex, 64u);
    fputs("recent guest entries:", stderr);
    for (uint32_t i = 0; i < count; ++i) fprintf(stderr, " %08X", ctx.trace[(ctx.traceIndex - count + i) & 63]);
    fputc('\n', stderr);
    fflush(stderr);
}
LONG exceptionFilter(EXCEPTION_POINTERS* exception) {
    _lock_file(stderr);
    auto* record = exception->ExceptionRecord;
    fprintf(stderr, "[FAULT] Windows exception 0x%08lX at %p", record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        uintptr_t address = record->ExceptionInformation[1];
        fprintf(stderr, "; access=%llu address=%p", record->ExceptionInformation[0], reinterpret_cast<void*>(address));
        if (memory) {
            uintptr_t base = uintptr_t(memory->base());
            if (address >= base && address - base < PPC_MEMORY_SIZE)
                fprintf(stderr, " guest=0x%08X", uint32_t(address - base));
            fprintf(stderr, " (guest-base=%p)", reinterpret_cast<void*>(base));
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)))
                fprintf(stderr, " mem-state=0x%lX protect=0x%lX type=0x%lX",
                    info.State, info.Protect, info.Type);
        }
    }
    if (exception->ContextRecord) {
        auto* rip = reinterpret_cast<const unsigned char*>(exception->ContextRecord->Rip);
        fprintf(stderr, " rip-bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
            rip[0], rip[1], rip[2], rip[3], rip[4], rip[5], rip[6], rip[7]);
        fprintf(stderr, " host-rcx=%p host-rdx=%p host-r8=%p host-r9=%p",
            reinterpret_cast<void*>(exception->ContextRecord->Rcx),
            reinterpret_cast<void*>(exception->ContextRecord->Rdx),
            reinterpret_cast<void*>(exception->ContextRecord->R8),
            reinterpret_cast<void*>(exception->ContextRecord->R9));
    }
    fputc('\n', stderr);
    if (currentContext) printContext(*currentContext);
    if (exception->ContextRecord) {
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = exception->ContextRecord->Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = exception->ContextRecord->Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = exception->ContextRecord->Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        CONTEXT copy = *exception->ContextRecord;
        for (int i = 0; i < 8; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &copy, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) break;
            char symbolBuffer[sizeof(SYMBOL_INFO) + 256]{};
            auto* symbol = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 256;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                fprintf(stderr, " host-stack[%d] %s+0x%llX\\n", i, symbol->Name, displacement);
        }
    }
    _unlock_file(stderr);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

[[noreturn]] void PPCRecompFailure(const PPCContext& ctx, uint32_t address, const char* reason) {
    _lock_file(stderr);
    fprintf(stderr, "[UNSUPPORTED] guest=0x%08X: %s\n", address, reason);
    DarkRecomp::Native::printContext(ctx);
    fflush(stderr);
    ExitProcess(2);
}
uint64_t PPCQueryTimebase() {
    static const uint64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return uint64_t(now.QuadPart / frequency) * DarkRecomp::Native::kTimebaseFrequency +
        (uint64_t(now.QuadPart) % frequency) * DarkRecomp::Native::kTimebaseFrequency / frequency;
}
extern "C" float roundevenf(float x) { return std::nearbyintf(x); }
extern "C" double roundeven(double x) { return std::nearbyint(x); }
