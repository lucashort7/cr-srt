// envwatch.cpp
#include "envwatch.h"

#include "log.h"
#include "memory.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace crsrt::envwatch {

namespace {

constexpr int kPerLine = 6;

// Every env pointer a tray has shown, any system's. Appended only; 18 hooked systems hold under 100.
constexpr int kKnownMax = 256;
SRWLOCK g_known_lock = SRWLOCK_INIT;
const uint8_t *g_known[kKnownMax];
int g_known_count;

void note(const uint8_t *p) {
    if (!p) return;
    AcquireSRWLockExclusive(&g_known_lock);
    bool seen = false;
    for (int i = 0; i < g_known_count && !seen; i++) seen = g_known[i] == p;
    if (!seen && g_known_count < kKnownMax) g_known[g_known_count++] = p;
    ReleaseSRWLockExclusive(&g_known_lock);
}

// The distance from `env` to the nearest known env above it, or `size` when none is closer; a multiple of 8.
int capped(const uint8_t *env, int size) {
    uintptr_t cap = static_cast<uintptr_t>(size);
    AcquireSRWLockShared(&g_known_lock);
    for (int i = 0; i < g_known_count; i++)
        if (g_known[i] > env && static_cast<uintptr_t>(g_known[i] - env) < cap)
            cap = static_cast<uintptr_t>(g_known[i] - env);
    ReleaseSRWLockShared(&g_known_lock);
    return static_cast<int>(cap) & ~7;
}

}  // namespace

void note_tray(const uint8_t *tray, int slots) {
    for (int i = 0; i < slots; i++) {
        const uint8_t *p = nullptr;
        if (mem::copy(tray + static_cast<uintptr_t>(i) * 8, &p, sizeof p)) note(p);
    }
}

void diff_bytes(const char *who, const char *name, const uint8_t *now, uint8_t *seen, Count *counts, int size,
                long call) {
    if (std::memcmp(now, seen, static_cast<size_t>(size)) == 0) {
        for (int q = 0; q < size / 8; q++)
            if (counts[q].run != kMuted) counts[q].run = 0;
        return;
    }
    char line[kPerLine * 44 + 8];
    char muted[kPerLine * 8 + 8];
    size_t k = 0, m = 0;
    int shown = 0, muted_now = 0;
    for (int off = 0; off + 8 <= size; off += 8) {
        Count &c = counts[off / 8];
        if (c.run == kMuted) continue;
        if (std::memcmp(now + off, seen + off, 8) == 0) {
            c.run = 0;
            continue;
        }
        c.run++;
        if (c.total < 0xff) c.total++;
        uint64_t was = 0, is = 0;
        std::memcpy(&was, seen + off, 8);
        std::memcpy(&is, now + off, 8);
        k += static_cast<size_t>(std::snprintf(line + k, sizeof line - k, "%s+%03x %016llx>%016llx", k ? " " : "", off,
                                               static_cast<unsigned long long>(was),
                                               static_cast<unsigned long long>(is)));
        if (++shown % kPerLine == 0) {
            log::write("%s: %s diff %s (call %ld)", who, name, line, call);
            k = 0;
        }
        if (c.run >= kMuteRun || c.total >= kMuteTotal) {
            m += static_cast<size_t>(
                std::snprintf(muted + m, sizeof muted - m, " +%03x %s", off, c.run >= kMuteRun ? "clock" : "counter"));
            c.run = kMuted;
            if (++muted_now % 3 == 0) {
                log::write("%s: %s muted:%s", who, name, muted);
                m = 0;
            }
        }
    }
    if (k) log::write("%s: %s diff %s (call %ld)", who, name, line, call);
    if (m) log::write("%s: %s muted:%s", who, name, muted);
    std::memcpy(seen, now, static_cast<size_t>(size));
}

void read(Watch &w, const uint8_t *tray, int slots, const char *who, long call) {
    if (!w.ever) note_tray(tray, slots);
    const uint8_t *env = nullptr;
    if (!mem::copy(tray + w.slot, &env, sizeof env)) return;
    if (!env) {
        if (!w.said_empty) {
            log::write("%s: %s slot +%lx is empty (call %ld)", who, w.name, static_cast<unsigned long>(w.slot), call);
            w.said_empty = true;
        }
        return;
    }
    const uint8_t *object = env;
    if (w.follow && (!mem::copy(env, &object, sizeof object) || !object)) {
        if (!w.said_unreadable) {
            log::write("%s: %s at %p holds no pointer to follow (call %ld)", who, w.name, env, call);
            w.said_unreadable = true;
        }
        return;
    }
    const int cap = w.follow ? w.size & ~7 : capped(env, w.size);
    uint8_t now[kMaxBytes];
    if (cap <= 0 || !mem::copy(object, now, static_cast<size_t>(cap))) {
        if (!w.said_unreadable) {
            log::write("%s: %s at %p unreadable over %d bytes (call %ld)", who, w.name, object, cap, call);
            w.said_unreadable = true;
        }
        return;
    }
    if (!w.ever) {
        std::memcpy(w.seen, now, static_cast<size_t>(cap));
        w.at = object;
        w.cap = cap;
        w.ever = true;
        log::write("%s: %s at %p: %d of %d bytes watched%s (call %ld)", who, w.name, object, cap, w.size,
                   w.follow ? ", followed" : "", call);
        return;
    }
    if (cap < w.cap) {
        log::write("%s: %s capped from %d to %d bytes by a neighbour another tray showed (call %ld)", who, w.name,
                   w.cap, cap, call);
        w.cap = cap;
    }
    if (object != w.at) {
        log::write("%s: %s moved %p -> %p (call %ld)", who, w.name, w.at, object, call);
        w.at = object;
    }
    diff_bytes(who, w.name, now, w.seen, w.counts, w.cap, call);
}

}  // namespace crsrt::envwatch
