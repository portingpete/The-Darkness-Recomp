#pragma once
#include <cstdint>

namespace DarkRecomp::Native {
// Issue #162. 825A4278 runs once per frame on the title's application system
// thread. While bit 0 of its object's [[+8]+4] word stays set, the original
// loop reads the clock, checks its own timeout, releases the object lock,
// pumps 822360B8 (two more guest locks) and reacquires the lock, with no
// delay. Its sibling loop above (lr 825A42E4) sleeps 10 ms per pass.
//
// On the console that thread owned a hardware thread. Here it is unpinned
// (thread_topology.h) and spends most of each frame in this loop: about 1.8
// million passes per second, three contended guest locks per pass, and a
// host core kept busy at any frame cap.
//
// After each pump returns at 825A4388 the thread holds none of those locks.
// It now yields there (SwitchToThread), so engine, display and worker threads
// that are ready on the same processor run first; with nothing ready the
// call returns at once and the loop continues as before. The guest predicate,
// timeout, lock order and pump calls are unchanged. Sleeping between passes
// was measured and rejected: it added 100-780 ms stalls at 60 FPS on four
// cores. DARK_APP_POLL=0 restores the original loop.
enum class ApplicationPollMode : int { original = 0, yield = 1 };
inline constexpr uint32_t applicationPollPumpReturn = 0x825A4388;
constexpr ApplicationPollMode applicationPollModeFrom(const char* value) {
    return value && value[0] == '0' && !value[1] ? ApplicationPollMode::original : ApplicationPollMode::yield;
}
// Only the unpaced pump call inside 825A4278 yields; the first pump at
// 825A42A0 and every other caller of 822360B8 run unchanged.
constexpr bool applicationPollPass(bool insideWait, uint32_t returnAddress) {
    return insideWait && returnAddress == applicationPollPumpReturn;
}
}
