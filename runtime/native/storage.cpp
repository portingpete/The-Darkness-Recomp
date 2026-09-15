#include "storage.h"
#include "runtime.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <unordered_map>

namespace DarkRecomp::Native::Storage {
namespace {
// mutex guards mounts + overrideRoot snapshots. opMutex serializes whole
// content operations (create/open/delete) so concurrent creates of one alias
// cannot both pass the alias check and silently replace each other.
// Lock order is always opMutex -> mutex; metadata helpers only take mutex.
std::mutex mutex;
std::mutex opMutex;
std::unordered_map<std::string, std::filesystem::path> mounts;
std::optional<std::filesystem::path> overrideRoot;
std::atomic<uint32_t> tempCounter{0};

std::string lowerOf(std::string_view value) {
    std::string out(value);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return out;
}

bool reservedAlias(const std::string& lower) {
    return lower == "game" || lower == "dvd" || lower == "d" || lower == "cache" ||
           lower == "cache0" || lower == "device" || lower == "cdrom0";
}

bool insideRoot(const std::wstring& rootCanon, const std::wstring& dirCanon) {
    if (dirCanon.size() <= rootCanon.size()) return false;
    if (_wcsnicmp(dirCanon.c_str(), rootCanon.c_str(), rootCanon.size()) != 0) return false;
    return dirCanon[rootCanon.size()] == L'\\';
}

bool containedDirLocked(const std::filesystem::path& root, const std::filesystem::path& dir,
                        std::filesystem::path* canonicalDir) {
    if (root.empty() || dir.empty()) return false;
    std::error_code ec;
    std::wstring rootCanon = std::filesystem::weakly_canonical(root, ec).wstring();
    if (ec) return false;
    std::wstring dirCanon = std::filesystem::weakly_canonical(dir, ec).wstring();
    if (ec) return false;
    if (!insideRoot(rootCanon, dirCanon)) return false;
    if (canonicalDir) *canonicalDir = std::filesystem::weakly_canonical(dir, ec);
    return !ec;
}

// Confinement for the metadata files themselves, not just the container:
// the meta/tmp path must not be a symlink/reparse point and its canonical
// parent must equal the canonical container directory.
bool confinedMetaLocked(const std::filesystem::path& canonicalDir, const std::filesystem::path& dir,
                        const char* fileName) {
    std::error_code ec;
    std::filesystem::path filePath = dir / fileName;
    // An absent path is the expected creation case, not an error; only genuine
    // filesystem errors or an actual reparse point reject the target.
    bool isLink = std::filesystem::is_symlink(filePath, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    if (isLink) return false;
    std::filesystem::path fileCanon = std::filesystem::weakly_canonical(filePath, ec);
    if (ec) return false;
    std::error_code cmp;
    bool same = std::filesystem::equivalent(fileCanon.parent_path(), canonicalDir, cmp);
    return !cmp && same;
}

void eraseMountsForDirLocked(const std::filesystem::path& dir) {
    std::error_code ec;
    std::wstring target = std::filesystem::weakly_canonical(dir, ec).wstring();
    if (ec) return;
    for (auto it = mounts.begin(); it != mounts.end();) {
        std::wstring mapped = std::filesystem::weakly_canonical(it->second, ec).wstring();
        if (!ec && _wcsicmp(mapped.c_str(), target.c_str()) == 0) it = mounts.erase(it);
        else ++it;
    }
}

bool clearContainer(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return false;
    bool ok = true;
    for (const auto& entry : it) {
        if (ec) {
            ok = false;
            break;
        }
        std::filesystem::remove_all(entry.path(), ec);
        if (ec) ok = false;
    }
    return ok;
}
}  // namespace

std::filesystem::path SaveRoot() {
    std::lock_guard lock(mutex);
    if (overrideRoot) return *overrideRoot;
    std::filesystem::path game = memory ? memory->gameDirectory() : std::filesystem::path();
    if (game.empty()) return std::filesystem::path();
    return game.parent_path() / "saves";
}

void SetSaveRootOverride(const std::filesystem::path& path) {
    std::lock_guard lock(mutex);
    overrideRoot = path;
}

void ClearSaveRootOverride() {
    std::lock_guard lock(mutex);
    overrideRoot.reset();
}

bool ValidRootName(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    if (name == "." || name == "..") return false;
    if (reservedAlias(lowerOf(name))) return false;
    return true;
}

bool ValidFileName(std::string_view name) {
    if (name.empty() || name.size() > 42) return false;
    if (name.front() == '\0') return false;
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    if (name == "." || name == "..") return false;
    return true;
}

std::filesystem::path ContentDir(const ContentInfo& content) {
    std::lock_guard lock(mutex);
    std::filesystem::path root;
    if (overrideRoot) root = *overrideRoot;
    else if (memory && !memory->gameDirectory().empty()) root = memory->gameDirectory().parent_path() / "saves";
    if (root.empty()) return {};
    char prefix[16]{};
    snprintf(prefix, sizeof(prefix), "%08X_", content.contentType);
    return root / (std::string(prefix) + content.fileName);
}

bool WriteContentMeta(const std::filesystem::path& dir, const ContentInfo& content) {
    std::filesystem::path root;
    {
        std::lock_guard lock(mutex);
        if (overrideRoot) root = *overrideRoot;
        else if (memory && !memory->gameDirectory().empty()) root = memory->gameDirectory().parent_path() / "saves";
    }
    std::filesystem::path canonicalDir;
    if (!containedDirLocked(root, dir, &canonicalDir)) return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return false;
    // Serialize the payload first so the file write is a single checked span.
    std::string blob;
    blob.append("XCS1", 4);
    uint32_t device = content.deviceId, type = content.contentType;
    blob.append(reinterpret_cast<const char*>(&device), 4);
    blob.append(reinterpret_cast<const char*>(&type), 4);
    uint32_t dlen = static_cast<uint32_t>(content.displayName.size());
    if (dlen > 128) dlen = 128;
    blob.append(reinterpret_cast<const char*>(&dlen), 4);
    for (uint32_t i = 0; i < dlen; ++i) {
        uint16_t c = static_cast<uint16_t>(content.displayName[i]);
        blob.append(reinterpret_cast<const char*>(&c), 2);
    }
    uint32_t flen = static_cast<uint32_t>(content.fileName.size());
    blob.append(reinterpret_cast<const char*>(&flen), 4);
    blob.append(content.fileName.data(), flen);
    // Unique temp with real fail-if-exists creation: CreateFileW(CREATE_NEW)
    // never truncates or follows a preexisting file; absent is the expected
    // creation case, other errors abort. Cleanup touches only this temp.
    uint32_t pid = GetCurrentProcessId();
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path tmp;
    for (int attempt = 0; attempt < 64; ++attempt) {
        uint32_t n = tempCounter.fetch_add(1);
        char name[64]{};
        snprintf(name, sizeof(name), "_xcontent.meta.tmp.%lu.%lu", static_cast<unsigned long>(pid),
                 static_cast<unsigned long>(n));
        std::filesystem::path candidate = dir / name;
        std::error_code linkEc;
        bool isLink = std::filesystem::is_symlink(candidate, linkEc);
        if (linkEc && linkEc != std::errc::no_such_file_or_directory) return false;
        if (isLink) return false;
        if (!confinedMetaLocked(canonicalDir, dir, name)) return false;
        handle = CreateFileW(candidate.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            tmp = candidate;
            break;
        }
        DWORD createError = GetLastError();
        if (createError != ERROR_ALREADY_EXISTS && createError != ERROR_FILE_EXISTS) return false;
    }
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool wrote = WriteFile(handle, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr) &&
                 written == blob.size() && FlushFileBuffers(handle);
    BOOL closed = CloseHandle(handle);
    if (!wrote || !closed) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (!confinedMetaLocked(canonicalDir, dir, "_xcontent.meta")) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    // Atomic replacement with write-through so updating an existing metadata
    // file works on Windows; the previous metadata survives any failure here.
    std::filesystem::path metaPath = dir / "_xcontent.meta";
    if (!MoveFileExW(tmp.wstring().c_str(), metaPath.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadContentMeta(const std::filesystem::path& dir, ContentInfo& out) {
    std::filesystem::path root;
    {
        std::lock_guard lock(mutex);
        if (overrideRoot) root = *overrideRoot;
        else if (memory && !memory->gameDirectory().empty()) root = memory->gameDirectory().parent_path() / "saves";
    }
    std::filesystem::path canonicalDir;
    if (!containedDirLocked(root, dir, &canonicalDir)) return false;
    if (!confinedMetaLocked(canonicalDir, dir, "_xcontent.meta")) return false;
    std::ifstream file(dir / "_xcontent.meta", std::ios::binary);
    if (!file) return false;
    char magic[4]{};
    file.read(magic, 4);
    if (!file || memcmp(magic, "XCS1", 4) != 0) return false;
    uint32_t device = 0, type = 0, dlen = 0;
    file.read(reinterpret_cast<char*>(&device), 4);
    file.read(reinterpret_cast<char*>(&type), 4);
    file.read(reinterpret_cast<char*>(&dlen), 4);
    if (!file || dlen > 128) return false;
    if (device != 0 && device != kSaveDeviceId) return false;
    if (type == 0) return false;
    out.deviceId = device == 0 ? kSaveDeviceId : device;
    out.contentType = type;
    out.displayName.resize(dlen);
    for (uint32_t i = 0; i < dlen; ++i) {
        uint16_t c = 0;
        file.read(reinterpret_cast<char*>(&c), 2);
        if (!file) return false;
        out.displayName[i] = static_cast<char16_t>(c);
    }
    uint32_t flen = 0;
    file.read(reinterpret_cast<char*>(&flen), 4);
    if (!file || flen == 0 || flen > 42) return false;
    out.fileName.resize(flen);
    file.read(out.fileName.data(), flen);
    if (!file || !ValidFileName(out.fileName)) return false;
    return true;
}

bool EnsureSaveRoot(std::error_code& ec) {
    std::filesystem::path root;
    {
        std::lock_guard lock(mutex);
        if (overrideRoot) root = *overrideRoot;
        else if (memory && !memory->gameDirectory().empty()) root = memory->gameDirectory().parent_path() / "saves";
        else return false;
    }
    if (root.empty()) return false;
    std::filesystem::create_directories(root, ec);
    if (ec) return false;
    bool isDir = std::filesystem::is_directory(root, ec);
    if (ec) return false;
    return isDir;
}

uint32_t DeviceState(uint32_t deviceId) {
    if (deviceId != kSaveDeviceId) return 0x48F;
    std::error_code ec;
    if (!EnsureSaveRoot(ec)) {
        std::fprintf(stderr, "[NativeStorage] Save root unavailable (%s)\n", ec.message().c_str());
        return 0x48F;
    }
    return 0;
}

uint32_t EnumerateChecked(uint32_t contentType, std::vector<ContentInfo>& out) {
    out.clear();
    if (contentType == 0) return 0x57;
    std::filesystem::path root;
    {
        std::lock_guard lock(mutex);
        if (overrideRoot) root = *overrideRoot;
        else if (memory && !memory->gameDirectory().empty()) root = memory->gameDirectory().parent_path() / "saves";
        else return 0x48F;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) return 0x48F;
    std::filesystem::directory_iterator it(root, ec);
    if (ec) return 0x48F;
    char want[16]{};
    snprintf(want, sizeof(want), "%08X_", contentType);
    // Skip policy: entries that look like save containers but carry corrupt,
    // missing, or mismatched metadata are skipped (never exposed as saves) and
    // counted here, so malformed stores are distinguishable from empty ones.
    uint32_t skippedMalformed = 0;
    for (const auto& entry : it) {
        if (ec) break;
        std::error_code entryEc;
        if (entry.is_symlink(entryEc) || entryEc) continue;
        if (!entry.is_directory(entryEc) || entryEc) continue;
        std::string name = entry.path().filename().string();
        if (name.size() <= 9 || name.compare(0, 9, want) != 0) continue;
        std::string fileName = name.substr(9);
        if (!ValidFileName(fileName)) {
            ++skippedMalformed;
            continue;
        }
        std::filesystem::path canonicalEntry;
        if (!containedDirLocked(root, entry.path(), &canonicalEntry)) {
            ++skippedMalformed;
            continue;
        }
        ContentInfo meta;
        if (!ReadContentMeta(entry.path(), meta)) {
            ++skippedMalformed;
            continue;
        }
        if (meta.fileName != fileName || meta.contentType != contentType) {
            ++skippedMalformed;
            continue;
        }
        if (meta.deviceId != kSaveDeviceId) {
            ++skippedMalformed;
            continue;
        }
        out.push_back(meta);
    }
    if (skippedMalformed)
        std::fprintf(stderr, "[NativeStorage] Enumerate skipped %u malformed container(s)\n",
                     skippedMalformed);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.fileName < b.fileName; });
    return 0;
}

uint32_t CreateContent(std::string_view rootAlias, const ContentInfo& content, uint32_t flags,
                       uint32_t* outDisposition, std::filesystem::path* outPath) {
    if (!ValidRootName(rootAlias) || !ValidFileName(content.fileName)) return 0x57;
    if (content.deviceId != 0 && content.deviceId != kSaveDeviceId) return 0x48F;
    if (content.contentType == 0) return 0x57;
    uint32_t mode = flags & 0xF;
    if (mode < 1 || mode > 5) return 0x57;
    // Serialize the whole operation: the alias check, disk mutation, and mount
    // publication are atomic with respect to other content operations, so two
    // concurrent creates of one alias cannot both pass and silently replace.
    std::lock_guard opLock(opMutex);
    std::string alias = lowerOf(rootAlias);
    char prefix[16]{};
    snprintf(prefix, sizeof(prefix), "%08X_", content.contentType);
    std::filesystem::path root;
    {
        std::lock_guard lock(mutex);
        root = overrideRoot ? *overrideRoot
            : (!memory || memory->gameDirectory().empty() ? std::filesystem::path()
                                                          : memory->gameDirectory().parent_path() / "saves");
        if (!root.empty()) {
            auto it = mounts.find(alias);
            if (it != mounts.end()) {
                std::error_code ec;
                std::wstring want = std::filesystem::weakly_canonical(root / (std::string(prefix) + content.fileName), ec).wstring();
                if (ec) return 0x57;
                std::wstring have = std::filesystem::weakly_canonical(it->second, ec).wstring();
                if (ec || _wcsicmp(want.c_str(), have.c_str()) != 0) return 0x57;
            }
        }
    }
    if (root.empty()) return 0x48F;
    std::filesystem::path dir = root / (std::string(prefix) + content.fileName);
    if (!containedDirLocked(root, dir, nullptr)) return 0x57;
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) return 0x48F;
    {
        std::error_code dirEc;
        bool rootIsDir = std::filesystem::is_directory(root, dirEc);
        if (dirEc || !rootIsDir) return 0x48F;
    }
    if (!containedDirLocked(root, dir, nullptr)) return 0x57;
    // A truly absent directory is the expected create case (ENOENT), not an
    // access failure: only genuine errors become 0x05.
    std::error_code existsEc;
    bool exists = std::filesystem::is_directory(dir, existsEc);
    if (existsEc) {
        if (existsEc == std::errc::no_such_file_or_directory) {
            existsEc.clear();
            exists = false;
        } else {
            return 0x05;
        }
    }
    if (exists) {
        std::filesystem::path canonicalDir;
        if (!containedDirLocked(root, dir, &canonicalDir)) return 0x57;
        std::error_code dirEc;
        if (std::filesystem::is_symlink(dir, dirEc) || dirEc) return 0x57;
        if (mode == 3 || mode == 4) {
            ContentInfo meta;
            if (!ReadContentMeta(dir, meta) || meta.fileName != content.fileName ||
                meta.contentType != content.contentType || meta.deviceId != kSaveDeviceId)
                return 0x03;
        }
    }
    uint32_t disposition = 0;
    uint32_t result = 0;
    switch (mode) {
        case 1:
            if (exists) result = 0xB7;
            else disposition = 1;
            break;
        case 2:
            disposition = 1;
            break;
        case 3:
            if (!exists) result = 0x03;
            else disposition = 2;
            break;
        case 4:
            disposition = exists ? 2 : 1;
            break;
        case 5:
            if (!exists) result = 0x03;
            else disposition = 1;
            break;
    }
    if (result) {
        if (outDisposition) *outDisposition = 0;
        return result;
    }
    if (!exists) {
        // Track whether THIS operation created the directory: if another
        // creator won the race, fail safely without rollback of files this
        // operation never created. Never recursively remove a target whose
        // containment check just failed.
        std::error_code createEc;
        bool weCreated = std::filesystem::create_directory(dir, createEc);
        if (createEc) return 0x05;
        if (!weCreated) return 0xB7;
        if (!containedDirLocked(root, dir, nullptr)) return 0x57;
        if (!WriteContentMeta(dir, content)) {
            // Our fresh directory holds only our own temp (already cleaned by
            // WriteContentMeta): remove it non-recursively, never touching
            // preexisting or concurrently created files.
            if (containedDirLocked(root, dir, nullptr)) {
                std::error_code rmEc;
                std::filesystem::remove(dir, rmEc);
            }
            return 0x05;
        }
        std::fprintf(stderr, "[NativeStorage] Created content '%s' type=0x%08X at '%ls'\n",
                     content.fileName.c_str(), content.contentType, dir.c_str());
    } else if (mode == 2 || mode == 5) {
        if (!clearContainer(dir)) return 0x05;
        if (!WriteContentMeta(dir, content)) return 0x05;
        std::fprintf(stderr, "[NativeStorage] Overwrote content '%s' (mode=%u)\n",
                     content.fileName.c_str(), mode);
    }
    {
        std::lock_guard lock(mutex);
        mounts[alias] = dir;
    }
    if (outDisposition) *outDisposition = disposition;
    if (outPath) *outPath = dir;
    std::fprintf(stderr, "[NativeStorage] Mount '%.*s' -> '%ls' disp=%u\n",
                 int(rootAlias.size()), rootAlias.data(), dir.c_str(), disposition);
    return 0;
}

uint32_t OpenExisting(std::string_view rootAlias, const ContentInfo& content) {
    if (!ValidRootName(rootAlias) || !ValidFileName(content.fileName)) return 0x57;
    if (content.contentType == 0) return 0x57;
    // Same opMutex->mutex ordering as CreateContent: alias check, disk read,
    // and mount publication serialize with all other content mutations.
    std::lock_guard opLock(opMutex);
    std::string alias = lowerOf(rootAlias);
    char prefix[16]{};
    snprintf(prefix, sizeof(prefix), "%08X_", content.contentType);
    std::filesystem::path root;
    {
        std::lock_guard lock(mutex);
        root = overrideRoot ? *overrideRoot
            : (!memory || memory->gameDirectory().empty() ? std::filesystem::path()
                                                          : memory->gameDirectory().parent_path() / "saves");
        if (!root.empty()) {
            auto it = mounts.find(alias);
            if (it != mounts.end()) {
                std::error_code ec;
                std::wstring want = std::filesystem::weakly_canonical(root / (std::string(prefix) + content.fileName), ec).wstring();
                if (ec) return 0x57;
                std::wstring have = std::filesystem::weakly_canonical(it->second, ec).wstring();
                if (ec || _wcsicmp(want.c_str(), have.c_str()) != 0) return 0x57;
                return 0;
            }
        }
    }
    if (root.empty()) return 0x48F;
    std::filesystem::path dir = root / (std::string(prefix) + content.fileName);
    if (!containedDirLocked(root, dir, nullptr)) return 0x57;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) return 0x03;
    if (!containedDirLocked(root, dir, nullptr)) return 0x57;
    ContentInfo meta;
    if (!ReadContentMeta(dir, meta) || meta.fileName != content.fileName ||
        meta.contentType != content.contentType)
        return 0x03;
    {
        std::lock_guard lock(mutex);
        mounts.emplace(alias, dir);
    }
    return 0;
}

uint32_t CloseContent(std::string_view rootAlias) {
    if (!ValidRootName(rootAlias)) return 0x57;
    std::lock_guard opLock(opMutex);
    std::lock_guard lock(mutex);
    mounts.erase(lowerOf(rootAlias));
    return 0;
}

uint32_t DeleteContent(const ContentInfo& content) {
    if (!ValidFileName(content.fileName)) return 0x57;
    if (content.contentType == 0) return 0x57;
    std::lock_guard opLock(opMutex);
    std::filesystem::path root;
    char prefix[16]{};
    snprintf(prefix, sizeof(prefix), "%08X_", content.contentType);
    {
        std::lock_guard lock(mutex);
        root = overrideRoot ? *overrideRoot
            : (!memory || memory->gameDirectory().empty() ? std::filesystem::path()
                                                          : memory->gameDirectory().parent_path() / "saves");
        if (root.empty()) return 0x48F;
    }
    std::filesystem::path dir = root / (std::string(prefix) + content.fileName);
    std::filesystem::path canonicalDir;
    if (!containedDirLocked(root, dir, &canonicalDir)) return 0x57;
    std::error_code ec;
    if (std::filesystem::is_symlink(dir, ec) || ec) return 0x57;
    if (!std::filesystem::exists(canonicalDir, ec) || ec) return 0x02;
    {
        std::lock_guard lock(mutex);
        eraseMountsForDirLocked(canonicalDir);
    }
    std::filesystem::remove_all(canonicalDir, ec);
    if (ec) return 0x05;
    std::fprintf(stderr, "[NativeStorage] Deleted content '%s'\n", content.fileName.c_str());
    return 0;
}

std::optional<std::filesystem::path> LookupMount(std::string_view aliasLower) {
    std::lock_guard lock(mutex);
    auto it = mounts.find(std::string(aliasLower));
    if (it == mounts.end()) return std::nullopt;
    return it->second;
}

void ClearMounts() {
    std::lock_guard lock(mutex);
    mounts.clear();
}
}  // namespace DarkRecomp::Native::Storage
