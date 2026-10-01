// registry.cpp
#include "hooks/registry.h"

#include "clock.h"
#include "envwatch.h"
#include "game.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"

#include "MinHook.h"

#include <cstdio>
#include <cstring>

namespace crsrt::hooks {

namespace {

constexpr int kMaxHooks = 64;
Hook *g_hooks[kMaxHooks];
// g_taken counts the slots handed out, g_count the slots filled: a reader (the runner's log_meters, while the boot
// still installs) sees a count only once its slot holds a hook. Hooks are installed by one thread at a time.
volatile LONG g_taken, g_count;

void add(Hook *hook) {
    const LONG i = InterlockedIncrement(&g_taken) - 1;
    if (i >= kMaxHooks) return;
    g_hooks[i] = hook;
    InterlockedIncrement(&g_count);  // a full barrier: the slot is written before the count says so
}

int hooks_now() { return g_count < kMaxHooks ? static_cast<int>(g_count) : kMaxHooks; }

}  // namespace

bool Hook::install(void *target, void *detour, const char *name) {
    if (name) name_ = name;
    char where[96] = "?";
    if (target) mem::owner_of(target, where, sizeof where);
    MH_STATUS ms = MH_CreateHook(target, detour, &original_);
    if (ms != MH_OK) {
        log::write("hook %s on %s: MH_CreateHook %d", name_, where, static_cast<int>(ms));
        original_ = nullptr;
        return false;
    }
    ms = MH_EnableHook(target);
    log::write("hook %s on %s: %s (trampoline %p)", name_, where, ms == MH_OK ? "enabled" : "FAILED", original_);
    if (ms != MH_OK) {
        MH_RemoveHook(target);
        original_ = nullptr;
        return false;
    }
    hooked_ = true;
    add(this);
    return true;
}

bool Hook::install(HMODULE exe, uintptr_t offset, void *detour, const char *name) {
    return install(reinterpret_cast<uint8_t *>(exe) + offset, detour, name);
}

Call::Call(Hook &hook) : hook_(&hook), from_us_(clock::now_us()) { InterlockedIncrement(&hook.inflight_); }

Call::~Call() {
    before_original();
    InterlockedDecrement(&hook_->inflight_);
}

// A member, though it reads only the global flag: a detour asks its Call, and a later Call may know more.
bool Call::stopping() const {  // NOLINT(readability-convert-member-functions-to-static)
    return lifecycle::stopping;
}

void Call::before_original() {
    if (timed_) return;
    timed_ = true;
    const int64_t us = clock::now_us() - from_us_;
    InterlockedIncrement64(&hook_->calls_);
    InterlockedAdd64(&hook_->sum_us_, us);
    LONG64 seen = hook_->max_us_;
    while (us > seen && InterlockedCompareExchange64(&hook_->max_us_, us, seen) != seen) seen = hook_->max_us_;
}

int count() { return hooks_now(); }

long inflight() {
    long n = 0;
    for (int i = 0; i < hooks_now(); i++) n += g_hooks[i]->inflight();
    return n;
}

void describe_inflight(char *out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < hooks_now() && at + 1 < cap; i++)
        if (const long n = g_hooks[i]->inflight(); n) {
            const int w = std::snprintf(out + at, cap - at, "%s%s %ld", at ? ", " : "", g_hooks[i]->name(), n);
            if (w < 0) break;
            at += static_cast<size_t>(w);
        }
}

bool disable_all() {
    MH_STATUS ms = MH_DisableHook(MH_ALL_HOOKS);
    for (int t = 0; ms != MH_OK && t < 10; t++) {
        Sleep(100);
        ms = MH_DisableHook(MH_ALL_HOOKS);
    }
    return ms == MH_OK;
}

bool release() {
    if (!count()) return true;
    if (!disable_all()) {
        log::write("unload: %d hooks, disable FAILED - staying mapped", count());
        return false;
    }
    log::write("unload: %d hooks disabled", count());
    int waited = 0;
    for (;;) {
        Sleep(200);
        waited += 200;
        if (inflight() == 0) break;
        if (waited == 2000) {
            char busy[256];
            describe_inflight(busy, sizeof busy);
            log::write("unload: calls still in flight after 2 s (%s), waiting", busy);
        }
    }
    log::write("unload: no call in flight after %d ms", waited);
    return true;
}

void log_meters() {
    for (int i = 0; i < hooks_now(); i++) {
        Hook &h = *g_hooks[i];
        const LONG64 calls = InterlockedExchange64(&h.calls_, 0), sum = InterlockedExchange64(&h.sum_us_, 0);
        const LONG64 max = InterlockedExchange64(&h.max_us_, 0);
        if (calls)
            log::write("hooks: %s %ld calls, own avg %ld us, max %ld us", h.name(), static_cast<long>(calls),
                       static_cast<long>(sum / calls), static_cast<long>(max));
    }
}

bool tray(const void *ctx, const uint8_t **out) {
    return ctx && mem::copy(static_cast<const uint8_t *>(ctx) + game::kEnvTray, out, sizeof *out) && *out;
}

bool env(const void *ctx, uintptr_t offset, const uint8_t **out) {
    const uint8_t *t = nullptr;
    return tray(ctx, &t) && mem::copy(t + offset, out, sizeof *out) && *out;
}

void log_tray(const char *who, const void *ctx, int slots) {
    const uint8_t *t = nullptr;
    if (!tray(ctx, &t)) {
        log::write("%s: env tray: ctx %p unreadable", who, ctx);
        return;
    }
    log::write("%s: env tray %p (ctx %p), %d slots", who, t, ctx, slots);
    envwatch::note_tray(t, slots);
    for (int i = 0; i < slots; i++) {
        const void *p = nullptr;
        uint8_t b[16];
        char own[160] = "-", body[200] = "unreadable";
        if (!mem::copy(t + static_cast<uintptr_t>(i) * 8, &p, sizeof p)) {
            log::write("  slot %d +%x: unreadable", i, i * 8);
            continue;
        }
        if (mem::copy(p, b, sizeof b)) {
            float f[4];
            std::memcpy(f, b, sizeof f);
            mem::owner_of(p, own, sizeof own);
            std::snprintf(body, sizeof body,
                          "%g %g %g %g | %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", f[0],
                          f[1], f[2], f[3], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                          b[12], b[13], b[14], b[15]);
        }
        log::write("  slot %d +%x: %p (%s) %s", i, i * 8, p, own, body);
    }
}

}  // namespace crsrt::hooks
