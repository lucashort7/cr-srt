// spawn.cpp
#include "hooks/spawn.h"

#include "game.h"
#include "hooks/registry.h"
#include "log.h"
#include "memory.h"

#include <cstdint>
#include <cstring>

namespace crsrt::spawn {

namespace {

// Four register arguments and, in case either function takes more, eight stack slots forwarded as they came: the
// detour's own frame sits between the caller's stack and the original's. The return is read from rax; a float in
// xmm0 would be lost, and the Spawner mod reads the submit's return as an integer id.
using SpawnFn = uint64_t (*)(void *, void *, void *, void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t);

hooks::Hook g_submit("spawn_submit");
hooks::Hook g_bundle("spawn_bundle");
SRWLOCK g_lock = SRWLOCK_INIT;

// What one function's log keeps between calls. Both run often with nothing new (2026-10-01: the bundle 30 times a
// second with 0 records, the submit 3 a second with the same two keys), so a call is logged whole only when its
// shape is new, the shape being the caller's return address, the arguments the probe watches (by value, and by the
// first kArgBytes of what they point at) and, for the submit, the pair of keys in r8 and r9; the first kFirstCalls
// always. The rest are counted, and the count written every kQuietMs with the shapes seen.
constexpr uint32_t kArgBytes = 0x40;
constexpr long kFirstCalls = 4;
constexpr long kMaxFull = 300;
constexpr DWORD kQuietMs = 5000;
constexpr int kShapes = 48;

struct Shape {
    uintptr_t caller = 0;
    uint64_t key[2] = {};  // the submit's r8 and r9; 0 for the bundle
    long calls = 0;
};

struct Probe {
    const char *who;
    uint8_t watch;  // bit i: argument i+1's pointee takes part in the change test
    long calls = 0, full = 0, quiet = 0;
    DWORD last_quiet_tick = 0;
    uint8_t last[4][kArgBytes] = {};
    bool last_ok[4] = {};
    Shape shapes[kShapes];
    int shape_count = 0;
};
// The submit's rcx is an out struct on the caller's stack and r8/r9 are keys, so only rdx's pointee is watched; the
// bundle's r8 is a stack address that differs per thread, so it is left out.
Probe g_submit_probe{"submit", 0b0010}, g_bundle_probe{"bundle", 0b1011};

// `n` bytes at p as hex, 8 to a group, into out (cap bytes); "" when unreadable.
void hex_of(const void *p, uint32_t n, char *out, size_t cap) {
    out[0] = 0;
    if (!mem::readable(p, n) || cap < n * 3 + 1) return;
    const auto *b = static_cast<const uint8_t *>(p);
    int k = 0;
    for (uint32_t i = 0; i < n; i++) k += wsprintfA(out + k, "%02x%s", b[i], (i % 8 == 7) ? " " : "");
}

bool pointer_like(const void *p) { return reinterpret_cast<uintptr_t>(p) >= 0x10000 && mem::readable(p, 8); }

// One argument: its value, its owner and its first kArgBytes; then, for each 8-byte slot of those that points at
// readable memory, 0x20 bytes there, one level down.
void arg_lines(const char *who, long n, int slot, const void *p) {
    char own[96] = "-", hex[kArgBytes * 3 + 8] = "";
    if (pointer_like(p) && mem::readable(p, kArgBytes)) {
        mem::owner_of(p, own, sizeof own);
        hex_of(p, kArgBytes, hex, sizeof hex);
    }
    log::write("  %s %ld arg%d %p (%s) | %s", who, n, slot, p, own, hex);
    if (!hex[0]) return;
    const auto *b = static_cast<const uint8_t *>(p);
    for (uint32_t i = 0; i < kArgBytes / 8; i++) {
        const auto *q = *reinterpret_cast<const uint8_t *const *>(b + i * 8);
        if (!pointer_like(q) || !mem::readable(q, 0x20)) continue;
        char qown[96], qhex[0x20 * 3 + 8];
        mem::owner_of(q, qown, sizeof qown);
        hex_of(q, 0x20, qhex, sizeof qhex);
        log::write("    %s %ld arg%d[%lu] -> %p (%s) | %s", who, n, slot, static_cast<unsigned long>(i), q, qown, qhex);
    }
}

// True when a watched argument's pointee became or stopped being readable, or its first kArgBytes changed since the
// last call. Updates the probe's copy.
bool pointees_changed(Probe &pr, void *const args[4]) {
    bool diff = false;
    for (int i = 0; i < 4; i++) {
        if (!(pr.watch & (1 << i))) continue;
        uint8_t now[kArgBytes] = {};
        const bool ok = pointer_like(args[i]) && mem::copy(args[i], now, kArgBytes);
        if (ok != pr.last_ok[i] || (ok && std::memcmp(now, pr.last[i], kArgBytes) != 0)) diff = true;
        pr.last_ok[i] = ok;
        std::memcpy(pr.last[i], now, kArgBytes);
    }
    return diff;
}

// Counts the call under its shape; true when the shape is new (or the table is full, which counts as new once).
bool shape_is_new(Probe &pr, uintptr_t caller, uint64_t k0, uint64_t k1) {
    for (int i = 0; i < pr.shape_count; i++) {
        Shape &s = pr.shapes[i];
        if (s.caller == caller && s.key[0] == k0 && s.key[1] == k1) {
            s.calls++;
            return false;
        }
    }
    if (pr.shape_count < kShapes) pr.shapes[pr.shape_count++] = {caller, {k0, k1}, 1};
    return true;
}

void shapes_line(const Probe &pr) {
    char line[900];
    int k = 0;
    for (int i = 0; i < pr.shape_count && k < 700; i++) {
        const Shape &s = pr.shapes[i];
        char own[96];
        mem::owner_of(reinterpret_cast<const void *>(s.caller), own, sizeof own);
        k += wsprintfA(line + k, " | %ld from %s", s.calls, own);
        if (s.key[0] || s.key[1])
            k += wsprintfA(line + k, " %08lx%08lx/%08lx%08lx", static_cast<unsigned long>(s.key[0] >> 32),
                           static_cast<unsigned long>(s.key[0]), static_cast<unsigned long>(s.key[1] >> 32),
                           static_cast<unsigned long>(s.key[1]));
    }
    log::write("spawn: %s shapes so far%s", pr.who, line);
}

// The bundle's rdx: {records *, u32 count} of kSpawnRecordSize-byte records, which its first instructions walk. Up to
// kRecordsShown of them as hex, when there are any: 2026-10-01 the count read 0 on every frame between spawns.
constexpr uint32_t kRecordsShown = 8;
uint32_t record_count(const void *rdx) {
    if (!mem::readable(rdx, 12)) return 0;
    return *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(rdx) + 8);
}
void records(long n, const void *rdx) {
    const uint32_t count = record_count(rdx);
    if (!count) return;
    const auto *recs = *static_cast<const uint8_t *const *>(rdx);
    const uint32_t shown = count < kRecordsShown ? count : kRecordsShown;
    log::write("  bundle %ld records %p count %lu", n, recs, static_cast<unsigned long>(count));
    for (uint32_t i = 0; i < shown; i++) {
        const uint8_t *rec = recs + static_cast<size_t>(i) * game::kSpawnRecordSize;
        char hex[game::kSpawnRecordSize * 3 + 24] = "";
        hex_of(rec, game::kSpawnRecordSize, hex, sizeof hex);
        log::write("    rec %lu %p | %s", static_cast<unsigned long>(i), rec, hex[0] ? hex : "unreadable");
    }
}

// Under g_lock. `caller` is the return address into the game's code.
void note(Probe &pr, uintptr_t caller, void *a, void *b, void *c, void *d, uint64_t r) {
    void *args[4] = {a, b, c, d};
    const long n = ++pr.calls;
    const bool is_submit = &pr == &g_submit_probe;
    const bool fresh = shape_is_new(pr, caller, is_submit ? reinterpret_cast<uint64_t>(c) : 0,
                                    is_submit ? reinterpret_cast<uint64_t>(d) : 0);
    const bool diff = pointees_changed(pr, args);
    const bool loaded = !is_submit && record_count(b) > 0;
    const DWORD tick = GetTickCount();
    if ((n <= kFirstCalls || fresh || diff || loaded) && pr.full < kMaxFull) {
        pr.full++;
        if (pr.quiet) {
            log::write("spawn: %s %ld quiet calls", pr.who, pr.quiet);
            pr.quiet = 0;
        }
        char own[96];
        mem::owner_of(reinterpret_cast<const void *>(caller), own, sizeof own);
        log::write("spawn: %s %ld from %s rcx %p rdx %p r8 %p r9 %p -> %08lx%08lx%s%s%s", pr.who, n, own, a, b, c, d,
                   static_cast<unsigned long>(r >> 32), static_cast<unsigned long>(r & 0xffffffffu),
                   fresh ? " new shape" : "", diff ? " changed" : "", loaded ? " records" : "");
        for (int i = 0; i < 4; i++) arg_lines(pr.who, n, i + 1, args[i]);
        if (!is_submit) records(n, b);
        pr.last_quiet_tick = tick;
        return;
    }
    pr.quiet++;
    if (tick - pr.last_quiet_tick >= kQuietMs) {
        log::write("spawn: %s %ld quiet calls (%ld so far)", pr.who, pr.quiet, n);
        shapes_line(pr);
        pr.quiet = 0;
        pr.last_quiet_tick = tick;
    }
}

uint64_t hk_submit(void *a, void *b, void *c, void *d, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9,
                   uint64_t s10, uint64_t s11, uint64_t s12) {
    hooks::Call call(g_submit);
    const auto original = g_submit.original<SpawnFn>();
    if (call.stopping()) return original(a, b, c, d, s5, s6, s7, s8, s9, s10, s11, s12);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    call.before_original();
    const uint64_t r = original(a, b, c, d, s5, s6, s7, s8, s9, s10, s11, s12);
    AcquireSRWLockExclusive(&g_lock);
    note(g_submit_probe, caller, a, b, c, d, r);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

uint64_t hk_bundle(void *a, void *b, void *c, void *d, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9,
                   uint64_t s10, uint64_t s11, uint64_t s12) {
    hooks::Call call(g_bundle);
    const auto original = g_bundle.original<SpawnFn>();
    if (call.stopping()) return original(a, b, c, d, s5, s6, s7, s8, s9, s10, s11, s12);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    // the records are read before the original walks them, since it drains the queue: the first record, what its
    // +0x78 pointer holds (a heap object per record, unread until 2026-10-01), and the same record after the original
    char pre[game::kSpawnRecordSize * 3 + 24] = "", deep[0x80 * 3 + 8] = "", post[game::kSpawnRecordSize * 3 + 24] = "";
    char deep_own[96] = "-";
    const uint8_t *deep_at = nullptr;
    const uint32_t count = record_count(b);
    const uint8_t *rec0 = count ? *static_cast<const uint8_t *const *>(b) : nullptr;
    if (count) {
        hex_of(rec0, game::kSpawnRecordSize, pre, sizeof pre);
        if (mem::readable(rec0, 0x80)) {
            deep_at = *reinterpret_cast<const uint8_t *const *>(rec0 + 0x78);
            if (pointer_like(deep_at)) {
                mem::owner_of(deep_at, deep_own, sizeof deep_own);
                hex_of(deep_at, 0x80, deep, sizeof deep);
            }
        }
    }
    call.before_original();
    const uint64_t r = original(a, b, c, d, s5, s6, s7, s8, s9, s10, s11, s12);
    if (count) hex_of(rec0, game::kSpawnRecordSize, post, sizeof post);
    AcquireSRWLockExclusive(&g_lock);
    if (count) {
        log::write("spawn: bundle before the original: %lu records, rec 0 | %s", static_cast<unsigned long>(count),
                   pre);
        log::write("  rec 0 +0x78 -> %p (%s) | %s", deep_at, deep_own, deep[0] ? deep : "unreadable");
        log::write("  rec 0 after the original | %s", post[0] ? post : "unreadable");
    }
    note(g_bundle_probe, caller, a, b, c, d, r);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

bool prologue_is(const uint8_t *target, const uint8_t *want, size_t n, const char *who) {
    if (!mem::readable(target, n)) {
        log::write("spawn: %s at %p unreadable - not hooked", who, target);
        return false;
    }
    for (size_t i = 0; i < n; i++)
        if (target[i] != want[i]) {
            log::write("spawn: %s prologue differs at byte %d - not hooked (another build?)", who, static_cast<int>(i));
            return false;
        }
    return true;
}

}  // namespace

bool install(HMODULE exe) {
    auto *submit = reinterpret_cast<uint8_t *>(exe) + game::kSpawnSubmit;
    auto *bundle = reinterpret_cast<uint8_t *>(exe) + game::kSpawnBundle;
    bool any = false;
    if (prologue_is(submit, game::kSpawnSubmitPrologue, sizeof game::kSpawnSubmitPrologue, "submit"))
        any = g_submit.install(submit, reinterpret_cast<void *>(&hk_submit)) || any;
    if (prologue_is(bundle, game::kSpawnBundlePrologue, sizeof game::kSpawnBundlePrologue, "bundle"))
        any = g_bundle.install(bundle, reinterpret_cast<void *>(&hk_bundle)) || any;
    return any;
}

}  // namespace crsrt::spawn
