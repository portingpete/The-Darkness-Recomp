#pragma once
#include <cstdlib>

static bool objectOutputWithoutHostFault(PPCFunc* function, PPCContext& ctx, uint8_t* base) {
    __try { function(ctx, base); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_GUARD_PAGE
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

static void testObjectOutputs(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t scratch = memory->allocate(3 * 4096);
    check(scratch != 0, "Object output fixture allocation failed");
    const uint32_t page = scratch + 4096, out = scratch + 32;
    PPCContext call = ctx;
    call.r3.u64 = out; call.r4.u64 = 0; call.r5.u64 = 0; call.r6.u64 = 0;
    __imp__NtCreateEvent(call, base);
    check(call.r3.u32 == 0, "Object output event creation failed");
    const uint32_t event = memory->read32(out);
    call = ctx; call.r3.u64 = 0xfffffffe; call.r4.u64 = 0x80000000; call.r5.u64 = out;
    __imp__ObReferenceObjectByHandle(call, base);
    check(call.r3.u32 == 0, "Object output thread reference failed");
    const uint32_t thread = memory->read32(out);
    PPCFunc* functions[]{__imp__NtDuplicateObject, __imp__ObOpenObjectByPointer,
                         __imp__ObReferenceObjectByHandle, __imp__ObLookupThreadByThreadId};
    const char* operations[]{"duplicate", "open-reference", "reference-handle", "lookup-thread"};
    auto protect = [&](DWORD protection) {
        DWORD old;
        check(VirtualProtect(base + page, 4096, protection, &old), "Object output protection setup failed");
    };
    auto prepare = [&](unsigned operation, uint32_t output) {
        call = ctx;
        if (operation == 0) { call.r3.u64 = event; call.r4.u64 = output; call.r5.u64 = 0; }
        if (operation == 1) { call.r3.u64 = thread; call.r4.u64 = output; }
        if (operation == 2) {
            call.r3.u64 = 0xfffffffe; call.r4.u64 = 0x80000000; call.r5.u64 = output;
        }
        if (operation == 3) { call.r3.u64 = GetCurrentThreadId(); call.r4.u64 = output; }
    };
    const char* cases[]{"noaccess", "readonly", "guard", "cross-page", "wrap"};
    for (unsigned operation = 0; operation < std::size(functions); ++operation) {
        for (unsigned test = 0; test < std::size(cases); ++test) {
            protect(PAGE_READWRITE);
            memset(base + page - 4, 0xa5, 12);
            protect(test == 1 ? PAGE_READONLY : test == 2 ? PAGE_READWRITE | PAGE_GUARD : PAGE_NOACCESS);
            prepare(operation, test == 3 ? page - 2 : test == 4 ? 0xfffffffeu : page);
            if (!objectOutputWithoutHostFault(functions[operation], call, base)) {
                // /EHsc leaves the object mutex locked after the host fault.
                // Do not reenter it or run guest-resource destructors on failure.
                fprintf(stderr, "ObjectOutputs[%s %s] HOST FAULT\n", operations[operation], cases[test]);
                fflush(nullptr); std::_Exit(1);
            }
            check(call.r3.u32 == 0xc0000005, "Invalid object output did not return access violation");
            MEMORY_BASIC_INFORMATION info{};
            check(VirtualQuery(base + page, &info, sizeof(info)) != 0, "Cannot query object output guard");
            if (test == 2) check((info.Protect & PAGE_GUARD) != 0, "Rejected output consumed the guest guard");
            protect(PAGE_READWRITE);
            for (uint32_t i = page - 4; i < page + 8; ++i)
                check(base[i] == 0xa5, "Rejected output partially overwrote guest memory");
            // Prove the global lock and the source handle remain usable.
            call = ctx; call.r3.u64 = event; call.r4.u64 = 0;
            __imp__NtSetEvent(call, base);
            check(call.r3.u32 == 0, "Rejected publication damaged the source handle or object lock");
            printf("ObjectOutputs[%s %s] passed\n", operations[operation], cases[test]);
        }
        // A valid output can cross distinct writable VirtualQuery regions.
        protect(PAGE_EXECUTE_READWRITE);
        prepare(operation, page - 2);
        functions[operation](call, base);
        check(call.r3.u32 == 0, "Valid split-writable object output was rejected");
        const uint32_t published = memory->read32(page - 2);
        call = ctx; call.r3.u64 = published;
        if (operation < 2) {
            check(published != 0, "Object publication returned a null handle");
            __imp__NtClose(call, base);
            check(call.r3.u32 == 0, "Published object alias could not be closed");
        } else {
            check(published == thread, "Object reference publication changed the guest thread address");
            __imp__ObDereferenceObject(call, base);
        }
    }
    // Close-source duplication must reject bad output before losing the source.
    protect(PAGE_READONLY);
    prepare(0, page); call.r5.u64 = 1;
    __imp__NtDuplicateObject(call, base);
    check(call.r3.u32 == 0xc0000005, "Close-source duplication accepted a readonly output");
    prepare(0, out); call.r5.u64 = 1;
    __imp__NtDuplicateObject(call, base);
    check(call.r3.u32 == 0, "Rejected close-source duplication destroyed the original handle");
    call = ctx; call.r3.u64 = memory->read32(out); __imp__NtClose(call, base);
    check(call.r3.u32 == 0, "Moved alias close failed");
    call.r3.u64 = event; __imp__NtClose(call, base);
    check(call.r3.u32 == 0xc0000008, "Successful close-source duplication retained the source handle");
    call = ctx; call.r3.u64 = thread; __imp__ObDereferenceObject(call, base);
    protect(PAGE_READWRITE);
    check(memory->release(scratch), "Object output fixture release failed");
    puts("Kernel object publication validates complete output spans before mutating shared state.");
}
