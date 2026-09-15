#pragma once
#include "runtime.h"
#include "storage.h"
#include <memory>
#include <deque>
#include <utility>
#include <vector>

struct KernelObject {
    HANDLE handle;
    uint32_t guestAddress = 0;
    uint32_t allocation = 0;
    bool isThread = false;
    bool isFile = false;
    bool isEvent = false;
    bool writable = false;
    bool unbuffered = false;
    bool notificationListener = false;
    uint64_t notificationAreas = 0;
    uint32_t notificationMaxVersion = 0;
    std::deque<std::pair<uint32_t, uint32_t>> notifications;
    bool isEnumerator = false;
    uint32_t enumFetch = 0;
    size_t enumCursor = 0;
    std::vector<DarkRecomp::Native::Storage::ContentInfo> enumItems;
    uint32_t affinity = 0x3f;
    int32_t priority = 0;
    std::filesystem::path path;
    std::mutex ioMutex;
    // A native directory read advances before its UTF-8 guest record is copied.
    // Keep a truncated entry until delivered; aliases share this cursor state.
    std::vector<uint8_t> pendingDirectoryEntry;
    explicit KernelObject(HANDLE value) : handle(value) {}
    ~KernelObject();
};
uint32_t storeObject(HANDLE handle);
std::shared_ptr<KernelObject> object(uint32_t id);
void queueGuestApc(PPCContext& ctx, uint32_t routine, uint32_t argument, uint32_t ios);
void queueGuestXamApc(PPCContext& ctx, uint32_t routine, uint32_t error, uint32_t length,
                      uint32_t overlapped);
