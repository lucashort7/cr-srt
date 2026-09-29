// runner.cpp
#include "hooks/runner.h"

#include "game.h"
#include "lifecycle.h"
#include "memory.h"
#include "log.h"
#include "motion.h"
#include "player.h"

#include "MinHook.h"

#include <atomic>
#include <cstdio>

namespace crsrt::runner {

namespace {

using RunnerFn = void (*)(void *, void *, void *, void *);

RunnerFn g_original;
std::atomic<Tick> g_tick{nullptr};
std::atomic<void *> g_world{nullptr};  // read by the damage probe from other threads
volatile LONG g_calls;
volatile LONG g_inflight;

// Runner -> render thread: the player as the last call read it.
SRWLOCK g_lock = SRWLOCK_INIT;
player::State g_player;

// The last reading written to the log, so the log only gets a line when something changed.
player::State g_logged;

// The hook's own time on the game's thread, per 600 calls: what the runner call costs the game now that the enemy
// walk is off it (AKTH-0103's QA asks for its max).
int64_t g_own_sum_us, g_own_max_us;
const int64_t g_qpc_hz = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();

int64_t now_us() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / g_qpc_hz;
}

bool same(const player::State &a, const player::State &b) {
    return a.has_health == b.has_health && a.has_position == b.has_position && a.has_speed == b.has_speed &&
           a.fraction == b.fraction && a.max == b.max && a.position[0] == b.position[0] &&
           a.position[1] == b.position[1] && a.position[2] == b.position[2] && a.speed == b.speed && a.velocity[0] == b.velocity[0] && a.velocity[1] == b.velocity[1] &&
           a.velocity[2] == b.velocity[2] && a.arch == b.arch &&
           a.health_at == b.health_at && a.position_at == b.position_at && a.speed_at == b.speed_at &&
           a.velocity_at == b.velocity_at;
}

// "529.1 -43.9 -104.15 at 000001A1B04492F0", "(hidden)" after a value the HUD refuses, "--" for a row not read.
void raw_text(const float *v, int n, bool shown, uint64_t address, char *out, size_t cap) {
    if (!address) {
        std::snprintf(out, cap, "--");
        return;
    }
    int k = 0;
    for (int i = 0; i < n; i++) k += std::snprintf(out + k, cap - k, i ? " %.6g" : "%.6g", v[i]);
    std::snprintf(out + k, cap - k, "%s at %p", shown ? "" : " (hidden)", reinterpret_cast<void *>(address));
}

// The raw readings the HUD draws and the address of each, for a check against Cheat Engine.
void log_player(const player::State &s, LONG n) {
    if (!s.has_health) {
        if (g_logged.has_health) log::write("player: not resolved (call %ld)", static_cast<long>(n));
        g_logged = s;
        return;
    }
    if (same(s, g_logged)) return;
    char health[64], position[128], speed[64], velocity[128], calc[48];
    raw_text(&s.fraction, 1, true, s.health_at, health, sizeof health);
    raw_text(s.position, 3, s.has_position, s.position_at, position, sizeof position);
    raw_text(&s.speed, 1, s.has_speed, s.speed_at, speed, sizeof speed);
    raw_text(s.velocity, 3, s.has_velocity, s.velocity_at, velocity, sizeof velocity);
    motion::text(calc, sizeof calc);
    log::write("player: arch %ld health %s of %ld, pos %s, speed %s, vel %s, %s (call %ld)", static_cast<long>(s.arch),
               health, static_cast<long>(s.max), position, speed, velocity, calc, static_cast<long>(n));
    g_logged = s;
}

void hk_runner(void *a, void *b, void *c, void *d) {
    InterlockedIncrement(&g_inflight);
    LONG n = InterlockedIncrement(&g_calls);
    if (lifecycle::stopping) {
        g_original(a, b, c, d);
        InterlockedDecrement(&g_inflight);
        return;
    }
    void *world = a ? *static_cast<void **>(a) : nullptr;  // every call: nothing assumes the world outlives a load
    if (n == 1) {
        g_world = world;
        log::write("runner: first call tid %lu, holder=%p world=%p b=%p c=%p", GetCurrentThreadId(), a, world, b, c);
        log_env_tray("runner", c, 5);  // slot 3 is FixedTime: a fixed step reads as a small float
        player::log_first_look(world);
    } else {
        const int64_t t0 = now_us();
        if (void *was = g_world.load(); world != was) {
            log::write("runner: world %p -> %p (call %ld)", was, world, static_cast<long>(n));
            g_world = world;
        }
        player::State s;
        player::read(world, s);
        motion::sample(s, n);
        AcquireSRWLockExclusive(&g_lock);
        g_player = s;
        ReleaseSRWLockExclusive(&g_lock);
        const int64_t us = now_us() - t0;
        g_own_sum_us += us;
        if (us > g_own_max_us) g_own_max_us = us;
        if (n % 600 == 0) {  // ~every 20 s at the runner's 30 calls a second
            log_player(s, n);
            log::write("runner: own time avg %ld us, max %ld us over 600 calls", static_cast<long>(g_own_sum_us / 600),
                       static_cast<long>(g_own_max_us));
            g_own_sum_us = g_own_max_us = 0;
        }
    }
    if (Tick tick = g_tick.load(); tick && world) tick(world, n);
    g_original(a, b, c, d);
    InterlockedDecrement(&g_inflight);
}

}  // namespace

bool install(HMODULE exe, uintptr_t runner_offset) {
    void *target = reinterpret_cast<uint8_t *>(exe) + runner_offset;
    MH_STATUS ms = MH_CreateHook(target, reinterpret_cast<void *>(&hk_runner), reinterpret_cast<void **>(&g_original));
    if (ms != MH_OK) {
        log::write("MH_CreateHook runner exe+%lx: %d", static_cast<unsigned long>(runner_offset), static_cast<int>(ms));
        return false;
    }
    ms = MH_EnableHook(target);
    log::write("hook on runner exe+%lx: %s (trampoline %p)", static_cast<unsigned long>(runner_offset),
               ms == MH_OK ? "enabled" : "FAILED", reinterpret_cast<void *>(g_original));
    return ms == MH_OK;
}

void player_state(player::State &out, long *calls) {
    *calls = g_calls;
    AcquireSRWLockShared(&g_lock);
    out = g_player;
    ReleaseSRWLockShared(&g_lock);
}

void *world() { return g_world.load(); }

void log_env_tray(const char *who, void *ctx, int slots) {
    auto *at = static_cast<uint8_t *>(ctx) + game::kEnvTray;
    if (!ctx || !mem::readable(at, 8)) {
        log::write("%s: env tray: ctx %p unreadable", who, ctx);
        return;
    }
    auto **tray = *reinterpret_cast<void ***>(at);
    if (!mem::readable(tray, slots * 8)) {
        log::write("%s: env tray %p (ctx %p) unreadable", who, tray, ctx);
        return;
    }
    log::write("%s: env tray %p (ctx %p), %d slots", who, tray, ctx, slots);
    for (int i = 0; i < slots; i++) {
        void *p = tray[i];
        char own[160] = "-", body[200] = "unreadable";
        if (mem::readable(p, 16)) {
            mem::owner_of(p, own, sizeof own);
            const auto *f = static_cast<const float *>(p);
            const auto *b = static_cast<const uint8_t *>(p);
            std::snprintf(body, sizeof body, "%g %g %g %g | %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                          f[0], f[1], f[2], f[3], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
                          b[11], b[12], b[13], b[14], b[15]);
        }
        log::write("  slot %d +%x: %p (%s) %s", i, i * 8, p, own, body);
    }
}

void set_tick(Tick tick) { g_tick = tick; }

long inflight() { return g_inflight; }

}  // namespace crsrt::runner
