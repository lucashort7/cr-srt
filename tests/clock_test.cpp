// clock_test.cpp — the microsecond clock past the uptime where counter * 1 000 000 overflowed.
#include "clock.h"
#include "check.h"

#include <cstdint>

using namespace crsrt;

TEST(the_clock_converts_without_overflow) {
    constexpr int64_t hz = 10'000'000;
    CHECK(clock::to_us(hz, hz) == 1'000'000);
    CHECK(clock::to_us(hz + 5, hz) == 1'000'000);  // 0.5 us rounds down
    CHECK(clock::to_us(hz + 10, hz) == 1'000'001);
    // 30 days at 10 MHz: the old product, ticks * 1 000 000, is past INT64_MAX here.
    constexpr int64_t days30 = 30LL * 86400 * hz;
    CHECK(clock::to_us(days30, hz) == 30LL * 86400 * 1'000'000);
    CHECK(clock::to_us(days30 + 3, hz) == 30LL * 86400 * 1'000'000);
}

TEST(the_clock_moves_forward) {
    const int64_t a = clock::now_us();
    const int64_t b = clock::now_us();
    CHECK(a > 0);
    CHECK(b >= a);
}
