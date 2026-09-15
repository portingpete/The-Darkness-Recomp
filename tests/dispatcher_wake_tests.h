#pragma once

// A satisfied wait survives a later reset even if its thread has not run yet.
// Compare guest imports with native Windows waits using the same signal sequence.
static void testDispatcherWakeCommit() {
    auto* base=memory->base();
    const uint32_t scratch=memory->allocate(4096);
    check(scratch!=0,"Dispatcher wake fixture allocation failed");
    const uint32_t event=scratch,other=scratch+32,array=scratch+64,timeout=scratch+80,zero=scratch+88;
    const uint64_t ticks=_byteswap_uint64(uint64_t(-10000000ll)),zeroTicks=0;
    std::memcpy(base+timeout,&ticks,8);std::memcpy(base+zero,&zeroTicks,8);
    auto initializeEvent=[&](uint32_t at,bool manual) {
        std::memset(base+at,0,16);base[at]=manual?0:1;
        memory->write32(at+8,at+8);memory->write32(at+12,at+8);
    };
    enum Mode {manualReset,autoTwice,waitAny,waitAll,lateWaiter,semaphoreLate};
    const char* names[]{"manual-reset","auto-twice","wait-any","wait-all-semaphore","late-waiter","semaphore-late-waiter"};
    unsigned failures=0;
    for(unsigned mode=0;mode<6;++mode) {
        unsigned nativeCompleted=0,guestCompleted=0,lateStolen=0;
        for(unsigned round=0;round<4;++round) {
            const unsigned count=mode==manualReset?3:mode==autoTwice?2:1;
            initializeEvent(event,mode==manualReset);
            initializeEvent(other,false);
            if(mode==semaphoreLate) {
                PPCContext init{};init.r3.u64=event;init.r5.u64=1;
                __imp__KeInitializeSemaphore(init,base);
            }
            if(mode==waitAll) {
                PPCContext init{};init.r3.u64=other;init.r4.u64=init.r5.u64=1;
                __imp__KeInitializeSemaphore(init,base);
            }
            memory->write32(array,other);memory->write32(array+4,event);
            HANDLE nativeEvent=mode==semaphoreLate?CreateSemaphoreW(nullptr,0,1,nullptr):
                CreateEventW(nullptr,mode==manualReset,FALSE,nullptr);
            HANDLE nativeOther=mode==waitAll?CreateSemaphoreW(nullptr,1,1,nullptr):CreateEventW(nullptr,FALSE,FALSE,nullptr);
            check(nativeEvent && nativeOther,"Native wake reference creation failed");
            const HANDLE handles[]{nativeOther,nativeEvent};
            std::atomic<unsigned> entered=0;
            std::vector<std::future<uint32_t>> native,guest;
            for(unsigned i=0;i<count;++i) {
                native.push_back(std::async(std::launch::async,[&] {
                    ++entered;
                    return uint32_t(mode==waitAny || mode==waitAll?
                        WaitForMultipleObjects(2,handles,mode==waitAll,1000):WaitForSingleObject(nativeEvent,1000));
                }));
                guest.push_back(std::async(std::launch::async,[&] {
                    PPCContext wait{};
                    ++entered;
                    if(mode==waitAny || mode==waitAll) {
                        wait.r3.u64=2;wait.r4.u64=array;wait.r5.u64=mode==waitAll?0:1;wait.r9.u64=timeout;
                        __imp__KeWaitForMultipleObjects(wait,base);
                    } else {
                        wait.r3.u64=event;wait.r7.u64=timeout;__imp__KeWaitForSingleObject(wait,base);
                    }
                    return wait.r3.u32;
                }));
            }
            while(entered!=2*count)std::this_thread::yield();
            // Establish guest registration under the dispatcher's lock. A
            // worker descheduled before its wait must not be mistaken for a
            // registered waiter merely because its future is incomplete.
            const auto registrationLimit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(dispatcherWaiterCount(base,event)!=count) {
                check(std::chrono::steady_clock::now()<registrationLimit,"Guest waits did not register");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Give the native reference time to enter its kernel wait. The
            // native completion totals below validate that reference round.
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            for(auto& wait:native)check(wait.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout,
                                       "Native wake reference did not block");
            for(auto& wait:guest)check(wait.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout,
                                      "Guest wake fixture did not block");
            if(mode==semaphoreLate)check(ReleaseSemaphore(nativeEvent,1,nullptr)!=0,"Cannot release native wake semaphore");
            else check(SetEvent(nativeEvent)!=0,"Cannot signal native wake reference");
            if(mode==autoTwice)check(SetEvent(nativeEvent)!=0,"Cannot signal second native waiter");
            else if(mode!=lateWaiter && mode!=semaphoreLate && mode!=waitAll)check(ResetEvent(nativeEvent)!=0,"Cannot reset native wake reference");
            uint32_t nativeLate=WAIT_TIMEOUT;
            if(mode==lateWaiter || mode==semaphoreLate)nativeLate=WaitForSingleObject(nativeEvent,0);
            PPCContext signal{};signal.r3.u64=event;
            if(mode==semaphoreLate) {signal.r5.u64=1;__imp__KeReleaseSemaphore(signal,base);}
            else __imp__KeSetEvent(signal,base);
            if(mode==autoTwice) {signal.r3.u64=event;__imp__KeSetEvent(signal,base);}
            else if(mode!=lateWaiter && mode!=semaphoreLate && mode!=waitAll) {signal.r3.u64=event;__imp__KeResetEvent(signal,base);}
            if(mode==lateWaiter || mode==semaphoreLate) {
                signal={};signal.r3.u64=event;signal.r7.u64=zero;__imp__KeWaitForSingleObject(signal,base);
                if(signal.r3.u32!=0x102)++lateStolen;
            }
            const uint32_t expected=mode==waitAny?1:0;
            for(auto& wait:native)if(wait.get()==expected)++nativeCompleted;
            for(auto& wait:guest)if(wait.get()==expected)++guestCompleted;
            check(dispatcherWaiterCount(base,event)==0,"Completed dispatcher waits retained their registrations");
            CloseHandle(nativeOther);CloseHandle(nativeEvent);
            check(nativeLate==WAIT_TIMEOUT,"Native late waiter consumed an already assigned wake");
            if(mode==waitAll && memory->read32(other+4)!=0)++failures;
        }
        const unsigned expected=(mode==manualReset?3:mode==autoTwice?2:1)*4;
        std::printf("DispatcherWake[%s] native=%u guest=%u expected=%u late-stolen=%u\n",
                    names[mode],nativeCompleted,guestCompleted,expected,lateStolen);
        check(nativeCompleted==expected,"Native wake reference did not satisfy all registered waits");
        if(guestCompleted!=expected || lateStolen)++failures;
    }
    check(memory->release(scratch),"Dispatcher wake fixture cleanup failed");
    check(failures==0,"Dispatcher lost or reassigned waits after their objects were signaled");
}
