#pragma once
#include <cstdlib>
#include <string>

static std::string formattedGuestText;
static PPC_FUNC(formatCaptureProbe) {
    formattedGuestText.assign(reinterpret_cast<const char*>(base + ctx.r3.u32));
}

static void testNativeFormatAbi(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t fixture = memory->allocate(4096);
    check(fixture != 0, "Format ABI fixture allocation failed");
    const uint32_t format = fixture + 32, value = fixture + 96;
    strcpy_s(reinterpret_cast<char*>(base + format), 64, "%s %03d %x");
    strcpy_s(reinterpret_cast<char*>(base + value), 32, "ready");
    const auto original = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = formatCaptureProbe;
    memory->write32(fixture, PPC_CODE_BASE);
    ctx.r3.u64 = fixture; ctx.r4.u64 = format;
    ctx.r5.u64 = value; ctx.r6.u64 = 7; ctx.r7.u64 = 0xabcd;
    // Execute the real SDK's std spills and va_list construction, not a
    // synthetic layout patterned after the native formatter implementation.
    sub_8286C450(ctx, base);
    PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
    check(formattedGuestText == "ready 007 abcd",
          "Original SDK varargs lost the low words of big-endian GPR slots");
    // Original GPU diagnostics: guest calls 0x8286C510 and 0x8286C5DC.
    // Use the loaded image's formats and names, including the three literal spaces.
    auto checkTitleFormat = [&](uint32_t titleFormat, uint32_t titleName,
                                const char* expectedFormat, const char* expectedName,
                                const std::string& expectedOutput) {
        check(strcmp(reinterpret_cast<const char*>(base + titleFormat), expectedFormat) == 0 &&
              strcmp(reinterpret_cast<const char*>(base + titleName), expectedName) == 0,
              "Original title format/name bytes differ from the audited image");
        formattedGuestText.clear();
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = formatCaptureProbe;
        ctx.r3.u64 = fixture; ctx.r4.u64 = titleFormat;
        ctx.r5.u64 = titleName; ctx.r6.u64 = 0xabcd;
        sub_8286C450(ctx, base);
        PPC_LOOKUP_FUNC(base, PPC_CODE_BASE) = original;
        check(formattedGuestText == expectedOutput,
              "Original GPU diagnostic lost its %25s right alignment through the SDK wrapper");
    };
    checkTitleFormat(0x8209D050, 0x8209D044, "   %25s: 0x%08x (", "RBBM_STATUS",
                     std::string(17, ' ') + "RBBM_STATUS: 0x0000abcd (");
    checkTitleFormat(0x8209D090, 0x8209D07C, "   %25s: 0x%08x\n", "RB_SIDEBAND_DATA[0]",
                     std::string(9, ' ') + "RB_SIDEBAND_DATA[0]: 0x0000abcd\n");
    memory->release(fixture);
    puts("Original SDK formatting preserves varargs and title string alignment.");
}

static unsigned formatInvalidParameters;
static void formatInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    ++formatInvalidParameters;
}
static void testNativeFormatWidth(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t fixture = memory->allocate(4096);
    check(fixture != 0, "Format width fixture allocation failed");
    const uint32_t format = fixture, output = fixture + 128;
    struct RestoreHandler {
        _invalid_parameter_handler previous = _set_thread_local_invalid_parameter_handler(formatInvalidParameter);
        ~RestoreHandler() { _set_thread_local_invalid_parameter_handler(previous); }
    } restore;
    formatInvalidParameters = 0;
    strcpy_s(reinterpret_cast<char*>(base + format), 128, "%200d");
    ctx.r3.u64 = output; ctx.r4.u64 = format; ctx.r5.u64 = 7;
    __imp__sprintf(ctx, base);
    check(!formatInvalidParameters && ctx.r3.s32 == 200 &&
          std::string(reinterpret_cast<const char*>(base + output)) == std::string(199, ' ') + '7',
          "A valid wide integer format triggered the host CRT invalid-parameter handler");
    strcpy_s(reinterpret_cast<char*>(base + format), 128, "%0200d");
    ctx.r3.u64 = output; ctx.r4.u64 = format; ctx.r5.s64 = -7;
    __imp__sprintf(ctx, base);
    check(!formatInvalidParameters && ctx.r3.s32 == 200 &&
          std::string(reinterpret_cast<const char*>(base + output)) == '-' + std::string(198, '0') + '7',
          "Wide zero padding lost the sign or digit");
    // The bounded import must truncate without constructing a width-sized
    // host temporary, including when width is larger than its output buffer.
    strcpy_s(reinterpret_cast<char*>(base + format), 128, "%0200d");
    const uint32_t arguments = fixture + 512;
    PPC_STORE_U64(arguments, 7);
    memset(base + output, 0xa5, 64);
    ctx.r3.u64 = output; ctx.r4.u64 = 32; ctx.r5.u64 = format; ctx.r6.u64 = arguments;
    __imp___vsnprintf(ctx, base);
    check(!formatInvalidParameters && ctx.r3.s32 == -1 &&
          std::string(reinterpret_cast<const char*>(base + output)) == std::string(31, '0') &&
          base[output + 32] == 0xa5,
          "Wide bounded formatting aborted or overwrote its destination");
    // Cover the shared string-width path in sprintf with the exact title format.
    ctx.r3.u64 = output; ctx.r4.u64 = 0x8209D050;
    ctx.r5.u64 = 0x8209D044; ctx.r6.u64 = 0xabcd;
    __imp__sprintf(ctx, base);
    check(!formatInvalidParameters && ctx.r3.s32 == 42 &&
          std::string(reinterpret_cast<const char*>(base + output)) ==
              std::string(17, ' ') + "RBBM_STATUS: 0x0000abcd (",
          "sprintf lost the original title's %25s right alignment");

    // Truncate first inside the padding, then inside the original title's name.
    strcpy_s(reinterpret_cast<char*>(base + format), 128, "%25s");
    PPC_STORE_U64(arguments, 0x8209D044);
    auto checkBoundedString = [&](uint32_t capacity, const std::string& expectedOutput,
                                  int32_t expectedResult) {
        memset(base + output - 1, 0xa5, 65);
        ctx.r3.u64 = output; ctx.r4.u64 = capacity;
        ctx.r5.u64 = format; ctx.r6.u64 = arguments;
        __imp___vsnprintf(ctx, base);
        bool guardsIntact = base[output - 1] == 0xa5;
        for (uint32_t i = capacity; i < 64; ++i)
            guardsIntact = guardsIntact && base[output + i] == 0xa5;
        check(!formatInvalidParameters && ctx.r3.s32 == expectedResult && guardsIntact &&
              memcmp(base + output, expectedOutput.data(), expectedOutput.size()) == 0 &&
              base[output + expectedOutput.size()] == 0,
              "Bounded %25s lost padding, truncation, termination, or destination guards");
    };
    checkBoundedString(8, std::string(7, ' '), -1);
    checkBoundedString(20, std::string(14, ' ') + "RBBM_", -1);
    checkBoundedString(26, std::string(14, ' ') + "RBBM_STATUS", 25);
    memory->release(fixture);
    puts("Numeric and title string widths preserve padding and bounded writes.");
}
