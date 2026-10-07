#include "app/windows/native_shutdown.h"
#include <cstdio>
#include <stdexcept>

using DarkRecomp::Native::takeNativeMessage;

namespace {
constexpr UINT kWindowMessage = WM_APP + 70;
constexpr UINT kThreadMessage = WM_APP + 71;
constexpr wchar_t kWindowClass[] = L"DarkRecompNativeMessagePumpTests";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Window {
    HWND handle = nullptr;
    unsigned delivered = 0, destroyed = 0;
    WPARAM key = 0;
    LPARAM detail = 0;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM key, LPARAM detail) {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(detail)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == kWindowMessage) {
            ++self->delivered;
            self->key = key;
            self->detail = detail;
            return 0;
        }
        if (self && message == WM_DESTROY) {
            ++self->destroyed;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, key, detail);
    }

    Window() {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = kWindowClass;
        require(RegisterClassW(&type) != 0, "Cannot register message pump test window");
        handle = CreateWindowExW(0, kWindowClass, L"Message pump contract", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
        require(handle != nullptr, "Cannot create message pump test window");
    }

    ~Window() {
        if (IsWindow(handle)) DestroyWindow(handle);
        UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
    }
};

void ordinaryMessages(Window& window) {
    MSG message{};
    while (takeNativeMessage(message)) DispatchMessageW(&message);
    require(message.message != WM_QUIT, "New message pump unexpectedly requested shutdown");
    require(!takeNativeMessage(message), "Empty queue reported a message");

    require(PostMessageW(window.handle, kWindowMessage, 17, 29) != FALSE,
        "Cannot post ordinary window message");
    require(PostThreadMessageW(GetCurrentThreadId(), kThreadMessage, 31, 43) != FALSE,
        "Cannot post ordinary thread message");
    require(takeNativeMessage(message) && message.hwnd == window.handle &&
        message.message == kWindowMessage && message.wParam == 17 && message.lParam == 29,
        "Window message was lost or changed");
    DispatchMessageW(&message);
    require(window.delivered == 1 && window.key == 17 && window.detail == 29,
        "Ordinary window message did not reach its handler");
    require(takeNativeMessage(message) && !message.hwnd && message.message == kThreadMessage &&
        message.wParam == 31 && message.lParam == 43,
        "Thread message was lost or changed");
    require(!takeNativeMessage(message), "Queue did not become empty after ordinary messages");
}

void quitBeforeLaterMessages(Window& window) {
    // PostQuitMessage generates WM_QUIT at low priority. Explicitly enqueue it
    // here to deterministically exercise messages becoming available after a
    // quit has been retrieved, without timing a second posting thread.
    require(PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 7, 0) != FALSE,
        "Cannot post regression quit message");
    require(PostMessageW(window.handle, kWindowMessage, 53, 67) != FALSE,
        "Cannot post window message after quit");
    require(PostThreadMessageW(GetCurrentThreadId(), kThreadMessage, 71, 83) != FALSE,
        "Cannot post thread message after quit");

    MSG message{};
    unsigned handled = 0;
    while (takeNativeMessage(message)) {
        ++handled;
        DispatchMessageW(&message);
    }
    require(message.message == WM_QUIT && message.wParam == 7,
        "Message pump consumed quit and overwrote shutdown state");
    require(handled == 0 && window.delivered == 1,
        "Message pump dispatched work after shutdown was requested");

    MSG pending{};
    require(PeekMessageW(&pending, nullptr, 0, 0, PM_NOREMOVE) != FALSE &&
        pending.hwnd == window.handle && pending.message == kWindowMessage,
        "Shutdown drained a later queued window message");
    require(message.message == WM_QUIT && message.wParam == 7,
        "Inspecting pending work changed the retained quit message");

    // Clean up the test queue explicitly after the simulated display loop has
    // ended. These messages must still be available in their original order.
    require(takeNativeMessage(pending) && pending.message == kWindowMessage &&
        pending.wParam == 53 && pending.lParam == 67,
        "Later window message was not preserved");
    DispatchMessageW(&pending);
    require(window.delivered == 2 && window.key == 53 && window.detail == 67,
        "Preserved window message could not be dispatched by the fixture");
    require(takeNativeMessage(pending) && !pending.hwnd && pending.message == kThreadMessage &&
        pending.wParam == 71 && pending.lParam == 83,
        "Later thread message was not preserved");
    require(!takeNativeMessage(pending), "Regression fixture left queued messages");
}

void windowClose(Window& window) {
    require(PostMessageW(window.handle, WM_CLOSE, 0, 0) != FALSE,
        "Cannot request test window close");
    MSG message{};
    while (takeNativeMessage(message)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    require(window.destroyed == 1 && !IsWindow(window.handle),
        "Window close did not destroy the owned window");
    require(message.message == WM_QUIT && message.wParam == 0,
        "Window destruction did not preserve its real PostQuitMessage result");
}
}

int main() {
    try {
        Window window;
        ordinaryMessages(window);
        quitBeforeLaterMessages(window);
        windowClose(window);
        std::puts("NativeMessagePump passed: ordinary messages delivered; quit preserved before later queued work; window close ends the pump.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NativeMessagePump failed: %s\n", error.what());
        return 1;
    }
}
