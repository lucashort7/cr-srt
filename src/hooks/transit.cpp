// transit.cpp
#include "hooks/transit.h"

#include "envwatch.h"
#include "game.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "solve.h"

#include "MinHook.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace crsrt::transit {

namespace {

using SystemFn = void (*)(void *, void *, void *, void *);

constexpr ULONGLONG kSilenceMs = 1000;  // a gap this long between two calls is logged as a resume
constexpr ULONGLONG kDiffEveryMs = 100;
constexpr ULONGLONG kRateEveryMs = 20000;

volatile LONG g_inflight;
volatile LONG64 g_area;

// The level id off the transition system's GameState, every call: three reads.
void read_area(void *ctx) {
    const uint8_t *tray = nullptr, *env = nullptr, *state = nullptr;
    uint64_t id = 0;
    if (mem::copy(static_cast<const uint8_t *>(ctx) + game::kEnvTray, &tray, sizeof tray) && tray &&
        mem::copy(tray, &env, sizeof env) && env && mem::copy(env, &state, sizeof state) && state &&
        mem::copy(state + game::kGameStateArea, &id, sizeof id))
        InterlockedExchange64(&g_area, static_cast<LONG64>(id));
}

const uint8_t *g_request_text;  // the last string pointer logged, so each is logged once

// The string a TransitionRequest carries, logged when its pointer changes: a probe for the fast travel destination.
constexpr long kPendingCalls = 3000;  // ~30 s of transition calls before the string is given up
long g_pending_calls;
uint32_t g_pending_len;
const char *g_pending_chars;  // a request string not yet filled when it was seen

void read_pending_string() {
    if (!g_pending_chars || g_pending_len == 0 || g_pending_len > 256) return;
    char buf[257] = {};
    if (!mem::copy(g_pending_chars, buf, g_pending_len) || ++g_pending_calls > kPendingCalls) {
        log::write("transit: request string gave up after %ld calls", g_pending_calls);
        g_pending_chars = nullptr;
        return;
    }
    uint32_t printable = 0;
    for (uint32_t i = 0; i < g_pending_len; i++)
        if (buf[i] >= 0x20 && buf[i] <= 0x7e) printable++;
    // text, not the leftovers of whatever held the buffer before
    if (printable * 10 < g_pending_len * 9) return;
    char hex[2 * 256 + 1] = {};
    for (uint32_t i = 0; i < g_pending_len; i++)
        std::snprintf(hex + i * 2, 3, "%02x", static_cast<unsigned char>(buf[i]));
    for (char &c : buf)
        if (c < 0x20 || c > 0x7e) c = '.';
    buf[g_pending_len] = 0;
    log::write("transit: request string (%u, after %ld calls): \"%s\" %s", g_pending_len, g_pending_calls, buf, hex);
    g_pending_chars = nullptr;
}

void read_request(void *ctx, long call) {
    read_pending_string();
    const uint8_t *tray = nullptr, *env = nullptr, *req = nullptr, *text = nullptr;
    if (!(mem::copy(static_cast<const uint8_t *>(ctx) + game::kEnvTray, &tray, sizeof tray) && tray &&
          mem::copy(tray + 2 * 8, &env, sizeof env) && env && mem::copy(env, &req, sizeof req) && req &&
          mem::copy(req + game::kTransitionRequestText, &text, sizeof text)))
        return;
    if (text == g_request_text) return;
    g_request_text = text;
    if (!text) return;
    uint8_t raw[0x40] = {};
    if (!mem::copy(text, raw, sizeof raw)) {
        log::write("transit: request +38 -> %p unreadable (call %ld)", text, call);
        return;
    }
    char hex[sizeof raw * 2 + 1] = {};
    for (size_t i = 0; i < sizeof raw; i++) std::snprintf(hex + i * 2, 3, "%02x", raw[i]);
    log::write("transit: request +38 -> %p: %s (call %ld)", text, hex, call);
    // {u64, u32, u32, u32 flags, u32 len, const char *}: the destination's load-screen image path, filled a little
    // after the request appears (2026-10-01: empty at the diff, so it is read on later calls until a byte shows)
    std::memcpy(&g_pending_len, raw + 0x14, 4);
    std::memcpy(&g_pending_chars, raw + 0x18, 8);
    g_pending_calls = 0;
    uint8_t own[0x50] = {};
    if (mem::copy(req + 0x30, own, sizeof own)) {
        char h2[sizeof own * 2 + 1] = {};
        for (size_t i = 0; i < sizeof own; i++) std::snprintf(h2 + i * 2, 3, "%02x", own[i]);
        log::write("transit: request +30..+80: %s", h2);
    }
}

// Each env lives in one table only: TimelineCinematicsControl is uistate's (the hud_visibility tray), the
// r::Ptr<GameState> is followed here. Slot = parameter * 8.
envwatch::Watch g_transition[] = {
    {"GameState", 0 * 8, false, 64},          {"GameState*", 0 * 8, true, 256},
    {"PlayerTransition", 1 * 8, false, 256},  {"TransitionRequest", 2 * 8, false, 64},
    {"TransitionRequest*", 2 * 8, true, 256}, {"FadeToColorState", 7 * 8, false, 128},
};
envwatch::Watch g_jumppoint[] = {
    {"PendingJumpPoint", 0 * 8, false, 256},    {"JumpPointNotification", 1 * 8, false, 128},
    {"LuaEvents", 2 * 8, false, 128},           {"WorldKind", 4 * 8, false, 64},
    {"SharedStreamingState", 5 * 8, false, 32}, {"SharedStreamingState*", 5 * 8, true, 256},
};
envwatch::Watch g_loading[] = {
    {"ActiveActions", 1 * 8, false, 256},
    {"SharedBundleState*", 3 * 8, true, 128},
    {"LuaThrottleState", 4 * 8, false, 64},
};
envwatch::Watch g_consume[] = {
    {"StreamingPositions", 6 * 8, false, 256},
    // +0 is 1 for a load screen: the byte the notes call UIVisibility+0x10, 16 bytes past UIStateStacks (2026-09-30)
    {"VSyncControl", 8 * 8, false, 16},
    {"GameReadiness*", 7 * 8, true, 128},
    {"UIPendingEvents", 9 * 8, false, 128},
};
envwatch::Watch g_timeline[] = {
    {"TimelineState", 1 * 8, false, 256},
};
envwatch::Watch g_levelselect[] = {
    {"LevelSelectState", 0 * 8, false, 256},
};
envwatch::Watch g_gapentry[] = {
    {"UIGapState", 1 * 8, false, 128},
};

struct System {
    const char *key;   // the log's prefix
    const char *name;  // the getName prefix solve::system looks for
    envwatch::Watch *watches;
    int count;
    int slots;
    SystemFn original = nullptr;
    volatile LONG calls = 0;
    LONG rate_calls = 0;
    ULONGLONG last_ms = 0, next_diff_ms = 0, rate_ms = 0;
};

template <size_t N>
constexpr int count_of(const envwatch::Watch (&)[N]) {
    return static_cast<int>(N);
}

// A system is called by one thread at a time, so its own fields need no lock; the log is safe from any thread.
System g_systems[] = {
    {"transition", game::kTransitionName, g_transition, count_of(g_transition), game::kTransitionSlots},
    {"transfix", game::kTransitionFixedName, nullptr, 0, game::kTransitionFixedSlots},
    {"jumppoint", game::kJumpPointName, g_jumppoint, count_of(g_jumppoint), game::kJumpPointSlots},
    {"loading", game::kLoadingStateName, g_loading, count_of(g_loading), game::kLoadingStateSlots},
    {"consume", game::kConsumeMainWorldName, g_consume, count_of(g_consume), game::kConsumeMainWorldSlots},
    {"timeline", game::kTimelineHudName, g_timeline, count_of(g_timeline), game::kTimelineHudSlots},
    {"levelselect", game::kLevelSelectName, g_levelselect, count_of(g_levelselect), game::kLevelSelectSlots},
    {"gapentry", game::kGapEntryName, g_gapentry, count_of(g_gapentry), game::kGapEntrySlots},
    {"gapexit", game::kGapExitName, nullptr, 0, game::kGapExitSlots},
};
constexpr int kSystems = static_cast<int>(std::size(g_systems));

void on_call(System &s, void *ctx) {
    const LONG n = InterlockedIncrement(&s.calls);
    const ULONGLONG now = GetTickCount64();
    bool diff_now = false;
    if (n == 1) {
        log::write("transit: %s first call tid %lu, ctx %p", s.key, GetCurrentThreadId(), ctx);
        runner::log_env_tray(s.key, ctx, s.slots);
        s.rate_ms = now;
        diff_now = true;
    } else if (now - s.last_ms >= kSilenceMs) {
        log::write("transit: %s resumes after %lu ms (call %ld)", s.key, static_cast<unsigned long>(now - s.last_ms),
                   static_cast<long>(n));
        diff_now = true;
    }
    s.last_ms = now;
    if (s.count && (diff_now || now >= s.next_diff_ms)) {
        s.next_diff_ms = now + kDiffEveryMs;
        const uint8_t *tray = nullptr;
        if (mem::copy(static_cast<const uint8_t *>(ctx) + game::kEnvTray, &tray, sizeof tray) && tray)
            for (int i = 0; i < s.count; i++) envwatch::read(s.watches[i], tray, s.slots, s.key, n);
    }
    if (now - s.rate_ms >= kRateEveryMs) {
        log::write("transit: %s %ld calls in %lu ms (call %ld)", s.key, static_cast<long>(n - s.rate_calls),
                   static_cast<unsigned long>(now - s.rate_ms), static_cast<long>(n));
        s.rate_calls = n;
        s.rate_ms = now;
    }
}

template <int I>
void hk(void *a, void *b, void *c, void *d) {
    InterlockedIncrement(&g_inflight);
    System &s = g_systems[I];
    if (!lifecycle::stopping) {
        on_call(s, c);
        if (I == 0) {
            read_area(c);
            read_request(c, s.calls);
        }
    }
    s.original(a, b, c, d);
    InterlockedDecrement(&g_inflight);
}

constexpr SystemFn kDetours[] = {&hk<0>, &hk<1>, &hk<2>, &hk<3>, &hk<4>, &hk<5>, &hk<6>, &hk<7>, &hk<8>};
static_assert(std::size(kDetours) == std::size(g_systems), "one detour per system");

}  // namespace

int install(HMODULE exe) {
    int hooked = 0;
    for (int i = 0; i < kSystems && !lifecycle::stopping; i++) {
        System &s = g_systems[i];
        const uintptr_t offset = solve::system(exe, s.name, s.key);
        if (!offset) continue;
        void *target = reinterpret_cast<uint8_t *>(exe) + offset;
        MH_STATUS ms =
            MH_CreateHook(target, reinterpret_cast<void *>(kDetours[i]), reinterpret_cast<void **>(&s.original));
        if (ms != MH_OK) {
            log::write("MH_CreateHook %s exe+%lx: %d", s.key, static_cast<unsigned long>(offset), static_cast<int>(ms));
            continue;
        }
        ms = MH_EnableHook(target);
        log::write("hook on %s exe+%lx: %s", s.key, static_cast<unsigned long>(offset),
                   ms == MH_OK ? "enabled" : "FAILED");
        if (ms == MH_OK) hooked++;
    }
    log::write("transit: %d of %d systems hooked%s", hooked, kSystems,
               lifecycle::stopping ? " (stopped by the unloader)" : "");
    return hooked;
}

long inflight() { return g_inflight; }

uint64_t area() { return static_cast<uint64_t>(InterlockedCompareExchange64(&g_area, 0, 0)); }

}  // namespace crsrt::transit
