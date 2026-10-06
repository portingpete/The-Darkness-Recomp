#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include "runtime/native/stall_profiler.h"

namespace DarkRecomp::ShaderBytecodeCache {
inline uint64_t hashBytes(const void* data,size_t size,uint64_t hash=14695981039346656037ull) noexcept {
    const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i) {hash^=bytes[i];hash*=1099511628211ull;}
    return hash;
}
inline uint64_t contentKey(uint64_t compiler,std::string_view source,std::string_view sourceName,
                           std::string_view entry,std::string_view profile,UINT flags1,UINT flags2,
                           const D3D_SHADER_MACRO* macros=nullptr) noexcept {
    constexpr uint64_t version=2;
    uint64_t hash=hashBytes(&version,sizeof(version));
    auto field=[&](const void* data,size_t bytes) {
        const uint64_t size=bytes;hash=hashBytes(&size,sizeof(size),hash);hash=hashBytes(data,bytes,hash);
    };
    field(&compiler,sizeof(compiler));field(source.data(),source.size());field(sourceName.data(),sourceName.size());
    field(entry.data(),entry.size());field(profile.data(),profile.size());field(&flags1,sizeof(flags1));field(&flags2,sizeof(flags2));
    if(macros)for(auto* macro=macros;macro->Name;++macro) {
        field(macro->Name,std::strlen(macro->Name));
        const bool defined=macro->Definition!=nullptr;field(&defined,sizeof(defined));
        if(defined)field(macro->Definition,std::strlen(macro->Definition));
        else field(nullptr,0);
    }
    return hash;
}
// Hash the exact imported compiler DLL once. If its identity cannot be read,
// bypass the disk cache and compile normally. Driver changes require no key
// change: cached data is compiler DXBC, checked by Create*Shader on each device.
inline std::optional<uint64_t> compilerIdentity() {
    static const auto identity=[]()->std::optional<uint64_t> {
        Native::StallProfiler::Scope stallIo(Native::StallProfiler::Section::FileIO,
                                            "ShaderBytecodeCache compiler DLL read");
        try {
            const auto module=GetModuleHandleW(D3DCOMPILER_DLL_W);
            if(!module)return {};
            std::array<wchar_t,32768> path{};
            const auto length=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
            if(!length || length>=path.size())return {};
            std::ifstream file(std::filesystem::path(path.data()),std::ios::binary);
            if(!file)return {};
            std::array<char,16384> bytes{};uint64_t hash=14695981039346656037ull,size=0;
            while(file) {
                file.read(bytes.data(),bytes.size());const auto count=file.gcount();
                hash=hashBytes(bytes.data(),size_t(count),hash);size+=uint64_t(count);
            }
            if(!file.eof() || !size)return {};
            return hash;
        } catch(...) {return {};}
    }();
    return identity;
}
inline std::optional<uint64_t> key(std::string_view source,std::string_view sourceName,
                                 std::string_view entry,std::string_view profile,UINT flags1,UINT flags2,
                                 const D3D_SHADER_MACRO* macros=nullptr) {
    const auto compiler=compilerIdentity();
    if(!compiler)return {};
    return contentKey(*compiler,source,sourceName,entry,profile,flags1,flags2,macros);
}
class Cache {
    struct Header {std::array<char,8> magic;uint32_t version,bytes;uint64_t key,checksum;};
    static_assert(sizeof(Header)==32);
    static constexpr std::array<char,8> magic{'D','R','S','B','C','0','2','\0'};
    static constexpr size_t limit=1024*1024;
    std::filesystem::path directory_;
public:
    explicit Cache(std::filesystem::path directory):directory_(std::move(directory)) {}
    std::filesystem::path path(uint64_t key) const {
        char name[64]{};std::snprintf(name,sizeof(name),"native_v2_%016llX.cso",key);return directory_/name;
    }
    bool load(uint64_t key,Microsoft::WRL::ComPtr<ID3DBlob>& result) const noexcept {
        Native::StallProfiler::Scope stallIo(Native::StallProfiler::Section::FileIO,
                                            "ShaderBytecodeCache::load", 0, 0, key, "shader cache key");
        try {
            if(directory_.empty())return false;
            const auto target=path(key);std::error_code status;
            const auto size=std::filesystem::file_size(target,status);
            if(status || size<=sizeof(Header) || size>sizeof(Header)+limit)return false;
            std::ifstream file(target,std::ios::binary);Header header{};
            if(!file.read(reinterpret_cast<char*>(&header),sizeof(header)) || header.magic!=magic ||
               header.version!=2 || header.key!=key || !header.bytes || header.bytes>limit || size!=sizeof(Header)+header.bytes)return false;
            Microsoft::WRL::ComPtr<ID3DBlob> blob;
            if(FAILED(D3DCreateBlob(header.bytes,&blob)))return false;
            if(!file.read(static_cast<char*>(blob->GetBufferPointer()),header.bytes) ||
               hashBytes(blob->GetBufferPointer(),blob->GetBufferSize())!=header.checksum)return false;
            result=std::move(blob);return true;
        } catch(...) {return false;}
    }
    bool save(uint64_t key,ID3DBlob* code) const noexcept {
        Native::StallProfiler::Scope stallIo(Native::StallProfiler::Section::FileIO,
                                            "ShaderBytecodeCache::save", 0, 0, key, "shader cache key");
        if(directory_.empty() || !code || !code->GetBufferSize() || code->GetBufferSize()>limit)return false;
        try {
            std::error_code status;std::filesystem::create_directories(directory_,status);if(status)return false;
            static std::atomic<uint64_t> sequence{};
            static const auto run=std::chrono::steady_clock::now().time_since_epoch().count();
            auto staging=path(key);staging+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+
                std::to_wstring(run)+L"."+std::to_wstring(sequence.fetch_add(1,std::memory_order_relaxed))+L".tmp";
            struct StagingFile {
                const std::filesystem::path& path;bool created=false;
                ~StagingFile() {if(created) {std::error_code ignored;std::filesystem::remove(path,ignored);}}
            } cleanup{staging};
            const Header header{magic,2,uint32_t(code->GetBufferSize()),key,hashBytes(code->GetBufferPointer(),code->GetBufferSize())};
            {
                std::ofstream file(staging,std::ios::binary|std::ios::trunc);if(!file)return false;
                cleanup.created=true;
                file.write(reinterpret_cast<const char*>(&header),sizeof(header));
                file.write(static_cast<const char*>(code->GetBufferPointer()),code->GetBufferSize());
                file.close();if(!file)return false;
            }
            // Atomically replace even a corrupt old entry. A failed write or
            // rename leaves the original entry intact and compilation usable.
            const bool saved=MoveFileExW(staging.c_str(),path(key).c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;
            if(saved)cleanup.created=false;
            return saved;
        } catch(...) {return false;}
    }
};
inline const Cache& defaultCache() {
    static const Cache cache([] {
        try {
            std::array<wchar_t,32768> executable{};
            const auto length=GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
            if(length && length<executable.size()) {
                auto directory=std::filesystem::path(executable.data()).parent_path()/"DarkRecompShaderCache";
                std::error_code status;std::filesystem::create_directories(directory,status);if(!status)return directory;
            }
            auto directory=std::filesystem::temp_directory_path()/"DarkRecompShaderCache";
            std::error_code status;std::filesystem::create_directories(directory,status);if(!status)return directory;
        } catch(...) {}
        return std::filesystem::path{};
    }());
    return cache;
}
}
