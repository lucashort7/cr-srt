// whereabouts.cpp
#include "whereabouts.h"

#include "log.h"
#include "names.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace crsrt::whereabouts {

namespace {

constexpr int kNameLen = 64;
constexpr int kBound = 32;

struct Bound {
    uint64_t level;
    char name[kNameLen];
    bool written;  // in [areas] already
};

// Everything under g_lock: the naming from any thread, the clock and the label from the render and walk threads.
SRWLOCK g_lock = SRWLOCK_INIT;
Bound g_bound[kBound];
int g_count;
char g_pending[kNameLen];
uint64_t g_pending_from, g_pending_at;
bool g_has_pending;

// The [areas] label of g_label_level, as flush() last read it; g_seen_level the level the last frame saw.
uint64_t g_label_level, g_seen_level;
char g_label[32];

// The zone's clock. g_zone is the zone it runs in, the sonar's id or "" outside every district, and g_shown that zone
// as the sonar last read it. g_away is the running time since the sonar last read g_zone, which no zone owns until one
// is taken. g_next is a district the sonar reads that has not held kZoneSettle yet, g_next_held how long it has, and
// g_empty the running time of the current stretch with no district.
char g_zone[32], g_next[32];
double g_zone_secs, g_away, g_next_held, g_empty;
sonar::District g_shown, g_next_shown;

void take(const char *key, const sonar::District &shown) {
    if (std::strncmp(key, g_zone, sizeof g_zone) == 0) {
        g_zone_secs += g_away;
    } else {
        std::snprintf(g_zone, sizeof g_zone, "%s", key);
        g_zone_secs = g_away;
    }
    g_shown = shown;
    g_next[0] = '\0';
    g_away = g_next_held = g_empty = 0.0;
}

void tick(const sonar::District &d, double run) {
    const char *key = d.has ? d.id : "";
    const bool gap = key[0] == '\0' && g_next[0] != '\0';  // no district while one is held pending
    if (!gap && std::strncmp(key, g_zone, sizeof g_zone) == 0) {
        g_zone_secs += g_away + run;
        g_shown = d;
        g_next[0] = '\0';
        g_away = g_next_held = g_empty = 0.0;
        return;
    }
    g_away += run;
    if (key[0] == '\0') {
        g_empty += run;
        g_next_held += run;
        if (g_empty >= kZoneSettle) take("", d);
        return;
    }
    g_empty = 0.0;
    if (std::strncmp(key, g_next, sizeof g_next) != 0) {
        std::snprintf(g_next, sizeof g_next, "%s", key);
        g_next_held = 0.0;
    }
    g_next_held += run;
    g_next_shown = d;
    if (g_next_held >= kZoneSettle) take(key, g_next_shown);
}

// The pending name onto `level` once the level is no longer the one it was named from.
void bind(uint64_t level, uint64_t now_ms) {
    // the two times are read outside the lock, so a name can carry a later stamp than the frame that sees it
    if (g_has_pending && now_ms > g_pending_at && now_ms - g_pending_at > kPendingMs) g_has_pending = false;
    if (!g_has_pending || !level || level == g_pending_from) return;
    int i = 0;
    while (i < g_count && g_bound[i].level != level) i++;
    if (i == g_count && g_count < kBound) g_count++;
    if (i < kBound) {
        g_bound[i] = {level, {}, false};
        std::snprintf(g_bound[i].name, sizeof g_bound[i].name, "%s", g_pending);
        log::write("whereabouts: level %08lx%08lx is %s", static_cast<unsigned long>(level >> 32),
                   static_cast<unsigned long>(level & 0xffffffffULL), g_pending);
    }
    g_has_pending = false;
}

const char *name_of(uint64_t level) {
    for (int i = 0; i < g_count; i++)
        if (g_bound[i].level == level) return g_bound[i].name;
    return nullptr;
}

const char *machine_label(uint32_t hash, char *hex, size_t cap) {
    for (const auto &m : names::kMachines)
        if (m.hash == hash) return m.name;
    std::snprintf(hex, cap, "%08x", static_cast<unsigned>(hash));
    return hex;
}

}  // namespace

void level_named(const char *level, uint64_t from, uint64_t at_ms) {
    const char *base = level;
    for (const char *p = level; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    AcquireSRWLockExclusive(&g_lock);
    std::snprintf(g_pending, sizeof g_pending, "%s", base);
    g_pending_from = from;
    g_pending_at = at_ms;
    g_has_pending = true;
    ReleaseSRWLockExclusive(&g_lock);
    log::write("whereabouts: %s named by a call, from level %08lx%08lx", base, static_cast<unsigned long>(from >> 32),
               static_cast<unsigned long>(from & 0xffffffffULL));
}

void frame(const Inputs &in, View &out) {
    AcquireSRWLockExclusive(&g_lock);
    tick(in.district, in.run);
    bind(in.level, in.now_ms);
    g_seen_level = in.level;

    char hex[12];
    // a level a call named, other than a zone's own, with no district around it: a sequence (the taxi, a dungeon)
    const char *name = name_of(in.level);
    const bool sequence = in.level && name && std::strncmp(name, "zone_", 5) != 0;
    if (!g_shown.has && in.in_gap)
        std::snprintf(out.zone, sizeof out.zone, "THE_GAP");
    else if (!g_shown.has && in.challenge)
        std::snprintf(out.zone, sizeof out.zone, "CHALLENGE %s", machine_label(in.machine, hex, sizeof hex));
    else if (!g_shown.has && sequence)
        std::snprintf(out.zone, sizeof out.zone, "MINIQUEST");
    else if (!g_shown.has)
        std::snprintf(out.zone, sizeof out.zone, "--");
    else if (g_shown.level == 0)
        std::snprintf(out.zone, sizeof out.zone, "%s", g_shown.id);
    else
        std::snprintf(out.zone, sizeof out.zone, "%s [lv%d]", g_shown.id, g_shown.level);
    if (in.in_story) std::strncat(out.zone, " [story]", sizeof out.zone - std::strlen(out.zone) - 1);

    // a fast travel names the area it landed in, finer than the level, and holds while the level is that one
    if (in.point[0] && in.level && in.point_level == in.level)
        std::snprintf(out.section, sizeof out.section, "%s", in.point);
    else if (in.level && name)
        std::snprintf(out.section, sizeof out.section, "%s", name);
    else if (in.level && g_label[0] && g_label_level == in.level)
        std::snprintf(out.section, sizeof out.section, "%s", g_label);
    else if (in.level)
        std::snprintf(out.section, sizeof out.section, "%08lx", static_cast<unsigned long>(in.level >> 32));
    else
        std::snprintf(out.section, sizeof out.section, "--");

    out.seconds = static_cast<long>(g_zone_secs + g_away);
    out.paused = in.paused;
    ReleaseSRWLockExclusive(&g_lock);
}

void flush(const wchar_t *ini) {
    // the pairs to write and the level to label, taken under the lock; the file touched outside it
    Bound fresh[kBound];
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_count; i++)
        if (!g_bound[i].written) {
            fresh[n++] = g_bound[i];
            g_bound[i].written = true;
        }
    const uint64_t level = g_seen_level;
    ReleaseSRWLockExclusive(&g_lock);

    wchar_t key[16], value[kNameLen];
    for (int i = 0; i < n; i++) {
        std::swprintf(key, std::size(key), L"%08lx", static_cast<unsigned long>(fresh[i].level >> 32));
        std::swprintf(value, std::size(value), L"%hs", fresh[i].name);
        WritePrivateProfileStringW(L"areas", key, value, ini);
    }
    wchar_t label[32] = L"";
    if (level) {
        std::swprintf(key, std::size(key), L"%08lx", static_cast<unsigned long>(level >> 32));
        GetPrivateProfileStringW(L"areas", key, L"", label, static_cast<DWORD>(std::size(label)), ini);
    }
    AcquireSRWLockExclusive(&g_lock);
    std::snprintf(g_label, sizeof g_label, "%ls", label);
    g_label_level = level;
    ReleaseSRWLockExclusive(&g_lock);
}

void reset() {
    AcquireSRWLockExclusive(&g_lock);
    g_count = 0;
    g_has_pending = false;
    g_label_level = g_seen_level = 0;
    g_label[0] = g_zone[0] = g_next[0] = '\0';
    g_zone_secs = g_away = g_next_held = g_empty = 0.0;
    g_shown = g_next_shown = {};
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace crsrt::whereabouts
