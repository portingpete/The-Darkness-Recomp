#pragma once
#include <windows.h>
#include <Xinput.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "keyboard_bindings.h"

namespace DarkRecomp::Native {
class Memory;

struct ControllerApi {
    decltype(&XInputGetState) state = &XInputGetState;
    decltype(&XInputGetCapabilities) capabilities = &XInputGetCapabilities;
    decltype(&XInputSetState) vibration = &XInputSetState;
    decltype(&GetTickCount64) ticks = &GetTickCount64;
};

struct InputCounters {
    uint64_t polls = 0;
    uint64_t connected = 0;
    uint64_t nonneutral = 0;
    uint64_t changes = 0;
    uint64_t mouseEvents = 0;
};

// Last meaningful physical input source, for adaptive button prompts.
// Defaults to keyboard/mouse; a real controller steals it only on new
// over-deadzone activity, and any meaningful keyboard/mouse use wins back.
enum class PromptInputSource : uint8_t { KeyboardMouse = 0, Controller = 1 };

// Relative device counts, never a stick position or a velocity. Positive Y
// follows Win32 (down). Only the gameplay look boundary consumes this batch.
struct MouseLookDelta {
    int64_t x = 0, y = 0;
    float sensitivity = 1;
    uint64_t epoch = 0;
};

struct MenuCursorSnapshot {
    int x = 0, y = 0;
    bool valid = false, leftDown = false;
    uint64_t movement = 0, presses = 0, epoch = 0;
};

// Buttons, keyboard movement and physical controllers use the XAM ABI.
// Relative mouse look is consumed separately at the gameplay look boundary.
// No guest input structures are cast to Windows structs: every multi-byte
// field is serialized big endian.
class NativeInput {
public:
    // Injected APIs stay synchronous unless a test explicitly requests the
    // production backend. The native singleton uses the background backend.
    explicit NativeInput(ControllerApi api = {}, bool backgroundController = false);
    ~NativeInput();
    NativeInput(const NativeInput&) = delete;
    NativeInput& operator=(const NativeInput&) = delete;
    void attachWindow(HWND window);
    void setSettingsOpen(bool open);
    // True when native menu capture consumes a keyboard edge before shortcuts.
    bool windowMessage(HWND window, UINT message, WPARAM key, LPARAM detail);
    void setMouseLookEnabled(bool enabled);
    bool mouseLookEnabled();
    // The original client owns dialogue/menu focus independently of capture.
    // Before its first signal, released capture permits startup menu input.
    void setGuestMenuActive(bool active);
    bool setMouseSensitivity(float sensitivity);
    KeyboardBindings keyboardBindings();
    bool setKeyboardBindings(const KeyboardBindings& bindings);
    void suppressMenuActivationKeys();
    MenuCursorSnapshot menuCursor();
    // Before the original client first reports its GUI, startup menus use the
    // observed root. Afterwards an inactive GUI proves gameplay ownership.
    bool guestMenuAllowsPointer();
    void mouseMotion(LONG dx, LONG dy);
    MouseLookDelta consumeMouseLook();
    DWORD getState(Memory& owner, uint32_t user, uint32_t flags, uint32_t output);
    DWORD getCapabilities(Memory& owner, uint32_t user, uint32_t flags, uint32_t output);
    DWORD setState(Memory& owner, uint32_t user, uint32_t flags, uint32_t input);
    InputCounters counters();
    // Lock-free render-boundary read; updated under the input mutex on polls.
    PromptInputSource promptSource() const noexcept {
        return promptSource_.load(std::memory_order_acquire);
    }
private:
    struct Slot {
        XINPUT_GAMEPAD previous{};
        uint32_t packet = 0;
        bool connected = false;
        bool rumbling = false;
    };
    void clearKeysLocked();
    void suppressMenuActivationKeysLocked();
    void stopVibrationLocked();
    XINPUT_GAMEPAD keyboardLocked();
    void clearMouseLocked();
    void resetPromptLocked();
    void noteKeyboardActivityLocked(bool active);
    static bool controllerEdgeLocked(const XINPUT_GAMEPAD& current, const XINPUT_GAMEPAD& previous, bool haveBaseline);
    void updatePromptSourceLocked(uint32_t user, const XINPUT_GAMEPAD& physical, bool connected, bool keyboardMouseActive);
    void controllerWorker();
    void stopControllerWorker();
    DWORD physicalState(uint32_t user, XINPUT_STATE& state, uint64_t& generation);
    DWORD physicalCapabilities(uint32_t user, uint32_t flags, XINPUT_CAPABILITIES& caps);
    DWORD physicalVibration(uint32_t user, XINPUT_VIBRATION vibration);
    ControllerApi api_;
    struct PhysicalSlot {
        XINPUT_STATE state{};
        XINPUT_CAPABILITIES caps{};
        DWORD status = ERROR_DEVICE_NOT_CONNECTED;
        DWORD capsStatus = ERROR_DEVICE_NOT_CONNECTED;
        uint64_t generation = 0;
        uint64_t startedGeneration = 0;
        XINPUT_VIBRATION vibration{};
        uint64_t vibrationVersion = 0;
    };
    bool backgroundController_ = false;
    std::mutex controllerMutex_;
    std::condition_variable controllerChanged_;
    std::array<PhysicalSlot, XUSER_MAX_COUNT> physicalSlots_{};
    bool controllerStop_ = false, controllerCommand_ = false;
    std::thread controllerThread_;
    std::mutex mutex_;
    HWND window_ = nullptr;
    bool focused_ = false;
    bool settingsOpen_ = false;
    bool captureWasBlocked_ = false;
    std::array<bool, XUSER_MAX_COUNT> waitForControllerRelease_{};
    std::array<uint64_t, XUSER_MAX_COUNT> controllerFreshAfter_{};
    bool leftMouse_ = false;
    bool rightMouse_ = false;
    bool middleMouse_ = false;
    bool mouseLook_ = false, escapePauses_ = false, guestMenuActive_ = false;
    bool guestMenuContextKnown_ = false;
    float mouseSensitivity_ = 1;
    int64_t mouseX_ = 0, mouseY_ = 0;
    uint64_t mouseEpoch_ = 0;
    bool mousePending_ = false;
    int wheelRemainder_ = 0, wheelPending_ = 0;
    WORD wheelButton_ = 0;
    ULONGLONG wheelNext_ = 0;
    std::array<bool, 256> keys_{};
    std::array<bool, 256> capturedKeys_{};
    KeyboardBindings bindings_ = defaultKeyboardBindings();
    uint32_t heldKeys_ = 0;
    bool keyboardMouseEvent_ = false;
    int lastMenuX_ = 0, lastMenuY_ = 0;
    bool haveMenuPos_ = false;
    uint64_t menuMovement_ = 0, menuPresses_ = 0, menuEpoch_ = 0;
    std::array<Slot, XUSER_MAX_COUNT> slots_{};
    InputCounters counters_{};
    std::atomic<PromptInputSource> promptSource_{PromptInputSource::KeyboardMouse};
    std::array<XINPUT_GAMEPAD, XUSER_MAX_COUNT> lastPhysical_{};
    std::array<bool, XUSER_MAX_COUNT> physicalBaseline_{};
    struct AnalogAnchor { SHORT lx = 0, ly = 0, rx = 0, ry = 0; BYTE lt = 0, rt = 0; bool valid = false; };
    std::array<AnalogAnchor, XUSER_MAX_COUNT> analogAnchor_{};
    int selectingSlot_ = -1;
};

NativeInput& nativeInput();
}
