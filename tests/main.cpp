// main.cpp — runs every registered test and returns the number of failed checks: 0 is a pass for ctest.
#include "check.h"

#include <cstdio>

namespace crsrt::test {

namespace {

struct Case {
    const char *name;
    Fn fn;
};

constexpr int kMaxCases = 256;
Case g_cases[kMaxCases];
int g_count;
int g_failures;

}  // namespace

Register::Register(const char *name, Fn fn) {
    if (g_count < kMaxCases) g_cases[g_count++] = {name, fn};
}

void fail(const char *file, int line, const char *text) {
    std::printf("  %s:%d: CHECK(%s) failed\n", file, line, text);
    g_failures++;
}

}  // namespace crsrt::test

int main() {
    using namespace crsrt::test;
    int failed_cases = 0;
    for (int i = 0; i < g_count; i++) {
        const int before = g_failures;
        g_cases[i].fn();
        const bool ok = g_failures == before;
        if (!ok) failed_cases++;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", g_cases[i].name);
    }
    std::printf("%d tests, %d failed, %d failed checks\n", g_count, failed_cases, g_failures);
    return g_failures;
}
