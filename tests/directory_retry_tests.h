#pragma once
#include <winternl.h>

static void testDirectoryRetry(PPCContext& ctx) {
    auto* base=memory->base();
    const uint32_t scratch=memory->allocate(4096);
    check(scratch!=0,"Directory retry fixture allocation failed");
    const uint32_t ios=scratch+64,out=scratch+1024,pattern=scratch+128;
    const std::string folder="directory-retry-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
    const auto root=memory->gameDirectory().parent_path()/"build_native/run/cache"/folder;
    check(std::filesystem::create_directories(root),"Directory retry fixture already exists");
    const std::wstring names[]{L"entry-a",L"entry-b-with-a-long-filename.dat",L"entry-c-\u00e9-\u20ac.dat"};
    for(const auto& name:names) {
        std::ofstream stream(root/name,std::ios::binary);stream<<"directory retry fixture";
        check(bool(stream),"Cannot create directory retry entry");
    }
    auto utf8=[](const std::wstring& value) {
        const int count=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
        std::string result(count,'\0');
        WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),count,nullptr,nullptr);
        return result;
    };
    using Query=NTSTATUS (NTAPI*)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,
        ULONG,FILE_INFORMATION_CLASS,BOOLEAN,PUNICODE_STRING,BOOLEAN);
    auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryDirectoryFile"));
    check(query!=nullptr,"Native directory query unavailable");
    struct Step {uint32_t capacity;bool restart=false,move=false;};
    const std::vector<Step> scenarios[]{
        {{4},{4},{256},{256},{256},{256}},
        {{256},{4},{8},{256},{256},{256}},
        {{256},{4},{4,true},{256},{256},{256},{256}},
        {{256},{4},{256,false,true},{256},{256}},
        {{256},{4},{256,true},{256}}
    };
    const char* labels[]{"initial-overflow","subsequent-overflow","restart-pending","alias-after-close","restart-new-filter"};
    unsigned failures=0;
    for(unsigned scenario=0;scenario<5;++scenario) {
        const std::string path="cache:\\"+folder;
        std::memcpy(base+scratch+256,path.c_str(),path.size()+1);
        memory->write32(scratch,0xfffffffd);memory->write32(scratch+4,scratch+16);memory->write32(scratch+8,0x40);
        memory->write32(scratch+16,uint32_t(path.size()<<16)|uint32_t(path.size()+1));
        memory->write32(scratch+20,scratch+256);
        ctx.r3.u64=scratch+80;ctx.r4.u64=GENERIC_READ|SYNCHRONIZE;ctx.r5.u64=scratch;
        ctx.r6.u64=ios;ctx.r7.u64=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;ctx.r8.u64=0x21;
        __imp__NtOpenFile(ctx,base);check(ctx.r3.u32==0,"Cannot open guest directory retry fixture");
        uint32_t guest=memory->read32(scratch+80);
        HANDLE native=CreateFileW(root.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                  nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        check(native!=INVALID_HANDLE_VALUE,"Cannot open native directory retry fixture");
        unsigned cursor=0,stepIndex=0;
        for(const auto step:scenarios[scenario]) {
            if(step.move) {
                HANDLE alias=nullptr;
                check(DuplicateHandle(GetCurrentProcess(),native,GetCurrentProcess(),&alias,0,FALSE,DUPLICATE_SAME_ACCESS),
                      "Cannot duplicate native directory cursor");
                CloseHandle(native);native=alias;
                ctx.r3.u64=guest;ctx.r4.u64=scratch+80;ctx.r5.u64=1;
                __imp__NtDuplicateObject(ctx,base);check(ctx.r3.u32==0,"Cannot move guest directory cursor");
                guest=memory->read32(scratch+80);
            }
            if(step.restart)cursor=scenario==4?3:0;
            // Ordinary retries ignore filter changes. NTFS can replace the
            // filter on an explicit restart, including a search with no hits.
            const char* filter=stepIndex==0 || (step.restart && scenario!=4)?"entry-*":"missing-*";
            std::wstring wideFilter(filter,filter+std::strlen(filter));
            UNICODE_STRING match{USHORT(wideFilter.size()*2),USHORT(wideFilter.size()*2),wideFilter.data()};
            alignas(8) uint8_t bytes[1024];std::memset(bytes,0xa5,sizeof(bytes));
            IO_STATUS_BLOCK result{};
            const uint32_t status=uint32_t(query(native,nullptr,nullptr,nullptr,&result,bytes,
                64+step.capacity*2,FILE_INFORMATION_CLASS(1),TRUE,&match,step.restart));
            const std::string expected=cursor<3?utf8(names[cursor]):"";
            const uint32_t expectedStatus=cursor==3?0x80000006:step.capacity<expected.size()?0x80000005:0;
            check(status==expectedStatus,"Native directory retry status disagrees with fixture");
            if(cursor<3) {
                const auto* name=reinterpret_cast<const wchar_t*>(bytes+64);
                const std::string prefix=utf8(std::wstring(name,(result.Information-64)/2));
                check(prefix==expected.substr(0,step.capacity),"Native directory cursor skipped the expected entry");
            }
            memory->write32(pattern,uint32_t(std::strlen(filter)<<16)|uint32_t(std::strlen(filter)+1));
            memory->write32(pattern+4,scratch+512);std::strcpy(reinterpret_cast<char*>(base+scratch+512),filter);
            std::memset(base+out,0xa5,1024);
            ctx.r3.u64=guest;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;ctx.r7.u64=ios;
            ctx.r8.u64=out;ctx.r9.u64=64+step.capacity;ctx.r10.u64=pattern;
            memory->write32(ctx.r1.u32+84,step.restart);
            __imp__NtQueryDirectoryFile(ctx,base);
            const uint32_t written=cursor<3?64+(std::min)(step.capacity,uint32_t(expected.size())):0;
            bool valid=ctx.r3.u32==status && memory->read32(ios)==status && memory->read32(ios+4)==written;
            if(cursor<3) {
                valid=valid && memory->read32(out+60)==expected.size() &&
                    std::memcmp(base+out+64,expected.data(),written-64)==0;
                for(unsigned field=1;field<=6;++field)
                    valid=valid && PPC_LOAD_U64(out+field*8)==reinterpret_cast<const uint64_t*>(bytes)[field];
            }
            for(unsigned i=written;i<1024;++i)valid=valid && base[out+i]==0xa5;
            std::printf("DirectoryRetry[%s/%u] native=%08X guest=%08X bytes=%u expected=%u %s\n",
                labels[scenario],stepIndex,status,ctx.r3.u32,memory->read32(ios+4),written,valid?"passed":"FAILED");
            if(!valid)++failures;
            if(status==0)++cursor;
            ++stepIndex;
        }
        CloseHandle(native);ctx.r3.u64=guest;__imp__NtClose(ctx,base);
        check(ctx.r3.u32==0,"Directory retry handle cleanup failed");
    }
    // These cases need distinct native and guest lengths/statuses. Keep the
    // original five scenarios above unchanged, including their native oracle.
    struct NativeDirectoryResult {
        alignas(8) uint8_t bytes[1024];
        IO_STATUS_BLOCK io{};
        uint32_t status=0;
    };
    for(unsigned bounded=0;bounded<2;++bounded) {
        const char* label=bounded==0?"restart-rejected":"utf8-byte-boundary";
        const std::string path="cache:\\"+folder;
        std::memcpy(base+scratch+256,path.c_str(),path.size()+1);
        memory->write32(scratch,0xfffffffd);memory->write32(scratch+4,scratch+16);memory->write32(scratch+8,0x40);
        memory->write32(scratch+16,uint32_t(path.size()<<16)|uint32_t(path.size()+1));
        memory->write32(scratch+20,scratch+256);
        ctx.r3.u64=scratch+80;ctx.r4.u64=GENERIC_READ|SYNCHRONIZE;ctx.r5.u64=scratch;
        ctx.r6.u64=ios;ctx.r7.u64=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;ctx.r8.u64=0x21;
        __imp__NtOpenFile(ctx,base);check(ctx.r3.u32==0,"Cannot open bounded guest directory fixture");
        const uint32_t guest=memory->read32(scratch+80);
        HANDLE native=CreateFileW(root.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                  nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        check(native!=INVALID_HANDLE_VALUE,"Cannot open bounded native directory fixture");
        unsigned stepIndex=0;
        auto nativeQuery=[&](uint32_t length,bool restart,const char* filter) {
            NativeDirectoryResult reference;
            std::memset(reference.bytes,0xa5,sizeof(reference.bytes));
            std::wstring wideFilter(filter,filter+std::strlen(filter));
            UNICODE_STRING match{USHORT(wideFilter.size()*2),USHORT(wideFilter.size()*2),wideFilter.data()};
            reference.status=uint32_t(query(native,nullptr,nullptr,nullptr,&reference.io,reference.bytes,
                length,FILE_INFORMATION_CLASS(1),TRUE,&match,restart));
            return reference;
        };
        auto fullNativeName=[&](const NativeDirectoryResult& reference) {
            check(reference.status==0 && uint32_t(reference.io.Status)==0,"Native full directory reference failed");
            const uint32_t nameBytes=*reinterpret_cast<const uint32_t*>(reference.bytes+60);
            check(!(nameBytes&1) && nameBytes<=sizeof(reference.bytes)-64 &&
                  reference.io.Information>=64+nameBytes && reference.io.Information<=sizeof(reference.bytes),
                  "Native directory reference did not contain the full name");
            return utf8(std::wstring(reinterpret_cast<const wchar_t*>(reference.bytes+64),nameBytes/2));
        };
        auto guestQuery=[&](const NativeDirectoryResult& reference,uint32_t length,uint32_t expectedStatus,
                            const std::string& name,bool restart,const char* filter) {
            memory->write32(pattern,uint32_t(std::strlen(filter)<<16)|uint32_t(std::strlen(filter)+1));
            memory->write32(pattern+4,scratch+512);std::strcpy(reinterpret_cast<char*>(base+scratch+512),filter);
            memory->write32(ios,0xcccccccc);memory->write32(ios+4,0xcccccccc);
            std::memset(base+out,0xa5,1024);
            ctx.r3.u64=guest;ctx.r4.u64=ctx.r5.u64=ctx.r6.u64=0;ctx.r7.u64=ios;
            ctx.r8.u64=out;ctx.r9.u64=length;ctx.r10.u64=pattern;
            memory->write32(ctx.r1.u32+84,restart);
            __imp__NtQueryDirectoryFile(ctx,base);
            const bool hasRecord=expectedStatus==0 || expectedStatus==0x80000005;
            const uint32_t written=hasRecord?64+(std::min)(length-64,uint32_t(name.size())):0;
            bool valid=ctx.r3.u32==expectedStatus && memory->read32(ios)==expectedStatus && memory->read32(ios+4)==written;
            if(hasRecord) {
                valid=valid && memory->read32(out)==0 &&
                    memory->read32(out+4)==*reinterpret_cast<const uint32_t*>(reference.bytes+4) &&
                    memory->read32(out+56)==*reinterpret_cast<const uint32_t*>(reference.bytes+56) &&
                    memory->read32(out+60)==name.size() && std::memcmp(base+out+64,name.data(),written-64)==0;
                for(unsigned field=1;field<=6;++field)
                    valid=valid && PPC_LOAD_U64(out+field*8)==reinterpret_cast<const uint64_t*>(reference.bytes)[field];
            }
            for(unsigned i=written;i<1024;++i)valid=valid && base[out+i]==0xa5;
            std::printf("DirectoryRetry[%s/%u] native=%08X guest=%08X expected-status=%08X bytes=%u expected=%u %s\n",
                label,stepIndex++,reference.status,ctx.r3.u32,expectedStatus,memory->read32(ios+4),written,valid?"passed":"FAILED");
            if(!valid)++failures;
        };
        auto fullStep=[&](unsigned entry) {
            const char* filter=entry==0?"entry-*":"missing-*";
            const auto reference=nativeQuery(1024,false,filter);
            const auto name=fullNativeName(reference);
            check(name==utf8(names[entry]),"Bounded native directory cursor skipped the expected entry");
            guestQuery(reference,320,0,name,false,filter);
        };
        fullStep(0);
        if(bounded==0) {
            const auto partial=nativeQuery(72,false,"missing-*");
            check(partial.status==0x80000005 && uint32_t(partial.io.Status)==partial.status && partial.io.Information==72 &&
                  *reinterpret_cast<const uint32_t*>(partial.bytes+60)==names[1].size()*2 &&
                  std::wstring(reinterpret_cast<const wchar_t*>(partial.bytes+64),4)==names[1].substr(0,4),
                  "Native short query did not retain the second entry");
            guestQuery(partial,68,0x80000005,utf8(names[1]),false,"missing-*");
            // Both requests are below their ABI's minimum header size. The
            // rejected restart must not replace the filter or rewind the cursor.
            const auto rejected=nativeQuery(71,true,"missing-*");
            check(rejected.status==0xc0000004,"Native undersized restart was not rejected");
            for(const auto byte:rejected.bytes)check(byte==0xa5,"Rejected native restart modified output");
            // Native validation may leave its IOSB untouched; the guest bridge
            // explicitly reports the rejection and zero bytes in its IOSB.
            guestQuery(rejected,63,rejected.status,"",true,"missing-*");
            fullStep(1);
            fullStep(2);
        } else {
            fullStep(1);
            // Fifteen UTF-16 units fit natively, but eighteen UTF-8 bytes do
            // not fit the sixteen-byte guest tail. Native success advances its
            // cursor, so retain this full reference for both guest calls.
            constexpr uint32_t capacity=16;
            const auto reference=nativeQuery(64+capacity*2,false,"missing-*");
            const auto name=fullNativeName(reference);
            check(name==utf8(names[2]) && names[2].size()<=capacity && name.size()>capacity,
                  "Unicode fixture does not straddle the native/guest byte boundary");
            guestQuery(reference,64+capacity,0x80000005,name,false,"missing-*");
            guestQuery(reference,64+uint32_t(name.size()),0,name,false,"missing-*");
        }
        const auto end=nativeQuery(1024,false,"missing-*");
        check(end.status==0x80000006 && end.io.Information==0,"Bounded native directory cursor did not reach EOF");
        for(const auto byte:end.bytes)check(byte==0xa5,"Native directory EOF modified output");
        guestQuery(end,320,end.status,"",false,"missing-*");
        CloseHandle(native);ctx.r3.u64=guest;__imp__NtClose(ctx,base);
        check(ctx.r3.u32==0,"Bounded directory retry handle cleanup failed");
    }
    for(const auto& name:names)check(std::filesystem::remove(root/name),"Directory retry entry cleanup failed");
    check(std::filesystem::remove(root),"Directory retry folder cleanup failed");
    check(memory->release(scratch),"Directory retry memory cleanup failed");
    check(failures==0,"Short directory buffers lost pending entries, including across restart or handle duplication");
}
