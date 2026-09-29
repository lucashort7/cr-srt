// sonar.cpp
#include "hooks/sonar.h"

#include "game.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"

#include "MinHook.h"

#include <algorithm>
#include <cstring>

namespace crsrt::sonar {

namespace {

using SonarFn = void (*)(void *, void *, void *, void *);

constexpr LONG kEvery = 15;  // calls between two reads, ~0.5 s at the sonar's 30 a second
constexpr uint32_t kMaxDistricts = 16;
constexpr uint32_t kMaxCosts = 32;
constexpr uint32_t kMaxActive = 8;

SonarFn g_original;
volatile LONG g_calls;
volatile LONG g_inflight;

// Sonar thread -> render thread.
SRWLOCK g_lock = SRWLOCK_INIT;
District g_district;

// Read by the sonar's own thread only: the copies each read is made from, and what the log last said.
uint8_t g_catalogue[kMaxDistricts * game::kDistrictStride];
uint8_t g_records[kMaxDistricts * game::kRuntimeStride];
uint32_t g_costs[kMaxCosts];
uint8_t g_active[kMaxActive * game::kActiveStride];
District g_logged;
uint32_t g_logged_others[kMaxActive];
uint32_t g_logged_count = ~0u;  // matches no list: the first look is always logged

template <typename T>
bool field(const uint8_t *base, uintptr_t off, T &out) {
    return mem::copy(base + off, &out, sizeof out);
}

// The active list's entries copied into g_active, the first one's district hash in *hash; 0 when the list is empty.
uint32_t read_active(const uint8_t *active, uint32_t *hash) {
    const uint8_t *list = nullptr;
    uint32_t count = 0;
    if (!field(active, 0, list) || !field(active, game::kActiveCount, count) || count == 0) return 0;
    count = std::min(count, kMaxActive);
    if (!mem::copy(list, g_active, count * game::kActiveStride)) return 0;
    std::memcpy(hash, g_active + game::kActiveDistrictHash, 4);
    return count;
}

// The catalogue entry whose hash is `hash`: its id into `out`. False when none is.
bool read_name(const uint8_t *db, uint32_t hash, char *out, size_t cap) {
    const uint8_t *array = nullptr;
    uint32_t count = 0;
    if (!field(db, game::kDistrictArray, array) || !field(db, game::kDistrictCount, count) || count == 0) return false;
    count = std::min(count, kMaxDistricts);
    if (!mem::copy(array, g_catalogue, count * game::kDistrictStride)) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = g_catalogue + i * game::kDistrictStride;
        uint32_t h = 0;
        std::memcpy(&h, e + game::kDistrictHash, 4);
        if (h != hash) continue;
        const char *name = reinterpret_cast<const char *>(e + game::kDistrictName);
        size_t n = 0;
        while (n + 1 < cap && n < game::kDistrictHash - game::kDistrictName && name[n] >= 0x20 && name[n] < 0x7f) n++;
        std::memcpy(out, name, n);
        out[n] = 0;
        return n > 0;
    }
    return false;
}

// The runtime record whose hash is `hash`: its level and XP into `d`, the map's way. False when none is.
bool read_level(const uint8_t *runtime, uint32_t hash, District &d) {
    const uint8_t *array = nullptr, *costs = nullptr;
    uint32_t count = 0, cost_count = 0;
    if (!field(runtime, game::kRuntimeArray, array) || !field(runtime, game::kRuntimeCount, count) || count == 0 ||
        !field(runtime, game::kRuntimeCosts, costs) || !field(runtime, game::kRuntimeCostCount, cost_count))
        return false;
    count = std::min(count, kMaxDistricts);
    cost_count = std::min(cost_count, kMaxCosts);
    if (!mem::copy(array, g_records, count * game::kRuntimeStride) || !mem::copy(costs, g_costs, cost_count * 4))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = g_records + i * game::kRuntimeStride;
        uint32_t h = 0, max_level = 0, levels = 0, xp = 0;
        std::memcpy(&h, r + game::kRecordHash, 4);
        if (h != hash) continue;
        std::memcpy(&max_level, r + game::kRecordMaxLevel, 4);
        std::memcpy(&levels, r + game::kRecordLevels, 4);
        std::memcpy(&xp, r + game::kRecordXp, 4);
        if (levels > cost_count || max_level > 100) return false;
        uint32_t spent = 0;
        for (uint32_t k = 0; k < levels; k++) spent += g_costs[k];
        d.level = static_cast<int>(levels) + 1;
        d.max_level = static_cast<int>(max_level);
        d.xp = xp >= spent ? static_cast<int>(xp - spent) : 0;
        d.cost = levels + 1 < max_level && levels < cost_count ? static_cast<int>(g_costs[levels]) : 0;
        return true;
    }
    return false;
}

// One line when the district, its level or the active list's other hashes change: the section is still a lead.
void log_change(const District &d, uint32_t hash, uint32_t entries, LONG n) {
    uint32_t others[kMaxActive] = {};
    for (uint32_t i = 0; i < entries; i++)
        std::memcpy(&others[i], g_active + i * game::kActiveStride + game::kActiveOtherHash, 4);
    const bool same = d.has == g_logged.has && std::strcmp(d.id, g_logged.id) == 0 && d.level == g_logged.level &&
                      entries == g_logged_count && std::memcmp(others, g_logged_others, sizeof others) == 0;
    if (same) return;
    char list[160] = "";
    for (uint32_t i = 0, k = 0; i < entries && k + 12 < sizeof list; i++)
        k += wsprintfA(list + k, " %08lx", static_cast<unsigned long>(others[i]));
    if (d.has)
        log::write("sonar: district %s (%08lx) lvl %d of %d, %d/%d; active list %lu:%s (call %ld)", d.id,
                   static_cast<unsigned long>(hash), d.level, d.max_level, d.xp, d.cost,
                   static_cast<unsigned long>(entries), list, static_cast<long>(n));
    else
        log::write("sonar: no district (hash %08lx, active list %lu:%s) (call %ld)", static_cast<unsigned long>(hash),
                   static_cast<unsigned long>(entries), list, static_cast<long>(n));
    g_logged = d;
    std::memcpy(g_logged_others, others, sizeof others);
    g_logged_count = entries;
}

// Why a look stopped short, logged once per change: 0 none, 1 the tray, 2 its slots.
int g_short_said = -1;

void short_of(int why, const void *ctx, LONG n) {
    if (why == g_short_said) return;
    g_short_said = why;
    if (why)
        log::write("sonar: look stopped at the %s (ctx %p, call %ld)", why == 1 ? "tray" : "tray's slots", ctx,
                   static_cast<long>(n));
    else
        log::write("sonar: look reads again (call %ld)", static_cast<long>(n));
}

void look(void *ctx, LONG n) {
    const uint8_t *tray = nullptr;
    if (!field(static_cast<const uint8_t *>(ctx), game::kEnvTray, tray)) {
        short_of(1, ctx, n);
        return;
    }
    const uint8_t *db = nullptr, *runtime = nullptr, *active = nullptr;
    if (!field(tray, game::kSonarDistrictDatabase, db) || !field(tray, game::kSonarDistrictRuntime, runtime) ||
        !field(tray, game::kSonarActiveDistrict, active)) {
        short_of(2, ctx, n);
        return;
    }
    short_of(0, ctx, n);
    District d;
    uint32_t hash = 0;
    const uint32_t entries = read_active(active, &hash);
    if (entries && read_name(db, hash, d.id, sizeof d.id)) {
        d.has = true;
        read_level(runtime, hash, d);  // a name without a record is drawn without a level
    }
    log_change(d, hash, entries, n);
    AcquireSRWLockExclusive(&g_lock);
    g_district = d;
    ReleaseSRWLockExclusive(&g_lock);
}

void hk_sonar(void *a, void *b, void *c, void *d) {
    InterlockedIncrement(&g_inflight);
    LONG n = InterlockedIncrement(&g_calls);
    if (!lifecycle::stopping) {
        if (n == 1) {
            log::write("sonar: first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            runner::log_env_tray("sonar", c, game::kSonarSlots);
        }
        if (n % kEvery == 1) look(c, n);
        if (n % 1800 == 0) log::write("sonar: %ld calls", static_cast<long>(n));  // ~every minute: still called
    }
    g_original(a, b, c, d);
    InterlockedDecrement(&g_inflight);
}

}  // namespace

bool install(HMODULE exe, uintptr_t sonar_offset) {
    void *target = reinterpret_cast<uint8_t *>(exe) + sonar_offset;
    MH_STATUS ms = MH_CreateHook(target, reinterpret_cast<void *>(&hk_sonar), reinterpret_cast<void **>(&g_original));
    if (ms != MH_OK) {
        log::write("MH_CreateHook sonar exe+%lx: %d", static_cast<unsigned long>(sonar_offset), static_cast<int>(ms));
        return false;
    }
    ms = MH_EnableHook(target);
    log::write("hook on sonar exe+%lx: %s (trampoline %p)", static_cast<unsigned long>(sonar_offset),
               ms == MH_OK ? "enabled" : "FAILED", reinterpret_cast<void *>(g_original));
    return ms == MH_OK;
}

void district(District &out) {
    AcquireSRWLockShared(&g_lock);
    out = g_district;
    ReleaseSRWLockShared(&g_lock);
}

long inflight() { return g_inflight; }

}  // namespace crsrt::sonar
