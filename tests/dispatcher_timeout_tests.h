#pragma once

// Pause a resumed waiter outside the dispatcher lock. This keeps its record
// enrolled while the issuer supplies a late signal, independent of scheduling.
struct DispatcherResumeGate {
    HANDLE reached=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> valid=true;
    static void pause(void* context) noexcept {
        auto& gate=*static_cast<DispatcherResumeGate*>(context);
        if (!SetEvent(gate.reached) || WaitForSingleObject(gate.resume,5000)!=WAIT_OBJECT_0) gate.valid=false;
    }
    ~DispatcherResumeGate() {if(reached)CloseHandle(reached);if(resume)CloseHandle(resume);}
};
struct DispatcherResumeHookScope {
    explicit DispatcherResumeHookScope(DispatcherResumeGate* gate) {
        setDispatcherWaitResumeHook(gate?DispatcherResumeGate::pause:nullptr,gate);
    }
    ~DispatcherResumeHookScope() {setDispatcherWaitResumeHook(nullptr,nullptr);}
};
struct DispatcherResumeOnExit {
    HANDLE event;
    ~DispatcherResumeOnExit() {SetEvent(event);}
};

static void testDispatcherTimeoutOwnership() {
    auto* base=memory->base();
    const uint32_t scratch=memory->allocate(4096);
    check(scratch!=0,"Dispatcher timeout fixture allocation failed");
    const uint32_t event=scratch,other=scratch+32,array=scratch+64,timeout=scratch+80,longTimeout=scratch+88;
    unsigned failures=0;
    enum Mode {relative,absolute,any,all,semaphore,liveSuccessor,earlySignal};
    const char* names[]{"relative","absolute","wait-any","wait-all","semaphore","live-successor","early-signal"};
    for(unsigned mode=0;mode<7;++mode) {
        std::memset(base+scratch,0,128);base[event]=1;
        memory->write32(event+8,event+8);memory->write32(event+12,event+8);
        memory->write32(array,other);memory->write32(array+4,event);
        PPCContext init{};init.r3.u64=other;init.r4.u64=mode==all?1:0;init.r5.u64=1;
        __imp__KeInitializeSemaphore(init,base);
        if(mode==semaphore) {init={};init.r3.u64=event;init.r5.u64=1;__imp__KeInitializeSemaphore(init,base);}
        int64_t ticks=-10000000; // One second gives workers time to enroll under load.
        if(mode==absolute) {
            FILETIME now;GetSystemTimeAsFileTime(&now);
            ticks=int64_t((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime)+10000000;
        }
        const uint64_t encoded=_byteswap_uint64(uint64_t(ticks)),longEncoded=_byteswap_uint64(uint64_t(-100000000ll));
        std::memcpy(base+timeout,&encoded,8);std::memcpy(base+longTimeout,&longEncoded,8);
        HANDLE nativeEvent=mode==semaphore?CreateSemaphoreW(nullptr,0,1,nullptr):CreateEventW(nullptr,FALSE,FALSE,nullptr);
        HANDLE nativeOther=CreateSemaphoreW(nullptr,mode==all?1:0,1,nullptr);
        check(nativeEvent && nativeOther,"Cannot create native timeout reference");
        const HANDLE handles[]{nativeOther,nativeEvent};
        const unsigned count=mode==liveSuccessor?2:1;
        std::atomic<unsigned> entered=0;
        DispatcherResumeGate gate;
        check(gate.reached && gate.resume,"Cannot create dispatcher resume barrier");
        std::vector<std::future<uint32_t>> native,guest;
        // Release before future destruction joins workers, including unwinding.
        DispatcherResumeOnExit release{gate.resume};
        for(unsigned i=0;i<count;++i) {
            native.push_back(std::async(std::launch::async,[&,i] {
                ++entered;
                return uint32_t(mode==any || mode==all?WaitForMultipleObjects(2,handles,mode==all,1000):
                    WaitForSingleObject(nativeEvent,i?10000:1000));
            }));
            guest.push_back(std::async(std::launch::async,[&,i] {
                DispatcherResumeHookScope hook(i?nullptr:&gate);
                PPCContext wait{};++entered;
                if(mode==any || mode==all) {
                    wait.r3.u64=2;wait.r4.u64=array;wait.r5.u64=mode==all?0:1;wait.r9.u64=timeout;
                    __imp__KeWaitForMultipleObjects(wait,base);
                } else {
                    wait.r3.u64=event;wait.r7.u64=i?longTimeout:timeout;__imp__KeWaitForSingleObject(wait,base);
                }
                return wait.r3.u32;
            }));
            if(mode==liveSuccessor && i==0) {
                const auto firstLimit=GetTickCount64()+2000;
                while(dispatcherWaiterCount(base,event)!=1) {
                    check(GetTickCount64()<firstLimit,"Expiring waiter did not register first");Sleep(1);
                }
            }
        }
        const auto readyLimit=GetTickCount64()+2000;
        while(entered!=2*count || dispatcherWaiterCount(base,event)!=count) {
            check(GetTickCount64()<readyLimit,"Timeout fixture waiters did not register");Sleep(1);
        }
        auto signal=[&] {
            if(mode==semaphore)check(ReleaseSemaphore(nativeEvent,1,nullptr)!=0,"Cannot release timeout reference");
            else check(SetEvent(nativeEvent)!=0,"Cannot signal timeout reference");
            PPCContext call{};call.r3.u64=event;
            if(mode==semaphore) {call.r5.u64=1;__imp__KeReleaseSemaphore(call,base);}
            else __imp__KeSetEvent(call,base);
        };
        // Bound expiry from confirmed enrollment, not worker launch timing.
        const auto signalAfter=GetTickCount64()+1100;
        if(mode==earlySignal)signal();
        const bool paused=WaitForSingleObject(gate.reached,2000)==WAIT_OBJECT_0;
        const auto now=GetTickCount64();
        if(now<signalAfter)Sleep(DWORD(signalAfter-now));
        // Observe the native timeout before issuing a late signal, even if
        // the native worker entered its kernel wait later than the guest.
        const uint32_t nativeResult=native[0].get();
        if(mode!=earlySignal)signal();
        SetEvent(gate.resume);
        const uint32_t guestResult=guest[0].get();
        uint32_t nativeSuccessor=0,guestSuccessor=0;
        if(count==2) {nativeSuccessor=native[1].get();guestSuccessor=guest[1].get();}
        const uint32_t expected=mode==earlySignal?0:0x102;
        const uint32_t remaining=WaitForSingleObject(nativeEvent,0),guestRemaining=memory->read32(event+4);
        const bool shouldRemain=mode!=liveSuccessor && mode!=earlySignal;
        std::printf("DispatcherTimeout[%s] native=%08X guest=%08X successor=%08X remaining=%u expected=%u\n",
                    names[mode],nativeResult,guestResult,guestSuccessor,guestRemaining,unsigned(shouldRemain));
        CloseHandle(nativeOther);CloseHandle(nativeEvent);
        check(nativeResult==expected && nativeSuccessor==0 && remaining==(shouldRemain?0u:0x102u),
              "Native timeout reference disagreed with the scheduling preconditions");
        check(paused && gate.valid,"Dispatcher waiter did not remain at its resume barrier");
        if(guestResult!=expected || guestSuccessor!=0 || guestRemaining!=unsigned(shouldRemain) ||
            (mode==all && memory->read32(other+4)!=1))++failures;
        check(dispatcherWaiterCount(base,event)==0,"Timed-out dispatcher wait retained its registration");
    }
    check(memory->release(scratch),"Dispatcher timeout fixture cleanup failed");
    check(failures==0,"Expired dispatcher wait consumed a later signal or displaced a live waiter");
}
