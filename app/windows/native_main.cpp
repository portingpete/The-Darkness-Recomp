#include <fstream>
#include <intrin.h>
#include "runtime/native/runtime.h"
#include "runtime/native/audio_driver.h"
#include "runtime/native/input.h"
#include "native_mouse.h"
#include "developer_tools_window.h"
#include "keyboard_settings.h"
#include "runtime/native/keyboard_menu.h"
#include "display_options.h"
#include "display_window.h"
#include "display_settings.h"
#include "runtime/native/display_mode.h"
#include "runtime/native/fov_settings.h"
#include "runtime/native/language_settings.h"
#include "runtime/native/video_settings_menu.h"
#include "runtime/native/menu_pointer.h"
#include "runtime/native/developer_tools.h"
#include "test_input_parser.h"
#include "frame_metrics.h"
#include "renderer/engine/engine_performance.h"
#include "frame_pacer.h"
#include "display_reuse.h"
#include "resolution_status.h"
#include "thread_policy.h"
#include "renderer/engine/scene_work.h"
#include "cpu_sampler.h"
#include "renderer/d3d11/display_context_d3d11.h"
#include "renderer/engine/render_trace.h"
#include "renderer/d3d11/engine_preview.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <string>
#include <algorithm>
#include <sstream>

using namespace DarkRecomp::Native;
static NativeMouseWindow* mouseWindow = nullptr;
static DeveloperToolsWindow* developerToolsWindow = nullptr;
static bool displayResizePending = false;
static bool fullscreenTogglePending = false;
static bool resolutionTogglePending = false;
// Consume both edges before guest input. Windows' held-key repeats must not
// flip the setting; two new presses queued before a frame boundary cancel.
static bool handleResolutionHotkey(UINT message, WPARAM key, LPARAM detail) {
    if (key != VK_F6 || (message != WM_KEYDOWN && message != WM_KEYUP)) return false;
    if (message == WM_KEYDOWN && !(detail & (LPARAM(1) << 30)))
        resolutionTogglePending = !resolutionTogglePending;
    return true;
}
// The owned renderer target survives flip-discard presents. Once DXGI accepts
// its display copy, keep that image until rendering or display state changes.
static NativeDisplayReuse displayReuse;
// Flush buffered logs on a fatal Windows exception before WER terminates the
// process; buffered logging keeps per-write disk latency off hot threads.
static LONG WINAPI DarkFlushLogsOnFatalException(PEXCEPTION_POINTERS) {
    fflush(nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void finishAudioEvidence() {
    // ExitProcess bypasses object destruction. For an opt-in recording, use
    // the driver's existing cancellation/join path before exiting so the tap
    // and trace writers persist partial captures as well as completed ones.
    if (GetEnvironmentVariableW(L"DARKRECOMP_AUDIO_TAP", nullptr, 0) ||
        GetEnvironmentVariableW(L"DARKRECOMP_RESAMPLER_TRACE", nullptr, 0))
        AudioRenderDriver::instance().shutdown();
}
static void printAudioHealth() {
    const auto audio=AudioRenderDriver::instance().counters();
    std::fprintf(stderr,"[AudioHealth] queuedBuffers=%u queuedFrames=%llu started=%u starvationPasses=%llu starvationBytes=%llu engineGlitches=%u maxCallbackUs=%llu mmcss=%u errors=%llu\n",
        audio.queuedBuffers,audio.queuedFrames,unsigned(audio.playbackStarted),audio.starvationPasses,
        audio.starvationBytes,audio.engineGlitches,audio.maxCallbackMicros,unsigned(audio.workerMmcss),audio.deviceErrors);
}
static LRESULT CALLBACK NativeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (nativeInput().windowMessage(window, message, wParam, lParam)) return 0;
    if (handleResolutionHotkey(message, wParam, lParam)) return 0;
    if (message == WM_GETMINMAXINFO) {
        RECT minimum{0, 0, 320, 180};
        AdjustWindowRect(&minimum, WS_OVERLAPPEDWINDOW, FALSE);
        auto limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }
    if (message == WM_SYSKEYDOWN && wParam == VK_RETURN && (lParam & (LPARAM(1) << 29))) {
        if (!(lParam & (LPARAM(1) << 30))) fullscreenTogglePending = true;
        return 0;
    }
    if (message == WM_SIZE && wParam != SIZE_MINIMIZED) displayResizePending = true;
    if (message == WM_PAINT || message == WM_SIZE || message == WM_DISPLAYCHANGE ||
        message == WM_SHOWWINDOW || message == WM_ACTIVATEAPP)
        displayReuse.invalidate();
    // The modeless panel keeps rendering/engine updates running, but game
    // input and mouse capture stay suspended even if the owner gets focus.
    if (mouseWindow && (!developerToolsWindow || !developerToolsWindow->isOpen()) &&
        mouseWindow->message(message, wParam, lParam)) return 0;
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

int wmain(int argc, wchar_t** argv) {
    std::fprintf(stderr,"[Build] nativeInputs=%s\n",DARK_NATIVE_INPUT_FINGERPRINT);
    // Buffer both streams: unbuffered stderr turned every log write into a
    // disk syscall, stalling the display thread ~50ms every 5s report (and
    // jittering hot threads on every write). 64KB buffers make steady-state
    // logging memcpy-fast; the 5s report, clean shutdown and every fatal
    // path below flush. DARK_UNBUFFERED_LOG=1 restores _IONBF for debugging.
    if (std::getenv("DARK_UNBUFFERED_LOG") && std::getenv("DARK_UNBUFFERED_LOG")[0] == '1') {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    } else {
        setvbuf(stdout, nullptr, _IOFBF, 65536);
        setvbuf(stderr, nullptr, _IOFBF, 65536);
    }
    SetUnhandledExceptionFilter(DarkFlushLogsOnFatalException);
#if DARK_NATIVE_SSSE3
    int cpuFeatures[4]{};__cpuid(cpuFeatures,1);
    if(!(cpuFeatures[2]&(1<<9))) {
        fputs("This build requires SSSE3. Rebuild with -DDARK_NATIVE_SSSE3=OFF for baseline x64.\n",stderr);
        return 1;
    }
#endif
    std::filesystem::path gameDir = L"Darkness";
    uint32_t timeout = 30000;
    unsigned targetFps = 60;
    bool rendererSmoke = false;
    std::filesystem::path rendererTrace;
    bool manualShadowCapture=false;
    bool enginePreview = false;
    bool muted = false;
    bool sampleEngine=false,sampleWorkers=false;
    unsigned sceneWorkers=0;
    bool traceFrameHitches=false;
    std::filesystem::path previewFrame,testInputPath;
    bool testStart=false,testSkipIntros=false;
    bool fullscreen = false;
    uint32_t windowWidth = 0, windowHeight = 0, renderHeight = 720;
    float commandLineFov = 0;
    bool overrideFov = false;
    GameLanguage commandLineLanguage = GameLanguage::System;
    bool overrideLanguage = false;
    bool overrideFps = false, overrideFullscreen = false, overrideRenderHeight = false;
    bool overrideVsync = false, verticalSync = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--game-dir" && i + 1 < argc) gameDir = argv[++i];
        else if (arg == L"--timeout-ms" && i + 1 < argc) timeout = wcstoul(argv[++i], nullptr, 10);
        else if (arg == L"--fps" && i + 1 < argc) {
            const wchar_t* text = argv[++i];
            wchar_t* end = nullptr; targetFps = wcstoul(text, &end, 10);
            if (!end || end == text || *end || targetFps > 1000) { fputs("FPS must be 0 (uncapped) through 1000.\n", stderr); return 1; }
            overrideFps = true;
        }
        else if (arg == L"--renderer-smoke") rendererSmoke = true;
        else if (arg == L"--profile-engine") profileEngineCpu=true;
        else if (arg == L"--trace-frame-hitches") traceFrameHitches=true;
        else if (arg == L"--sample-engine") sampleEngine=true;
        else if (arg == L"--sample-renderer") sampleRendererCpu=true;
        else if (arg == L"--sample-workers") sampleWorkers=true;
        else if(arg==L"--scene-workers" && i+1<argc) {
            const wchar_t* text=argv[++i];wchar_t* end=nullptr;
            const auto value=wcstoul(text,&end,10);
            if(!*text || !end || *end || value>256) {fputs("Scene workers must be 0 through 256.\n",stderr);return 1;}
            sceneWorkers=unsigned(value);
        }
        else if (arg == L"--trace-renderer" && i + 1 < argc) rendererTrace = argv[++i];
        else if (arg == L"--shadow-capture" && i + 1 < argc) {
            rendererTrace=argv[++i];manualShadowCapture=true;enginePreview=true;
            if(rendererTrace.empty()) {fputs("Shadow capture requires a new directory.\n",stderr);return 1;}
        }
        else if (arg == L"--test-input" && i + 1 < argc) testInputPath=argv[++i];
        else if (arg == L"--test-start") testStart=true;
        else if (arg == L"--test-skip-intros") testSkipIntros=true;
        else if (arg == L"--engine-preview") enginePreview = true;
        else if (arg == L"--mute") muted = true;
        else if (arg == L"--fullscreen") { fullscreen = true; overrideFullscreen = true; }
        else if (arg == L"--windowed") { fullscreen = false; overrideFullscreen = true; }
        else if (arg == L"--vsync") { verticalSync = true; overrideVsync = true; }
        else if (arg == L"--no-vsync") { verticalSync = false; overrideVsync = true; }
        else if (arg == L"--language" && i + 1 < argc) {
            if (!parseGameLanguage(argv[++i], commandLineLanguage)) {
                fputs("Language must be auto, en, de, fr, es or it.\n", stderr); return 1;
            }
            overrideLanguage = true;
        }
        else if (arg == L"--fov" && i + 1 < argc) {
            if (!DarkRecomp::parseFieldOfView(argv[++i], commandLineFov)) {
                fputs("FOV must be 0 (Original) or 60 through 120 horizontal degrees at 16:9.\n", stderr); return 1;
            }
            overrideFov = true;
        }
        else if ((arg == L"--width" || arg == L"--height" || arg == L"--render-height") && i + 1 < argc) {
            uint32_t value = 0;
            if (!parseDisplayDimension(argv[++i], value)) { fputs("Invalid display dimension.\n", stderr); return 1; }
            if (arg == L"--width") windowWidth = value;
            else if (arg == L"--height") windowHeight = value;
            else { renderHeight = value; overrideRenderHeight = true; }
        }
        else if (arg == L"--mouse-sensitivity" && i + 1 < argc) {
            wchar_t* end = nullptr;
            const float value = float(wcstod(argv[++i], &end));
            if (!end || *end || !nativeInput().setMouseSensitivity(value)) {
                fputs("Mouse sensitivity must be between 0.1 and 10.\n", stderr); return 1;
            }
        }
        else if (arg == L"--preview-frame" && i + 1 < argc) previewFrame = argv[++i];
        else {
            fputs("Usage: DarkRecomp --game-dir <directory> [--timeout-ms 30000 (0 disables deadline)] [--renderer-smoke] [--trace-renderer <new directory>] [--engine-preview] [--mute] [--fps 60 (default; 0 uncapped)] [--profile-engine] [--sample-engine] [--mouse-sensitivity 1.0] [--language auto|en|de|fr|es|it] [--preview-frame <new BMP path>] [--test-input <file>] [--test-start] [--test-skip-intros]\n", stderr);
            fputs("  --test-input file lines (max 64, own-process diagnostics only): numeric '<key> [<holdMs 1..10000>]' (bare menu keys hold 250ms, I/J/K/L hold 2000ms; gameplay keys WASD/E/R/F/X/Z/C/Q/G/1-4/Tab/Back/Shift/Ctrl plus arrows/Space/Return/Esc/IJKL; 116=F5 panel); 'mouse <dx> <dy>' (+/-10000, held 2000ms); 'capture' game screenshots; '0' inspects; 'dev open|close|status', 'dev mission <ID>', 'dev speed <preset>', 'dev invincible on|off', 'dev capture <absolute BMP path>'; an invalid line blocks later commands until that line is fixed.\n", stderr);
            fputs("  Display: --fullscreen or --windowed; --width W --height H selects window/aspect size; --render-height H controls internal resolution (180..2160, default 720). Alt+Enter toggles borderless fullscreen; F6 switches live 720p/1440p when started with a 720-high guest mode.\n", stderr);
            fputs("  Options > Video Settings contains native PC graphics controls. Saved settings apply unless explicitly overridden. --vsync / --no-vsync overrides vertical sync; --fov 0 (Original) or 60..120 overrides horizontal FOV at 16:9 for this run.\n", stderr);
            fputs("  --trace-frame-hitches records bounded slow-frame stage timings without enabling per-draw profiling or instruction sampling.\n", stderr);
            fputs("  --sample-renderer samples only active rendering on this game's display thread; opt-in diagnostics add overhead.\n", stderr);
            fputs("  --shadow-capture <new directory> enables two F8 captures of complete rendered frames, shadow resources and matching screenshots.\n",stderr);
            fputs("  --scene-workers N opts into experimental parallel geometry/texture jobs (up to N helpers); default 0, serial.\n",stderr);
            return 1;
        }
    }
    if ((windowWidth == 0) != (windowHeight == 0) ||
        (windowWidth && (windowWidth < 320 || windowHeight < 180)) || renderHeight < 180 || renderHeight > kMaximumRenderHeight) {
        fputs("Specify both --width (320..16384) and --height (180..16384); render height must be 180..2160.\n", stderr);
        return 1;
    }
    puts("DarkRecomp native Windows AOT runtime - development build, gameplay incomplete");
    try {
        const auto settingsPath = std::filesystem::absolute(gameDir).parent_path() / L"DarkRecomp.settings.ini";
        initializeGameLanguageSetting(loadGameLanguage(settingsPath));
        if (overrideLanguage) overrideGameLanguageForRun(commandLineLanguage);
        setFieldOfViewSetting(overrideFov ? commandLineFov : DarkRecomp::loadFieldOfView(settingsPath));
        auto activeGraphics = DarkRecomp::loadGraphicsSettings(settingsPath);
        if (overrideFps) activeGraphics.frameRateLimit = targetFps;
        if (overrideFullscreen) activeGraphics.fullscreen = fullscreen;
        if (overrideRenderHeight) activeGraphics.renderHeight = renderHeight;
        if (overrideVsync) activeGraphics.verticalSync = verticalSync;
        setGraphicsSettings(activeGraphics);
        initializeVideoSettingsMenu();
        initializeMenuPointer();
        nativeInput().setKeyboardBindings(DarkRecomp::loadKeyboardBindings(settingsPath));
        initializeDeveloperTools();
        targetFps = activeGraphics.frameRateLimit;
        fullscreen = activeGraphics.fullscreen;
        renderHeight = activeGraphics.renderHeight;
        if (!AudioRenderDriver::instance().setMuted(muted))
            throw std::runtime_error("Cannot configure native audio output volume");
        if(manualShadowCapture)configureShadowCapture(rendererTrace);
        else configureRenderTrace(rendererTrace);
        if (enginePreview) enableEnginePreview();
        if (!previewFrame.empty() && std::filesystem::exists(previewFrame))
            throw std::runtime_error("Preview frame path already exists");
        Memory addressSpace;
        memory = &addressSpace;
        addressSpace.load(gameDir);
        PPCContext ctx{};
        addressSpace.initThread(ctx);
        initializeKernel();
        if (timeout) std::thread([timeout, &ctx] {
            Sleep(timeout);
            fputs("[TIMEOUT] Guest execution exceeded the diagnostic deadline.\n", stderr);
            auto audio = AudioRenderDriver::instance().counters();
            fprintf(stderr, "[Audio] deadline counters: callbacks=%llu submitted=%llu completed=%llu errors=%llu ready=%u worker=%u\n",
                    audio.callbacks, audio.framesSubmitted, audio.buffersCompleted, audio.deviceErrors,
                    unsigned(audio.deviceReady), unsigned(audio.workerRunning));
            printAudioHealth();
            auto input = nativeInput().counters();
            fprintf(stderr, "[Input] deadline counters: polls=%llu connected=%llu nonneutral=%llu changes=%llu\n",
                    input.polls, input.connected, input.nonneutral, input.changes);
            fprintf(stderr, "[Input] native mouse events=%llu\n", input.mouseEvents);
            printPreviewCounters();
            printContext(ctx);
            finishAudioEvidence();
            fflush(nullptr);
            ExitProcess(5);
        }).detach();

        SetProcessDPIAware();
        NativeDisplaySize requestedSize{windowWidth ? windowWidth : 1280, windowHeight ? windowHeight : 720};
        if (fullscreen && !windowWidth) {
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
                throw std::runtime_error("Cannot determine display resolution");
            requestedSize = {uint32_t(monitor.rcMonitor.right - monitor.rcMonitor.left),
                             uint32_t(monitor.rcMonitor.bottom - monitor.rcMonitor.top)};
        }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = NativeWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.lpszClassName = L"DarkRecompNativeWindow";
        RegisterClassW(&windowClass);
        NativeDisplaySize restoredSize = requestedSize;
        if (fullscreen) {
            const float scale = (std::min)({1.f, 1280.f / requestedSize.width, 720.f / requestedSize.height});
            restoredSize = {uint32_t(requestedSize.width * scale), uint32_t(requestedSize.height * scale)};
        }
        RECT client{0, 0, LONG(restoredSize.width), LONG(restoredSize.height)};
        AdjustWindowRect(&client, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowW(windowClass.lpszClassName, L"The Darkness - DarkRecomp",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            client.right - client.left, client.bottom - client.top, nullptr, nullptr,
            windowClass.hInstance, nullptr);
        if (!window) throw std::runtime_error("Cannot create native display window");
        NativeDisplayWindow displayWindow(window);
        if (fullscreen) displayWindow.toggleFullscreen();
        nativeInput().attachWindow(window);
        nativeInput().windowMessage(window, WM_ACTIVATEAPP, GetForegroundWindow() == window, 0);
        struct ClearMouseWindow { ~ClearMouseWindow() { mouseWindow = nullptr; developerToolsWindow = nullptr; } };
        NativeMouseWindow mouse(window); mouseWindow = &mouse;
        NativeResolutionStatus resolutionStatus(window);
        if (!mouse.registered()) throw std::runtime_error("Cannot register native raw mouse input");
        DeveloperToolsWindow developerTools(window, mouse); developerToolsWindow = &developerTools;
        ClearMouseWindow clearMouseWindow;
        puts("[Input] Native Win32 keyboard/raw mouse ready. Click to capture, Esc releases, F1 controls, F5 developer tools, F6 720p/1440p. WASD=move; E=use; R=reload; captured Space=jump; menu Space=confirm/skip.");
        if (!testInputPath.empty())
            puts("[InputTest] Opt-in script active: '<key> [<holdMs 1..10000>]' (bare menu 250ms, I/J/K/L 2000ms; 116=F5 panel), 'mouse <dx> <dy>', 'capture', '0' inspect; 'dev open|close|status', 'dev mission <ID>', 'dev speed <preset>', 'dev invincible on|off', 'dev capture <absolute BMP path>'; an invalid line blocks later commands until fixed; release lines report poll/nonneutral/change deltas.");

        DarkRecomp::CDisplayContextD3D11 display;
        RECT actualClient{}; GetClientRect(window, &actualClient);
        // Windows may place the new window on a different monitor. Derive
        // automatic fullscreen aspect from the monitor it actually occupies.
        if (fullscreen && !windowWidth)
            requestedSize = {uint32_t(actualClient.right), uint32_t(actualClient.bottom)};
        const auto renderSize = renderSizeForDisplay(requestedSize, renderHeight);
        const auto resolutionScale = nativeResolutionScale(renderSize);
        mouse.setRenderHeight(renderSize.height);
        if (!setNativeVideoMode(renderSize.width / resolutionScale, renderSize.height / resolutionScale))
            throw std::runtime_error("Requested render aspect or size is unsupported");
        display.Init(window, uint32_t(actualClient.right), uint32_t(actualClient.bottom));
        setMenuPointerDisplay(renderSize.width, renderSize.height, uint32_t(actualClient.right), uint32_t(actualClient.bottom));
        displayResizePending = false;
        std::printf("[Display] output=%ldx%ld render=%ux%u aspect=%.6f fullscreen=%u scale=%u guest=%ux%u\n",
                    actualClient.right, actualClient.bottom, renderSize.width, renderSize.height,
                    double(renderSize.width) / renderSize.height, unsigned(fullscreen), resolutionScale,
                    renderSize.width / resolutionScale, renderSize.height / resolutionScale);
        setNativeDisplayContext(&display);
        if (rendererSmoke) {
            display.Clear(DarkRecomp::CDisplayContextD3D11::ClearAll,
                          0.2f, 0.5f, 0.9f, 1.0f, 1.0f);
            uint32_t pixel = 0;
            if (!display.ReadbackCenterPixel(pixel)) {
                fputs("[Frame] D3D11 readback failed.\n", stderr);
                return 6;
            }
            // Verify requested color before Present discards the back buffer.
            const int expected[] = {51, 128, 230, 255};
            for (unsigned channel = 0; channel < 4; ++channel) {
                const int actual = (pixel >> (channel * 8)) & 255;
                if (actual < expected[channel] - 1 || actual > expected[channel] + 1) {
                    fprintf(stderr, "[Frame] Incorrect native clear color: 0x%08X\n", pixel);
                    return 7;
                }
            }
            const HRESULT presentStatus = display.Present();
            if (FAILED(presentStatus)) {
                fprintf(stderr, "[Frame] D3D11 Present failed: 0x%08X\n", unsigned(presentStatus));
                return 6;
            }
            printf("[Frame] Native diagnostic clear verified; center RGBA=0x%08X; Present status=0x%08X\n",
                   pixel, unsigned(presentStatus));
            display.Destroy();
            return 0;
        }
        puts("[Boot] Native D3D11 display initialized; calling original game entry at 0x828AA3E8");
        std::unique_ptr<DarkRecomp::EnginePreviewD3D11> preview;
        if (enginePreview) {
            preview = std::make_unique<DarkRecomp::EnginePreviewD3D11>(display.GetDevice(), display.GetContext(), display.GetSwapChain(),
                                                                    renderSize.width, renderSize.height, resolutionScale,true);
            preview->setDiagnostics(!rendererTrace.empty() && !manualShadowCapture);
            setPreviewTextureBudget(preview->textureBudget());
            setPreviewFrameBackpressure(true,true);
            puts("[EnginePreview] Experimental original text, YUV video and BC1/BC3 color rendering; unsupported materials are skipped.");
        }
        bool savedPreview = false;
        unsigned shadowCaptureCount=0,shadowCaptureActive=0;
        bool shadowCapturePending=false;
        if(manualShadowCapture)
            puts("[ShadowCapture] Press F8 once with the shadow visible, then once where it disappears. Two captures maximum; keep still until each capture finishes.");
        unsigned videoCaptureCount = 0;
        unsigned colorCaptureCount = 0, worldCaptureCount = 0;
        const auto inputTestEpoch=GetTickCount64();
        ULONGLONG nextIntroSkip=inputTestEpoch+3000,worldInputStart=0,keyRelease=0;
        unsigned testKey=0;bool startSent=false;
        ULONGLONG testKeyHoldMs=kTestInputMenuHoldMs;
        InputCounters testKeyBase{};
        unsigned testInputInvalidLogs=0;
        size_t liveInputCount=0,captureInputCount=0;
        ULONGLONG inputCaptureAt=0,nextInputRead=0;bool liveKey=false;
        bool testMouse=false;LONG testMouseX=0,testMouseY=0;
        InputCounters testMouseBase{};
        ULONGLONG testMouseUntil=0,nextMouseSample=0;
        ULONGLONG nextWorldCapture = 0;
        ULONGLONG nextColorCapture = 0;
        std::shared_ptr<const DarkRecomp::VideoFrame> lastCapturedVideo;
        std::vector<DarkRecomp::SimpleMesh> previewMeshes;
        double pendingFrameRenderMs=0;
        size_t pendingFrameMeshes=0,pendingSimpleSubmissions=0;
        bool pendingFrameColor=false;
        uint32_t pendingFirstColor=0;
        std::shared_ptr<const DarkRecomp::VideoFrame> pendingFrameVideo;

        int guestStatus = 0;
        NativeTimerResolution timerResolution;
        NativeFramePacer framePacer(targetFps);
        // Present/input thread above normal: level loads saturate cores with
        // engine workers and previously starved presentation for seconds.
        configureGameThread("display",1,THREAD_PRIORITY_ABOVE_NORMAL);
        initializeSceneWorkers(sceneWorkers);
        std::thread guest([&] {
            configureGameThread("engine",0);
            // The guest context is thread-local for diagnostics and APC delivery.
            currentContext = &ctx;
            guestStatus = runGuest(ctx, addressSpace.base());
            fprintf(stderr, "[STOP] Guest execution ended with status %d.\n", guestStatus);
            // Worker threads still own guest memory. A failed main entry must
            // use process teardown before the address space destructor runs.
            if (guestStatus) { fflush(nullptr); ExitProcess(guestStatus); }
            PostMessageW(window, WM_CLOSE, 0, 0);
        });

        NativeCpuSampler sampler(guest.native_handle(),sampleEngine,sampleWorkers,sampleRendererCpu);
        MSG message{};
        FrameMetrics performance;
        uint64_t unchangedDisplaySkips = 0;
        FrameOutlierTrace outlier;
        // Stage-boundary clocks work independently of per-draw profiling and
        // instruction sampling, so normal launches can diagnose slow frames.
        const bool outlierOn = traceFrameHitches || profileEngineCpu;
        outlier.reset(outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{});
        outlier.setEnabled(outlierOn);
        std::printf("[FrameHitchTrace] enabled=%u thresholdMs=%.1f minLogGapMs=%.0f maxLines=%u profileEngine=%u sampleEngine=%u sampleWorkers=%u sampleRenderer=%u automaticCaptures=%u\n",
            unsigned(outlierOn), FrameOutlierTrace::kThresholdMs, FrameOutlierTrace::kMinEmitGapMs,
            FrameOutlierTrace::kMaxLines, unsigned(profileEngineCpu), unsigned(sampleEngine),
            unsigned(sampleWorkers), unsigned(sampleRendererCpu), unsigned(!previewFrame.empty()));
        std::printf("[Performance] Native frame target=%u; engine frames are counted independently of presentation.\n", targetFps);
        std::puts("[PerformanceDefinition] version=3 frameTimestamp=Present-return frameCount=new-engine-frame-and-S_OK worldCount=accepted-world-frame presents=all-calls-presentOk-presentOccluded-presentFailed-presentOther rendered=all-rendered-frames renderedFPS=rendered-per-second renderCPUms=accepted-render-only intervals=completed-accepted-frame-returns-not-scanout lastAcceptedAgeMs=since-last-accepted-or-since-start intervalSamples=completed-interval-count haveAcceptedFrame=any-accepted-yet historicalFrameTimestamp=render-completion");
        while (message.message != WM_QUIT) {
            FrameMetrics::Clock::time_point loopTop{}, afterPump{}, takeEnd{}, renderEnd{}, postEnd{}, pacerEnd{};
            if (outlierOn) {
                loopTop = FrameMetrics::now();
                outlier.onLoopTop(loopTop);
            }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (keyboardMenuInputBlocked() && message.hwnd == window &&
                    (message.message == WM_KEYDOWN || message.message == WM_KEYUP ||
                     message.message == WM_SYSKEYDOWN || message.message == WM_SYSKEYUP)) {
                    DispatchMessageW(&message);
                    continue;
                }
                if (developerTools.handleMessage(message)) continue;
                if(manualShadowCapture && message.hwnd==window && message.message==WM_KEYDOWN &&
                   message.wParam==VK_F8 && !(message.lParam&(LPARAM(1)<<30))) {
                    if(preview && !shadowCapturePending && !shadowCaptureActive && shadowCaptureCount<2) {
                        shadowCapturePending=true;
                        std::printf("[ShadowCapture] requested=%u; waiting for a complete render-frame boundary.\n",shadowCaptureCount+1);
                    }
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (message.message == WM_QUIT) break;
            if (guestMenuPointerActive()) mouse.release();
            KeyboardMenuSaveRequest keyboardSave;
            if (takeKeyboardMenuSaveRequest(keyboardSave)) {
                const bool saved = DarkRecomp::saveKeyboardBindings(settingsPath, keyboardSave.bindings) &&
                                   nativeInput().setKeyboardBindings(keyboardSave.bindings);
                reportKeyboardMenuSave(keyboardSave.id, saved);
                std::printf("[KeyboardMenu] Save id=%llu success=%u\n", keyboardSave.id, unsigned(saved));
            }
            if (takeExitGameRequest()) {
                std::puts("[Menu] Exit Game confirmed; closing native display.");
                PostMessageW(window, WM_CLOSE, 0, 0);
                continue;
            }
            resolutionStatus.update();
            try {
                const bool frameOpen=preview && preview->frameInProgress();
                if (!frameOpen) {
                    if (resolutionTogglePending) {
                        resolutionTogglePending = false;
                        const auto guestMode = nativeVideoMode();
                        if (!preview || guestMode.height != 720) {
                            resolutionStatus.show(L"F6 is unavailable at this launch resolution/aspect.");
                            std::printf("[ResolutionHotkey] unavailable guest=%ux%u; select a 720-high guest mode at startup.\n",
                                guestMode.width, guestMode.height);
                        } else {
                            const auto nextScale = preview->renderHeight() == 720 ? 2u : 1u;
                            const auto width = guestMode.width * nextScale;
                            const auto height = guestMode.height * nextScale;
                            const auto resizeStarted = FrameMetrics::now();
                            bool resized = false;
                            try {
                                preview->resizeRenderTarget(width, height, nextScale);
                                resized = true;
                            } catch (const std::exception& resolutionError) {
                                displayReuse.invalidate();
                                resolutionStatus.show(L"Resolution switch failed; the previous resolution is retained.");
                                std::fprintf(stderr,"[ResolutionHotkey] failed: %s\n",resolutionError.what());
                            }
                            if (resized) {
                                displayReuse.invalidate();
                                auto updated = graphicsSettings(); updated.renderHeight = height;
                                setGraphicsSettings(updated); requestDisplaySettingsSave();
                                mouse.setRenderHeight(height);
                                RECT pointerClient{}; GetClientRect(window, &pointerClient);
                                setMenuPointerDisplay(width, height, uint32_t(pointerClient.right), uint32_t(pointerClient.bottom));
                                resolutionStatus.show(L"Render resolution: " + std::to_wstring(width) + L" x " +
                                    std::to_wstring(height) + L" (F6 to switch)");
                                std::printf("[ResolutionHotkey] applied render=%ux%u scale=%u guest=%ux%u elapsedMs=%.3f\n",
                                    width, height, nextScale, guestMode.width, guestMode.height, FrameMetrics::ms(resizeStarted));
                            }
                        }
                    }
                    if (fullscreenTogglePending) {
                        fullscreenTogglePending = false;
                        auto selected = graphicsSettings();
                        selected.fullscreen = !displayWindow.fullscreen();
                        setGraphicsSettings(selected);
                        requestDisplaySettingsSave();
                    }
                    if (takeDisplaySettingsSaveRequest()) {
                        displayReuse.invalidate();
                        const bool saved = DarkRecomp::saveDisplaySettings(settingsPath, fieldOfViewSetting(), graphicsSettings(),
                                                                          gameLanguageSetting());
                        reportDisplaySettingsSave(saved);
                        if (!saved) std::fputs("[Settings] Could not save display/language settings.\n", stderr);
                    }
                    const auto selected = graphicsSettings();
                    if (selected != activeGraphics) displayReuse.invalidate();
                    if (selected.frameRateLimit != activeGraphics.frameRateLimit)
                        framePacer.setFrameRate(selected.frameRateLimit);
                    if (selected.fullscreen != displayWindow.fullscreen()) {
                        mouse.release(); displayWindow.toggleFullscreen();
                    }
                    // Menu resolution changes still apply at startup. F6 only
                    // changes integer raster scale while retaining guest layout.
                    activeGraphics = selected;
                }
                if (displayResizePending && !frameOpen && !IsIconic(window)) {
                    displayResizePending = false;
                    displayReuse.invalidate();
                    RECT size{}; GetClientRect(window, &size);
                    if (size.right > 0 && size.bottom > 0) {
                        if (preview) preview->releaseDisplayTarget();
                        display.Resize(uint32_t(size.right), uint32_t(size.bottom));
                        setMenuPointerDisplay(preview ? preview->renderWidth() : renderSize.width,
                            preview ? preview->renderHeight() : renderSize.height, uint32_t(size.right), uint32_t(size.bottom));
                    }
                }
            } catch (const std::exception& resizeError) {
                std::                fprintf(stderr, "[Display] Resize failed: %s\n", resizeError.what());
                fflush(nullptr);
                ExitProcess(6); // Guest threads still own the address space.
            }
            // Opt-in integration input enters the same native input adapter
            // exercised by InputContract. It sends no input to another process.
            const auto inputNow=GetTickCount64();
            if(testKey && inputNow>=keyRelease) {
                MSG keyMessage{window, WM_KEYUP, WPARAM(testKey), 0};
                if (!nativeInput().windowMessage(window,WM_KEYUP,testKey,0) &&
                    !handleResolutionHotkey(WM_KEYUP, testKey, 0)) developerTools.handleMessage(keyMessage);
                nativeInput().windowMessage(window,WM_ACTIVATEAPP,GetForegroundWindow()==window,0);
                const auto released=nativeInput().counters();
                std::printf("[InputTest] released key=%u elapsed=%llu polls=+%llu nonneutral=+%llu changes=+%llu\n",testKey,inputNow-inputTestEpoch,
                    released.polls-testKeyBase.polls,released.nonneutral-testKeyBase.nonneutral,released.changes-testKeyBase.changes);testKey=0;
                if(liveKey){inputCaptureAt=inputNow+1000;captureInputCount=liveInputCount;liveKey=false;}
            }
            if(!worldInputStart && previewWorldActive())worldInputStart=inputNow;
            if(!testKey && !testMouse && testStart && worldInputStart && !startSent && inputNow>=worldInputStart+4000) {
                testKey=VK_RETURN;testKeyHoldMs=kTestInputMenuHoldMs;startSent=true;
            } else if(!testKey && !testMouse && testSkipIntros && !worldInputStart && inputNow>=nextIntroSkip) {
                testKey=VK_SPACE;testKeyHoldMs=kTestInputMenuHoldMs;nextIntroSkip=inputNow+5000;
            }
            if(!testKey && !testMouse && !testInputPath.empty() && inputNow>=nextInputRead) {
                nextInputRead=inputNow+250;std::ifstream stream(testInputPath);size_t count=0;
                std::string command;
                while(std::getline(stream,command)) {
                    if(++count>64)break;
                    if(count<=liveInputCount)continue;
                    // Explicit diagnostic commands take over from automatic
                    // intro skipping, so synthetic Space cannot activate a
                    // menu while a live test is navigating it.
                    if (!command.empty()) testSkipIntros = false;
                    std::istringstream fields(command);
                    if(command.starts_with("mouse ")) {
                        std::string token,extra;LONG dx=0,dy=0;
                        if(!(fields>>token>>dx>>dy) || (fields>>extra) || dx< -10000 || dx>10000 || dy< -10000 || dy>10000)break;
                        nativeInput().windowMessage(window,WM_SETFOCUS,0,0);
                        nativeInput().setMouseLookEnabled(true);
                        testMouse=true;testMouseX=dx;testMouseY=dy;testMouseUntil=inputNow+2000;nextMouseSample=inputNow;
                        testMouseBase=nativeInput().counters();
                        liveInputCount=count;
                        std::printf("[InputTest] native mouse dx=%ld dy=%ld elapsed=%llu\n",dx,dy,inputNow-inputTestEpoch);
                        break;
                    }
                    if(command=="capture") {liveInputCount=count;captureInputCount=count;inputCaptureAt=inputNow;break;}
                    if(command=="0"){liveInputCount=count;captureInputCount=count;
                        if(manualShadowCapture) {
                            if(!shadowCapturePending && !shadowCaptureActive && shadowCaptureCount<2)shadowCapturePending=true;
                        } else {
                            inputCaptureAt=inputNow;inspectNextEngineFrame();if(preview)preview->inspectNextWorldFrame();
                        }
                        break;}
                    if(command.starts_with("dev ") && developerTools.handleTestCommand(command)) {
                        liveInputCount=count;
                        continue;
                    }
                    if(command == "keyboard status") {
                        std::printf("[KeyboardMenu] capture=%u reload=%s status=%s\n", unsigned(keyboardMenuCaptureActive()),
                            keyboardMenuLabel("darkrecomp.keyboard.bind.10.0").c_str(),
                            keyboardMenuLabel("darkrecomp.keyboard.status").c_str());
                        liveInputCount=count;
                        continue;
                    }
                    if(command.starts_with("pointer ") || command.starts_with("click ")) {
                        std::string token, extra; int x=0,y=0;
                        RECT pointerClient{}; GetClientRect(window,&pointerClient);
                        if (!(fields>>token>>x>>y) || (fields>>extra) || x<0 || y<0 ||
                            x>=pointerClient.right || y>=pointerClient.bottom) break;
                        nativeInput().windowMessage(window,WM_SETFOCUS,0,0);
                        const LPARAM point=MAKELPARAM(x,y);
                        nativeInput().windowMessage(window,WM_MOUSEMOVE,0,point);
                        if(token=="click") {
                            nativeInput().windowMessage(window,WM_LBUTTONDOWN,MK_LBUTTON,point);
                            nativeInput().windowMessage(window,WM_LBUTTONUP,0,point);
                        }
                        liveInputCount=count; captureInputCount=count; inputCaptureAt=inputNow+1000;
                        std::printf("[InputTest] %s x=%d y=%d elapsed=%llu\n",token.c_str(),x,y,inputNow-inputTestEpoch);
                        break;
                    }
                    unsigned key=0;ULONGLONG hold=kTestInputMenuHoldMs;
                    if(!parseTestInputKeyLine(command,key,hold)) {
                        if(testInputInvalidLogs<8) {
                            ++testInputInvalidLogs;
                            std::fprintf(stderr,"[InputTest] blocked on invalid command %zu elapsed=%llu (later commands wait until this line is fixed)\n",count,inputNow-inputTestEpoch);
                        }
                        break;
                    }
                    testKey=key;testKeyHoldMs=hold;liveInputCount=count;liveKey=true;break;
                }
            }
            if(testMouse) {
                if(inputNow>=testMouseUntil) {
                    nativeInput().setMouseLookEnabled(false);
                    nativeInput().windowMessage(window,WM_ACTIVATEAPP,GetForegroundWindow()==window,0);
                    testMouse=false;inputCaptureAt=inputNow+1000;captureInputCount=liveInputCount;
                    const auto mouseReleased=nativeInput().counters();
                    std::printf("[InputTest] released native mouse elapsed=%llu polls=+%llu nonneutral=+%llu changes=+%llu\n",inputNow-inputTestEpoch,
                        mouseReleased.polls-testMouseBase.polls,mouseReleased.nonneutral-testMouseBase.nonneutral,mouseReleased.changes-testMouseBase.changes);
                } else if(inputNow>=nextMouseSample) {
                    nativeInput().mouseMotion(testMouseX,testMouseY);nextMouseSample=inputNow+16;
                }
            }
            if(testKey && inputNow>=keyRelease) {
                if (testKey != VK_F5) nativeInput().windowMessage(window,WM_SETFOCUS,0,0);
                MSG keyMessage{window, WM_KEYDOWN, WPARAM(testKey), 0};
                if (!nativeInput().windowMessage(window,WM_KEYDOWN,testKey,0) &&
                    !handleResolutionHotkey(WM_KEYDOWN, testKey, 0)) developerTools.handleMessage(keyMessage);
                testKeyBase=nativeInput().counters();
                keyRelease=inputNow+testKeyHoldMs;
                std::printf("[InputTest] pressed key=%u hold=%llums elapsed=%llu\n",testKey,testKeyHoldMs,inputNow-inputTestEpoch);
            }
            if (outlierOn) {
                afterPump = FrameMetrics::now();
                takeEnd = renderEnd = postEnd = pacerEnd = afterPump;
            }
            bool frameRendered=false;
            bool frameWorld=false;
            bool displayCopied=false;
            double frameRenderMs=0;
            if (preview) {
                try {
                    PreviewFramePart part;
                    if (takePreviewFrame(previewMeshes,8,&part)) {
                        displayReuse.invalidate();
                        if(part.first) {
                            if(shadowCapturePending) {
                                shadowCapturePending=false;shadowCaptureActive=++shadowCaptureCount;
                                // Arm the renderer before this first chunk. The
                                // producer may already be building a later frame;
                                // its boundary trace cannot identify this image.
                                preview->inspectNextWorldFrame();
                                std::printf("[ShadowCapture] begin=%u\n",shadowCaptureActive);
                            }
                            pendingFrameRenderMs=0;pendingFrameMeshes=pendingSimpleSubmissions=0;
                            pendingFrameColor=false;pendingFirstColor=0;pendingFrameVideo.reset();
                        }
                        if (outlierOn) takeEnd = FrameMetrics::now();
                        const auto renderStart = FrameMetrics::now();
                        setRenderSamplePhase(RenderSamplePhase::other);sampler.beginRender();
                        preview->render(previewMeshes,part);
                        sampler.endRender();
                        if (outlierOn) renderEnd = FrameMetrics::now();
                        pendingFrameRenderMs+=FrameMetrics::ms(renderStart);
                        pendingFrameMeshes+=previewMeshes.size();
                        for(const auto& mesh:previewMeshes) {
                            pendingSimpleSubmissions+=!mesh.vertices.empty();
                            if(mesh.video && !pendingFrameVideo)pendingFrameVideo=mesh.video;
                            if(mesh.colorTexture && !pendingFrameColor) {
                                pendingFrameColor=true;pendingFirstColor=mesh.textureId;
                            }
                        }
                        frameRendered=part.last;
                        frameRenderMs=pendingFrameRenderMs;
                        frameWorld=preview->worldPresented();
                        if(frameRendered) {
                        if(shadowCaptureActive) {
                            const auto completedCapture=shadowCaptureActive;
                            shadowCaptureActive=0;
                            const auto path=rendererTrace/(L"inspection-"+std::to_wstring(completedCapture)+L".bmp");
                            const bool saved=preview->trySaveShadowCapture(path);
                            std::printf("[ShadowCapture] %s=%u worldPresented=%u screenshot=%s\n",
                                saved?"saved":"failed",completedCapture,unsigned(preview->worldPresented()),path.string().c_str());
                            const auto title=L"The Darkness - Shadow capture "+std::to_wstring(completedCapture)+
                                (saved?L"/2 saved":L"/2 screenshot failed; see log");
                            SetWindowTextW(window,title.c_str());
                        }
                        if(preview->worldPresented() && !previewFrame.empty() && worldCaptureCount<4 && GetTickCount64()>=nextWorldCapture) {
                            ++worldCaptureCount;nextWorldCapture=GetTickCount64()+5000;
                            const auto path=previewFrame.parent_path()/(previewFrame.stem().wstring()+L"-world-"+std::to_wstring(worldCaptureCount)+L".bmp");
                            if(std::filesystem::exists(path))throw std::runtime_error("World capture path already exists");
                            preview->saveBmp(path);
                            std::printf("[EngineWorldPresent] Captured resolved original frontbuffer %u.\n",worldCaptureCount);
                        }
                        recordPreviewRender(pendingSimpleSubmissions);
                        if (pendingSimpleSubmissions && !savedPreview && !previewFrame.empty()) {
                            preview->saveBmp(previewFrame); savedPreview = true;
                            printf("[EnginePreview] Captured native frame containing %zu original mesh submissions.\n", pendingFrameMeshes);
                        }
                        if (pendingFrameVideo && pendingFrameVideo != lastCapturedVideo) {
                            lastCapturedVideo = pendingFrameVideo;
                            ++videoCaptureCount;
                            if (!previewFrame.empty() && (videoCaptureCount == 30 || videoCaptureCount == 90)) {
                                auto path = previewFrame.parent_path() / (previewFrame.stem().wstring() +
                                            L"-video-" + std::to_wstring(videoCaptureCount) + L".bmp");
                                if (std::filesystem::exists(path)) throw std::runtime_error("Video capture path already exists");
                                preview->saveBmp(path);
                                printf("[EnginePreview] Captured native video frame %u at original timestamp %.9f seconds.\n",
                                       videoCaptureCount, pendingFrameVideo->timestampSeconds);
                            }
                        }
                        if (pendingFrameColor && !previewFrame.empty() && colorCaptureCount < 4 && GetTickCount64() >= nextColorCapture) {
                            ++colorCaptureCount; nextColorCapture = GetTickCount64() + 5000;
                            auto path = previewFrame.parent_path() / (previewFrame.stem().wstring() +
                                        L"-color-" + std::to_wstring(colorCaptureCount) + L".bmp");
                            if (std::filesystem::exists(path)) throw std::runtime_error("Color capture path already exists");
                            preview->saveBmp(path);
                            printf("[EnginePreview] Captured native color frame %u containing %zu original meshes; first color texture=%u.\n",
                                   colorCaptureCount, pendingFrameMeshes, pendingFirstColor);
                        }
                        }
                    } else if (outlierOn) {
                        takeEnd = renderEnd = FrameMetrics::now();
                    }
                    if(!preview->frameInProgress() && inputCaptureAt && inputNow>=inputCaptureAt && !previewFrame.empty()) {
                        const auto path=previewFrame.parent_path()/(previewFrame.stem().wstring()+L"-input-"+std::to_wstring(captureInputCount)+L".bmp");
                        if(std::filesystem::exists(path))throw std::runtime_error("Input capture path already exists");
                        preview->saveBmp(path);inputCaptureAt=0;
                        std::printf("[InputTest] captured native frame after command %zu\n",captureInputCount);
                    }
                    if(displayReuse.shouldCopy(preview->frameInProgress())) {
                        preview->copyToDisplay();displayCopied=true;
                    }
                    if (outlierOn) postEnd = FrameMetrics::now();
                } catch (const std::exception& error) {
                    fprintf(stderr, "[EnginePreview] %s\n", error.what());
                    fflush(nullptr);
                    ExitProcess(6);
                }
            }
            // Missing an engine frame must not consume another capped frame
            // slot. The bounded queue wait above already sleeps until ready.
            // Pace after render/capture/copy, immediately before submission.
            if(!preview || frameRendered) {
                try {
                    framePacer.wait();
                } catch (const std::exception& timerError) {
                    fprintf(stderr, "[Frame] Native frame timer failed: %s\n", timerError.what());
                    fflush(nullptr);
                    ExitProcess(6);
                }
            }
            if (outlierOn) pacerEnd = FrameMetrics::now();
            if (frameRendered) performance.rendered();
            const auto presentStart = outlierOn ? pacerEnd : FrameMetrics::now();
            const bool shouldPresent=!preview || displayCopied;
            const HRESULT presentStatus = shouldPresent ? display.Present(activeGraphics.verticalSync ? 1 : 0) : S_FALSE;
            // Occlusion and other statuses keep the regular retry path active.
            // Only an accepted copy can arm the unchanged-image fast path.
            if(shouldPresent)displayReuse.presented(presentStatus);
            else if(preview && !preview->frameInProgress())++unchangedDisplaySkips;
            const auto presentEnd = FrameMetrics::now();
            if(shouldPresent)
                performance.present(presentStatus, std::chrono::duration<double,std::milli>(presentEnd-presentStart).count());
            const bool isAccepted = shouldPresent && frameRendered && presentStatus == S_OK;
            if(isAccepted)
                performance.frame(frameWorld,frameRenderMs,presentEnd);
            if (outlierOn) {
                FrameOutlierTrace::LoopStages stages;
                stages.pump = FrameMetrics::ms(loopTop, afterPump);
                stages.take = FrameMetrics::ms(afterPump, takeEnd);
                stages.render = FrameMetrics::ms(takeEnd, renderEnd);
                stages.post = FrameMetrics::ms(renderEnd, postEnd);
                stages.pacer = FrameMetrics::ms(postEnd, pacerEnd);
                stages.present = shouldPresent ? FrameMetrics::ms(presentStart, presentEnd) : 0;
                FrameOutlierTrace::Report outlierReport;
                if(shouldPresent)outlierReport=outlier.onPresent(presentEnd, presentStatus, stages, isAccepted, frameWorld);
                else outlier.onUnpresentedLoop(presentEnd,stages);
                if (outlierReport.shouldLog) {
                    std::fprintf(stderr, "[FrameOutlier] elapsedMs=%.3f serial=%u intervalMs=%.3f loops=%u miss=%u world=%u presentOk=%u occluded=%u failed=%u other=%u pumpSum=%.3f pumpMax=%.3f takeSum=%.3f takeMax=%.3f renderSum=%.3f renderMax=%.3f postSum=%.3f postMax=%.3f pacerSum=%.3f pacerMax=%.3f presentSum=%.3f presentMax=%.3f tailSum=%.3f tailMax=%.3f residualMs=%.3f emitted=%u suppressed=%u elapsedNotCpu=1\n",
                        outlierReport.runElapsedMs, outlierReport.serial, outlierReport.intervalMs, outlierReport.loops, outlierReport.miss, outlierReport.world ? 1u : 0u,
                        outlierReport.presentOk, outlierReport.presentOccluded, outlierReport.presentFailed, outlierReport.presentOther,
                        outlierReport.sum[FrameOutlierTrace::Pump], outlierReport.max[FrameOutlierTrace::Pump],
                        outlierReport.sum[FrameOutlierTrace::Take], outlierReport.max[FrameOutlierTrace::Take],
                        outlierReport.sum[FrameOutlierTrace::Render], outlierReport.max[FrameOutlierTrace::Render],
                        outlierReport.sum[FrameOutlierTrace::Post], outlierReport.max[FrameOutlierTrace::Post],
                        outlierReport.sum[FrameOutlierTrace::Pacer], outlierReport.max[FrameOutlierTrace::Pacer],
                        outlierReport.sum[FrameOutlierTrace::Present], outlierReport.max[FrameOutlierTrace::Present],
                        outlierReport.sum[FrameOutlierTrace::Tail], outlierReport.max[FrameOutlierTrace::Tail],
                        outlierReport.residualMs, outlierReport.emitted, outlierReport.suppressed);
                }
            }
            double measuredFps = 0;
            if (performance.report(measuredFps)) {
                // A live 150s run showed a metronomic ~73ms tail hitch on the
                // first accepted frame after each 5s report. Time each step
                // and log only hitch-sized totals; steady runs stay quiet.
                // Clock reads are gated on the hitch trace (normal launchers).
                const auto reportStart = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                mouse.setFrameRate(measuredFps);
                const auto afterFps = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                if(preview)preview->printPerformance();
                if(preview)std::printf("[DisplayReuse] unchangedSkips=%llu\n",unchangedDisplaySkips);
                const auto afterPreview = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                printEngineCpuPerformance();
                const auto afterEngine = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                printSceneWorkCounters();
                const auto afterScene = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                performance.threadCpu(guest.native_handle());
                const auto afterThread = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                printAudioHealth();
                const auto afterAudio = outlierOn ? FrameMetrics::now() : FrameMetrics::Clock::time_point{};
                sampler.report();
                // Flush the 64KB log buffers here so no hot thread ever pays
                // disk latency per write; steady-state logging stays memcpy.
                fflush(nullptr);
                if (outlierOn) {
                    const double totalMs = FrameMetrics::ms(reportStart, FrameMetrics::now());
                    if (totalMs > 10.0)
                        std::fprintf(stderr, "[ReportCost] totalMs=%.3f fps=%.3f preview=%.3f engine=%.3f scene=%.3f threadCpu=%.3f audio=%.3f\n",
                            totalMs, FrameMetrics::ms(reportStart, afterFps), FrameMetrics::ms(afterFps, afterPreview),
                            FrameMetrics::ms(afterPreview, afterEngine), FrameMetrics::ms(afterEngine, afterScene),
                            FrameMetrics::ms(afterScene, afterThread), FrameMetrics::ms(afterThread, afterAudio));
                }
            }
            if (FAILED(presentStatus)) {
                fprintf(stderr, "[Frame] Native Present failed: 0x%08X\n", unsigned(presentStatus));
                // The guest still owns live threads. Match the diagnostic
                // deadline's process teardown rather than unwinding past them.
                fflush(nullptr);
                ExitProcess(6);
            }
        }
        setPreviewFrameBackpressure(false);
        timerResolution.stop();
        sampler.stop();
        developerTools.close();
        endKeyboardMenu();
        mouse.release();
        nativeInput().attachWindow(nullptr);
        printAudioHealth();
        // The original game workers still own the address space. Closing the
        // native window must also end an interactive run with no deadline,
        // without waiting forever for the original main loop to return.
        finishAudioEvidence();
        fflush(nullptr);
        ExitProcess(0);
    } catch (const std::exception& error) {
        fprintf(stderr, "[ERROR] %s\n", error.what());
        return 1;
    }
}

