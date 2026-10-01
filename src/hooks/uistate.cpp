// uistate.cpp
#include "hooks/uistate.h"

#include "envwatch.h"
#include "game.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"

#include "MinHook.h"

#include <atomic>
#include <cstring>

namespace crsrt::uistate {

namespace {

using SystemFn = void (*)(void *, void *, void *, void *);

constexpr LONG kEvery = 15;  // hud_visibility calls between two reads of the bitsets, ~0.13 s at frame rate

SystemFn g_original_hud, g_original_pause, g_original_challenge;
volatile LONG g_calls_hud, g_calls_pause, g_calls_challenge;
volatile LONG g_inflight;

std::atomic<bool> g_paused{false};
int g_paused_logged = -1;  // -1: not yet
std::atomic<bool> g_challenge{false};
std::atomic<float> g_challenge_elapsed{0.0f};
std::atomic<uint32_t> g_challenge_machine{0};
int g_challenge_logged = -1;

// The three bitset qwords as the log last saw them.
uint64_t g_bits[3];

void hex64(uint64_t v, char *out) {
    wsprintfA(out, "%08lx%08lx", static_cast<unsigned long>(v >> 32), static_cast<unsigned long>(v & 0xffffffffULL));
}

void bits(void *ctx, LONG n) {
    const uint8_t *tray = nullptr, *stacks = nullptr;
    uint64_t now[3] = {};
    if (!mem::copy(static_cast<const uint8_t *>(ctx) + game::kEnvTray, &tray, sizeof tray) ||
        !mem::copy(tray + game::kHudVisFactDictionary, &stacks, sizeof stacks) || !stacks ||
        !mem::copy(stacks + game::kFactBits, now, sizeof now) || std::memcmp(now, g_bits, sizeof now) == 0)
        return;
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
    InterlockedIncrement(&g_inflight);
    const LONG n = InterlockedIncrement(&g_calls_challenge);
    if (!lifecycle::stopping) {
        if (n == 1) {
            log::write("uistate: challenge_mode::update_score first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            runner::log_env_tray("challenge", c, game::kChallengeSlots);
        }
        const uint8_t *tray = nullptr, *info = nullptr;
        uint32_t active = 0, machine = 0;
        float elapsed = 0.0f;
        if (mem::copy(static_cast<const uint8_t *>(c) + game::kEnvTray, &tray, sizeof tray) &&
            mem::copy(tray + game::kChallengeInfo, &info, sizeof info) && info &&
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
    g_original_challenge(a, b, c, d);
    InterlockedDecrement(&g_inflight);
}

void hk_hud(void *a, void *b, void *c, void *d) {
    InterlockedIncrement(&g_inflight);
    const LONG n = InterlockedIncrement(&g_calls_hud);
    if (!lifecycle::stopping) {
        if (n == 1) {
            log::write("uistate: hud_visibility first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            runner::log_env_tray("uistate", c, game::kHudVisSlots);
        }
        if (n % kEvery == 1) {
            bits(c, n);
            const uint8_t *tray = nullptr;
            if (mem::copy(static_cast<const uint8_t *>(c) + game::kEnvTray, &tray, sizeof tray))
                for (envwatch::Watch &w : g_hud_watch) envwatch::read(w, tray, game::kHudVisSlots, "uistate", n);
        }
    }
    g_original_hud(a, b, c, d);
    InterlockedDecrement(&g_inflight);
}

void hk_pause(void *a, void *b, void *c, void *d) {
    InterlockedIncrement(&g_inflight);
    const LONG n = InterlockedIncrement(&g_calls_pause);
    if (!lifecycle::stopping) {
        if (n == 1) {
            log::write("uistate: pause_state_detection first call tid %lu, ctx %p", GetCurrentThreadId(), c);
            runner::log_env_tray("pausedet", c, game::kPauseDetectSlots);
        }
        const uint8_t *tray = nullptr, *env = nullptr;
        uint8_t flag = 0;
        if (mem::copy(static_cast<const uint8_t *>(c) + game::kEnvTray, &tray, sizeof tray) &&
            mem::copy(tray + game::kPauseDetectAudioPause, &env, sizeof env) && env &&
            mem::copy(env + game::kAudioPauseFlag, &flag, 1)) {
            const bool paused = flag != 0;
            g_paused = paused;
            if (static_cast<int>(paused) != g_paused_logged) {
                log::write("uistate: %s (call %ld)", paused ? "paused, a screen is open" : "in the world",
                           static_cast<long>(n));
                g_paused_logged = paused;
            }
        }
    }
    g_original_pause(a, b, c, d);
    InterlockedDecrement(&g_inflight);
}

bool hook(const char *what, HMODULE exe, uintptr_t offset, void *detour, void **original) {
    void *target = reinterpret_cast<uint8_t *>(exe) + offset;
    MH_STATUS ms = MH_CreateHook(target, detour, original);
    if (ms != MH_OK) {
        log::write("MH_CreateHook %s exe+%lx: %d", what, static_cast<unsigned long>(offset), static_cast<int>(ms));
        return false;
    }
    ms = MH_EnableHook(target);
    log::write("hook on %s exe+%lx: %s", what, static_cast<unsigned long>(offset), ms == MH_OK ? "enabled" : "FAILED");
    return ms == MH_OK;
}

}  // namespace

bool install(HMODULE exe, uintptr_t hud_visibility_offset) {
    return hook("hud_visibility", exe, hud_visibility_offset, reinterpret_cast<void *>(&hk_hud),
                reinterpret_cast<void **>(&g_original_hud));
}

bool install_pause(HMODULE exe, uintptr_t pause_detection_offset) {
    return hook("pause_state_detection", exe, pause_detection_offset, reinterpret_cast<void *>(&hk_pause),
                reinterpret_cast<void **>(&g_original_pause));
}

bool install_challenge(HMODULE exe, uintptr_t update_score_offset) {
    return hook("challenge_mode::update_score", exe, update_score_offset, reinterpret_cast<void *>(&hk_challenge),
                reinterpret_cast<void **>(&g_original_challenge));
}

bool paused() { return g_paused.load(); }

bool challenge(float *elapsed, uint32_t *machine) {
    *elapsed = g_challenge_elapsed.load();
    *machine = g_challenge_machine.load();
    return g_challenge.load();
}

long inflight() { return g_inflight; }

}  // namespace crsrt::uistate
