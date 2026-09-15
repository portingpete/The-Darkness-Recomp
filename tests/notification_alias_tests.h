#pragma once

static void testNotificationAliases(PPCContext& ctx) {
    auto* base=memory->base();
    const uint32_t scratch=memory->allocate(128);
    check(scratch!=0,"Notification alias fixture allocation failed");
    memory->write32(scratch+16,2);memory->write32(scratch+20,0);
    const uint64_t zero=0;std::memcpy(base+scratch+32,&zero,sizeof(zero));
    auto create=[&] {
        ctx.r3.u64=0x20;ctx.r4.u64=2;
        __imp__XamNotifyCreateListener(ctx,base);
        check(ctx.r3.u32!=0,"Notification alias listener creation failed");
        return ctx.r3.u32;
    };
    auto duplicate=[&](uint32_t source,bool closeSource=false) {
        ctx.r3.u64=source;ctx.r4.u64=scratch;ctx.r5.u64=closeSource?1:0;
        __imp__NtDuplicateObject(ctx,base);
        check(ctx.r3.u32==0,"Cannot duplicate notification listener");
        const auto handle=memory->read32(scratch);
        check(handle && handle!=source,"Duplicate did not return a distinct handle");
        return handle;
    };
    auto close=[&](uint32_t handle) {
        ctx.r3.u64=handle;__imp__NtClose(ctx,base);
        check(ctx.r3.u32==0,"Cannot close notification alias");
    };
    auto wait=[&](uint32_t handle) {
        ctx.r3.u64=handle;ctx.r4.u64=ctx.r5.u64=0;ctx.r6.u64=scratch+32;
        __imp__NtWaitForSingleObjectEx(ctx,base);return ctx.r3.u32;
    };
    auto emit=[&](uint32_t playback) {
        memory->write32(scratch+24,playback);
        ctx.r3.u64=0xFA;ctx.r4.u64=0x7001A;ctx.r5.u64=0;
        ctx.r6.u64=scratch+16;ctx.r7.u64=12;
        __imp__XMsgStartIORequestEx(ctx,base);
        check(ctx.r3.u32==0,"Notification alias playback request failed");
    };
    auto next=[&](uint32_t handle,uint32_t expected) {
        ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=scratch+40;ctx.r6.u64=scratch+44;
        __imp__XNotifyGetNext(ctx,base);
        if(!ctx.r3.u32)return false;
        check(memory->read32(scratch+40)==0x0A000003 && memory->read32(scratch+44)==expected,
              "Notification alias delivered the wrong payload");
        return true;
    };
    const uint32_t primary=create(),independent=create();
    const uint32_t alias=duplicate(primary),second=duplicate(primary);
    auto drain=[&](std::initializer_list<uint32_t> handles,uint32_t expected,const char* phase) {
        for(auto handle:handles)check(wait(handle)==0,"Listener aliases did not share notification signal");
        unsigned count=0;
        for(unsigned i=0;i<8;++i) {
            const auto handle=*(handles.begin()+i%handles.size());
            if(!next(handle,expected))break;
            ++count;
        }
        std::printf("NotificationAliases[%s] delivered=%u expected=1 handles=%zu\n",phase,count,handles.size());
        check(count==1,"One broadcast enqueued multiple notifications for a duplicated listener");
        for(auto handle:handles)
            check(wait(handle)==0x102 && !next(handle,expected),"Draining through an alias left a shared notification or signal");
        check(wait(independent)==0 && next(independent,expected) && !next(independent,expected) && wait(independent)==0x102,
              "Alias delivery consumed or duplicated a distinct listener's notification");
    };
    emit(1);drain({alias,second,primary},0,"three-handles");
    close(primary);
    const uint32_t moved=duplicate(alias,true);
    check(wait(primary)==0xC0000008 && wait(alias)==0xC0000008,"Closed listener aliases stayed valid");
    emit(0);drain({second,moved},1,"closed-source");
    emit(1);
    const uint32_t pending=duplicate(second);
    close(second);
    drain({pending,moved},0,"duplicate-pending");
    close(pending);close(moved);close(independent);memory->release(scratch);
    std::puts("Notification aliases preserve single delivery, shared draining, independent listeners and pending-event lifetime.");
}
