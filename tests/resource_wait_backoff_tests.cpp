#include "renderer/engine/resource_wait_backoff.h"
#undef NDEBUG
#include <cassert>
#include "runtime/native/native_timed_wait.h"
#include <thread>
#include <array>
#include <atomic>
int main() {
    assert(resourceDependencyYieldEligible(0x8222CD30,1,0,0x7FF00000,1,2));
    assert(!resourceDependencyYieldEligible(0x8222CD31,1,0,0x7FF00000,1,2));
    assert(!resourceDependencyYieldEligible(0x8222CD30,0,0,0x7FF00000,1,2));
    assert(!resourceDependencyYieldEligible(0x8222CD30,2,0,0x7FF00000,1,2));
    assert(!resourceDependencyYieldEligible(0x8222CD30,1,1,0x7FF00000,1,2));
    assert(resourceDependencyYieldEligible(0x8222CD30,1,0,0x7FF10000,1,2));
    assert(!resourceDependencyYieldEligible(0x8222CD30,1,0,0,1,2));
    assert(!resourceDependencyYieldEligible(0x8222CD30,1,0,0x7FF00000,0,2));
    assert(!resourceDependencyYieldEligible(0x8222CD30,1,0,0x7FF00000,1,0));
    DarkResourceWaitToken token{};
    HANDLE event=reinterpret_cast<HANDLE>(dark_resource_wait_begin(&token,dark_resource_wait_register()));
    assert(event);
    dark_resource_wait_notify();
    assert(dark_resource_wait_arm(&token)==0); // completion before arm
    dark_resource_wait_finish(&token);
    const int64_t timeout=-10000;
    assert(dark_resource_wait_arm(&token)==1);
    assert(DarkRecomp::Native::nativeTimedWait(event,FALSE,&timeout)==WAIT_TIMEOUT);
    dark_resource_wait_finish(&token);
    for(unsigned i=0;i<1000;++i) {
        assert(dark_resource_wait_arm(&token)==1);
        std::thread worker([]{dark_resource_wait_notify();});
        worker.join();
        assert(DarkRecomp::Native::nativeTimedWait(event,FALSE,&timeout)==WAIT_OBJECT_0);
        dark_resource_wait_finish(&token);
    }
    assert(dark_resource_wait_arm(&token)==1);
    std::thread delayedWorker([]{Sleep(2);dark_resource_wait_notify();});
    const int64_t generousWakeDeadline=-10000000;
    assert(DarkRecomp::Native::nativeTimedWait(event,FALSE,&generousWakeDeadline)==WAIT_OBJECT_0);
    delayedWorker.join();
    dark_resource_wait_finish(&token);
    // Five independent blocked waiters must all wake; one auto-reset event
    // shared between them would fail this check.
    std::atomic<unsigned> ready{0}, completed{0};
    std::array<std::thread,5> waiters;
    for (auto& waiter:waiters) waiter=std::thread([&]{
        DarkResourceWaitToken t{};
        auto handle=reinterpret_cast<HANDLE>(dark_resource_wait_begin(&t,dark_resource_wait_register()));
        assert(handle && dark_resource_wait_arm(&t)==1);
        ready.fetch_add(1);
        assert(DarkRecomp::Native::nativeTimedWait(handle,FALSE,&generousWakeDeadline)==WAIT_OBJECT_0);
        dark_resource_wait_finish(&t);
        completed.fetch_add(1);
    });
    while(ready.load()!=waiters.size()) std::this_thread::yield();
    Sleep(5);
    dark_resource_wait_notify();
    for(auto& waiter:waiters) waiter.join();
    assert(completed.load()==waiters.size());
    // Exhaustion is bounded and falls back; no slot wrap or handle reuse.
    while(dark_resource_wait_register()!=UINT32_MAX) {}
    assert(dark_resource_wait_register()==UINT32_MAX);
    DarkResourceWaitToken invalid{};
    assert(dark_resource_wait_begin(&invalid,UINT32_MAX)==0);
}
