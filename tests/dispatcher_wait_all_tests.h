#pragma once

struct DispatcherAllResumeGate {
    DispatcherResumeGate stages[2];
    std::atomic<unsigned> next=0;
    static void pause(void* context) noexcept {
        auto& gate=*static_cast<DispatcherAllResumeGate*>(context);
        const auto index=gate.next.fetch_add(1);
        if(index<2)DispatcherResumeGate::pause(&gate.stages[index]);
    }
};
struct DispatcherAllResumeScope {
    explicit DispatcherAllResumeScope(DispatcherAllResumeGate& gate) {
        setDispatcherWaitResumeHook(DispatcherAllResumeGate::pause,&gate);
    }
    ~DispatcherAllResumeScope() {setDispatcherWaitResumeHook(nullptr,nullptr);}
};
struct DispatcherAllResumeRelease {
    DispatcherAllResumeGate& gate;
    ~DispatcherAllResumeRelease() {for(auto& stage:gate.stages)SetEvent(stage.resume);}
};

static void testDispatcherWaitAllSharedWake() {
    auto* base=memory->base();
    const uint32_t scratch=memory->allocate(256),event=scratch,timeout=scratch+128;
    const uint32_t semaphores[]{scratch+32,scratch+64},arrays[]{scratch+96,scratch+104};
    check(scratch!=0,"Shared wait-all fixture allocation failed");
    std::memset(base+scratch,0,160);base[event]=1;
    memory->write32(event+8,event+8);memory->write32(event+12,event+8);
    const auto encoded=_byteswap_uint64(uint64_t(-10000000ll));std::memcpy(base+timeout,&encoded,8);
    for(unsigned i=0;i<2;++i) {
        PPCContext init{};init.r3.u64=semaphores[i];init.r4.u64=init.r5.u64=1;
        __imp__KeInitializeSemaphore(init,base);
        memory->write32(arrays[i],semaphores[i]);memory->write32(arrays[i]+4,event);
    }
    DispatcherAllResumeGate gates[2];
    for(const auto& gate:gates)for(const auto& stage:gate.stages)
        check(stage.reached && stage.resume,"Cannot create shared wait-all gates");
    std::vector<std::future<uint32_t>> workers;
    DispatcherAllResumeRelease releaseFirst{gates[0]},releaseSecond{gates[1]};
    for(unsigned i=0;i<2;++i)workers.push_back(std::async(std::launch::async,[&,i] {
        DispatcherAllResumeScope scope(gates[i]);
        PPCContext wait{};wait.r3.u64=2;wait.r4.u64=arrays[i];wait.r5.u64=0;wait.r9.u64=timeout;
        __imp__KeWaitForMultipleObjects(wait,base);return wait.r3.u32;
    }));
    const auto registrationLimit=GetTickCount64()+2000;
    while(dispatcherWaiterCount(base,event)!=2) {
        check(GetTickCount64()<registrationLimit,"Shared wait-all fixtures did not register");Sleep(1);
    }
    const auto afterDeadline=GetTickCount64()+1100;
    auto signal=[&] {PPCContext call{};call.r3.u64=event;__imp__KeSetEvent(call,base);};
    signal();
    for(auto& gate:gates)check(WaitForSingleObject(gate.stages[0].reached,2000)==WAIT_OBJECT_0,
                              "One shared wait-all did not wake for its recheck");
    check(memory->read32(event+4)==1 && memory->read32(semaphores[0]+4)==1 && memory->read32(semaphores[1]+4)==1,
          "Shared wait-all reserved objects before resumption");
    const auto now=GetTickCount64();if(now<afterDeadline)Sleep(DWORD(afterDeadline-now));
    SetEvent(gates[0].stages[0].resume);
    const auto first=workers[0].get();
    const bool firstValid=first==0 && memory->read32(event+4)==0 && memory->read32(semaphores[0]+4)==0 &&
                          memory->read32(semaphores[1]+4)==1;
    // Both native waiters become Ready from the original auto-event signal.
    // The second retains its recheck while the first consumes and we restore.
    signal();SetEvent(gates[1].stages[0].resume);
    const auto second=workers[1].get();
    const bool valid=firstValid && second==0 && memory->read32(event+4)==0 && memory->read32(semaphores[1]+4)==0;
    for(const auto& gate:gates)for(const auto& stage:gate.stages)check(stage.valid,"Shared wait-all gate timed out");
    check(dispatcherWaiterCount(base,event)==0,"Shared wait-all retained a completed registration");
    check(memory->release(scratch),"Shared wait-all fixture cleanup failed");
    std::printf("DispatcherWaitAll[shared-auto-wake] first=%08X second=%08X %s\n",first,second,valid?"passed":"FAILED");
    check(valid,"An auto-event signal did not preserve both pending wait-all rechecks");
}

static void testDispatcherWaitAllWake() {
    auto* base=memory->base();
    const uint32_t scratch=memory->allocate(4096);
    check(scratch!=0,"Wait-all wake fixture allocation failed");
    const uint32_t event=scratch,semaphore=scratch+32,array=scratch+64,timeout=scratch+80,zero=scratch+88,other=scratch+128;
    enum Mode {relativeReady,absoluteReady,reset,compete,partial,unrelated,rearm,restoreLate,oneReset,onePoll,alreadyEvent,positiveSemaphore};
    const char* names[]{"relative-ready","absolute-ready","reset-before-resume","competing-waiter",
                        "partial-before-deadline","unrelated-late-signal","rearm-after-reset",
                        "restore-before-resume","one-object-reset","one-object-late-poll",
                        "already-signaled-event","already-positive-semaphore"};
    unsigned failures=0;
    for(unsigned mode=0;mode<12;++mode) {
        const bool single=mode==oneReset || mode==onePoll;
        std::memset(base+scratch,0,160);base[event]=base[other]=1;
        for(auto address:{event,other}) {
            memory->write32(address+8,address+8);memory->write32(address+12,address+8);
        }
        if(mode==alreadyEvent)memory->write32(event+4,1);
        PPCContext init{};init.r3.u64=semaphore;init.r4.u64=mode==partial || mode==alreadyEvent?0:1;
        init.r5.u64=mode==positiveSemaphore?2:1;
        __imp__KeInitializeSemaphore(init,base);
        memory->write32(array,single?event:semaphore);memory->write32(array+4,event);
        int64_t ticks=-10000000;
        if(mode==absoluteReady) {
            FILETIME now;GetSystemTimeAsFileTime(&now);
            ticks=int64_t((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime)+10000000;
        }
        const auto encoded=_byteswap_uint64(uint64_t(ticks));
        std::memcpy(base+timeout,&encoded,8);
        DispatcherAllResumeGate gate;
        for(const auto& stage:gate.stages)check(stage.reached && stage.resume,"Cannot create wait-all resume gates");
        auto waiter=std::async(std::launch::async,[&] {
            DispatcherAllResumeScope scope(gate);
            PPCContext wait{};wait.r3.u64=single?1:2;wait.r4.u64=array;wait.r5.u64=0;wait.r9.u64=timeout;
            __imp__KeWaitForMultipleObjects(wait,base);
            return wait.r3.u32;
        });
        DispatcherAllResumeRelease release{gate};
        const auto registrationLimit=GetTickCount64()+2000;
        while(dispatcherWaiterCount(base,event)!=1) {
            check(GetTickCount64()<registrationLimit,"Wait-all fixture did not register");Sleep(1);
        }
        const auto afterDeadline=GetTickCount64()+1100;
        auto signal=[&](uint32_t address) {
            PPCContext call{};call.r3.u64=address;__imp__KeSetEvent(call,base);
            check(call.r3.u32<=1,"Wait-all fixture event signal failed");
        };
        if(mode==positiveSemaphore) {
            PPCContext call{};call.r3.u64=semaphore;call.r5.u64=1;__imp__KeReleaseSemaphore(call,base);
        } else signal(event);
        check(WaitForSingleObject(gate.stages[0].reached,2000)==WAIT_OBJECT_0,
              "Wait-all did not reach its first resume gate");
        uint32_t action=UINT32_MAX;
        if(mode==reset || mode==rearm || mode==restoreLate || mode==oneReset) {
            PPCContext call{};call.r3.u64=event;__imp__KeResetEvent(call,base);
            action=call.r3.u32;
            if(!single)check(action==1,"Wait-all reserved the event before its thread resumed");
        }
        if(mode==compete) {
            PPCContext call{};call.r3.u64=semaphore;call.r7.u64=zero;
            __imp__KeWaitForSingleObject(call,base);
            check(call.r3.u32==0,"Wait-all reserved a semaphore before its thread resumed");
        }
        if(mode==rearm) {
            SetEvent(gate.stages[0].resume);
            check(WaitForSingleObject(gate.stages[1].reached,2000)==WAIT_OBJECT_0,
                  "Incomplete wait-all did not resume its timed wait");
        }
        const auto now=GetTickCount64();
        if(now<afterDeadline)Sleep(DWORD(afterDeadline-now));
        if(mode==partial || mode==alreadyEvent) {
            PPCContext call{};call.r3.u64=semaphore;call.r5.u64=1;__imp__KeReleaseSemaphore(call,base);
        }
        if(mode==unrelated)signal(other);
        if(mode==rearm || mode==restoreLate || mode==positiveSemaphore)signal(event);
        if(mode==onePoll) {
            PPCContext call{};call.r3.u64=event;call.r7.u64=zero;
            __imp__KeWaitForSingleObject(call,base);action=call.r3.u32;
        }
        for(auto& stage:gate.stages)SetEvent(stage.resume);
        const uint32_t result=waiter.get();
        const bool success=mode==relativeReady || mode==absoluteReady || mode==unrelated ||
                           mode==partial || mode==restoreLate || single;
        const uint32_t expected=success?0:0x102;
        const uint32_t expectedSemaphore=mode==positiveSemaphore?2:single?1:success || mode==compete?0:1;
        const uint32_t expectedEvent=success || mode==reset?0:1;
        const bool valid=result==expected && memory->read32(semaphore+4)==expectedSemaphore &&
                         memory->read32(event+4)==expectedEvent &&
                         (mode!=oneReset || action==0) && (mode!=onePoll || action==0x102);
        std::printf("DispatcherWaitAll[%s] result=%08X expected=%08X semaphore=%u event=%u action=%08X %s\n",
                    names[mode],result,expected,memory->read32(semaphore+4),memory->read32(event+4),action,valid?"passed":"FAILED");
        if(!valid)++failures;
        for(const auto& stage:gate.stages)check(stage.valid,"Wait-all resume gate timed out");
        check(dispatcherWaiterCount(base,event)==0,"Wait-all wake retained a completed registration");
    }
    check(memory->release(scratch),"Wait-all wake fixture cleanup failed");
    check(failures==0,"Wait-all lost a timely wake or reserved objects before resumption");
    testDispatcherWaitAllSharedWake();
}
