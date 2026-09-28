// enemies.cpp
#include "enemies.h"

#include "game.h"
#include "log.h"
#include "memory.h"
#include "player.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace crsrt::enemies {

namespace {

constexpr uint32_t kMaxTypes = 4096;     // an archetype's column count, at most (ecs::archetype's bound)
constexpr uint32_t kMaxCapacity = 256;   // rows in one chunk of interest, at most; 10 is the most seen
constexpr int kMaxFound = 512;
constexpr long kLogEvery = 600;          // runner calls between two cost lines, ~20 s
constexpr long kListGap = 30;            // runner calls between two logs of the list, ~1 s
constexpr int kListLogRows = 8;
constexpr int64_t kBudgetUs = 1000;     // a walk's budget, a runner call

enum Kind : uint8_t { kUnread, kOther, kNpc, kAttached };

// What an archetype's columns say, kept while its entry points at the same hashes: an archetype's types never change,
// its offsets do (a chunk reallocated from 10 rows to 2 moved every column), so offsets are read on every walk.
struct Class {
    const void *hashes = nullptr;
    uint32_t count = 0;
    Kind kind = kUnread;
    int16_t health = -1, spawned = -1, faction = -1, level = -1, transform = -1, attachment = -1, combat = -1;
};

struct Shield {
    uint64_t owner;
    float fraction;
    int32_t max;
};

Class g_class[game::kChunkSlots];
const uint8_t *g_class_world;

// Runner thread only. Every copy of game memory outside the world object lands in one of these.
uint32_t g_types[kMaxTypes];
uint32_t g_offsets[kMaxTypes];
uint64_t g_handles[kMaxCapacity];
uint8_t g_health[kMaxCapacity * game::kHealthRowSize];
uint8_t g_spawned[kMaxCapacity * game::kSpawnedNpcRowSize];
uint8_t g_faction[kMaxCapacity * game::kFactionRowSize];
uint8_t g_level[kMaxCapacity * game::kNpcLevelRowSize];
uint8_t g_transform[kMaxCapacity * game::kWorldTransformRowSize];
uint8_t g_attachment[kMaxCapacity * game::kAttachmentRowSize];
Enemy g_found[kMaxFound];
Shield g_shields[kMaxFound];

// What one walk did. Classifications: a slot never seen, an entry now pointing at other hashes, a failed copy tried
// again. Failed copies: an archetype's hashes, its offsets, its chunk's rows.
struct Count {
    int fresh = 0, changed = 0, retried = 0, chunks = 0, unbounded = 0;
    int lost_types = 0, lost_offsets = 0, lost_rows = 0;
    int64_t player_us = 0, slots_us = 0;  // where the walk's time went: the player's position, the slot loop
    uint64_t cycles = 0;                  // the CPU cycles this thread spent in it: wall time minus time off the CPU

    void add(const Count &o) {
        fresh += o.fresh, changed += o.changed, retried += o.retried, chunks += o.chunks, unbounded += o.unbounded;
        lost_types += o.lost_types, lost_offsets += o.lost_offsets, lost_rows += o.lost_rows;
    }
} g_walk;

// Per log period: the sum of its walks, and the slowest one's own counts.
struct Period {
    long walks = 0, slow = 0;  // slow: over kBudgetUs
    int64_t sum_us = 0, max_us = 0;
    uint64_t sum_cycles = 0;
    Count sum, slowest;
} g_period;
int g_found_n, g_shields_n, g_total_logged = -1;
long g_list_logged_at = -kListGap;
bool g_player_row_said, g_first_said, g_unreadable_said;
uint8_t g_combat_logged[32];
constexpr int kKindsLogged = 256;
uint64_t g_kinds_seen[kKindsLogged];  // every kind logged once, on its first sighting
int g_kinds_seen_n;
long g_combat_logged_at = -kListGap;

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

SRWLOCK g_lock = SRWLOCK_INIT;
List g_list;

template <class T>
T at(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

const uint8_t *entry_of(const uint8_t *w, int64_t a) { return w + 8 + (a + game::kTypeEntryBias) * 32; }

// The class of archetype a, read again when its entry points at another hashes array. A failed copy stays unread and
// is tried again on the next walk.
const Class &class_of(const uint8_t *w, int64_t a) {
    const uint8_t *entry = entry_of(w, a);
    const void *hashes = at<const void *>(entry + 8);
    const uint32_t count = at<uint32_t>(entry + 0x1c);
    Class &c = g_class[a];
    if (c.kind != kUnread && c.hashes == hashes && c.count == count) return c;
    (c.kind != kUnread ? g_walk.changed : c.hashes ? g_walk.retried : g_walk.fresh)++;
    c = {};
    if (!hashes || !count || count > kMaxTypes) {
        c = {hashes, count, kOther};
        return c;
    }
    if (!mem::copy(hashes, g_types, count * 4)) {
        g_walk.lost_types++;
        c.hashes = hashes;  // still kUnread: tried again next walk, counted as a retry
        return c;
    }
    c.hashes = hashes;
    c.count = count;
    for (uint32_t i = 0; i < count; i++) {
        const auto k = static_cast<int16_t>(i);
        switch (g_types[i]) {
            case game::kHealth: c.health = k; break;
            case game::kSpawnedNpc: c.spawned = k; break;
            case game::kFactionData: c.faction = k; break;
            case game::kNpcLevel: c.level = k; break;
            case game::kWorldTransform: c.transform = k; break;
            case game::kAttachment: c.attachment = k; break;
            case game::kCombatStatus: c.combat = k; break;
            default: break;
        }
    }
    c.kind = c.health < 0 ? kOther : c.spawned >= 0 ? kNpc : c.attachment >= 0 ? kAttached : kOther;
    return c;
}

// The archetype's offsets into g_offsets and its chunk's capacity: the Health column's width over its row, exact since
// a 0x38-byte row outweighs the column's padding. 0 when the copy fails or nothing bounds the column.
uint32_t read_offsets(const uint8_t *entry, const Class &c) {
    if (!mem::copy(at<const void *>(entry + 0x10), g_offsets, c.count * 4)) {
        g_walk.lost_offsets++;
        return 0;
    }
    const uint32_t column = g_offsets[c.health];
    uint32_t next = UINT32_MAX;
    for (uint32_t i = 0; i < c.count; i++)
        if (g_offsets[i] > column && g_offsets[i] < next) next = g_offsets[i];
    if (next == UINT32_MAX) {
        g_walk.unbounded++;
        return 0;
    }
    const uint32_t capacity = (next - column) / game::kHealthRowSize;
    return capacity <= kMaxCapacity ? capacity : 0;
}

// Column `index` of the chunk, capacity rows of `size` bytes, into out; zeros when the archetype lacks it.
bool copy_column(uint64_t chunk, int16_t index, uint32_t capacity, uint32_t size, uint8_t *out) {
    if (index < 0) {
        std::memset(out, 0, static_cast<size_t>(capacity) * size);
        return true;
    }
    if (mem::copy(reinterpret_cast<const void *>(chunk + g_offsets[index]), out, static_cast<size_t>(capacity) * size))
        return true;
    g_walk.lost_rows++;
    return false;
}

bool alive(float fraction, int32_t max) { return std::isfinite(fraction) && fraction > 0.0f && max > 0; }

void read_npcs(uint64_t chunk, const Class &c, uint32_t capacity) {
    if (!copy_column(chunk, c.spawned, capacity, game::kSpawnedNpcRowSize, g_spawned) ||
        !copy_column(chunk, c.faction, capacity, game::kFactionRowSize, g_faction) ||
        !copy_column(chunk, c.level, capacity, game::kNpcLevelRowSize, g_level) ||
        !copy_column(chunk, c.transform, capacity, game::kWorldTransformRowSize, g_transform))
        return;
    for (uint32_t r = 0; r < capacity && g_found_n < kMaxFound; r++) {
        const uint8_t *health = g_health + r * game::kHealthRowSize;
        const auto fraction = at<float>(health);
        const auto max = at<int32_t>(health + 4);
        if (!g_handles[r] || !alive(fraction, max)) continue;
        Enemy &e = g_found[g_found_n++];
        e = {};
        e.id = static_cast<uint32_t>(g_handles[r]);
        e.generation = static_cast<uint32_t>(g_handles[r] >> 32);
        e.fraction = fraction;
        e.max = max;
        e.faction = at<uint32_t>(g_faction + r * game::kFactionRowSize);
        e.level = at<int32_t>(g_level + r * game::kNpcLevelRowSize);
        const uint8_t *spawned = g_spawned + r * game::kSpawnedNpcRowSize;
        std::memcpy(e.kind, spawned + game::kSpawnedKind, sizeof e.kind);
        e.byte3c = spawned[game::kSpawnedByte3c];
        e.source_id = at<uint32_t>(spawned + game::kSpawnedSource);
        e.source_generation = at<uint32_t>(spawned + game::kSpawnedSource + 4);
        std::memcpy(e.position, g_transform + r * game::kWorldTransformRowSize + game::kWorldPosition,
                    sizeof e.position);
    }
}

// Attachments with more than 1 max health, by owner. Every enemy also carries a 1/1-health attachment whose role is
// unknown; the max is what tells it from a shield, a heuristic until something names the difference.
void read_attachments(uint64_t chunk, const Class &c, uint32_t capacity) {
    if (!copy_column(chunk, c.attachment, capacity, game::kAttachmentRowSize, g_attachment)) return;
    for (uint32_t r = 0; r < capacity && g_shields_n < kMaxFound; r++) {
        const uint8_t *health = g_health + r * game::kHealthRowSize;
        const auto max = at<int32_t>(health + 4);
        if (!g_handles[r] || max <= 1) continue;
        g_shields[g_shields_n++] = {at<uint64_t>(g_attachment + r * game::kAttachmentRowSize), at<float>(health), max};
    }
}

// The player's CombatStatus row, up to 32 bytes, to the log when it changes, at most once a second: a probe of what
// flips when a fight starts and ends. g_offsets holds the player's archetype.
void log_combat(uint64_t chunk, const Class &c, long calls) {
    const uint32_t column = g_offsets[c.combat];
    uint32_t next = UINT32_MAX;
    for (uint32_t i = 0; i < c.count; i++)
        if (g_offsets[i] > column && g_offsets[i] < next) next = g_offsets[i];
    const uint32_t width = std::min<uint32_t>(next == UINT32_MAX ? 16 : next - column, sizeof g_combat_logged);
    uint8_t row[sizeof g_combat_logged] = {};
    if (calls - g_combat_logged_at < kListGap || !mem::copy(reinterpret_cast<const void *>(chunk + column), row, width) ||
        !std::memcmp(row, g_combat_logged, sizeof row))
        return;
    char hex[sizeof row * 3 + 1];
    int n = 0;
    for (uint32_t i = 0; i < width; i++) n += wsprintfA(hex + n, i % 4 == 3 ? "%02x " : "%02x", row[i]);
    log::write("combat status (call %ld): %s", calls, hex);
    std::memcpy(g_combat_logged, row, sizeof row);
    g_combat_logged_at = calls;
}

// The player's position: row 0 of the chunk get_player names, as player::health_row reads its Health.
bool player_position(void *world, int64_t slots, long calls, float out[3]) {
    int64_t arch = -1;
    uint64_t chunk = 0;
    if (!player::health_row(world, &arch, &chunk) || arch < 0 || arch >= slots) return false;
    const auto *w = static_cast<const uint8_t *>(world);
    const Class &c = class_of(w, arch);
    if (c.health < 0 || c.transform < 0) return false;
    const uint32_t capacity = read_offsets(entry_of(w, arch), c);
    if (capacity != 1 && !g_player_row_said) {
        log::write("enemies: the player's chunk holds %lu rows, read as row 0", static_cast<unsigned long>(capacity));
        g_player_row_said = true;
    }
    if (capacity && c.combat >= 0) log_combat(chunk, c, calls);
    uint8_t row[game::kWorldTransformRowSize];
    if (!capacity || !mem::copy(reinterpret_cast<const void *>(chunk + g_offsets[c.transform]), row, sizeof row))
        return false;
    std::memcpy(out, row + game::kWorldPosition, 12);
    return true;
}

// Slots [0, slots) and their entries, all inside the world object's readable region.
void walk(void *world, int64_t slots, long calls) {
    const auto *w = static_cast<const uint8_t *>(world);
    if (w != g_class_world) {
        for (Class &c : g_class) c = {};
        g_class_world = w;
    }
    g_found_n = g_shields_n = 0;
    float player[3];
    const int64_t t0 = now_us();
    const bool located = player_position(world, slots, calls, player);
    const int64_t t1 = now_us();
    g_walk.player_us = t1 - t0;

    const auto *slot = reinterpret_cast<const uint64_t *>(w + game::kChunkSlotBias * 8);
    for (int64_t a = 0; a < slots; a++) {
        const uint64_t chunk = slot[a];
        if (!chunk) continue;
        const Class &c = class_of(w, a);
        if (c.kind != kNpc && c.kind != kAttached) continue;
        g_walk.chunks++;
        const uint32_t capacity = read_offsets(entry_of(w, a), c);
        if (!capacity) continue;
        if (!mem::copy(reinterpret_cast<const void *>(chunk + game::kEntityColumn), g_handles, capacity * 8ull)) {
            g_walk.lost_rows++;
            continue;
        }
        if (!copy_column(chunk, c.health, capacity, game::kHealthRowSize, g_health)) continue;
        if (c.kind == kNpc)
            read_npcs(chunk, c, capacity);
        else
            read_attachments(chunk, c, capacity);
    }

    g_walk.slots_us = now_us() - t1;
    for (int i = 0; i < g_found_n; i++) {
        Enemy &e = g_found[i];
        const uint64_t handle = e.id | static_cast<uint64_t>(e.generation) << 32;
        for (int s = 0; s < g_shields_n; s++)
            if (g_shields[s].owner == handle && g_shields[s].max > e.shield_max) {
                e.shield_fraction = g_shields[s].fraction;
                e.shield_max = g_shields[s].max;
            }
        if (!located) {
            e.distance = -1.0f;
            continue;
        }
        const float dx = e.position[0] - player[0], dy = e.position[1] - player[1], dz = e.position[2] - player[2];
        e.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (located)
        std::sort(g_found, g_found + g_found_n, [](const Enemy &a, const Enemy &b) { return a.distance < b.distance; });
}

void publish(long calls) {
    AcquireSRWLockExclusive(&g_lock);
    g_list.calls = calls;
    g_list.total = g_found_n;
    g_list.count = std::min(g_found_n, kMaxRows);
    std::copy(g_found, g_found + g_list.count, g_list.rows);
    ReleaseSRWLockExclusive(&g_lock);
}

// "12.3": wsprintf has no floats.
void tenths(float v, char *out) {
    const long t = std::lround(v * 10.0f);
    wsprintfA(out, "%s%ld.%ld", t < 0 ? "-" : "", std::labs(t) / 10, std::labs(t) % 10);
}

// A kind never seen in this process, to the log once with what it was met as: the key a panel shows only by its
// first two bytes, whole, for the kinds' names.
void log_new_kinds(long calls) {
    for (int i = 0; i < g_found_n && g_kinds_seen_n < kKindsLogged; i++) {
        const Enemy &e = g_found[i];
        uint64_t kind;
        std::memcpy(&kind, e.kind, sizeof kind);
        bool seen = false;
        for (int k = 0; k < g_kinds_seen_n && !seen; k++) seen = g_kinds_seen[k] == kind;
        if (seen) continue;
        g_kinds_seen[g_kinds_seen_n++] = kind;
        char dist[24];
        tenths(e.distance, dist);
        log::write("enemies: new kind %02x%02x%02x%02x%02x%02x%02x%02x  %ld max  faction %08lx  lvl %ld  b3c %u  from "
                   "%lx/%lu  dist %s (call %ld)",
                   e.kind[0], e.kind[1], e.kind[2], e.kind[3], e.kind[4], e.kind[5], e.kind[6], e.kind[7],
                   static_cast<long>(e.max), static_cast<unsigned long>(e.faction), static_cast<long>(e.level),
                   e.byte3c, static_cast<unsigned long>(e.source_id), static_cast<unsigned long>(e.source_generation),
                   dist, calls);
    }
}

void log_list(long calls) {
    log::write("enemies: %d (call %ld), nearest first:", g_found_n, calls);
    for (int i = 0; i < g_found_n && i < kListLogRows; i++) {
        const Enemy &e = g_found[i];
        char dist[24], shield[40] = "";
        tenths(e.distance, dist);
        if (e.shield_max)
            wsprintfA(shield, "  shield %d/%ld", static_cast<int>(e.shield_fraction * e.shield_max + 0.5f),
                      static_cast<long>(e.shield_max));
        log::write("  %lx/%lu  %d/%ld  faction %08lx  lvl %ld  kind %02x%02x%02x%02x%02x%02x%02x%02x  b3c %u  "
                   "from %lx/%lu  dist %s%s",
                   static_cast<unsigned long>(e.id), static_cast<unsigned long>(e.generation),
                   static_cast<int>(e.fraction * e.max + 0.5f), static_cast<long>(e.max),
                   static_cast<unsigned long>(e.faction), static_cast<long>(e.level), e.kind[0], e.kind[1], e.kind[2],
                   e.kind[3], e.kind[4], e.kind[5], e.kind[6], e.kind[7], e.byte3c,
                   static_cast<unsigned long>(e.source_id), static_cast<unsigned long>(e.source_generation), dist,
                   shield);
    }
}

}  // namespace

void tick(void *world, long calls) {
    const int64_t t0 = now_us();
    // One check covers every slot and entry the walk reads: the slots whose entry ends inside the world's region.
    const size_t run = mem::readable_run(world);
    const int64_t entries = run > 8 ? static_cast<int64_t>((run - 8) / 32) - game::kTypeEntryBias : 0;
    const int64_t slots = std::min(game::kChunkSlots, entries);
    if (slots <= 0) {
        if (!g_unreadable_said)
            log::write("enemies: the world %p is readable for %lu bytes, short of any archetype entry", world,
                       static_cast<unsigned long>(run));
        g_unreadable_said = true;
        return;
    }
    g_walk = {};
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    walk(world, slots, calls);
    publish(calls);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    const int64_t us = now_us() - t0;
    g_walk.cycles = c1 - c0;

    if (!g_first_said) {
        log::write("enemies: first walk, %ld slots, %d archetypes classified in %ld us", static_cast<long>(slots),
                   g_walk.fresh, static_cast<long>(us));
        g_first_said = true;
    }
    g_period.walks++;
    g_period.sum_us += us;
    g_period.sum_cycles += g_walk.cycles;
    g_period.sum.add(g_walk);
    if (us > kBudgetUs) g_period.slow++;
    if (us > g_period.max_us) {
        g_period.max_us = us;
        g_period.slowest = g_walk;
    }
    log_new_kinds(calls);
    if (g_found_n != g_total_logged && calls - g_list_logged_at >= kListGap) {
        log_list(calls);
        g_total_logged = g_found_n;
        g_list_logged_at = calls;
    }
    if (calls % kLogEvery == 0) {
        const Count &t = g_period.sum, &m = g_period.slowest;
        log::write("enemies: walk avg %ld us, max %ld us, %ld over %ld us, of %ld calls; %d enemies, %d shields",
                   static_cast<long>(g_period.sum_us / g_period.walks), static_cast<long>(g_period.max_us),
                   g_period.slow, static_cast<long>(kBudgetUs), g_period.walks, g_found_n, g_shields_n);
        log::write("  classified %d new, %d changed, %d retried; %d chunks; copies failed: %d types, %d offsets, "
                   "%d rows; %d Health columns unbounded",
                   t.fresh, t.changed, t.retried, t.chunks, t.lost_types, t.lost_offsets, t.lost_rows, t.unbounded);
        // The slowest walk's cycles as microseconds at the period's rate: far under its wall time = off the CPU.
        const long on_cpu = g_period.sum_cycles
                                ? static_cast<long>(m.cycles * static_cast<uint64_t>(g_period.sum_us) / g_period.sum_cycles)
                                : -1;
        log::write("  the slowest walk: player %ld us, slots %ld us, on the CPU ~%ld us; classified %d new, %d changed, "
                   "%d retried; %d chunks; copies failed: %d types, %d offsets, %d rows",
                   static_cast<long>(m.player_us), static_cast<long>(m.slots_us), on_cpu, m.fresh, m.changed,
                   m.retried, m.chunks, m.lost_types, m.lost_offsets, m.lost_rows);
        g_period = {};
    }
}

void read(List &out) {
    AcquireSRWLockShared(&g_lock);
    out = g_list;
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace crsrt::enemies
