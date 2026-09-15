#include <windows.h>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Quote one Windows command-line argument, including trailing backslashes.
std::wstring quote(const std::filesystem::path& path) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : path.wstring()) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    try {
        bool muted = true;
        bool captureFrames = false;
        std::wistringstream options(arguments ? arguments : L"");
        for (std::wstring option; options >> option;) {
            if (option == L"--sound") muted = false;
            else if (option == L"--mute") muted = true;
            else if (option == L"--capture-frames") captureFrames = true;
            else throw std::runtime_error("Supported launcher options: --sound, --mute, --capture-frames.");
        }
        std::vector<wchar_t> module(32768);
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
        if (!length || length >= module.size()) throw std::runtime_error("Cannot locate the preview launcher.");
        const auto binaryDirectory = std::filesystem::path(module.data()).parent_path();
        const auto executable = binaryDirectory / L"DarkRecomp.exe";
        auto root = binaryDirectory;
        bool found = false;
        for (unsigned i = 0; i < 4; ++i) {
            if (std::filesystem::is_regular_file(root / L"Darkness/basefile.exe")) { found = true; break; }
            root = root.parent_path();
        }
        if (!found || !std::filesystem::is_regular_file(executable))
            throw std::runtime_error("Keep this launcher beside DarkRecomp.exe in the built project, with the original Darkness assets present.");

        SYSTEMTIME now{};
        GetSystemTime(&now);
        wchar_t name[96]{};
        swprintf_s(name, L"desktop-%04u%02u%02u-%02u%02u%02u-%03u-%lu", now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
        const auto runDirectory = root / L"build_native/run";
        std::filesystem::create_directories(runDirectory);
        const auto evidence = runDirectory / name;
        if (!std::filesystem::create_directory(evidence))
            throw std::runtime_error("Refusing to reuse an existing preview evidence directory.");
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle log{CreateFileW((evidence / L"runtime.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open the preview diagnostic log.");

        std::wstring command = quote(executable) + L" --game-dir " + quote(root / L"Darkness") +
            L" --timeout-ms 0 --engine-preview --trace-frame-hitches" + (muted ? L" --mute" : L"");
        // Captures synchronously read back the GPU and write a full BMP on the
        // display thread. Keep that diagnostic work out of normal gameplay.
        if (captureFrames) command += L" --preview-frame " + quote(evidence / L"preview.bmp");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = log.value;
        startup.hStdInput = input.value;
        PROCESS_INFORMATION process{};
        // Inherit the interactive launcher's desktop. CREATE_NO_WINDOW suppresses
        // only a console; the child's native game window remains visible.
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process))
            throw std::runtime_error("Cannot start DarkRecomp.exe on the interactive desktop.");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "DarkRecomp preview launch failed", MB_OK | MB_ICONERROR);
        return 1;
    }
}
