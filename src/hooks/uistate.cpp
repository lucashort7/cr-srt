// uistate.cpp
#include "hooks/uistate.h"

#include "envwatch.h"
#include "facts.h"
#include "game.h"
#include "hooks/registry.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"

#include <atomic>
#include <cstring>

namespace crsrt::uistate {

namespace {

using SystemFn = void (*)(void *, void *, void *, void *);

constexpr LONG kEvery = 15;  // hud_visibility calls between two reads of the bitsets, ~0.13 s at frame rate

hooks::Hook g_hud("hud_visibility"), g_pause("pause_state_detection"), g_challenge_hook("challenge_mode::update_score");
volatile LONG g_calls_hud, g_calls_pause, g_calls_challenge;

std::atomic<bool> g_paused{false};
int g_paused_logged = -1;  // -1: not yet
std::atomic<bool> g_challenge{false};
std::atomic<float> g_challenge_elapsed{0.0f};
std::atomic<uint32_t> g_challenge_machine{0};
int g_challenge_logged = -1;
std::atomic<bool> g_has_currency{false};
std::atomic<double> g_source{0.0}, g_talent{0.0};
int g_source_logged = -1, g_talent_logged = -1;

// Source and Talent Points, two doubles of the FactDictionary (facts.h), read every kEvery calls and logged when a
// whole number moves: a pickup, a purchase, a point earned.
void currencies(void *ctx, LONG n) {
    const uint8_t *dict = nullptr;
    double source = 0.0, talent = 0.0;
    if (!hooks::env(ctx, game::kHudVisFactDictionary, &dict) ||
        !facts::read_double(dict, game::kFactCurrencyCash, &source) ||
        !facts::read_double(dict, game::kFactCurrencyTalentPoints, &talent)) {
        g_has_currency = false;
        return;
    }
    g_source = source;
    g_talent = talent;
    g_has_currency = true;
    const int s = static_cast<int>(source), t = static_cast<int>(talent);
    if (s != g_source_logged || t != g_talent_logged) {
        log::write("uistate: source %d, talent points %d (call %ld)", s, t, static_cast<long>(n));
        g_source_logged = s;
        g_talent_logged = t;
    }
}

// The three bitset qwords as the log last saw them.
uint64_t g_bits[3];
const uint8_t *g_dict = nullptr;

void hex64(uint64_t v, char *out) {
    wsprintfA(out, "%08lx%08lx", static_cast<unsigned long>(v >> 32), static_cast<unsigned long>(v & 0xffffffffULL));
}

void bits(void *ctx, LONG n) {
    const uint8_t *stacks = nullptr;
    uint64_t now[3] = {};
    if (!hooks::env(ctx, game::kHudVisFactDictionary, &stacks) ||
        !mem::copy(stacks + game::kFactBits, now, sizeof now) || std::memcmp(now, g_bits, sizeof now) == 0)
        return;
    // The dictionary's address once per world: the outside probes (tools/_facts.py) take it as their argument.
    if (stacks != g_dict) {
        log::write("uistate: FactDictionary at %p (call %ld)", static_cast<const void *>(stacks), static_cast<long>(n));
        g_dict = stacks;
    }
    char text[3][24];
    for (int i = 0; i < 3; i++) hex64(now[i], text[i]);
    log::write("uistate: FactDictionary bits +128 %s +130 %s +138 %s (call %ld)", text[0], text[1], text[2],
               static_cast<long>(n));
    std::memcpy(g_bits, now, sizeof now);
}

// The hud_visibility envs watched for the world kind (AKTH-0110), each capped at its nearest known neighbour
// (envwatch.h). The load-screen byte the notes call "UIVisibility+0x10" is VSyncControl+0, the env 16 bytes past
// UIStateStacks, watched by the transit probe; so is the r::Ptr<GameState> in slot 5, followed.
envwatch::Watch g_hud_watch[] = {
    {"UIVisibility", 1 * 8, false, 0xb8},
    {"UIStateStacks", 2 * 8, false, 0xb8},
    {"TimelineCinematicsControl", 4 * 8, false, 64},
    {"JRPGState", game::kHudVisJRPGState, false, 0x70},
};
envwatch::Watch g_challenge_watch[] = {
    {"ChallengeModeInfo", 2 * 8, false, 512},
    {"ChallengeModeHudState", 3 * 8, false, 512},
};

void hk_challenge(void *a, void *b, void *c, void *d) {
    hooks::Call call(g_challenge_hook);
    const LONG n = InterlockedIncrement(&g_calls_challenge);
    if (!call.stopping()) {
        if (n == 1) {
            log::write("uistate: challenge_mode::update_score first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            hooks::log_tray("challenge", c, game::kChallengeSlots);
        }
        const uint8_t *tray = nullptr, *info = nullptr;
        uint32_t active = 0, machine = 0;
        float elapsed = 0.0f;
        if (hooks::tray(c, &tray) && hooks::env(c, game::kChallengeInfo, &info) &&
            mem::copy(info + game::kChallengeActive, &active, sizeof active) &&
            mem::copy(info + game::kChallengeMachine, &machine, sizeof machine) &&
            mem::copy(info + game::kChallengeElapsed, &elapsed, sizeof elapsed)) {
            g_challenge = active == 1;
            g_challenge_elapsed = elapsed;
            g_challenge_machine = machine;
            if (static_cast<int>(active == 1) != g_challenge_logged) {
                log::write("uistate: %s (call %ld)", active == 1 ? "a challenge runs" : "no challenge",
                           static_cast<long>(n));
                g_challenge_logged = active == 1;
            }
            if (n % kEvery == 1)
                for (envwatch::Watch &w : g_challenge_watch)
                    envwatch::read(w, tray, game::kChallengeSlots, "uistate", n);
        }
        if (n % 1800 == 0) log::write("uistate: challenge %ld calls", static_cast<long>(n));
    }
    call.before_original();
    g_challenge_hook.original<SystemFn>()(a, b, c, d);
}

void hk_hud(void *a, void *b, void *c, void *d) {
    hooks::Call call(g_hud);
    const LONG n = InterlockedIncrement(&g_calls_hud);
    if (!call.stopping()) {
        if (n == 1) {
            log::write("uistate: hud_visibility first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            hooks::log_tray("uistate", c, game::kHudVisSlots);
        }
        if (n % kEvery == 1) {
            bits(c, n);
            currencies(c, n);
            const uint8_t *tray = nullptr;
            if (hooks::tray(c, &tray))
                for (envwatch::Watch &w : g_hud_watch) envwatch::read(w, tray, game::kHudVisSlots, "uistate", n);
        }
    }
    call.before_original();
    g_hud.original<SystemFn>()(a, b, c, d);
}

void hk_pause(void *a, void *b, void *c, void *d) {
    hooks::Call call(g_pause);
    const LONG n = InterlockedIncrement(&g_calls_pause);
    if (!call.stopping()) {
        if (n == 1) {
            log::write("uistate: pause_state_detection first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            hooks::log_tray("pausedet", c, game::kPauseDetectSlots);
        }
        const uint8_t *env = nullptr;
        uint8_t flag = 0;
        if (hooks::env(c, game::kPauseDetectAudioPause, &env) && mem::copy(env + game::kAudioPauseFlag, &flag, 1)) {
            const bool paused = flag != 0;
            g_paused = paused;
            if (static_cast<int>(paused) != g_paused_logged) {
                log::write("uistate: %s (call %ld)", paused ? "paused, a screen is open" : "in the world",
                           static_cast<long>(n));
                g_paused_logged = paused;
            }
        }
    }
    call.before_original();
    g_pause.original<SystemFn>()(a, b, c, d);
}

}  // namespace

bool install(HMODULE exe, uintptr_t hud_visibility_offset) {
    return g_hud.install(exe, hud_visibility_offset, reinterpret_cast<void *>(&hk_hud));
}

bool install_pause(HMODULE exe, uintptr_t pause_detection_offset) {
    return g_pause.install(exe, pause_detection_offset, reinterpret_cast<void *>(&hk_pause));
}

bool install_challenge(HMODULE exe, uintptr_t update_score_offset) {
    return g_challenge_hook.install(exe, update_score_offset, reinterpret_cast<void *>(&hk_challenge));
}

bool paused() { return g_paused.load(); }

bool currency(double *source, double *talent_points) {
    if (!g_has_currency.load()) return false;
    *source = g_source.load();
    *talent_points = g_talent.load();
    return true;
}

bool challenge(float *elapsed, uint32_t *machine) {
    *elapsed = g_challenge_elapsed.load();
    *machine = g_challenge_machine.load();
    return g_challenge.load();
}

}  // namespace crsrt::uistate
