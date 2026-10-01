// damage.cpp
#include "hooks/damage.h"

#include "ecs.h"
#include "game.h"
#include "hooks/registry.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "source.h"

#include <cstdint>

namespace crsrt::damage {

namespace {

// Four arguments in rcx, rdx, r8 and r9, five more on the stack; the float it returns comes back in xmm0.
using TakeDamageFn = float (*)(float *row, const uint8_t *src, void *cfg, uint64_t ent, float amount, double now,
                               void *evt, void *bus, int64_t *tags);

hooks::Hook g_hook("take_damage");
SRWLOCK g_lock = SRWLOCK_INIT;  // hits can come from several game threads at once

// Hits logged one line each, then only new archetypes: a damage-over-time effect calls this every frame.
constexpr long kHitLines = 64;
long g_hits;

// Under g_lock: the slot table copied for the placing of a new row, and the columns of the archetype described.
ecs::World g_world;
ecs::Columns g_columns;

// Rows already placed, so the chunk walk runs once per row.
struct Memo {
    uint64_t row;
    ecs::RowPlace place;
    bool found;
};
Memo g_memo[64];
int g_memo_next;

// Archetypes whose component list is already in the log.
int64_t g_seen[64];
int g_seen_count;

// Entities already described, by the handle the caller passes.
uint64_t g_described[256];
int g_described_count;

bool first_meeting(uint64_t ent) {
    for (int i = 0; i < g_described_count; i++)
        if (g_described[i] == ent) return false;
    if (g_described_count < 256) g_described[g_described_count++] = ent;
    return true;
}

// Up to 0x60 bytes of a row as hex, and, when its first 8 bytes point at readable memory, the text found there.
void dump_row(const char *what, const uint8_t *row, uint32_t size) {
    if (!row) {
        log::write("  %s: not in this archetype", what);
        return;
    }
    uint32_t n = size < 0x60 ? size : 0x60;
    if (!n || !mem::readable(row, n)) {
        log::write("  %s: row %p (size %lu) unreadable", what, row, static_cast<unsigned long>(size));
        return;
    }
    char hex[0x60 * 3 + 1];
    int k = 0;
    for (uint32_t i = 0; i < n; i++) k += wsprintfA(hex + k, "%02x%s", row[i], (i % 8 == 7) ? " " : "");
    char text[64] = "";
    const auto *ptr = *reinterpret_cast<const char *const *>(row);
    if (n >= 8 && mem::readable(ptr, 48)) {
        int t = 0;
        for (int i = 0; i < 48 && ptr[i]; i++) text[t++] = (ptr[i] >= 32 && ptr[i] < 127) ? ptr[i] : '.';
        text[t] = 0;
    }
    log::write("  %s: row %p size %lx | %s%s%s%s", what, row, static_cast<unsigned long>(size), hex,
               text[0] ? "| *[0] -> \"" : "", text, text[0] ? "\"" : "");
}

// For each of a row's first four 8-byte slots that points at readable memory, 0x40 bytes there as hex and as text:
// one level down, where a component that only holds a pointer keeps its content.
void follow_row(const char *what, const uint8_t *row, uint32_t size) {
    if (!row || size < 8 || !mem::readable(row, size < 32 ? size : 32)) return;
    for (uint32_t slot = 0; slot < 4 && (slot + 1) * 8 <= size; slot++) {
        const auto *p = *reinterpret_cast<const uint8_t *const *>(row + slot * 8);
        if (reinterpret_cast<uintptr_t>(p) < 0x10000 || !mem::readable(p, 0x40)) continue;
        char own[96];
        mem::owner_of(p, own, sizeof own);
        char hex[0x40 * 3 + 8], text[0x41];
        int k = 0;
        for (int i = 0; i < 0x40; i++) {
            k += wsprintfA(hex + k, "%02x%s", p[i], (i % 8 == 7) ? " " : "");
            text[i] = (p[i] >= 32 && p[i] < 127) ? static_cast<char>(p[i]) : '.';
        }
        text[0x40] = 0;
        log::write("    %s [%lu] -> %p (%s) | %s| %s", what, static_cast<unsigned long>(slot), p, own, hex, text);
    }
}

void describe(const ecs::RowPlace &place, uint64_t ent, int max) {
    if (!g_columns.read(g_world, place.arch)) return;
    log::write("entity %p: arch %ld index %lu, max health %d", reinterpret_cast<void *>(ent),
               static_cast<long>(place.arch), static_cast<unsigned long>(place.index), max);
    static const struct {
        const char *name;
        uint32_t hash;
    } kLook[] = {
        {"DebugName", game::kDebugName},
        {"NPCLevel", game::kNpcLevel},
        {"FactionData", game::kFactionData},
        {"SpawnedNPC", game::kSpawnedNpc},
        {"HealthComponent", game::kHealthComponent},
    };
    for (const auto &t : kLook) {
        uint32_t size = 0;
        const auto *row = reinterpret_cast<const uint8_t *>(
            ecs::row_of(g_columns, place.chunk, place.capacity, place.index, t.hash, &size));
        dump_row(t.name, row, size);
        if (t.hash == game::kHealthComponent) follow_row(t.name, row, size);
    }
}

// "12.345" into out: log::write has no floats.
void fixed3(char *out, float f) {
    const char *sign = f < 0 ? "-" : "";
    if (f < 0) f = -f;
    int whole = static_cast<int>(f), frac = static_cast<int>((f - static_cast<float>(whole)) * 1000);
    wsprintfA(out, "%s%d.%03d", sign, whole, frac);
}

const Memo &place_of(const uint8_t *world, uint64_t row) {
    for (const Memo &m : g_memo)
        if (m.row == row) return m;
    Memo &m = g_memo[g_memo_next];
    g_memo_next = (g_memo_next + 1) % 64;
    m = {row, {}, false};
    m.found = world && g_world.load(mem::live(), reinterpret_cast<uint64_t>(world)) &&
              ecs::locate_row(g_world, row, game::kHealth, game::kHealthRowSize, m.place);
    return m;
}

bool first_sight(int64_t arch) {
    for (int i = 0; i < g_seen_count; i++)
        if (g_seen[i] == arch) return false;
    if (g_seen_count < 64) g_seen[g_seen_count++] = arch;
    return true;
}

void note(float *row, const uint8_t *src, uint64_t ent, float amount, float before, float after, int max,
          float returned) {
    const auto *world = static_cast<const uint8_t *>(runner::world());
    AcquireSRWLockExclusive(&g_lock);
    long n = ++g_hits;
    const Memo &m = place_of(world, reinterpret_cast<uint64_t>(row));
    bool new_arch = m.found && first_sight(m.place.arch);
    if (n <= kHitLines || new_arch) {
        char a[24], b[24], c[24], r[24], s[40] = "?";
        fixed3(a, amount);
        fixed3(b, before);
        fixed3(c, after);
        fixed3(r, returned);
        if (mem::readable(src, 6))
            wsprintfA(s, "%02x %02x %02x %02x %02x %02x", src[0], src[1], src[2], src[3], src[4], src[5]);
        if (m.found)
            log::write(
                "hit %ld: row %p ent %p src %s amount %s health %s -> %s of %d, returned %s | arch %ld chunk %p "
                "column +%lx capacity %lu index %lu",
                n, row, reinterpret_cast<void *>(ent), s, a, b, c, max, r, static_cast<long>(m.place.arch),
                reinterpret_cast<void *>(m.place.chunk), static_cast<unsigned long>(m.place.column),
                static_cast<unsigned long>(m.place.capacity), static_cast<unsigned long>(m.place.index));
        else
            log::write(
                "hit %ld: row %p ent %p src %s amount %s health %s -> %s of %d, returned %s | not placed "
                "(world %p)",
                n, row, reinterpret_cast<void *>(ent), s, a, b, c, max, r, world);
    }
    if (m.found && first_meeting(ent)) describe(m.place, ent, max);
    if (new_arch) {
        if (g_columns.read(g_world, m.place.arch))
            ecs::log_types(m.place.arch, g_columns);
        else
            log::write("arch %ld: tables unreadable", static_cast<long>(m.place.arch));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

float hk_take_damage(float *row, const uint8_t *src, void *cfg, uint64_t ent, float amount, double now, void *evt,
                     void *bus, int64_t *tags) {
    hooks::Call call(g_hook);
    const auto original = g_hook.original<TakeDamageFn>();
    if (call.stopping()) return original(row, src, cfg, ent, amount, now, evt, bus, tags);
    const bool readable = mem::readable(row, game::kHealthRowSize);
    const float before = readable ? row[0] : 0.0f;
    call.before_original();  // note() after the original is ours too, but its cost is a log line and not metered
    const float r = original(row, src, cfg, ent, amount, now, evt, bus, tags);
    if (readable) note(row, src, ent, amount, before, row[0], reinterpret_cast<const int32_t *>(row)[1], r);
    return r;
}

}  // namespace

bool install(HMODULE exe) {
    auto *target = reinterpret_cast<uint8_t *>(exe) + game::kTakeDamage;
    if (!mem::readable(target, sizeof game::kTakeDamagePrologue)) return false;
    for (size_t i = 0; i < sizeof game::kTakeDamagePrologue; i++)
        if (target[i] != game::kTakeDamagePrologue[i]) {
            log::write("damage: prologue differs at byte %d - not hooked (another build?)", static_cast<int>(i));
            return false;
        }
    return g_hook.install(target, reinterpret_cast<void *>(&hk_take_damage));
}

}  // namespace crsrt::damage
