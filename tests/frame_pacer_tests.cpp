#include "app/windows/frame_pacer.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
static void require(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
int main() {
    try {
        static_assert(!std::is_copy_constructible_v<NativeFramePacer>, "pacer handle must not copy");
        static_assert(!std::is_copy_assignable_v<NativeFramePacer>, "pacer handle must not copy");
        static_assert(!std::is_copy_constructible_v<NativeTimerResolution>, "timer resolution must not copy");
        using Clock = NativeFramePacer::Clock;
        const auto epoch = Clock::time_point{} + std::chrono::seconds(1000);
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(16));
        auto ticked = epoch + std::chrono::milliseconds(100);
        require(NativeFramePacer::resolveOverrun(ticked, epoch, period) == ticked, "future deadline moved");
        require(NativeFramePacer::resolveOverrun(epoch, epoch + std::chrono::milliseconds(5), period) == epoch, "single overrun lost phase");
        require(NativeFramePacer::resolveOverrun(epoch, epoch + period, period) == epoch + period, "boundary must resync");
        require(NativeFramePacer::resolveOverrun(epoch, epoch + std::chrono::milliseconds(50), period) == epoch + std::chrono::milliseconds(50), "multi-miss must resync");
        // Sequence: small miss retains cadence, long stall recovers with exactly one period and no burst.
        auto d = epoch;
        d += period;
        const auto d1 = d;
        require(NativeFramePacer::resolveOverrun(d1, epoch + std::chrono::milliseconds(5), period) == d1, "early tick moved");
        d = d1;
        d += period;
        const auto d2 = d;
        const auto now2 = d1 + std::chrono::milliseconds(18);
        require(NativeFramePacer::resolveOverrun(d2, now2, period) == d2, "small miss must preserve slot");
        require(d2 - d1 == period, "small miss broke cadence");
        d = d2;
        d += period;
        const auto d3 = d;
        require(d3 - d2 == period, "cadence step wrong");
        d += period;
        const auto d4tick = d;
        const auto nowStall = d3 + std::chrono::seconds(5);
        const auto d4 = NativeFramePacer::resolveOverrun(d4tick, nowStall, period);
        require(d4 == nowStall, "long stall must resync to now");
        auto d5 = d4;
        d5 += period;
        require(NativeFramePacer::resolveOverrun(d5, d4 + std::chrono::milliseconds(5), period) == d5, "post-stall tick moved");
        require(d5 - d4 == period, "post-stall recovery burst");
        NativeFramePacer uncapped(0);
        require(!uncapped.capped(), "fps=0 must stay uncapped");
        uncapped.wait();
        require(!uncapped.capped(), "uncapped wait must not arm timer");
        NativeFramePacer sixty(60);
        require(sixty.capped(), "fps=60 must cap");
        const double sixtyMs = std::chrono::duration<double, std::milli>(sixty.period()).count();
        require(sixtyMs > 16.6 && sixtyMs < 16.7, "60fps period wrong");
        NativeFramePacer fast(120);
        const double fastMs = std::chrono::duration<double, std::milli>(fast.period()).count();
        require(fastMs > 8.3 && fastMs < 8.4, "120fps period wrong");
        uncapped.setFrameRate(120);
        require(uncapped.capped() && uncapped.period() == fast.period(), "live cap from uncapped failed");
        fast.setFrameRate(0);
        fast.wait();
        require(!fast.capped(), "live uncapping failed");
        fast.setFrameRate(60);
        require(fast.period() == sixty.period(), "restoring previous frame cap failed");
        std::puts("FramePacerContract passed: uncapped kept; deadline sequence keeps cadence on small miss and resyncs long stalls without burst. Wait-failure path not exercised.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FramePacerContract failed: %s\n", e.what());
        return 1;
    }
}
