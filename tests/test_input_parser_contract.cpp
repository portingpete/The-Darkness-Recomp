#include "app/windows/test_input_parser.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void requireAccept(const std::string& line, unsigned wantKey, ULONGLONG wantHold) {
    unsigned key = 0xDEAD;
    ULONGLONG hold = 0xDEAD;
    if (!parseTestInputKeyLine(line, key, hold) || key != wantKey || hold != wantHold)
        throw std::runtime_error("accepted line parsed wrong: " + line);
}
static void requireReject(const std::string& line) {
    unsigned key = 0xBE;
    ULONGLONG hold = 0xEF;
    if (parseTestInputKeyLine(line, key, hold))
        throw std::runtime_error("invalid line accepted: " + line);
    require(key == 0xBE && hold == 0xEF, "rejection mutated outputs");
}
static void requireWait(const std::string& line, TestInputWait wantKind, ULONGLONG wantMs) {
    TestInputWait kind = TestInputWait::Player;
    ULONGLONG waitMs = 0xDEAD;
    if (!parseTestInputWaitLine(line, kind, waitMs) || kind != wantKind || waitMs != wantMs)
        throw std::runtime_error("wait line parsed wrong: " + line);
}
static void requireNoWait(const std::string& line) {
    TestInputWait kind = TestInputWait::Menu;
    ULONGLONG waitMs = 0xBEEF;
    if (parseTestInputWaitLine(line, kind, waitMs))
        throw std::runtime_error("invalid wait line accepted: " + line);
    require(kind == TestInputWait::Menu && waitMs == 0xBEEF, "wait rejection mutated outputs");
}
static void testWaitLines() {
    requireWait("wait 1", TestInputWait::Milliseconds, 1);
    requireWait("wait 600000", TestInputWait::Milliseconds, 600000);
    requireWait("wait world", TestInputWait::World, 0);
    requireWait("wait menu", TestInputWait::Menu, 0);
    requireWait("wait player", TestInputWait::Player, 0);
    for (const char* line : {"wait", "wait 0", "wait 600001", "wait -5", "wait 5 5", "wait 12x", "wait level",
                             "waits 5", "wait 1000000", ""})
        requireNoWait(line);
}
namespace {
struct TestEvent {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    TestEvent() { require(value != nullptr, "Cannot create reader test event"); }
    ~TestEvent() { CloseHandle(value); }
};
struct TestScript {
    std::filesystem::path path;
    TestScript() {
        wchar_t directory[MAX_PATH]{}, name[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"dti", 0, name),
                "Cannot create reader test file");
        path = name;
    }
    ~TestScript() { DeleteFileW(path.c_str()); }
    void write(std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), std::streamsize(bytes.size()));
        output.close();
        require(bool(output), "Cannot write reader test file");
    }
};
NativeTestInputFileReader::Snapshot waitSnapshot(NativeTestInputFileReader& reader,
                                                const std::vector<std::string>& expected) {
    const auto until = GetTickCount64() + 5000;
    do {
        auto snapshot = reader.latest();
        if (snapshot && *snapshot == expected) return snapshot;
        Sleep(1);
    } while (GetTickCount64() < until);
    throw std::runtime_error("Reader did not publish expected completed snapshot");
}
void testScriptReader() {
    require(completeTestInputLines("32\n87") == std::vector<std::string>{"32"},
            "Truncated trailing command was published");
    require(completeTestInputLines("32\r\n\r\n87 100\r\n") == std::vector<std::string>{"32", "", "87 100"},
            "CRLF or empty-line indexing changed");
    require(completeTestInputLines("32").empty(), "Unterminated command was published");
    std::string many;
    for (unsigned i = 0; i < 65; ++i) many += std::to_string(i) + '\n';
    const auto bounded = completeTestInputLines(many);
    require(bounded.size() == 64 && bounded.front() == "0" && bounded.back() == "63",
            "Reader command bound or indexes changed");

    TestScript file;
    file.write("32\n87");
    NativeTestInputFileReader reader(file.path, NativeTestInputFileReader::readFile, 5);
    const auto original = waitSnapshot(reader, {"32"});
    file.write("32\n87 100\n");
    waitSnapshot(reader, {"32", "87 100"});
    require(*original == std::vector<std::string>{"32"}, "Published snapshot changed during append");
    file.write("32\ninvalid\n");
    const auto invalid = waitSnapshot(reader, {"32", "invalid"});
    file.write("32\n87 300\n");
    const auto corrected = waitSnapshot(reader, {"32", "87 300"});
    require((*invalid)[1] == "invalid", "Concurrent rewrite mutated a borrowed snapshot");
    unsigned key = 0; ULONGLONG hold = 0;
    require(parseTestInputKeyLine((*corrected)[1], key, hold) && key == 'W' && hold == 300,
            "Corrected unconsumed command lost its index or duration");
    reader.stop(); reader.stop();
    TestEvent stop;
    file.write(std::string(kTestInputMaxFileBytes + 1, 'x'));
    require(!NativeTestInputFileReader::readFile(file.path, stop.value), "Oversized script was read");

    // A file can return transient bytes during truncate/rewrite. No single
    // read is enough to publish a command, including a syntactically valid one.
    std::atomic<unsigned> reads = 0;
    std::atomic<bool> stable = false;
    NativeTestInputFileReader changing(L"injected-reader",
        [&](const std::filesystem::path&, HANDLE) -> std::optional<std::string> {
            const auto sequence = ++reads;
            return stable.load() || sequence % 2 ? "32\n" : "87\n";
        }, 1);
    const auto until = GetTickCount64() + 5000;
    while (reads.load() < 20 && GetTickCount64() < until) Sleep(1);
    require(reads.load() >= 20 && !changing.latest(), "Unstable rewrite bytes were published");
    stable = true;
    waitSnapshot(changing, {"32"});
    changing.stop();

    // Block the actual read provider after publishing a snapshot. The display
    // must still obtain that immutable snapshot while file I/O cannot finish.
    TestEvent blocked;
    std::atomic<unsigned> blockedReads = 0;
    NativeTestInputFileReader stalled(L"injected-reader",
        [&](const std::filesystem::path&, HANDLE stopped) -> std::optional<std::string> {
            if (++blockedReads >= 3) {
                SetEvent(blocked.value);
                WaitForSingleObject(stopped, INFINITE);
                return {};
            }
            return "32\n";
        }, 1);
    require(WaitForSingleObject(blocked.value, 5000) == WAIT_OBJECT_0, "Fake read did not reach its block");
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 1000; ++i) {
        const auto snapshot = stalled.latest();
        require(snapshot && *snapshot == std::vector<std::string>{"32"}, "Blocked I/O hid completed input");
    }
    require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2),
            "Snapshot retrieval waited on blocked file I/O");
    stalled.stop(); // Stop also releases the fake read; no detached lifetime.
    NativeTestInputFileReader unused(std::filesystem::path{});
    require(!unused.latest(), "Ordinary gameplay unexpectedly has a diagnostic snapshot");
}

void testQuitFileReader() {
    TestScript quitFile;
    TestEvent stop;
    for (const auto bytes : {"", "quit", "quit ", "QUIT\n", "quit\nextra", "quit\n\n", "87\n"}) {
        quitFile.write(bytes);
        require(!NativeTestInputFileReader::readQuitFile(quitFile.path, stop.value),
                "Incomplete or extra quit-file bytes requested shutdown");
    }
    for (const auto bytes : {"quit\n", "quit\r\n"}) {
        quitFile.write(bytes);
        require(NativeTestInputFileReader::readQuitFile(quitFile.path, stop.value) == "quit\n",
                "Complete quit-file line was not normalized");
    }

    quitFile.write("quit");
    std::atomic<unsigned> quitReads = 0;
    NativeTestInputFileReader quitReader(quitFile.path,
        [&](const std::filesystem::path& path, HANDLE stopped) {
            ++quitReads;
            return NativeTestInputFileReader::readQuitFile(path, stopped);
        }, 1);
    const auto until = GetTickCount64() + 5000;
    while (quitReads.load() < 20 && GetTickCount64() < until) Sleep(1);
    require(quitReads.load() >= 20 && !quitReader.latest(), "Partial quit line was published");

    // Script I/O may be blocked or its current command invalid. Shutdown has
    // a separate owned worker and never waits for that script's publication.
    TestEvent scriptBlocked;
    NativeTestInputFileReader blockedScript(L"blocked-script",
        [&](const std::filesystem::path&, HANDLE stopped) -> std::optional<std::string> {
            SetEvent(scriptBlocked.value);
            WaitForSingleObject(stopped, INFINITE);
            return {};
        }, 1);
    require(WaitForSingleObject(scriptBlocked.value, 5000) == WAIT_OBJECT_0,
            "Script read did not block for independent quit test");
    quitFile.write("quit\n");
    waitSnapshot(quitReader, {"quit"});
    quitFile.write("");
    const auto publishedQuit = quitReader.latest();
    require(publishedQuit && *publishedQuit == std::vector<std::string>{"quit"},
            "A published quit request was revoked by a later file change");
    blockedScript.stop(); quitReader.stop();
}
}
int main() {
    try {
        require(kTestInputMaxHoldMs == 10000, "max hold changed");
        require(kTestInputMenuHoldMs == 250, "menu hold changed");
        require(kTestInputCameraHoldMs == 2000, "camera hold changed");
        // Historical bare-key holds.
        requireAccept("32", 32, 250);
        requireAccept("13", 13, 250);
        requireAccept("27", 27, 250);
        requireAccept("37", 37, 250);
        requireAccept("40", 40, 250);
        requireAccept("73", 73, 2000);
        requireAccept("74", 74, 2000);
        requireAccept("75", 75, 2000);
        requireAccept("76", 76, 2000);
        // New gameplay keys keep the menu hold.
        requireAccept("87", 87, 250);
        requireAccept("65", 65, 250);
        requireAccept("83", 83, 250);
        requireAccept("68", 68, 250);
        requireAccept("69", 69, 250);
        requireAccept("82", 82, 250);
        requireAccept("70", 70, 250);
        requireAccept("88", 88, 250);
        requireAccept("90", 90, 250);
        requireAccept("67", 67, 250);
        requireAccept("81", 81, 250);
        requireAccept("71", 71, 250);
        requireAccept("49", 49, 250);
        requireAccept("52", 52, 250);
        requireAccept("9", 9, 250);
        requireAccept("8", 8, 250);
        requireAccept("16", 16, 250);
        requireAccept("17", 17, 250);
        requireAccept("116", VK_F5, 250);
        requireAccept("117", VK_F6, 250);
        // Explicit bounded holds, including both edges.
        requireAccept("87 1", 87, 1);
        requireAccept("32 10000", 32, 10000);
        requireAccept("73 500", 73, 500);
        requireAccept("116 500", VK_F5, 500);
        requireAccept("117 1", VK_F6, 1);
        requireAccept("117 10000", VK_F6, 10000);
        requireAccept("  69   2000  ", 69, 2000);
        requireAccept("32 ", 32, 250);
        requireAccept(std::string("32") + std::string(254, ' '), 32, 250);
        // Invalid: bad durations, trailing text, signs, overflow, length.
        requireReject("32 0");
        requireReject("32 10001");
        requireReject("32 100000");
        requireReject("32 -1");
        requireReject("32 1.5");
        requireReject("32 abc");
        requireReject("32 100 200");
        requireReject("87 W");
        requireReject("+32");
        requireReject("-1");
        requireReject("3.5");
        requireReject("32x");
        requireReject("x32");
        requireReject("0x20");
        requireReject("W");
        requireReject("0");
        requireReject("256");
        requireReject("999");
        requireReject("7");
        requireReject("115"); // Adjacent F4/F7 keys remain outside the allowlist.
        requireReject("118");
        requireReject("116 0");
        requireReject("116 10001");
        requireReject("116 close");
        requireReject("117 0");
        requireReject("117 10001");
        requireReject("117 close");
        requireReject("117 250 1");
        requireReject("117 -1");
        requireReject("117 1.5");
        requireReject("");
        requireReject("   ");
        requireReject("4294967296");
        requireReject(std::string(300, '1'));
        requireReject(std::string("32") + std::string(255, ' '));
        require(!isTestInputKey(0) && !isTestInputKey(255) && !isTestInputKey(7),
                "allowlist admits an unmapped key");
        require(isTestInputKey('W') && isTestInputKey(VK_TAB) && isTestInputKey(VK_CONTROL) &&
                isTestInputKey(VK_F5) && isTestInputKey(VK_F6),
                "allowlist lost a mapped key");
        testScriptReader();
        testQuitFileReader();
        testWaitLines();
        puts("Test input parser contract: historical holds, explicit 1/10000, "
             "rejected bad/trailing/sign/overflow, unchanged outputs, and "
             "gameplay keys plus F5/F6; asynchronous script snapshots preserve "
             "complete lines, rewrites, bounds and blocked-I/O isolation.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "TestInputParserContract: %s\n", error.what());
        return 1;
    }
}
