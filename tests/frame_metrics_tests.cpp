#include "app/windows/frame_metrics.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
static void require(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
static void expectNear(double v, double e, const char* msg) { if (std::fabs(v - e) > 1e-9) throw std::runtime_error(msg); }
int main() {
    try {
        using Clock = FrameMetrics::Clock;
        const auto t0 = Clock::time_point{} + std::chrono::seconds(1000);
        auto ms = [](int n) { return std::chrono::milliseconds(n); };
        {
            FrameMetrics m;
            m.reset(t0);
            auto s = m.snapshot(t0 + std::chrono::seconds(6));
            require(!s.haveAcceptedFrame && s.frames == 0 && s.intervalSamples == 0, "startup state wrong");
            require(s.p95 == 0 && s.p99 == 0 && s.maximum == 0, "startup must report 0 with no samples");
            expectNear(s.lastAcceptedAgeMs, 6000.0, "startup age must be since start");
            double fps = 999;
            require(m.report(fps, t0 + std::chrono::seconds(6)), "startup window must report");
            require(fps == 0.0, "startup fps must be 0");
        }
        {
            FrameMetrics m;
            const auto origin = Clock::time_point{};
            m.reset(origin);
            m.frame(true, 1.0, origin);
            m.frame(true, 1.0, origin + ms(16));
            auto s = m.snapshot(origin + ms(6000));
            require(s.haveAcceptedFrame && s.intervalSamples == 1, "epoch-zero interval lost");
            expectNear(s.maximum, 16.0, "epoch-zero max wrong");
            expectNear(s.p95, 16.0, "epoch-zero p95 wrong");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            for (unsigned i = 0; i < 6; ++i) m.frame(true, 4.0, t0 + ms(int(i) * 10));
            for (unsigned i = 0; i < 6; ++i) m.present(S_OK, 0.2);
            auto s = m.snapshot(t0 + ms(6000));
            require(s.frames == 6 && s.intervalSamples == 5, "steady counts wrong");
            expectNear(s.fps, 1.0, "steady fps wrong");
            expectNear(s.p95, 10.0, "steady p95 wrong");
            expectNear(s.p99, 10.0, "steady p99 wrong");
            expectNear(s.maximum, 10.0, "steady max wrong");
            expectNear(s.lastAcceptedAgeMs, 5950.0, "steady age wrong");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            m.frame(true, 4.0, t0);
            m.frame(true, 4.0, t0 + ms(10));
            m.frame(true, 4.0, t0 + ms(40));
            m.frame(true, 4.0, t0 + ms(50));
            auto s = m.snapshot(t0 + ms(6000));
            require(s.intervalSamples == 3, "overrun sample count wrong");
            expectNear(s.p95, 10.0, "overrun p95 wrong");
            expectNear(s.maximum, 30.0, "overrun max wrong");
            expectNear(s.lastAcceptedAgeMs, 5950.0, "overrun age wrong");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            m.frame(true, 4.0, t0);
            m.frame(true, 4.0, t0 + ms(10));
            m.frame(true, 4.0, t0 + ms(20));
            double fps = 0;
            require(m.report(fps, t0 + ms(6000)), "window1 must report");
            expectNear(fps, 0.5, "window1 fps wrong");
            auto stalled = m.snapshot(t0 + ms(12000));
            require(stalled.frames == 0 && stalled.intervalSamples == 0, "stalled window must hold no new samples");
            require(stalled.p95 == 0 && stalled.p99 == 0 && stalled.maximum == 0, "stall must not synthesize samples");
            expectNear(stalled.lastAcceptedAgeMs, 11980.0, "stall age must grow");
            m.present(DXGI_STATUS_OCCLUDED, 0.1);
            m.present(DXGI_STATUS_OCCLUDED, 0.1);
            auto occluded = m.snapshot(t0 + ms(12000));
            require(occluded.presentOccluded == 2 && occluded.fps == 0.0, "occluded presents misclassified");
            m.frame(true, 4.0, t0 + ms(15000));
            auto recovered = m.snapshot(t0 + ms(16000));
            require(recovered.intervalSamples == 1, "recovery must add exactly one interval");
            expectNear(recovered.maximum, 14980.0, "recovery interval wrong");
            expectNear(recovered.p95, 14980.0, "recovery p95 wrong");
            expectNear(recovered.lastAcceptedAgeMs, 1000.0, "recovery age wrong");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            m.frame(true, 1.0, t0);
            m.frame(false, 1.0, t0 + ms(10));
            for (unsigned i = 0; i < 200; ++i) m.present(DXGI_STATUS_OCCLUDED, 0.1);
            auto s = m.snapshot(t0 + ms(6000));
            expectNear(s.fps, 2.0 / 6.0, "occluded engine fps inflated");
            expectNear(s.presentsFps, 200.0 / 6.0, "presents fps wrong");
            require(s.presentOccluded == 200 && s.presentOk == 0, "present counters wrong");
            require(s.presentsFps > s.fps, "repeated presents leaked into engine fps");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            for (unsigned i = 0; i < 3; ++i) m.present(S_OK, 0.1);
            for (unsigned i = 0; i < 2; ++i) m.present(DXGI_STATUS_OCCLUDED, 0.1);
            m.present(E_FAIL, 0.1);
            m.present(S_FALSE, 0.1);
            auto s = m.snapshot(t0 + ms(6000));
            require(s.presents == 7, "present total wrong");
            require(s.presentOk == 3 && s.presentOccluded == 2 && s.presentFailed == 1 && s.presentOther == 1, "present outcomes wrong");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            m.frame(true, 1.0, t0);
            m.rendered();
            m.rendered();
            auto s = m.snapshot(t0 + ms(6000));
            require(s.rendered == 2, "rendered count wrong");
            expectNear(s.renderedFps, 2.0 / 6.0, "rendered fps wrong");
            expectNear(s.fps, 1.0 / 6.0, "rendered frames leaked into engine fps");
        }
        {
            FrameMetrics m;
            m.reset(t0);
            double fps = 1234;
            require(!m.report(fps, t0 + std::chrono::seconds(1)), "short window must not report");
        }
        {
            FrameOutlierTrace t;
            t.reset(t0);
            FrameOutlierTrace::LoopStages s;
            s.pump = 1; s.take = 2; s.render = 3; s.post = 1; s.pacer = 8; s.present = 1;
            t.onLoopTop(t0);
            auto r = t.onPresent(t0 + ms(16), S_OK, s, true, true);
            require(!r.shouldLog && !r.isOutlier && t.emitted() == 0 && t.suppressed() == 0, "disabled trace must record nothing");
            t.setEnabled(true);
            t.reset(t0);
            t.onLoopTop(t0);
            auto rb = t.onPresent(t0, S_OK, s, true, false);
            require(!rb.shouldLog && !rb.isOutlier && rb.serial == 1, "first accepted must not be a hitch");
            require(t.emitted() == 0 && t.suppressed() == 0, "baseline must not emit");
        }
        {
            FrameOutlierTrace t;
            t.setEnabled(true);
            t.reset(t0);
            FrameOutlierTrace::LoopStages b;
            b.pump = 1; b.take = 1; b.render = 1; b.post = 1; b.pacer = 1; b.present = 1;
            t.onLoopTop(t0);
            t.onPresent(t0, S_OK, b, true, false);
            FrameOutlierTrace::LoopStages m1;
            m1.pump = 1; m1.take = 8; m1.render = 0; m1.post = 2; m1.pacer = 0; m1.present = 1;
            const auto top1 = t0 + ms(3);
            t.onLoopTop(top1);
            const auto end1 = top1 + ms(12);
            t.onPresent(end1, S_OK, m1, false, false);
            FrameOutlierTrace::LoopStages m2;
            m2.pump = 2; m2.take = 8; m2.render = 0; m2.post = 1; m2.pacer = 0; m2.present = 1;
            const auto top2 = end1 + ms(4);
            t.onLoopTop(top2);
            const auto end2 = top2 + ms(12);
            t.onPresent(end2, DXGI_STATUS_OCCLUDED, m2, false, false);
            FrameOutlierTrace::LoopStages a;
            a.pump = 1; a.take = 2; a.render = 5; a.post = 2; a.pacer = 1; a.present = 1;
            const auto top3 = end2 + ms(5);
            t.onLoopTop(top3);
            const auto end3 = top3 + ms(12);
            auto r = t.onPresent(end3, S_OK, a, true, true);
            require(r.isOutlier && r.shouldLog, "gap must log outlier");
            require(r.loops == 3 && r.miss == 2, "miss count wrong");
            require(r.serial == 2 && r.world, "serial/world wrong");
            require(r.presentOk == 2 && r.presentOccluded == 1 && r.presentFailed == 0 && r.presentOther == 0, "gap present outcomes wrong");
            expectNear(r.intervalMs, 48.0, "gap interval must use Present-return anchors");
            expectNear(r.runElapsedMs, 48.0, "run elapsed wrong");
            expectNear(r.sum[FrameOutlierTrace::Pump], 4.0, "pump sum wrong");
            expectNear(r.max[FrameOutlierTrace::Pump], 2.0, "pump max wrong");
            expectNear(r.sum[FrameOutlierTrace::Take], 18.0, "take sum wrong");
            expectNear(r.max[FrameOutlierTrace::Take], 8.0, "take max wrong");
            expectNear(r.sum[FrameOutlierTrace::Render], 5.0, "render sum wrong");
            expectNear(r.max[FrameOutlierTrace::Render], 5.0, "render max wrong");
            expectNear(r.sum[FrameOutlierTrace::Tail], 12.0, "tail sum wrong");
            expectNear(r.max[FrameOutlierTrace::Tail], 5.0, "tail max wrong");
            expectNear(r.sum[FrameOutlierTrace::Pump] + r.sum[FrameOutlierTrace::Take] + r.sum[FrameOutlierTrace::Render] + r.sum[FrameOutlierTrace::Post] + r.sum[FrameOutlierTrace::Pacer] + r.sum[FrameOutlierTrace::Present] + r.sum[FrameOutlierTrace::Tail] + r.residualMs, r.intervalMs, "residual must close the interval");
            require(t.emitted() == 1 && t.suppressed() == 0, "emit count wrong");
            FrameOutlierTrace::LoopStages n;
            n.pump = 1; n.take = 1; n.render = 2; n.post = 1; n.pacer = 8; n.present = 1;
            const auto top4 = end3 + ms(2);
            t.onLoopTop(top4);
            const auto end4 = top4 + ms(14);
            auto rn = t.onPresent(end4, S_OK, n, true, false);
            require(!rn.isOutlier && !rn.shouldLog, "normal interval must not log");
            require(rn.loops == 1 && rn.miss == 0, "reset after accepted wrong");
            expectNear(rn.sum[FrameOutlierTrace::Take], 1.0, "post-accept reset wrong");
        }
        {
            FrameOutlierTrace t;
            t.setEnabled(true);
            t.reset(t0);
            FrameOutlierTrace::LoopStages b;
            t.onLoopTop(t0);
            t.onPresent(t0, S_OK, b, true, false);
            FrameOutlierTrace::LoopStages big;
            big.pump = 5; big.take = 10; big.render = 10; big.post = 5; big.pacer = 10; big.present = 5;
            const auto top1 = t0 + ms(5);
            t.onLoopTop(top1);
            const auto end1 = top1 + ms(45);
            auto r1 = t.onPresent(end1, S_OK, big, true, true);
            require(r1.isOutlier && r1.shouldLog, "first outlier must log");
            const auto top2 = end1 + ms(5);
            t.onLoopTop(top2);
            const auto end2 = top2 + ms(45);
            auto r2 = t.onPresent(end2, S_OK, big, true, true);
            require(r2.isOutlier && !r2.shouldLog, "rate-limited outlier must suppress");
            require(t.emitted() == 1 && t.suppressed() == 1, "rate-limit bookkeeping wrong");
            require(r2.loops == 1 && r2.miss == 0, "suppressed accept must still reset");
            expectNear(r2.sum[FrameOutlierTrace::Take], 10.0, "suppressed reset wrong");
        }
        {
            FrameOutlierTrace t;
            t.setEnabled(true);
            t.reset(t0);
            FrameOutlierTrace::LoopStages b;
            t.onLoopTop(t0);
            t.onPresent(t0, S_OK, b, true, false);
            FrameOutlierTrace::LoopStages s;
            s.pump = 5; s.take = 5; s.render = 5; s.post = 5; s.pacer = 5; s.present = 5;
            auto base = t0;
            for (unsigned i = 0; i < 300; ++i) {
                const auto top = base + ms(1005);
                t.onLoopTop(top);
                base = top + ms(50);
                t.onPresent(base, S_OK, s, true, false);
            }
            require(t.emitted() == 256, "line cap must hold at 256");
            require(t.suppressed() == 44, "suppressed count wrong at cap");
        }
        {
            FrameOutlierTrace t;
            t.setEnabled(true);
            t.reset(t0);
            FrameOutlierTrace::LoopStages b;
            t.onLoopTop(t0);
            t.onPresent(t0, S_OK, b, true, false);
            FrameOutlierTrace::LoopStages s;
            t.onLoopTop(t0 + ms(1));
            const auto exactEnd = t0 + ms(34);
            auto exact = t.onPresent(exactEnd, S_OK, s, true, false);
            require(!exact.isOutlier && !exact.shouldLog, "34ms boundary must not be an outlier");
            t.onLoopTop(exactEnd + ms(1));
            auto over = t.onPresent(exactEnd + ms(1) + std::chrono::microseconds(33001), S_OK, s, true, false);
            require(over.isOutlier && over.shouldLog, "just-over-threshold must log");
        }
        {
            FrameOutlierTrace t;t.setEnabled(true);t.reset(t0);
            FrameOutlierTrace::LoopStages empty;
            t.onPresent(t0,S_OK,empty,true,false);
            FrameOutlierTrace::LoopStages part;part.pump=1;part.take=2;part.render=7;part.present=999;
            t.onLoopTop(t0+ms(3));
            t.onLoopTop(t0+ms(3)); // Tail must be charged once, even on a repeated observation.
            t.onUnpresentedLoop(t0+ms(13),part);
            t.onLoopTop(t0+ms(17));
            part.take=8;part.render=0;part.post=1;
            t.onUnpresentedLoop(t0+ms(27),part);
            t.onLoopTop(t0+ms(32));
            FrameOutlierTrace::LoopStages last;last.pump=1;last.take=2;last.render=5;
            last.post=2;last.pacer=3;last.present=1;
            const auto r=t.onPresent(t0+ms(46),S_OK,last,true,true);
            require(r.serial==2 && r.loops==3 && r.miss==2 && r.world && r.shouldLog,
                    "Unpresented parts changed accepted frames or missed-loop accounting");
            require(r.presentOk==1 && !r.presentOccluded && !r.presentFailed && !r.presentOther,
                    "Unpresented loops fabricated presentation outcomes");
            expectNear(r.intervalMs,46,"Streamed interval did not use complete-frame Present returns");
            expectNear(r.sum[FrameOutlierTrace::Render],12,"Streamed render durations were lost or counted twice");
            expectNear(r.sum[FrameOutlierTrace::Take],12,"Streamed waiting time was lost");
            expectNear(r.sum[FrameOutlierTrace::Tail],12,"Unpresented loops double-counted the prior tail");
            expectNear(r.max[FrameOutlierTrace::Tail],5,"Streamed tail maximum was wrong");
            expectNear(r.sum[FrameOutlierTrace::Present],1,"A non-presentation was charged as Present time");
            expectNear(r.residualMs,0,"Streamed stages did not close the measured interval");
            t.onLoopTop(t0+ms(48));
            const auto next=t.onPresent(t0+ms(62),S_OK,last,true,false);
            require(next.loops==1 && next.presentOk==1 && !next.shouldLog,
                    "A completed streamed frame leaked state into the next frame");
            expectNear(next.sum[FrameOutlierTrace::Render],5,"Previous parts leaked into next-frame render duration");
            t.setEnabled(false);t.onUnpresentedLoop(t0+ms(70),part);
            t.setEnabled(true);t.reset(t0+ms(80));
            const auto reset=t.onPresent(t0+ms(80),S_OK,empty,true,false);
            require(reset.serial==1 && !reset.shouldLog,"Disabled/unpresented state survived reset");
        }
        std::puts("FrameMetricsContract passed: completed-only percentiles, growing age, epoch-zero, stall/recovery, present outcomes and rendered separation verified without wall clock.");
        std::puts("FrameStreamingMetrics: non-present loops, complete-frame timing, zero fabricated outcomes, single-charge tail and reset passed.");
        std::puts("FrameOutlierContract passed: disabled gate, baseline, miss accumulation, reset, rate-limit, cap and threshold verified without wall clock.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FrameMetricsContract failed: %s\n", e.what());
        return 1;
    }
}
