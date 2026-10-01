// enemies.cpp
#include "enemies.h"

#include "ecs.h"
#include "game.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "names.h"
#include "player.h"
#include "source.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace crsrt::enemies {

namespace {

constexpr uint32_t kMaxCapacity = 256;  // rows in one chunk of interest, at most; 10 is the most seen
constexpr int kMaxFound = 512;
constexpr int64_t kLogPeriodUs = 20'000'000;  // between two cost lines
constexpr int64_t kListGapUs = 1'000'000;     // between two logs of the list, and between two reads of the ini
constexpr int kListLogRows = 8;
constexpr int64_t kBudgetUs = 1000;  // a walk's budget: what one runner call used to be allowed
constexpr DWORD kIntervalDefaultMs = 100, kIntervalMinMs = 10, kIntervalMaxMs = 10000;
constexpr DWORD kStopWaitMs = 5000;  // the unloader's wait for the thread, a walk being milliseconds

// The walk thread. The runner stores the world and its call count (runner::world, runner::player_state); this thread
// wakes every [enemies] interval_ms, walks when the runner was called since its last walk, and publishes.
HANDLE g_thread, g_stop_event;
std::atomic<bool> g_stop{false};
DWORD g_interval_ms = kIntervalDefaultMs;

enum Kind : uint8_t { kUnread, kOther, kNpc, kAttached, kLinked };

// What an archetype's columns say, kept while its entry points at the same hashes: an archetype's types never change,
// its offsets do (a chunk reallocated from 10 rows to 2 moved every column), so its columns are read on every walk.
// Each field is a column index in ecs::Columns, -1 when absent.
struct Class {
    uint64_t hashes = 0;
    uint32_t count = 0;
    Kind kind = kUnread;
    int16_t health = -1, spawned = -1, faction = -1, level = -1, transform = -1, attachment = -1, combat = -1;
    int16_t link = -1;  // HealthLink: a probe of entities whose health is another's, the boss's mini wheels
};

struct Shield {
    uint64_t owner;
    float fraction;
    int32_t max;
};

Class g_class[game::kChunkSlots];
uint64_t g_class_world;

// Walk thread only. Every byte the walk reads is a copy in one of these: the slot table and entries in g_world, an
// archetype's columns in g_columns, a chunk's rows in the arrays below.
ecs::World g_world;
ecs::Columns g_columns;
uint64_t g_handles[kMaxCapacity];
uint8_t g_health[kMaxCapacity * game::kHealthRowSize];
uint8_t g_spawned[kMaxCapacity * game::kSpawnedNpcRowSize];
uint8_t g_faction[kMaxCapacity * game::kFactionRowSize];
uint8_t g_level[kMaxCapacity * game::kNpcLevelRowSize];
uint8_t g_transform[kMaxCapacity * game::kWorldTransformRowSize];
uint8_t g_attachment[kMaxCapacity * game::kAttachmentRowSize];
Enemy g_found[kMaxFound];
uint8_t g_link[kMaxCapacity * 32];

// A placed row with Health and a faction and no SpawnedNPC, for the log only: what the panel does not list (the player,
// a quest NPC such as the Firebreaker escort, a linked health bar).
struct Linked {
    uint64_t handle;
    int64_t arch;
    float fraction;
    int32_t max;
    float position[3];
    uint32_t faction;
    uint32_t link_size;
    uint8_t link[16];
};
constexpr int kMaxLinked = 64;
Linked g_linked[kMaxLinked];
int g_linked_n, g_linked_logged = -1, g_linked_nearest_logged = -1;
int64_t g_linked_logged_at = -kListGapUs;
float g_player_pos[3];
bool g_player_located;
Shield g_shields[kMaxFound];

// What one walk did. Classifications: a slot never seen, an entry now pointing at other hashes, a failed copy tried
// again. Failed copies: an archetype's columns (its hashes and offsets), its chunk's rows. Rejected: rows with an
// entity handle whose Health did not read as alive (a fraction outside (0, 1], a max of 0): a dead row, or one torn by
// the game writing it while this thread read, since the walk no longer runs inside a system call.
struct Count {
    int fresh = 0, changed = 0, retried = 0, chunks = 0, unbounded = 0;
    int lost_columns = 0, lost_rows = 0, rejected = 0;
    int64_t combat_us = 0, slots_us = 0;  // where the walk's time went: the CombatStatus probe, the slot loop
    uint64_t cycles = 0;                  // the CPU cycles this thread spent in it: wall time minus time off the CPU

    void add(const Count &o) {
        fresh += o.fresh, changed += o.changed, retried += o.retried, chunks += o.chunks, unbounded += o.unbounded;
        lost_columns += o.lost_columns, lost_rows += o.lost_rows, rejected += o.rejected;
    }
} g_walk;

// Per log period: the sum of its walks, and the slowest one's own counts.
struct Period {
    long walks = 0, slow = 0, idle = 0;  // slow: over kBudgetUs; idle: wakes with no runner call since the last walk
    int64_t sum_us = 0, max_us = 0;
    uint64_t sum_cycles = 0;
    Count sum, slowest;
} g_period;
int64_t g_period_at;
int g_found_n, g_shields_n, g_total_logged = -1;
int64_t g_list_logged_at = -kListGapUs;
bool g_player_row_said, g_first_said, g_unreadable_said;
uint8_t g_combat_logged[32];
constexpr int kKindsLogged = 256;
uint64_t g_kinds_seen[kKindsLogged];  // every kind logged once, on its first sighting
int g_kinds_seen_n;
int64_t g_combat_logged_at = -kListGapUs;

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

// The class of slot a, read again when its entry points at another hashes array; g_columns then holds its columns. A
// failed copy stays unread and is tried again on the next walk.
const Class &class_of(int64_t a) {
    const ecs::Entry &entry = g_world.entry(a);
    Class &c = g_class[a];
    if (c.kind != kUnread && c.hashes == entry.hashes && c.count == entry.count) return c;
    (c.kind != kUnread ? g_walk.changed : c.hashes ? g_walk.retried : g_walk.fresh)++;
    c = {};
    if (!entry.hashes || !entry.count || entry.count > ecs::kMaxColumns) {
        c = {entry.hashes, entry.count, kOther};
        return c;
    }
    if (!g_columns.read(g_world, a)) {
        g_walk.lost_columns++;
        c.hashes = entry.hashes;  // still kUnread: tried again next walk, counted as a retry
        return c;
    }
    c.hashes = entry.hashes;
    c.count = entry.count;
    for (uint32_t i = 0; i < c.count; i++) {
        const auto k = static_cast<int16_t>(i);
        switch (g_columns.hash(i)) {
            case game::kHealth: c.health = k; break;
            case game::kSpawnedNpc: c.spawned = k; break;
            case game::kFactionData: c.faction = k; break;
            case game::kNpcLevel: c.level = k; break;
            case game::kWorldTransform: c.transform = k; break;
            case game::kAttachment: c.attachment = k; break;
            case game::kCombatStatus: c.combat = k; break;
            case game::kHealthLink: c.link = k; break;
            default: break;
        }
    }
    c.kind = c.health < 0        ? kOther
             : c.spawned >= 0    ? kNpc
             : c.attachment >= 0 ? kAttached
             : c.transform >= 0  ? kLinked  // any placed row with Health and no spawner: quest NPCs, the player
                                 : kOther;
    return c;
}

// Slot a's columns into g_columns, fresh, and its chunk's capacity: the Health column's width over its row, exact since
// a 0x38-byte row outweighs the column's padding. 0 when the copy fails or nothing bounds the column.
uint32_t read_columns(int64_t a) {
    if (!g_columns.read(g_world, a)) {
        g_walk.lost_columns++;
        return 0;
    }
    const uint32_t capacity = g_columns.capacity(game::kHealth, game::kHealthRowSize);
    if (!capacity) g_walk.unbounded++;
    return capacity <= kMaxCapacity ? capacity : 0;
}

// Column `index` of the chunk, capacity rows of `size` bytes, into out; zeros when the archetype lacks it.
bool copy_column(uint64_t chunk, int16_t index, uint32_t capacity, uint32_t size, uint8_t *out) {
    if (index < 0) {
        std::memset(out, 0, static_cast<size_t>(capacity) * size);
        return true;
    }
    if (ecs::copy_rows(g_world.source(), chunk, g_columns.offset_at(static_cast<uint32_t>(index)), capacity, size, out))
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
        if (!g_handles[r]) continue;
        if (!alive(fraction, max)) {
            g_walk.rejected++;
            continue;
        }
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

// Rows of an archetype with Health and HealthLink and no SpawnedNPC, with the link row's first bytes: whose health
// they carry, when the link names a handle. The link's width is its column's over the capacity, at most 32 bytes.
void read_linked(uint64_t chunk, const Class &c, uint32_t capacity, int64_t arch) {
    uint32_t width = 0;
    if (c.link >= 0) {
        const uint32_t column = g_columns.width(game::kHealthLink);
        width = std::min<uint32_t>(column ? column / capacity : 16, 32);
        if (!width || !copy_column(chunk, c.link, capacity, width, g_link)) return;
    }
    // Rows without a faction are the destructible props (50/50, 9999/9999 by the hundred) and would fill the sample.
    if (c.faction < 0 || !copy_column(chunk, c.faction, capacity, game::kFactionRowSize, g_faction) ||
        !copy_column(chunk, c.transform, capacity, game::kWorldTransformRowSize, g_transform))
        return;
    for (uint32_t r = 0; r < capacity && g_linked_n < kMaxLinked; r++) {
        const uint8_t *health = g_health + r * game::kHealthRowSize;
        const auto fraction = at<float>(health);
        const auto max = at<int32_t>(health + 4);
        if (!g_handles[r]) continue;
        if (!alive(fraction, max)) {
            g_walk.rejected++;
            continue;
        }
        Linked &l = g_linked[g_linked_n++];
        l = {};
        l.handle = g_handles[r];
        l.arch = arch;
        l.fraction = fraction;
        l.max = max;
        std::memcpy(l.position, g_transform + r * game::kWorldTransformRowSize + game::kWorldPosition,
                    sizeof l.position);
        l.faction = at<uint32_t>(g_faction + r * game::kFactionRowSize);
        l.link_size = width;
        if (width) std::memcpy(l.link, g_link + r * width, width < sizeof l.link ? width : sizeof l.link);
    }
}

// The player's CombatStatus row, up to 32 bytes, to the log when it changes, at most once a second: a probe of what
// flips when a fight starts and ends. g_columns holds the player's archetype.
void log_combat(uint64_t chunk, const Class &c, long calls, int64_t now) {
    const uint32_t column = g_columns.offset_at(static_cast<uint32_t>(c.combat));
    const uint32_t bytes = g_columns.width(game::kCombatStatus);
    const uint32_t width = std::min<uint32_t>(bytes ? bytes : 16, sizeof g_combat_logged);
    uint8_t row[sizeof g_combat_logged] = {};
    if (now - g_combat_logged_at < kListGapUs || !ecs::copy_rows(g_world.source(), chunk, column, 1, width, row) ||
        !std::memcmp(row, g_combat_logged, sizeof row))
        return;
    char hex[sizeof row * 3 + 1];
    int n = 0;
    for (uint32_t i = 0; i < width; i++) n += wsprintfA(hex + n, i % 4 == 3 ? "%02x " : "%02x", row[i]);
    log::write("combat status (call %ld): %s", calls, hex);
    std::memcpy(g_combat_logged, row, sizeof row);
    g_combat_logged_at = now;
}

// The CombatStatus probe on the player's chunk, which is the slot of the archetype the runner resolved (player::State
// arch): the runner's memo (player::health_row) is its thread's, so nothing of it is read here.
void probe_combat(const player::State &s, long calls, int64_t now) {
    const uint64_t chunk = g_world.chunk(s.arch);
    if (!chunk) return;
    const Class &c = class_of(s.arch);
    if (c.health < 0 || c.combat < 0) return;
    const uint32_t capacity = read_columns(s.arch);
    if (capacity != 1 && !g_player_row_said) {
        log::write("enemies: the player's chunk holds %lu rows, read as row 0", static_cast<unsigned long>(capacity));
        g_player_row_said = true;
    }
    if (capacity) log_combat(chunk, c, calls, now);
}

// Every slot g_world copied. The player's position is the runner's last reading, not a lookup of this thread's: the
// distances are to where the runner last saw the player.
void walk(const player::State &s, long calls, int64_t now) {
    if (g_world.address() != g_class_world) {
        for (Class &c : g_class) c = {};
        g_class_world = g_world.address();
    }
    g_found_n = g_shields_n = 0;
    const int64_t t0 = now_us();
    probe_combat(s, calls, now);
    const int64_t t1 = now_us();
    g_walk.combat_us = t1 - t0;
    // The linked rows feed a log line at most once a second: their chunks are read on one walk in kListGapUs.
    const bool sample_linked = now - g_linked_logged_at >= kListGapUs;
    if (sample_linked) g_linked_n = 0;
    const bool located = s.has_position;
    const float *player = s.position;
    g_player_located = located;
    if (located) std::memcpy(g_player_pos, player, sizeof g_player_pos);

    const int64_t slots = std::min(g_world.slots(), game::kChunkSlots);
    for (int64_t a = 0; a < slots; a++) {
        const uint64_t chunk = g_world.chunk(a);
        if (!chunk) continue;
        const Class &c = class_of(a);
        if (c.kind != kNpc && c.kind != kAttached && (c.kind != kLinked || !sample_linked || g_linked_n >= kMaxLinked))
            continue;
        g_walk.chunks++;
        const uint32_t capacity = read_columns(a);
        if (!capacity) continue;
        if (!g_world.source().copy(chunk + game::kEntityColumn, g_handles, capacity * 8ull)) {
            g_walk.lost_rows++;
            continue;
        }
        if (!copy_column(chunk, c.health, capacity, game::kHealthRowSize, g_health)) continue;
        if (c.kind == kNpc)
            read_npcs(chunk, c, capacity);
        else if (c.kind == kLinked)
            read_linked(chunk, c, capacity, a);
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

// The walk's rows as a List: nearest first, at most kMaxRows, every row counted.
void fill(List &out, long calls) {
    out.calls = calls;
    out.total = g_found_n;
    out.allies = 0;
    for (int i = 0; i < g_found_n; i++)
        if (names::is_ally(g_found[i].faction)) out.allies++;
    out.count = std::min(g_found_n, kMaxRows);
    std::copy(g_found, g_found + out.count, out.rows);
}

void publish(long calls) {
    AcquireSRWLockExclusive(&g_lock);
    fill(g_list, calls);
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
        uint64_t kind = 0;
        std::memcpy(&kind, e.kind, sizeof kind);
        bool seen = false;
        for (int k = 0; k < g_kinds_seen_n && !seen; k++) seen = g_kinds_seen[k] == kind;
        if (seen) continue;
        g_kinds_seen[g_kinds_seen_n++] = kind;
        char dist[24];
        tenths(e.distance, dist);
        log::write(
            "enemies: new kind %02x%02x%02x%02x%02x%02x%02x%02x  %ld max  faction %08lx  lvl %ld  b3c %u  from "
            "%lx/%lu  dist %s (call %ld)",
            e.kind[0], e.kind[1], e.kind[2], e.kind[3], e.kind[4], e.kind[5], e.kind[6], e.kind[7],
            static_cast<long>(e.max), static_cast<unsigned long>(e.faction), static_cast<long>(e.level), e.byte3c,
            static_cast<unsigned long>(e.source_id), static_cast<unsigned long>(e.source_generation), dist, calls);
    }
}

float distance_to_player(const float p[3]) {
    if (!g_player_located) return -1.0f;
    const float dx = p[0] - g_player_pos[0], dy = p[1] - g_player_pos[1], dz = p[2] - g_player_pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void log_linked(long calls) {
    log::write("linked: %d placed rows with Health and a faction, no SpawnedNPC (call %ld), nearest first:", g_linked_n,
               calls);
    std::sort(g_linked, g_linked + g_linked_n, [](const Linked &a, const Linked &b) {
        return distance_to_player(a.position) < distance_to_player(b.position);
    });
    for (int i = 0; i < g_linked_n && i < kListLogRows; i++) {
        const Linked &l = g_linked[i];
        char dist[24], link[16 * 3 + 1] = "-";
        tenths(distance_to_player(l.position), dist);
        int n = 0;
        for (uint32_t b = 0; b < l.link_size && b < sizeof l.link; b++)
            n += wsprintfA(link + n, b % 4 == 3 ? "%02x " : "%02x", l.link[b]);
        log::write("  %lx/%lu  %d/%ld  arch %ld  faction %08lx  dist %s  link (%lu bytes) %s",
                   static_cast<unsigned long>(static_cast<uint32_t>(l.handle)),
                   static_cast<unsigned long>(l.handle >> 32), game::points(l.fraction, l.max),
                   static_cast<long>(l.max), static_cast<long>(l.arch), static_cast<unsigned long>(l.faction), dist,
                   static_cast<unsigned long>(l.link_size), link);
    }
}

void log_list(long calls) {
    log::write("enemies: %d (call %ld), nearest first:", g_found_n, calls);
    for (int i = 0; i < g_found_n && i < kListLogRows; i++) {
        const Enemy &e = g_found[i];
        char dist[24], shield[40] = "";
        tenths(e.distance, dist);
        if (e.shield_max)
            wsprintfA(shield, "  shield %d/%ld", game::points(e.shield_fraction, e.shield_max),
                      static_cast<long>(e.shield_max));
        log::write(
            "  %lx/%lu  %d/%ld  faction %08lx  lvl %ld  kind %02x%02x%02x%02x%02x%02x%02x%02x  b3c %u  "
            "from %lx/%lu  dist %s%s",
            static_cast<unsigned long>(e.id), static_cast<unsigned long>(e.generation), game::points(e.fraction, e.max),
            static_cast<long>(e.max), static_cast<unsigned long>(e.faction), static_cast<long>(e.level), e.kind[0],
            e.kind[1], e.kind[2], e.kind[3], e.kind[4], e.kind[5], e.kind[6], e.kind[7], e.byte3c,
            static_cast<unsigned long>(e.source_id), static_cast<unsigned long>(e.source_generation), dist, shield);
    }
}

// One walk of the world the runner last saw, at the runner's call `calls`, with the player as the runner last read it.
void walk_once(void *world, const player::State &s, long calls) {
    const int64_t t0 = now_us();
    g_walk = {};
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    // The slot table and the entries in use, copied: nothing below reads the world in place.
    if (!g_world.load(mem::live(), reinterpret_cast<uint64_t>(world))) {
        if (!g_unreadable_said) log::write("enemies: the world %p has no slot that reads", world);
        g_unreadable_said = true;
        return;
    }
    const int64_t slots = g_world.slots();
    walk(s, calls, t0);
    publish(calls);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    const int64_t us = now_us() - t0;
    g_walk.cycles = c1 - c0;

    if (!g_first_said) {
        log::write(
            "enemies: first walk, %ld slots, %d archetypes classified in %ld us, on a thread of ours every %lu ms",
            static_cast<long>(slots), g_walk.fresh, static_cast<long>(us), static_cast<unsigned long>(g_interval_ms));
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
    if (t0 - g_linked_logged_at >= kListGapUs) {
        // The sample is capped, so its count alone misses a hit on the nearest row: its points are the second key.
        int nearest = 0;
        for (int i = 0; i < g_linked_n; i++)
            if (distance_to_player(g_linked[i].position) < distance_to_player(g_linked[nearest].position)) nearest = i;
        const int points = g_linked_n ? game::points(g_linked[nearest].fraction, g_linked[nearest].max) : -1;
        if (g_linked_n != g_linked_logged || points != g_linked_nearest_logged) {
            log_linked(calls);
            g_linked_logged = g_linked_n;
            g_linked_nearest_logged = points;
        }
        g_linked_logged_at = t0;
    }
    if (g_found_n != g_total_logged && t0 - g_list_logged_at >= kListGapUs) {
        log_list(calls);
        g_total_logged = g_found_n;
        g_list_logged_at = t0;
    }
}

void log_period() {
    const Count &t = g_period.sum, &m = g_period.slowest;
    log::write(
        "enemies: walk avg %ld us, max %ld us, %ld over %ld us, of %ld walks (%ld wakes idle) every %lu ms; "
        "%d enemies, %d shields",
        static_cast<long>(g_period.walks ? g_period.sum_us / g_period.walks : 0), static_cast<long>(g_period.max_us),
        g_period.slow, static_cast<long>(kBudgetUs), g_period.walks, g_period.idle,
        static_cast<unsigned long>(g_interval_ms), g_found_n, g_shields_n);
    log::write(
        "  classified %d new, %d changed, %d retried; %d chunks; copies failed: %d columns, %d rows; %d rows "
        "rejected; %d Health columns unbounded",
        t.fresh, t.changed, t.retried, t.chunks, t.lost_columns, t.lost_rows, t.rejected, t.unbounded);
    // The slowest walk's cycles as microseconds at the period's rate: far under its wall time = off the CPU.
    const long on_cpu = g_period.sum_cycles
                            ? static_cast<long>(m.cycles * static_cast<uint64_t>(g_period.sum_us) / g_period.sum_cycles)
                            : -1;
    log::write(
        "  the slowest walk: combat probe %ld us, slots %ld us, on the CPU ~%ld us; classified %d new, "
        "%d changed, %d retried; %d chunks; copies failed: %d columns, %d rows; %d rows rejected",
        static_cast<long>(m.combat_us), static_cast<long>(m.slots_us), on_cpu, m.fresh, m.changed, m.retried, m.chunks,
        m.lost_columns, m.lost_rows, m.rejected);
    g_period = {};
}

// [enemies] interval_ms, re-read once a second as the panel re-reads radius, clamped to [10, 10000].
void read_ini() {
    static const std::wstring ini = log::path(L".ini");
    const UINT v = GetPrivateProfileIntW(L"enemies", L"interval_ms", kIntervalDefaultMs, ini.c_str());
    const DWORD was = g_interval_ms;
    g_interval_ms = v < kIntervalMinMs ? kIntervalMinMs : v > kIntervalMaxMs ? kIntervalMaxMs : v;
    if (g_interval_ms != was && g_first_said)
        log::write("enemies: interval %lu -> %lu ms", static_cast<unsigned long>(was),
                   static_cast<unsigned long>(g_interval_ms));
}

// Waits the interval on the stop event, then walks when the runner was called since the last walk: a menu, a load or
// a paused game walks nothing. A wake that lands during a walk is not queued; the next wait starts after the walk.
DWORD WINAPI walker(LPVOID) {
    log::write("enemies: walk thread tid %lu", GetCurrentThreadId());
    long walked_calls = 0;
    int64_t ini_at = -kListGapUs;
    g_period_at = now_us();
    while (!g_stop.load() && !lifecycle::stopping) {
        if (WaitForSingleObject(g_stop_event, g_interval_ms) == WAIT_OBJECT_0) break;
        const int64_t now = now_us();
        if (now - ini_at >= kListGapUs) {
            read_ini();
            ini_at = now;
        }
        player::State s;
        long calls = 0;
        runner::player_state(s, &calls);
        void *world = runner::world();
        if (world && calls != walked_calls) {
            walk_once(world, s, calls);
            walked_calls = calls;
        } else {
            g_period.idle++;
        }
        if (now - g_period_at >= kLogPeriodUs) {
            if (g_period.walks) log_period();
            g_period = {};
            g_period_at = now;
        }
    }
    log::write("enemies: walk thread ending");
    return 0;
}

}  // namespace

bool start() {
    read_ini();
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        log::write("enemies: CreateEvent failed - no walk");
        return false;
    }
    g_thread = CreateThread(nullptr, 0, walker, nullptr, 0, nullptr);
    if (!g_thread) {
        log::write("enemies: CreateThread failed - no walk");
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        return false;
    }
    return true;
}

bool stop() {
    if (!g_thread) return true;
    g_stop = true;
    SetEvent(g_stop_event);
    const DWORD w = WaitForSingleObject(g_thread, kStopWaitMs);
    log::write("enemies: walk thread %s", w == WAIT_OBJECT_0 ? "ended" : "did not end in 5 s - staying mapped");
    if (w != WAIT_OBJECT_0) return false;
    CloseHandle(g_thread);
    CloseHandle(g_stop_event);
    g_thread = g_stop_event = nullptr;
    return true;
}

void scan(const mem::Source &source, uint64_t world, const player::State &player, List &out) {
    out = {};
    if (!g_world.load(source, world)) return;
    walk(player, 0, now_us());
    fill(out, 0);
}

void read(List &out) {
    AcquireSRWLockShared(&g_lock);
    out = g_list;
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace crsrt::enemies
