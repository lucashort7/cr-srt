// clock.h — microseconds off QueryPerformanceCounter, for the cost meters and the walk's pacing. Whole seconds and the
// rest are scaled apart: the counter times 1 000 000 overflows an int64 past 10.7 days of uptime at 10 MHz.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::clock {

// `ticks` of a counter running at `hz`, in microseconds.
constexpr int64_t to_us(int64_t ticks, int64_t hz) { return ticks / hz * 1000000 + ticks % hz * 1000000 / hz; }

inline int64_t now_us() {
    static const int64_t hz = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return to_us(t.QuadPart, hz);
}

}  // namespace crsrt::clock
