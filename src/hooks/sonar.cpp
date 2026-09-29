// sonar.cpp
#include "hooks/sonar.h"

#include "game.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"

#include "MinHook.h"

#include <cstdio>
#include <cstring>

namespace crsrt::sonar {

namespace {

using SonarFn = void (*)(void *, void *, void *, void *);

SonarFn g_original;
volatile LONG g_calls;
volatile LONG g_inflight;

// Read by the sonar's own thread only.
bool g_bad;                    // the catalogue did not look like crrank's: stop reading, pass calls through
uint8_t g_active[0x40];        // ActiveDistrict's first bytes as last logged
constexpr size_t kRuntimeCmp = 0x400;
uint8_t g_runtime[kRuntimeCmp];  // DistrictRuntime's first bytes as last compared
size_t g_runtime_len;
ULONGLONG g_rate_t0;

void log_hex(const char *what, const void *p, const uint8_t *bytes, size_t n);
void snap(const char *name, const void *p, size_t cap, LONG n);

// The id at +kDistrictId of a catalogue entry, a pointer to text or the text itself; "" when neither.
void id_of(const uint8_t *entry, char *out, size_t cap) {
    out[0] = 0;
    const uint8_t *field = entry + game::kDistrictId;
    if (!mem::readable(field, 8)) return;
    auto printable = [](const uint8_t *s, size_t n) {
        size_t i = 0;
        while (i < n && s[i] >= 0x20 && s[i] < 0x7f) i++;
        return i;
    };
    const auto *text = *reinterpret_cast<const uint8_t *const *>(field);
    if (mem::readable(text, 48)) {
        size_t n = printable(text, cap - 1);
        if (n >= 3) {
            std::memcpy(out, text, n);
            out[n] = 0;
            return;
        }
    }
    size_t n = printable(field, 8 < cap - 1 ? 8 : cap - 1);
    std::memcpy(out, field, n);
    out[n] = 0;
}

// The catalogue as crrank reads it: each entry's index, address and id, and false when its shape is not that.
bool log_catalogue(const uint8_t *db) {
    if (!mem::readable(db, game::kDistrictCount + 8)) {
        log::write("sonar: DistrictDatabase %p unreadable", db);
        return false;
    }
    const auto *array = *reinterpret_cast<const uint8_t *const *>(db + game::kDistrictArray);
    const uint32_t count = *reinterpret_cast<const uint32_t *>(db + game::kDistrictCount);  // +4: capacity?
    if (count == 0 || count > 256 || !mem::readable(array, game::kDistrictStride)) {
        log::write("sonar: DistrictDatabase %p: array %p count %lu, %lu bytes readable there - not crrank's shape", db,
                   array, static_cast<unsigned long>(count), static_cast<unsigned long>(mem::readable_run(array)));
        log_hex("DistrictDatabase+100", db + 0x100, db + 0x100, 0x40);
        return false;
    }
    log::write("sonar: DistrictDatabase %p: %lu entries at %p", db, static_cast<unsigned long>(count), array);
    snap("districts", array, count * game::kDistrictStride, 1);
    int named = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = array + i * game::kDistrictStride;
        char id[64] = "";
        if (mem::readable(entry, game::kDistrictStride)) id_of(entry, id, sizeof id);
        if (id[0]) named++;
        log::write("  district %lu at %p: %s", static_cast<unsigned long>(i), entry, id[0] ? id : "(no id)");
    }
    return named > 0;
}

// 16 bytes a group, "+off: hex", as many groups as fit in one log line.
void log_hex(const char *what, const void *p, const uint8_t *bytes, size_t n) {
    char line[900];
    for (size_t at = 0; at < n;) {
        int k = std::snprintf(line, sizeof line, "sonar: %s %p +%lx:", what, p, static_cast<unsigned long>(at));
        for (size_t g = 0; g < 3 && at < n; g++, at += 16) {
            k += std::snprintf(line + k, sizeof line - k, " |");
            for (size_t i = 0; i < 16 && at + i < n; i++)
                k += std::snprintf(line + k, sizeof line - k, i % 4 ? "%02x" : " %02x", bytes[at + i]);
        }
        log::write("%s", line);
    }
}

// Up to `cap` bytes at p (0x2000 at most) to <basename>.<name>-<call>.bin, for reading offline.
void snap(const char *name, const void *p, size_t cap, LONG n) {
    size_t len = mem::readable_run(p);
    if (len > cap) len = cap;
    static uint8_t buf[0x2000];
    if (len > sizeof buf) len = sizeof buf;
    if (!len || !mem::copy(p, buf, len)) {
        log::write("sonar: %s %p unreadable", name, p);
        return;
    }
    wchar_t suffix[64];
    wsprintfW(suffix, L".%hs-%ld.bin", name, static_cast<long>(n));
    const std::wstring path = log::path(suffix);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, buf, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
    log::write("sonar: %s %p: %lu bytes to %ls", name, p, static_cast<unsigned long>(written), path.c_str());
}

// The runtime's own bytes, and 0x400 at each heap pointer among its first 0x30: it is small and points elsewhere.
void snap_runtime(const uint8_t *runtime, LONG n) {
    snap("runtime", runtime, 0x30, n);
    if (!mem::readable(runtime, 0x30)) return;
    for (int i = 0; i < 6; i++) {
        const void *q = reinterpret_cast<const void *const *>(runtime)[i];
        if (mem::readable(q, 16) && !mem::is_code(q)) {
            char name[24];
            wsprintfA(name, "runtime%x", i * 8);
            snap(name, q, 0x400, n);
        }
    }
}

// Which dwords of the runtime's first kRuntimeCmp bytes moved since the last look, up to 24 in one line.
void log_runtime_moves(const uint8_t *runtime) {
    uint8_t now[kRuntimeCmp];
    size_t len = mem::readable_run(runtime);
    if (len > kRuntimeCmp) len = kRuntimeCmp;
    if (!len || !mem::copy(runtime, now, len)) return;
    if (len == g_runtime_len) {
        char line[900];
        int k = 0, moved = 0;
        for (size_t at = 0; at + 4 <= len; at += 4) {
            uint32_t was, is;
            std::memcpy(&was, g_runtime + at, 4);
            std::memcpy(&is, now + at, 4);
            if (was == is) continue;
            if (moved++ < 24)
                k += std::snprintf(line + k, sizeof line - k, " +%lx %u->%u", static_cast<unsigned long>(at),
                                   static_cast<unsigned>(was), static_cast<unsigned>(is));
        }
        if (moved) log::write("sonar: runtime moved, %d dwords:%s", moved, line);
    }
    std::memcpy(g_runtime, now, len);
    g_runtime_len = len;
}

void look(void *ctx, LONG n) {
    auto *at = static_cast<uint8_t *>(ctx) + game::kEnvTray;
    if (!mem::readable(at, 8)) return;
    auto *tray = *reinterpret_cast<uint8_t **>(at);
    if (!mem::readable(tray, game::kSonarSlots * 8)) return;
    const auto *db = *reinterpret_cast<const uint8_t **>(tray + game::kSonarDistrictDatabase);
    const auto *runtime = *reinterpret_cast<const uint8_t **>(tray + game::kSonarDistrictRuntime);
    const auto *active = *reinterpret_cast<const uint8_t **>(tray + game::kSonarActiveDistrict);
    if (n == 1) {
        runner::log_env_tray("sonar", ctx, game::kSonarSlots);
        if (!log_catalogue(db)) {
            g_bad = true;
            log::write("sonar: slot +%x is not the catalogue - the tray model is wrong here, reading stops",
                       static_cast<unsigned>(game::kSonarDistrictDatabase));
            return;
        }
    }
    uint8_t now[sizeof g_active];
    if (!mem::copy(active, now, sizeof now)) return;
    if (n == 1 || std::memcmp(now, g_active, sizeof now) != 0) {
        log_hex("ActiveDistrict", active, now, sizeof now);
        std::memcpy(g_active, now, sizeof now);
        snap_runtime(runtime, n);
        log_runtime_moves(runtime);
    } else if (n % 300 == 0) {
        log_runtime_moves(runtime);
    }
}

void hk_sonar(void *a, void *b, void *c, void *d) {
    InterlockedIncrement(&g_inflight);
    LONG n = InterlockedIncrement(&g_calls);
    if (!lifecycle::stopping) {
        if (n == 1) {
            g_rate_t0 = GetTickCount64();
            log::write("sonar: first call tid %lu, a=%p b=%p c=%p", GetCurrentThreadId(), a, b, c);
        }
        if (!g_bad) look(c, n);
        if (n % 1800 == 0) {
            const ULONGLONG t = GetTickCount64();
            log::write("sonar: %ld calls, the last 1800 in %lu ms", static_cast<long>(n),
                       static_cast<unsigned long>(t - g_rate_t0));
            g_rate_t0 = t;
        }
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

long inflight() { return g_inflight; }

}  // namespace crsrt::sonar
