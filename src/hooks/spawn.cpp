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

// The game's own column lookup, int(hashes, count, hash): the index of a type hash in an archetype's hash array, which
// every system calls per chunk to find a component's column. Solved at boot (solve::find_type), a leaf of 0x60 bytes
// that starts with three plain movs.
using FindTypeFn = int (*)(const void *hashes, uint32_t count, uint32_t hash);

hooks::Hook g_submit("spawn_submit");
hooks::Hook g_bundle("spawn_bundle");
hooks::Hook g_find("spawn_findtype");
SRWLOCK g_lock = SRWLOCK_INIT;

// The window: from a bundle call with records to process until the next bundle call, one frame, on every thread, each
// lookup of a hash kNamed knows is recorded by caller and hash, with the first archetype, thread and result seen: the
// systems that resolve a Health or NPCLevel column in the frame the entity is born. Inside the bundle itself only
// NPCLevel and one unnamed hash are looked up, both absent on the parent's archetype (build 6, 2026-10-01), so the
// Health writer runs after it. Nothing is logged inside the lookup, which runs 5 M times a second (107 M in 20 s, the
// frame rate unmoved); the table is written when the window closes. Which of the callers writes the row is what the
// next build narrows (AKTH-0119).
constexpr int kLookups = 256;
constexpr int kFrames = 4;

struct Lookup {
    uintptr_t caller = 0;
    const void *hashes = nullptr;
    uint32_t hash = 0;
    int result = 0;
    long calls = 0;
    DWORD thread = 0;
    void *frames[kFrames] = {};  // the callers above the caller, the first seen
};

volatile LONG g_window = 0;  // 1 while recording
uintptr_t g_exe = 0;
SRWLOCK g_lookup_lock = SRWLOCK_INIT;
Lookup g_lookups[kLookups];
int g_lookup_n = 0;
volatile LONG g_lookup_calls = 0;
long g_lookup_over = 0;

struct Named {
    uint32_t hash;
    const char *name;
};
constexpr Named kNamed[] = {
    {game::kHealth, "Health"},
    {game::kHealthLink, "HealthLink"},
    {game::kHealthFlags, "HealthFlags"},
    {game::kHealthComponent, "HealthComponent"},
    {game::kDead, "Dead"},
    {game::kRegeneration, "Regeneration"},
    {game::kNpcLevel, "NPCLevel"},
    {game::kFactionData, "FactionData"},
    {game::kSpawnedNpc, "SpawnedNPC"},
    {game::kAttachment, "Attachment"},
};

const char *name_of(uint32_t hash) {
    for (const auto &n : kNamed)
        if (n.hash == hash) return n.name;
    return "";
}

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

int hk_find(const void *hashes, uint32_t count, uint32_t hash) {
    hooks::Call call(g_find);
    const auto original = g_find.original<FindTypeFn>();
    const int r = original(hashes, count, hash);
    if (!g_window) return r;
    InterlockedIncrement(&g_lookup_calls);
    if (!name_of(hash)[0]) return r;
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const DWORD thread = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_lookup_lock);
    bool seen = false;
    for (int i = 0; i < g_lookup_n && !seen; i++) {
        Lookup &l = g_lookups[i];
        if (l.caller == caller && l.hash == hash) {
            l.calls++;
            seen = true;
        }
    }
    if (!seen && g_lookup_n == kLookups) {
        g_lookup_over++;
    } else if (!seen) {
        Lookup &l = g_lookups[g_lookup_n++];
        l = {caller, hashes, hash, r, 1, thread};
        // frame 0 is this detour, 1 the caller already held
        RtlCaptureStackBackTrace(2, kFrames, l.frames, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lookup_lock);
    return r;
}

// The table one window recorded, several lookups to a line: the caller as exe+offset, its thread, the hashes array (one
// per archetype, so equal pointers are the same archetype), the hash with its name, the index found (-1 when the
// archetype lacks the type) and the calls. `lookups` counts every hash, named or not.
void log_lookups(uintptr_t exe) {
    AcquireSRWLockExclusive(&g_lookup_lock);
    log::write("spawn: findtype in the frame of the bundle: %ld lookups, %d named distinct%s",
               static_cast<long>(g_lookup_calls), g_lookup_n, g_lookup_over ? " (table full, more dropped)" : "");
    char line[640];
    int n = 0, on_line = 0;
    for (int i = 0; i < g_lookup_n; i++) {
        const Lookup &l = g_lookups[i];
        n += wsprintfA(line + n, "exe+%lx t%lu %p %s->%d x%ld <", static_cast<unsigned long>(l.caller - exe),
                       static_cast<unsigned long>(l.thread), l.hashes, name_of(l.hash), l.result, l.calls);
        for (void *f : l.frames) {
            const auto at = reinterpret_cast<uintptr_t>(f);
            if (!at) break;
            if (at > exe && at - exe < 0x4000000)
                n += wsprintfA(line + n, " exe+%lx", static_cast<unsigned long>(at - exe));
            else
                n += wsprintfA(line + n, " %p", f);
        }
        n += wsprintfA(line + n, " | ");
        if (++on_line == 3 || i + 1 == g_lookup_n) {
            log::write("  %s", line);
            n = on_line = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_lookup_lock);
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
    // the lookup window a bundle with records opened closes at the next bundle call, one frame later, and its table is
    // logged then; a new one opens before the original so the bundle's own lookups are in it
    if (g_find.hooked() && InterlockedExchange(&g_window, 0)) {
        AcquireSRWLockExclusive(&g_lock);
        log_lookups(g_exe);
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (count && g_find.hooked()) {
        AcquireSRWLockExclusive(&g_lookup_lock);
        g_lookup_n = 0, g_lookup_over = 0;
        g_lookup_calls = 0;
        ReleaseSRWLockExclusive(&g_lookup_lock);
        InterlockedExchange(&g_window, 1);
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

// --- The fill of a fresh entity's rows (game.h, kSpawnApply), logged when its list holds Health: the caller, the
// range, Health's record in the type registry with its two callbacks, and the first row's source object, which is
// what the +0x38 callback reads the row from. The original runs untouched.
using ApplyFn = void (*)(uint32_t, const uint64_t *, uint64_t, void *);
hooks::Hook g_apply("spawn_apply");
// the thread inside an apply whose list holds SpawnedNPC, an enemy's: Health's copy logs only under it
volatile LONG g_enemy_thread = 0;
long g_apply_logged = 0;

template <class T>
bool at(uint64_t p, uintptr_t off, T &out) {
    return mem::copy(reinterpret_cast<const void *>(p + off), &out, sizeof out);
}

uintptr_t rel(uint64_t p) { return p > g_exe && p - g_exe < 0x4000000 ? p - g_exe : 0; }

// At most kPerSecond lines a second from the two logs below: an area load fills hundreds of rows in a burst.
constexpr long kPerSecond = 40;
volatile LONG g_second = 0, g_in_second = 0;

bool budget() {
    const auto now = static_cast<LONG>(GetTickCount() / 1000);
    if (g_second != now) {
        InterlockedExchange(&g_second, now);
        InterlockedExchange(&g_in_second, 0);
    }
    return InterlockedIncrement(&g_in_second) <= kPerSecond;
}

void hk_apply(uint32_t chunk, const uint64_t *list, uint64_t range, void *holder) {
    hooks::Call call(g_apply);
    const auto original = g_apply.original<ApplyFn>();
    if (call.stopping()) {
        original(chunk, list, range, holder);
        return;
    }
    const auto list_at = reinterpret_cast<uint64_t>(list);
    uint64_t count = 0, entries = 0;
    bool health = false, npc = false, level = false, faction = false;
    if (at(list_at, 0, count) && at(list_at, 8, entries) && count < 512)
        for (uint64_t i = 0; i < count; i++) {
            uint64_t e = 0;
            if (!at(entries, i * 8, e)) continue;
            const auto h = static_cast<uint32_t>(e);
            health = health || h == game::kHealth;
            npc = npc || h == game::kSpawnedNpc;
            level = level || h == game::kNpcLevel;
            faction = faction || h == game::kFactionData;
        }
    if (!health && !npc && !level) {
        original(chunk, list, range, holder);
        return;
    }
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto tid = static_cast<LONG>(GetCurrentThreadId());
    const auto row0 = static_cast<uint32_t>(range), rows = static_cast<uint32_t>(range >> 32);
    call.before_original();
    // the copy logs under the mark, and its lines come before this one on the same thread
    if (health) InterlockedExchange(&g_enemy_thread, tid);
    original(chunk, list, range, holder);
    if (health) InterlockedExchange(&g_enemy_thread, 0);
    if (!budget()) return;
    AcquireSRWLockExclusive(&g_lock);
    g_apply_logged++;
    log::write("spawn: apply t%lu from exe+%lx holder %p chunk %lu rows %lu+%lu, %lu types:%s%s%s%s",
               static_cast<unsigned long>(tid), static_cast<unsigned long>(rel(caller)), holder,
               static_cast<unsigned long>(chunk), static_cast<unsigned long>(row0), static_cast<unsigned long>(rows),
               static_cast<unsigned long>(count), health ? " Health" : "", npc ? " SpawnedNPC" : "",
               level ? " NPCLevel" : "", faction ? " FactionData" : "");
    ReleaseSRWLockExclusive(&g_lock);
}

// --- Health's own callbacks (game.h, kHealthConstruct and kHealthFromSource): every fresh Health row goes through
// one of them. The copy is logged with its source's 64 bytes, the first four as float, int, float, int, and the
// caller; the zeroing only counted. Rows whose source max is 1 or less (the 1/1 attachments, props) are counted, not
// logged, so the kCopyMax logged are the ones with a health worth reading.
using ConstructFn = void (*)(void *, uint32_t);
using FromSourceFn = void (*)(const void *, void *, uint32_t);
hooks::Hook g_construct("health_construct");
hooks::Hook g_copy("health_copy");
volatile LONG g_construct_calls = 0, g_copy_calls = 0, g_copy_small = 0;
long g_copy_logged = 0;

void hk_construct(void *column, uint32_t row) {
    hooks::Call call(g_construct);
    InterlockedIncrement(&g_construct_calls);
    g_construct.original<ConstructFn>()(column, row);
}

void hk_copy(const void *source, void *column, uint32_t row) {
    hooks::Call call(g_copy);
    const auto original = g_copy.original<FromSourceFn>();
    if (call.stopping()) {
        original(source, column, row);
        return;
    }
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    uint8_t src[game::kHealthSourceSize] = {};
    const bool read = mem::copy(source, src, sizeof src);
    int32_t max = 0;
    float fraction = 0;
    int32_t i2 = 0, i3 = 0;
    std::memcpy(&fraction, src, 4), std::memcpy(&max, src + 4, 4), std::memcpy(&i2, src + 8, 4),
        std::memcpy(&i3, src + 12, 4);
    InterlockedIncrement(&g_copy_calls);
    call.before_original();
    original(source, column, row);
    if (!read || max <= 1) InterlockedIncrement(&g_copy_small);
    const auto tid = static_cast<LONG>(GetCurrentThreadId());
    if (!read || max <= 1 || g_enemy_thread != tid || !budget()) return;
    char hex[game::kHealthSourceSize * 3 + 8] = "", own[96] = "-";
    hex_of(src, sizeof src, hex, sizeof hex);
    mem::owner_of(source, own, sizeof own);
    AcquireSRWLockExclusive(&g_lock);
    g_copy_logged++;
    log::write(
        "health: copy %ld t%lu from exe+%lx: source %p (%s) -> %p row %lu | fraction %d/1000 max %ld +8 %ld +c "
        "%ld | %s",
        g_copy_logged, static_cast<unsigned long>(tid), static_cast<unsigned long>(rel(caller)), source, own, column,
        static_cast<unsigned long>(row), static_cast<int>(fraction * 1000), static_cast<long>(max),
        static_cast<long>(i2), static_cast<long>(i3), hex);
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

bool install(HMODULE exe, uintptr_t find_type) {
    g_exe = reinterpret_cast<uintptr_t>(exe);
    auto *submit = reinterpret_cast<uint8_t *>(exe) + game::kSpawnSubmit;
    auto *bundle = reinterpret_cast<uint8_t *>(exe) + game::kSpawnBundle;
    bool any = false;
    if (prologue_is(submit, game::kSpawnSubmitPrologue, sizeof game::kSpawnSubmitPrologue, "submit"))
        any = g_submit.install(submit, reinterpret_cast<void *>(&hk_submit)) || any;
    if (prologue_is(bundle, game::kSpawnBundlePrologue, sizeof game::kSpawnBundlePrologue, "bundle"))
        any = g_bundle.install(bundle, reinterpret_cast<void *>(&hk_bundle)) || any;
    auto *apply = reinterpret_cast<uint8_t *>(exe) + game::kSpawnApply;
    if (prologue_is(apply, game::kSpawnApplyPrologue, sizeof game::kSpawnApplyPrologue, "apply"))
        any = g_apply.install(apply, reinterpret_cast<void *>(&hk_apply)) || any;
    auto *construct = reinterpret_cast<uint8_t *>(exe) + game::kHealthConstruct;
    if (prologue_is(construct, game::kHealthConstructPrologue, sizeof game::kHealthConstructPrologue, "construct"))
        any = g_construct.install(construct, reinterpret_cast<void *>(&hk_construct)) || any;
    auto *copy = reinterpret_cast<uint8_t *>(exe) + game::kHealthFromSource;
    if (prologue_is(copy, game::kHealthFromSourcePrologue, sizeof game::kHealthFromSourcePrologue, "copy"))
        any = g_copy.install(copy, reinterpret_cast<void *>(&hk_copy)) || any;
    // the lookup only with the bundle, whose window is the only thing it records in
    if (find_type && g_bundle.hooked())
        any = g_find.install(exe, find_type, reinterpret_cast<void *>(&hk_find)) || any;
    else
        log::write("spawn: findtype %s - lookups not recorded", find_type ? "without the bundle" : "unsolved");
    return any;
}

}  // namespace crsrt::spawn
