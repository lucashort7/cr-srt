// sequence.cpp
#include "sequence.h"

#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace crsrt::sequence {

namespace {

constexpr int kNameLen = 64;
constexpr int kBound = 32;
constexpr ULONGLONG kPendingMs = 30000;  // a call whose id never changes within this is forgotten

struct Bound {
    uint64_t id;
    char name[kNameLen];
};

SRWLOCK g_lock = SRWLOCK_INIT;
Bound g_bound[kBound];
int g_count;
char g_pending[kNameLen];
uint64_t g_pending_from;
ULONGLONG g_pending_at;
bool g_has_pending;

}  // namespace

void enter(const char *level, uint64_t from) {
    const char *base = level;
    for (const char *p = level; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    AcquireSRWLockExclusive(&g_lock);
    std::snprintf(g_pending, sizeof g_pending, "%s", base);
    g_pending_from = from;
    g_pending_at = GetTickCount64();
    g_has_pending = true;
    ReleaseSRWLockExclusive(&g_lock);
    log::write("sequence: %s named by a call, from id %08lx%08lx", base, static_cast<unsigned long>(from >> 32),
               static_cast<unsigned long>(from & 0xffffffffULL));
}

bool name_for(uint64_t id, char *out, size_t cap) {
    bool found = false;
    char bound[kNameLen] = {};
    AcquireSRWLockExclusive(&g_lock);
    if (g_has_pending && GetTickCount64() - g_pending_at > kPendingMs) g_has_pending = false;
    if (g_has_pending && id && id != g_pending_from) {
        int i = 0;
        while (i < g_count && g_bound[i].id != id) i++;
        if (i == g_count && g_count < kBound) g_count++;
        if (i < kBound) {
            g_bound[i].id = id;
            std::snprintf(g_bound[i].name, sizeof g_bound[i].name, "%s", g_pending);
            std::snprintf(bound, sizeof bound, "%s", g_pending);
        }
        g_has_pending = false;
    }
    for (int i = 0; i < g_count && !found; i++)
        if (g_bound[i].id == id) {
            std::snprintf(out, cap, "%s", g_bound[i].name);
            found = true;
        }
    ReleaseSRWLockExclusive(&g_lock);
    if (bound[0])
        log::write("sequence: id %08lx%08lx is %s", static_cast<unsigned long>(id >> 32),
                   static_cast<unsigned long>(id & 0xffffffffULL), bound);
    return found;
}

}  // namespace crsrt::sequence
