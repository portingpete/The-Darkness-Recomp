#include "app/windows/display_reuse.h"
#include "renderer/d3d11/display_context_d3d11.h"
#include "renderer/d3d11/engine_preview.h"
#include "runtime/native/graphics_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
using Display = CDisplayContextD3D11;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* reuse = reinterpret_cast<NativeDisplayReuse*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (reuse && (message == WM_PAINT || message == WM_SIZE || message == WM_DISPLAYCHANGE ||
                  message == WM_SHOWWINDOW || message == WM_ACTIVATEAPP)) reuse->invalidate();
    return DefWindowProcW(window, message, wParam, lParam);
}

struct TestWindow {
    HWND handle = nullptr;
    explicit TestWindow(NativeDisplayReuse& reuse) {
        WNDCLASSW description{};
        description.lpfnWndProc = windowProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = L"DarkRecompDisplayReuseContract";
        require(RegisterClassW(&description) != 0, "Cannot register display-reuse test window");
        handle = CreateWindowExW(0, description.lpszClassName, L"Display reuse contract",
                                 WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr,
                                 description.hInstance, &reuse);
        require(handle != nullptr, "Cannot create display-reuse test window");
    }
    ~TestWindow() {
        if (handle) DestroyWindow(handle);
        UnregisterClassW(L"DarkRecompDisplayReuseContract", GetModuleHandleW(nullptr));
    }
};

static std::vector<uint32_t> readOutput(Display& display) {
    ComPtr<ID3D11Texture2D> back, staging;
    require(SUCCEEDED(display.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(&back))),
            "Display-reuse readback buffer missing");
    D3D11_TEXTURE2D_DESC description{};
    back->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(display.GetDevice()->CreateTexture2D(&description, nullptr, &staging)),
            "Display-reuse staging allocation failed");
    display.GetContext()->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(display.GetContext()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
            "Display-reuse readback map failed");
    std::vector<uint32_t> pixels(size_t(description.Width) * description.Height);
    for (unsigned y = 0; y < description.Height; ++y)
        memcpy(pixels.data() + size_t(y) * description.Width,
               static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
               size_t(description.Width) * 4);
    display.GetContext()->Unmap(staging.Get(), 0);
    return pixels;
}

static SimpleMesh flatMesh(uint8_t red, uint8_t green, uint8_t blue) {
    auto image = std::make_shared<ColorImage>();
    image->width = image->height = 1;
    image->pixels = {red, green, blue, 255};
    SimpleMesh mesh;
    mesh.colorTexture = image;
    mesh.opaque = true;
    mesh.projection = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    mesh.vertices = {{{-1,-1,.5f},{0,0},{1,1,1,1}}, {{3,-1,.5f},{0,0},{1,1,1,1}},
                     {{-1,3,.5f},{0,0},{1,1,1,1}}};
    mesh.indices = {0,1,2};
    return mesh;
}

static bool copyPending(NativeDisplayReuse& reuse, EnginePreviewD3D11& renderer, unsigned& copies) {
    if (!reuse.shouldCopy(renderer.frameInProgress())) return false;
    renderer.copyToDisplay();
    ++copies;
    return true;
}

static void statuses() {
    NativeDisplayReuse reuse;
    require(reuse.shouldCopy(false) && !reuse.shouldCopy(true), "Initial display copy readiness differs");
    for (HRESULT status : {S_FALSE, DXGI_STATUS_OCCLUDED, DXGI_STATUS_MODE_CHANGED,
                            E_FAIL, DXGI_ERROR_DEVICE_REMOVED}) {
        reuse.presented(S_OK);
        require(!reuse.shouldCopy(false), "Accepted presentation failed to arm display reuse");
        reuse.presented(status);
        require(reuse.shouldCopy(false) && !reuse.shouldCopy(true),
                "Non-accepted presentation lost retry or exposed an unfinished frame");
    }
    reuse.presented(S_OK);
    reuse.invalidate(); reuse.invalidate();
    require(reuse.shouldCopy(false), "Repeated display invalidation lost pending refresh");
}

static void retainedOutput() {
    const auto saved = graphicsSettings();
    struct RestoreSettings {
        GraphicsSettings settings;
        ~RestoreSettings() { setGraphicsSettings(settings); }
    } restore{saved};
    auto selected = saved;
    selected.antialiasing = AntialiasingMode::Off;
    selected.brightnessPercent = 100;
    require(setGraphicsSettings(selected), "Cannot select neutral display settings");
    NativeDisplayReuse reuse;
    TestWindow window(reuse);
    Display display;
    display.Init(window.handle, 64, 64);
    EnginePreviewD3D11 renderer(display.GetDevice(), display.GetContext(), display.GetSwapChain(), 64, 64);
    unsigned copies = 0, accepted = 0, occluded = 0, other = 0;
    const auto gray = flatMesh(64, 128, 192);
    renderer.render({gray}); // No new producer frame until the partial-frame case below.
    require(copyPending(reuse, renderer, copies), "Initial retained image was not copied");
    const auto original = readOutput(display);
    for (const auto pixel : original) require(pixel == 0xffc08040u, "Flat retained output differs");
    const auto source = renderer.readPixel(32, 32);
    const HRESULT realStatus = display.Present(0);
    require(SUCCEEDED(realStatus), "Real display-reuse presentation failed");
    accepted += realStatus == S_OK;
    occluded += realStatus == DXGI_STATUS_OCCLUDED;
    other += realStatus != S_OK && realStatus != DXGI_STATUS_OCCLUDED;
    reuse.presented(realStatus);
    require(reuse.shouldCopy(false) == (realStatus != S_OK), "Actual DXGI status lost display retry");
    // Hidden windows can always occlude. Model acceptance explicitly for the
    // skip branch, and check GPU pixels only before Present discards its buffer.
    reuse.presented(S_OK);
    const unsigned beforeQuiescent = copies;
    for (unsigned loop = 0; loop < 128; ++loop)
        require(!copyPending(reuse, renderer, copies), "Quiescent producer copied an accepted unchanged image");
    require(copies == beforeQuiescent && renderer.readPixel(32, 32) == source,
            "Quiescent reuse changed copy count or owned source image");
    InvalidateRect(window.handle, nullptr, FALSE);
    SendMessageW(window.handle, WM_PAINT, 0, 0);
    require(copyPending(reuse, renderer, copies), "Repaint did not refresh the quiescent image");
    require(readOutput(display) == original, "Repaint changed the retained image");
    reuse.presented(S_OK);
    for (UINT message : {WM_DISPLAYCHANGE, WM_SHOWWINDOW, WM_ACTIVATEAPP}) {
        SendMessageW(window.handle, message, 0, 0);
        require(copyPending(reuse, renderer, copies), "Display/show/activation event did not refresh retained output");
        require(readOutput(display) == original, "Display/show/activation refresh changed retained output");
        reuse.presented(S_OK);
    }
    for (const auto brightness : {50u,125u}) {
        selected.brightnessPercent = brightness;
        require(setGraphicsSettings(selected), "Cannot select quiescent image brightness");
        reuse.invalidate();
        require(copyPending(reuse, renderer, copies), "Brightness did not refresh the quiescent image");
        const auto corrected = readOutput(display);
        require(corrected != original, "Brightness left quiescent output unchanged");
        for (size_t i = 0; i < corrected.size(); ++i) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                const double input = double((original[i] >> (channel * 8)) & 255);
                const double expected = (std::min)(255.0, input * brightness / 100.0);
                require(std::abs(double((corrected[i] >> (channel * 8)) & 255) - expected) <= 1,
                        "Quiescent brightness differs from final-output intensity");
            }
            require((corrected[i] >> 24) == 255, "Brightness changed retained alpha");
        }
        require(renderer.readPixel(32, 32) == source, "Brightness refresh mutated the owned image");
        reuse.presented(S_OK);
    }
    selected.brightnessPercent = 100;
    require(setGraphicsSettings(selected), "Cannot restore neutral output settings");
    reuse.invalidate();
    require(copyPending(reuse, renderer, copies) && readOutput(display) == original,
            "Neutral settings did not restore the same retained image");
    reuse.presented(S_OK);
    renderer.releaseDisplayTarget();
    display.Resize(96, 64);
    SendMessageW(window.handle, WM_SIZE, SIZE_RESTORED, MAKELPARAM(96, 64));
    require(copyPending(reuse, renderer, copies), "Resize did not copy the quiescent image");
    const auto resized = readOutput(display);
    require(resized.size() == 96 * 64, "Resize retained the old output extent");
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 96; ++x)
        require(resized[size_t(y) * 96 + x] == (x >= 16 && x < 80 ? 0xffc08040u : 0xff000000u),
                "Quiescent resize stretched or lost retained pixels/borders");
    require(renderer.readPixel(32, 32) == source, "Resize mutated the owned image");
    reuse.presented(S_OK);
    renderer.render({flatMesh(0, 0, 255)}, {true, false});
    reuse.invalidate();
    require(!copyPending(reuse, renderer, copies), "Partial frame replaced the completed display image");
    SendMessageW(window.handle, WM_PAINT, 0, 0);
    require(!copyPending(reuse, renderer, copies) && readOutput(display) == resized,
            "Repaint exposed an unfinished frame or damaged prior display pixels");
    renderer.render({}, {false, true});
    require(copyPending(reuse, renderer, copies), "Partial-frame completion lost pending display refresh");
    const auto completed = readOutput(display);
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 96; ++x)
        require(completed[size_t(y) * 96 + x] == (x >= 16 && x < 80 ? 0xffff0000u : 0xff000000u),
                "Completed partial frame did not replace retained output");
    renderer.releaseDisplayTarget(); display.Resize(64, 64);
    auto diagonal = flatMesh(255, 255, 255);
    diagonal.vertices = {{{-.875f,.875f,.5f},{0,0},{1,1,1,1}},
                         {{.8125f,.5625f,.5f},{1,0},{1,1,1,1}},
                         {{-.6875f,-.875f,.5f},{0,1},{1,1,1,1}}};
    renderer.render({diagonal});
    reuse.invalidate();
    require(copyPending(reuse, renderer, copies), "Diagonal frame was not copied");
    const auto sharp = readOutput(display);
    const auto diagonalSource = renderer.readPixel(20, 20);
    reuse.presented(S_OK);
    for(auto aaMode:{AntialiasingMode::FXAA,AntialiasingMode::SMAA}) {
    selected.antialiasing = aaMode;
    require(setGraphicsSettings(selected), "Cannot select quiescent AA");
    reuse.invalidate();
    require(copyPending(reuse, renderer, copies), "AA change did not refresh the quiescent image");
    const auto filtered = readOutput(display);
    unsigned changed = 0, intermediate = 0;
    for (size_t i = 0; i < filtered.size(); ++i) {
        changed += filtered[i] != sharp[i];
        const auto value = filtered[i] & 255;
        intermediate += value > 0 && value < 255;
    }
    require(changed > 12 && intermediate > 12, "Quiescent AA did not smooth retained diagonal edges");
    require(renderer.readPixel(20, 20) == diagonalSource, "AA refresh mutated the owned image");
    reuse.presented(S_OK); reuse.invalidate();
    require(copyPending(reuse, renderer, copies) && readOutput(display) == filtered,
            "Repainting retained AA output accumulated blur");
    reuse.presented(S_OK);
    selected.antialiasing = AntialiasingMode::Off;
    require(setGraphicsSettings(selected), "Cannot disable quiescent AA");
    reuse.invalidate();
    require(copyPending(reuse, renderer, copies) && readOutput(display) == sharp,
            "Disabling AA did not restore every retained source pixel");
    }
    require(display.GetDevice()->GetDeviceRemovedReason() == S_OK, "Display reuse removed the graphics device");
    std::printf("DisplayReuseContract GPU copies=%u realAccepted=%u realOccluded=%u realOther=%u; "
                "128 quiescent loops, repaint/events, resize, partial frames, brightness and AA passed.\n",
                copies, accepted, occluded, other);
    std::puts("Hidden-window GPU checks read copied pixels before flip-discard Present. S_OK skip "
              "semantics are also modeled explicitly; visible DWM retention and monitor scanout are not measured.");
}

int main() {
    try {
        statuses();
        retainedOutput();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Display reuse contract: %s\n", error.what());
        return 1;
    }
}
