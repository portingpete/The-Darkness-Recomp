#include "app/windows/frame_timeline.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DarkRecomp::FrameTimeline;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static size_t columns(const std::string& line) {
    size_t count = 1;
    for (char c : line) count += c == ',';
    return count;
}
static std::vector<std::string> lines(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}
static Row world(uint64_t frame, double t, double interval, double take, double render, double present) {
    Row r; r.frame = frame; r.tMs = t; r.intervalMs = interval; r.targetFps = 60; r.world = true;
    r.takeMs = take; r.renderMs = render; r.presentMs = present; r.gpuId = frame;
    return r;
}
int main() {
    try {
        {
            const std::string header = csvHeader();
            const std::string row = formatRow(Row{});
            require(!row.empty() && row.back() == '\n', "row must be one complete line");
            require(columns(header) == columns(row), "row and header column counts differ");
            require(header.find("gpu_ms") != std::string::npos && header.find("engine_cpu_ms") != std::string::npos,
                    "header must name GPU and engine columns");
        }
        {
            require(std::fabs(budgetMs(60) - 1000.0 / 60) < 1e-9, "60 FPS budget");
            require(std::fabs(slowThresholdMs(60) - 25.0) < 1e-9, "60 FPS threshold is 1.5x budget");
            require(std::fabs(slowThresholdMs(144) - (1000.0 / 144 + 4.0)) < 1e-9, "high caps keep a 4 ms margin");
            require(std::fabs(slowThresholdMs(0) - 25.0) < 1e-9, "uncapped uses a 60 Hz budget");
        }
        {
            SummaryAccumulator s;
            for (unsigned i = 0; i < 97; ++i) s.add(world(i + 1, 16.667 * (i + 1), 16.667, 1, 3, 0.5));
            s.add(world(98, 2000, 40.0, 30, 3, 0.5));   // engine late
            s.add(world(99, 2100, 60.0, 1, 50, 0.5));   // render-thread spike
            s.add(world(100, 2200, 120.0, 1, 2, 110));  // present blocked
            Row menu = world(101, 2300, 500.0, 490, 1, 1); menu.world = false; menu.markers = 2;
            s.add(menu);
            const auto r = s.result();
            require(r.frames == 101 && r.worldFrames == 100, "frame counts");
            require(r.slow == 3 && r.over2x == 3 && r.over50 == 2 && r.over100 == 1, "slow-frame counts");
            require(r.markers == 2, "markers count on every row");
            using A = SummaryAccumulator;
            require(r.dominant[A::Take] == 1 && r.dominant[A::Render] == 1 && r.dominant[A::Present] == 1, "dominant stage per slow frame");
            require(std::fabs(r.maximum - 120.0) < 1e-9 && std::fabs(r.p50 - 16.667) < 1e-9, "percentiles use world frames only");
            const auto text = formatSummary(r, "[FrameDropSummary]");
            require(text.find("slowFrames=3") != std::string::npos && text.find("dominant engineWait=1") != std::string::npos,
                    "summary text");
        }
        {
            std::mt19937_64 random(std::random_device{}());
            const auto path = std::filesystem::temp_directory_path() /
                ("frame-timeline-test-" + std::to_string(random()) + ".csv");
            {
                Recorder recorder(path);
                recorder.comment("target_fps=60");
                bool rejected = false;
                try { Recorder duplicate(path); } catch (const std::runtime_error&) { rejected = true; }
                require(rejected, "existing CSV must be refused");
                for (uint64_t i = 1; i <= 600; ++i) {
                    recorder.append(world(i, double(i) * 16.667, 16.667, 1, 3, 0.5));
                    // Results arrive two frames late, as from the GPU ring.
                    if (i > 2) recorder.gpuResult(i - 2, 4.0 + double(i % 7), 0.25);
                }
                recorder.gpuResult(1000000, 1, 1); // unknown id is counted, not applied
                recorder.close();
                recorder.close(); // idempotent
                const auto s = recorder.summary();
                require(s.worldFrames == 600, "summary sees every row");
            }
            const auto text = lines(path);
            std::filesystem::remove(path);
            require(text.size() > 3 && text[0] == "# target_fps=60", "comment precedes header");
            require(text[1].rfind("frame,t_ms,interval_ms", 0) == 0, "column header follows comments");
            size_t rows = 0, measured = 0, trailer = 0;
            const size_t width = columns(text[1]);
            for (size_t i = 2; i < text.size(); ++i) {
                if (text[i].rfind("#", 0) == 0) { ++trailer; continue; }
                ++rows;
                require(columns(text[i]) == width, "every row has every column");
                std::stringstream fields(text[i]);
                std::string field;
                for (int column = 0; column <= 19 && std::getline(fields, field, ','); ++column)
                    if (column == 19 && std::stod(field) >= 4.0) ++measured;
            }
            require(rows == 600, "every appended row is written");
            require(measured == 598, "late GPU results patch held rows");
            require(trailer == 3, "trailer has row counts and the two summary lines");
            require(text.back().rfind("# summary slowFrameStageMs", 0) == 0, "summary ends the CSV");
        }
        std::puts("FrameTimelineContract passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FrameTimelineContract failed: %s\n", error.what());
        return 1;
    }
}
