#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DarkRecomp::Native::Storage {
constexpr uint32_t kSaveDeviceId = 1;
constexpr uint32_t kContentSavedGame = 1;

struct ContentInfo {
    uint32_t deviceId = kSaveDeviceId;
    uint32_t contentType = kContentSavedGame;
    std::u16string displayName;
    std::string fileName;
};

std::filesystem::path SaveRoot();
void SetSaveRootOverride(const std::filesystem::path& path);
void ClearSaveRootOverride();
bool ValidRootName(std::string_view name);
bool ValidFileName(std::string_view name);
std::filesystem::path ContentDir(const ContentInfo& content);
bool ReadContentMeta(const std::filesystem::path& dir, ContentInfo& out);
bool WriteContentMeta(const std::filesystem::path& dir, const ContentInfo& content);
bool EnsureSaveRoot(std::error_code& ec);
uint32_t DeviceState(uint32_t deviceId);
uint32_t EnumerateChecked(uint32_t contentType, std::vector<ContentInfo>& out);
uint32_t CreateContent(std::string_view rootAlias, const ContentInfo& content, uint32_t flags,
                       uint32_t* outDisposition, std::filesystem::path* outPath);
uint32_t OpenExisting(std::string_view rootAlias, const ContentInfo& content);
uint32_t CloseContent(std::string_view rootAlias);
uint32_t DeleteContent(const ContentInfo& content);
std::optional<std::filesystem::path> LookupMount(std::string_view aliasLower);
void ClearMounts();
}  // namespace DarkRecomp::Native::Storage
