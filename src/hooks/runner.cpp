// runner.cpp
#include "hooks/runner.h"

#include "lifecycle.h"
#include "log.h"
#include "player.h"

#include "MinHook.h"

#include <atomic>
#include <cstring>

namespace crsrt::runner {

namespace {

using RunnerFn = void (*)(void *, void *, void *, void *);

RunnerFn g_original;
std::atomic<Tick> g_tick{nullptr};
std::atomic<void *> g_world{nullptr};  // read by the damage probe from other threads
volatile LONG g_calls;
volatile LONG g_inflight;

// Runner -> render thread, one 64-bit word so the pair is never torn: max points in the high half, the fraction's
// float bits in the low half; 0 = not resolved.
volatile LONG64 g_health_word;

// The last health written to the log, so the log only gets a line when something changed.
const float *g_logged_row;
float g_logged_health;
int g_logged_max;

void log_health(const float *row, int64_t arch, uint64_t chunk, LONG n) {
    if (!row) {
        if (g_logged_row) {
            log::write("health: not resolved (call %ld)", static_cast<long>(n));
            g_logged_row = nullptr;
        }
        return;
    }
    float f = row[0];
    int max = reinterpret_cast<const int32_t *>(row)[1];
    if (row == g_logged_row && f == g_logged_health && max == g_logged_max) return;
    int whole = static_cast<int>(f), frac = static_cast<int>((f - static_cast<float>(whole)) * 1000);
    if (frac < 0) frac = -frac;
    log::write("health: arch %ld chunk %p row %p = %d.%03d of %d (call %ld)", static_cast<long>(arch),
               reinterpret_cast<void *>(chunk), row, whole, frac, max, static_cast<long>(n));
    g_logged_row = row;
    g_logged_health = f;
    g_logged_max = max;
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
        player::log_first_look(world);
    } else {
        if (void *was = g_world.load(); world != was) {
            log::write("runner: world %p -> %p (call %ld)", was, world, static_cast<long>(n));
            g_world = world;
        }
        int64_t arch = -1;
        uint64_t chunk = 0;
        const float *row = player::health_row(world, &arch, &chunk);
        LONG64 word = 0;
        if (row) {
            uint32_t bits;
            std::memcpy(&bits, row, 4);
            word = (static_cast<LONG64>(reinterpret_cast<const int32_t *>(row)[1]) << 32) | bits;
        }
        InterlockedExchange64(&g_health_word, word);
        if (n % 600 == 0) log_health(row, arch, chunk, n);  // ~every 20 s at the runner's 30 calls a second
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

bool health(float *frac, int *max, long *calls) {
    LONG64 v = InterlockedCompareExchange64(&g_health_word, 0, 0);
    *calls = g_calls;
    if (!v) return false;
    uint32_t bits = static_cast<uint32_t>(v);
    std::memcpy(frac, &bits, 4);
    *max = static_cast<int>(v >> 32);
    return true;
}

void *world() { return g_world.load(); }

void set_tick(Tick tick) { g_tick = tick; }

long inflight() { return g_inflight; }

}  // namespace crsrt::runner
