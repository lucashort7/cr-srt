// sequence.cpp
#include "sequence.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace crsrt::sequence {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
char g_name[64];
bool g_in;

}  // namespace

void enter(const char *level) {
    const char *base = level;
    for (const char *p = level; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    AcquireSRWLockExclusive(&g_lock);
    const bool changed = !g_in || std::strncmp(g_name, base, sizeof g_name) != 0;
    std::snprintf(g_name, sizeof g_name, "%s", base);
    g_in = true;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) log::write("sequence: in %s", base);
}

void leave() {
    AcquireSRWLockExclusive(&g_lock);
    const bool was = g_in;
    g_in = false;
    ReleaseSRWLockExclusive(&g_lock);
    if (was) log::write("sequence: out");
}

bool current(char *out, size_t cap) {
    AcquireSRWLockShared(&g_lock);
    const bool in = g_in;
    if (in) std::snprintf(out, cap, "%s", g_name);
    ReleaseSRWLockShared(&g_lock);
    return in;
}

}  // namespace crsrt::sequence
