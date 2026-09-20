#include "ppc_context.template.h"
#include <array>
#include <algorithm>
#include <bit>
#include <limits>

enum class Kind { Convert, PackWords, PackHalves, CompareEqual, CompareSigned, CompareUnsigned,
                  FusedNegativeMultiplyAdd, MultiplyHighUnsigned };
struct Fixture {
    const char* name;
    Kind kind;
    int destination, a, b, shift;
    bool record;
    void (*run)(PPCContext&);
};
#include "translated_instructions.inc"

static int failures = 0;
static int checks = 0;
static void check(bool condition, const Fixture& fixture, const char* detail) {
    ++checks;
    if (!condition) {
        if (failures < 20)
            std::fprintf(stderr, "%s d=%d a=%d b=%d shift=%d record=%d: %s\n", fixture.name,
                         fixture.destination, fixture.a, fixture.b, fixture.shift, fixture.record, detail);
        ++failures;
    }
}

static void conversion(const Fixture& fixture) {
    // Fixed expected results include truncation, both saturation boundaries,
    // infinities and NaNs. The VMX128 spelling must have the same semantics.
    const std::array<float, 12> values{0.0f, -0.0f, 1.0f, 1.75f, -1.0f, 0.5f,
        std::bit_cast<float>(0x4F7FFFFFu), std::bit_cast<float>(0x4F800000u),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN()};
    const std::array<std::array<uint32_t, 12>, 3> expected{{
        {0, 0, 1, 1, 0, 0, 0xFFFFFF00u, UINT32_MAX, UINT32_MAX, 0, 0, 0},
        {0, 0, 2, 3, 0, 1, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0, 0},
        {0, 0, 0x80000000u, 0xE0000000u, 0, 0x40000000u, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0, 0}
    }};
    const auto& result = expected[fixture.shift == 0 ? 0 : fixture.shift == 1 ? 1 : 2];
    for (size_t offset = 0; offset < values.size(); offset += 4) {
        PPCContext ctx{};
        ctx.fpscr.loadFromHost();
        PPCVRegister* registers[]{&ctx.v0, &ctx.v1, &ctx.v2};
        for (size_t lane = 0; lane < 4; ++lane) registers[fixture.a]->f32[lane] = values[offset + lane];
        auto source = *registers[fixture.a];
        fixture.run(ctx);
        for (size_t lane = 0; lane < 4; ++lane)
            check(registers[fixture.destination]->u32[lane] == result[offset + lane], fixture, "conversion result");
        if (fixture.destination != fixture.a)
            check(std::memcmp(&source, registers[fixture.a], sizeof(source)) == 0, fixture, "source preservation");
    }
}

static void packing(const Fixture& fixture) {
    for (int pattern = 0; pattern < 3; ++pattern) {
        PPCContext ctx{};
        ctx.fpscr.loadFromHost();
        PPCVRegister* registers[]{&ctx.v0, &ctx.v1, &ctx.v2};
        for (size_t reg = 0; reg < 3; ++reg) {
            if (fixture.kind == Kind::PackWords) {
                const uint32_t edges[]{0, 0xFFFFu, 0x10000u, UINT32_MAX};
                for (size_t lane = 0; lane < 4; ++lane)
                    registers[reg]->u32[lane] = pattern == 0 ? uint32_t(lane + 1) :
                        pattern == 1 ? uint32_t((reg + 1) * 0x10000 + lane + 10 * reg) : edges[lane];
            } else {
                const uint16_t edges[]{0, 1, 254, 255, 256, 257, 0x8000, 0xFFFF};
                for (size_t lane = 0; lane < 8; ++lane)
                    registers[reg]->u16[lane] = pattern == 0 ? uint16_t(lane + 1) :
                        pattern == 1 ? uint16_t(20 * reg + lane) : edges[lane];
            }
        }
        const auto a = *registers[fixture.a], b = *registers[fixture.b];
        PPCVRegister expected{};
        if (fixture.kind == Kind::PackWords) {
            for (size_t lane = 0; lane < 4; ++lane) {
                expected.u16[lane] = uint16_t(b.u32[lane] & 0xFFFF);
                expected.u16[lane + 4] = uint16_t(a.u32[lane] & 0xFFFF);
            }
        } else {
            for (size_t lane = 0; lane < 8; ++lane) {
                expected.u8[lane] = uint8_t((std::min)(b.u16[lane], uint16_t(255)));
                expected.u8[lane + 8] = uint8_t((std::min)(a.u16[lane], uint16_t(255)));
            }
        }
        fixture.run(ctx);
        check(std::memcmp(&expected, registers[fixture.destination], sizeof(expected)) == 0, fixture, "packed lanes");
        if (fixture.destination != fixture.a)
            check(std::memcmp(&a, registers[fixture.a], sizeof(a)) == 0, fixture, "source A preservation");
        if (fixture.destination != fixture.b)
            check(std::memcmp(&b, registers[fixture.b], sizeof(b)) == 0, fixture, "source B preservation");
    }
}

static void comparison(const Fixture& fixture) {
    for (int pattern = 0; pattern < 4; ++pattern) {
        PPCContext ctx{};
        ctx.fpscr.loadFromHost();
        PPCVRegister* registers[]{&ctx.v0, &ctx.v1, &ctx.v2};
        bool all = true, none = true;
        PPCVRegister expected{};
        for (size_t lane = 0; lane < 8; ++lane) {
            const bool matches = pattern == 0 || (pattern == 2 && lane < 4) || (pattern == 3 && lane % 2);
            uint16_t a, b;
            if (fixture.kind == Kind::CompareEqual) {
                a = uint16_t(0x8000 + lane); b = matches ? a : uint16_t(a + 1);
            } else if (fixture.kind == Kind::CompareSigned) {
                a = uint16_t(-100 + int(lane)); b = uint16_t((matches ? -200 : 100) + int(lane));
            } else {
                a = uint16_t(0xFF00 + lane); b = matches ? uint16_t(lane) : a;
            }
            registers[fixture.a]->u16[lane] = a;
            registers[fixture.b]->u16[lane] = b;
            expected.u16[lane] = matches ? 0xFFFF : 0;
            all &= matches; none &= !matches;
        }
        ctx.cr6.lt = ctx.cr6.gt = ctx.cr6.eq = ctx.cr6.so = 1;
        fixture.run(ctx);
        check(std::memcmp(&expected, registers[fixture.destination], sizeof(expected)) == 0, fixture, "comparison lanes");
        check(ctx.cr6.lt == (fixture.record ? all : 1) && ctx.cr6.eq == (fixture.record ? none : 1) &&
              ctx.cr6.gt == (fixture.record ? 0 : 1) && ctx.cr6.so == (fixture.record ? 0 : 1), fixture, "CR6 result");
    }
}

static void scalar(const Fixture& fixture) {
    if (fixture.kind == Kind::FusedNegativeMultiplyAdd) {
        const std::array<std::array<double, 4>, 3> cases{{
            {1.0 + 0x1p-27, 1.0 - 0x1p-27, -1.0, 0x1p-54},
            {2.0, 3.0, 4.0, -10.0}, {0.0, 3.0, 0.0, -0.0}
        }};
        for (auto values : cases) {
            PPCContext ctx{};
            ctx.fpscr.loadFromHost();
            PPCRegister* registers[]{&ctx.f0, &ctx.f1, &ctx.f2, &ctx.f3};
            ctx.f1.f64 = values[0]; ctx.f2.f64 = values[1]; ctx.f3.f64 = values[2];
            fixture.run(ctx);
            check(registers[fixture.destination]->u64 == std::bit_cast<uint64_t>(values[3]), fixture, "fused arithmetic result");
        }
    } else {
        const std::array<std::array<uint64_t, 3>, 3> cases{{
            {UINT64_MAX, UINT64_MAX, UINT64_MAX - 1},
            {0x100000000ull, 0x100000000ull, 1}, {0, UINT64_MAX, 0}
        }};
        for (auto values : cases) {
            PPCContext ctx{};
            ctx.fpscr.loadFromHost();
            PPCRegister* registers[]{&ctx.r0, &ctx.r1, &ctx.r2};
            ctx.r1.u64 = values[0]; ctx.r2.u64 = values[1]; ctx.xer.so = 1;
            ctx.cr0.lt = ctx.cr0.gt = ctx.cr0.eq = ctx.cr0.so = 1;
            fixture.run(ctx);
            check(registers[fixture.destination]->u64 == values[2], fixture, "unsigned multiply high result");
            const bool negative = (values[2] >> 63) != 0;
            check(ctx.cr0.lt == (fixture.record ? negative : 1) &&
                  ctx.cr0.gt == (fixture.record ? (!negative && values[2] != 0) : 1) &&
                  ctx.cr0.eq == (fixture.record ? values[2] == 0 : 1) && ctx.cr0.so == 1, fixture, "signed CR0 result");
        }
    }
}

int main() {
    const auto csr = _mm_getcsr();
    for (const auto& fixture : fixtures) {
        if (fixture.kind == Kind::Convert) conversion(fixture);
        else if (fixture.kind == Kind::PackWords || fixture.kind == Kind::PackHalves) packing(fixture);
        else if (fixture.kind == Kind::FusedNegativeMultiplyAdd || fixture.kind == Kind::MultiplyHighUnsigned) scalar(fixture);
        else comparison(fixture);
    }
    _mm_setcsr(csr);
    std::printf("%zu generated instruction variants; %d checks; %d failures\n", std::size(fixtures), checks, failures);
    return failures ? 1 : 0;
}
