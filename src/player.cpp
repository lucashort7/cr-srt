// player.cpp
#include "player.h"

#include "clock.h"
#include "ecs.h"
#include "game.h"
#include "log.h"
#include "source.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace crsrt::player {

namespace {

GetPlayerFn g_get_player;

constexpr uint32_t kAbsent = ecs::kAbsent;  // the archetype lacks the column
constexpr float kMapBound = 1.0e6f;         // metres; a coordinate past it is not a position
constexpr float kSpeedBound = 1000.0f;      // metres a second
constexpr int kResolutionsLogged = 50;

// The last resolution, reused while the world is the same, get_player hands back the same chunk and the table still
// holds that chunk at that archetype. The chunk alone is not enough: an entity can change archetype and keep its chunk.
// Each column's offset from the chunk, row 0 being the player's.
struct Memo {
    uint64_t world = 0;
    uint64_t chunk = 0;
    int64_t arch = -1;
    uint32_t health = kAbsent, transform = kAbsent, movement = kAbsent, state = kAbsent;
    uint32_t gap = kAbsent, status = kAbsent, logic = kAbsent;
    uint32_t resourcing = kAbsent, progression = kAbsent, upgrades = kAbsent;
    bool dead = false;
} g_memo;

// Runner thread only: the copies a resolution reads, the slot table and the player's archetype's columns.
ecs::World g_world;
ecs::Columns g_columns;

int g_resolutions;
bool g_pad_said;
// The archetypes whose full type list the log already has: the player cycles through a handful, and each list is 12
// synchronous writes on the game's thread (2-3 ms of runner max at every change on 2026-09-30).
constexpr int kListsLogged = 32;
int64_t g_listed[kListsLogged];
int g_listed_count;

bool listed(int64_t index) {
    for (int i = 0; i < g_listed_count; i++)
        if (g_listed[i] == index) return true;
    if (g_listed_count < kListsLogged) g_listed[g_listed_count++] = index;
    return false;
}

enum Resolved { kFailed, kKept, kFresh };

template <class T>
T at(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// get_player's chunk, or 0 when it finds no player.
uint64_t player_chunk(void *world) {
    uint64_t out[8] = {};
    if (!g_get_player || !world) return 0;
    g_get_player(out, world);
    return (out[3] & 1) ? out[1] : 0;
}

void offset_text(uint32_t offset, char *out) {
    if (offset == kAbsent)
        lstrcpyA(out, "none");
    else
        wsprintfA(out, "+%lx", static_cast<unsigned long>(offset));
}

// The memo for this chunk: kept unless `force`, else found again in a fresh copy of the slot table, and logged when
// that moved anything. kFailed when the chunk's archetype or its Health column is not found.
Resolved resolve(const mem::Source &source, uint64_t world, uint64_t chunk, bool force) {
    uint64_t slot = 0;
    if (!force && world == g_memo.world && chunk == g_memo.chunk && g_memo.arch >= 0 &&
        mem::read(source, ecs::slot_address(world, g_memo.arch), slot) && slot == chunk)
        return kKept;
    if (!g_world.load(source, world)) return kFailed;
    const int64_t index = g_world.find(chunk);
    if (index < 0 || !g_columns.read(g_world, index)) return kFailed;
    const uint32_t health = g_columns.offset(game::kHealth);
    if (health == kAbsent) return kFailed;
    if (const uint32_t logic = g_columns.offset(game::kLogicData); logic != kAbsent) {
        const uint32_t width = g_columns.width(game::kLogicData);
        log::write("player: LogicData column +%lx, width %ld", static_cast<unsigned long>(logic),
                   width ? static_cast<long>(width) : -1L);
    }
    const Memo was = g_memo;
    g_memo = {world,
              chunk,
              index,
              health,
              g_columns.offset(game::kWorldTransform),
              g_columns.offset(game::kMovement),
              g_columns.offset(game::kMovementState),
              g_columns.offset(game::kInGap),
              g_columns.offset(game::kStatus),
              g_columns.offset(game::kLogicData),
              g_columns.offset(game::kResourcing),
              g_columns.offset(game::kPlayerProgression),
              g_columns.offset(game::kCoreUpgrade),
              ecs::has_tag(g_world, index, game::kDead)};
    const bool moved = was.world != world || was.chunk != chunk || was.arch != index || was.health != g_memo.health ||
                       was.transform != g_memo.transform || was.movement != g_memo.movement ||
                       was.state != g_memo.state || was.gap != g_memo.gap || was.status != g_memo.status ||
                       was.logic != g_memo.logic || was.resourcing != g_memo.resourcing ||
                       was.progression != g_memo.progression || was.upgrades != g_memo.upgrades;
    if (moved && g_resolutions < kResolutionsLogged) {
        g_resolutions++;
        char health_at[16], transform[16], movement[16], state[16], gap[16], status[16], logic[16];
        char resourcing[16], progression[16], upgrades[16];
        offset_text(g_memo.health, health_at);
        offset_text(g_memo.transform, transform);
        offset_text(g_memo.movement, movement);
        offset_text(g_memo.state, state);
        offset_text(g_memo.gap, gap);
        offset_text(g_memo.status, status);
        offset_text(g_memo.logic, logic);
        offset_text(g_memo.resourcing, resourcing);
        offset_text(g_memo.progression, progression);
        offset_text(g_memo.upgrades, upgrades);
        log::write(
            "player: rows at arch %ld chunk %p: Health %s, transform %s, Movement %s, MovementState %s, InGap "
            "%s, Status %s, LogicData %s, ResourcingData %s, PlayerProgression %s, CoreUpgradeData %s%s",
            static_cast<long>(index), reinterpret_cast<void *>(chunk), health_at, transform, movement, state, gap,
            status, logic, resourcing, progression, upgrades,
            g_resolutions == kResolutionsLogged ? " (the last such line)" : "");
        // The full type list once per archetype new to this process, tags included: a diff of two lists names the
        // component a cutscene or a death adds (AKTH-0110).
        std::vector<uint32_t> tags;
        if (was.arch != index && !listed(index) && ecs::type_list(source, g_world.entry(index), tags))
            ecs::log_type_list(index, tags.data(), static_cast<uint32_t>(tags.size()));
    }
    return kFresh;
}

// Where one read's time went, for the stutter card's per-step timer: a read past kSlowReadUs logs it, at most once a
// second. Runner thread only.
constexpr int64_t kSlowReadUs = 1000;
struct Steps {
    int64_t get_player = 0, locate = 0, rows = 0, slowest_row = 0;
    uint32_t slowest_offset = kAbsent;
    int copies = 0;
    int64_t direct = 0;  // the same rows again by memcpy, once each copy proved them: the syscall's share of `rows`
    Resolved resolved = kFailed;
} g_steps;
int64_t g_slow_said_at = -1'000'000;

// Row `offset` of the chunk, `size` bytes, into out; false when the archetype lacks it or the copy fails. Timed.
bool row(const mem::Source &source, uint64_t chunk, uint32_t offset, uint32_t size, void *out) {
    if (offset == kAbsent) return false;
    const int64_t t = clock::now_us();
    const bool ok = ecs::copy_rows(source, chunk, offset, 1, size, out);
    const int64_t t1 = clock::now_us();
    const int64_t us = t1 - t;
    g_steps.copies++;
    if (us > g_steps.slowest_row) g_steps.slowest_row = us, g_steps.slowest_offset = offset;
    // A probe for the stutter card: the bytes the copy just proved readable, read again in place. Live source only.
    if (ok && &source == &mem::live()) {
        static uint8_t scratch[game::kLogicDataProbe];
        std::memcpy(scratch, reinterpret_cast<const void *>(chunk + offset),
                    size < sizeof scratch ? size : sizeof scratch);
        g_steps.direct += clock::now_us() - t1;
    }
    return ok;
}

// The memo resolved for the chunk, and its Health row copied into health. A kept memo whose row does not copy is wrong:
// the table decides again.
bool locate(const mem::Source &source, uint64_t world, uint64_t chunk, uint8_t *health) {
    Resolved r = resolve(source, world, chunk, false);
    if (r == kKept && !row(source, chunk, g_memo.health, game::kHealthRowSize, health))
        r = resolve(source, world, chunk, true);
    g_steps.resolved = r;
    return r != kFailed && row(source, chunk, g_memo.health, game::kHealthRowSize, health);
}

bool on_map(float v) { return std::isfinite(v) && std::fabs(v) < kMapBound; }

}  // namespace

void set_get_player(GetPlayerFn fn) { g_get_player = fn; }

void read(void *world, State &out) {
    g_steps = {};
    const int64_t t0 = clock::now_us();
    const uint64_t chunk = player_chunk(world);
    const int64_t t1 = clock::now_us();
    g_steps.get_player = t1 - t0;
    if (!chunk) {
        out = {};
    } else {
        read_chunk(mem::live(), reinterpret_cast<uint64_t>(world), chunk, out);
    }
    const int64_t t2 = clock::now_us();
    if (t2 - t0 > kSlowReadUs && t2 - g_slow_said_at > 1'000'000) {
        static const char *const kResolved[] = {"failed", "kept", "fresh"};
        log::write(
            "player: slow read %ld us: get_player %ld, locate %ld (%s), rows %ld in %d copies, slowest +%lx %ld us; "
            "the same rows by memcpy %ld us",
            static_cast<long>(t2 - t0), static_cast<long>(g_steps.get_player), static_cast<long>(g_steps.locate),
            kResolved[g_steps.resolved], static_cast<long>(g_steps.rows), g_steps.copies,
            static_cast<unsigned long>(g_steps.slowest_offset), static_cast<long>(g_steps.slowest_row),
            static_cast<long>(g_steps.direct));
        g_slow_said_at = t2;
    }
}

void read_chunk(const mem::Source &source, uint64_t world, uint64_t chunk, State &out) {
    out = {};
    uint8_t health[game::kHealthRowSize];
    const int64_t t0 = clock::now_us();
    const bool located = world && chunk && locate(source, world, chunk, health);
    const int64_t t1 = clock::now_us();
    g_steps.locate = t1 - t0;
    if (!located) return;
    struct RowsTimer {
        int64_t from;
        ~RowsTimer() { g_steps.rows = clock::now_us() - from; }
    } const timer{t1};
    out.arch = g_memo.arch;
    out.dead = g_memo.dead;
    out.health_at = chunk + g_memo.health;
    out.has_health = true;
    out.fraction = at<float>(health);
    out.max = at<int32_t>(health + 4);
    if (uint8_t r[game::kResourcingRowSize]; row(source, chunk, g_memo.resourcing, sizeof r, r)) {
        out.power = at<float>(r + game::kPower);
        out.power_max = at<float>(r + game::kPowerMax);
        out.command_max = at<float>(r + game::kCommandMax);
        out.command = at<float>(r + game::kCommand);
        out.command_charge = at<float>(r + game::kCommandChargeSize);
        out.command_charges = at<int32_t>(r + game::kCommandCharges);
        out.has_power = std::isfinite(out.power) && std::isfinite(out.power_max) && std::isfinite(out.command) &&
                        std::isfinite(out.command_max);
    }
    if (uint8_t r[game::kPlayerProgressionRowSize]; row(source, chunk, g_memo.progression, sizeof r, r)) {
        out.build_power = at<int32_t>(r + game::kBuildPower);
        out.aberrant = at<int32_t>(r + game::kAberrantStrength);
        out.ability = at<int32_t>(r + game::kAbilityStrength);
        out.has_progression = true;
    }
    if (uint8_t r[game::kCoreUpgradeRowSize]; row(source, chunk, g_memo.upgrades, sizeof r, r)) {
        out.health_upgrades = at<int32_t>(r + game::kHealthUpgrades);
        out.power_upgrades = at<int32_t>(r + game::kPowerUpgrades);
        out.has_upgrades = true;
    }
    if (row(source, chunk, g_memo.gap, game::kInGapRowSize, &out.gap)) {
        out.has_gap = true;
        out.in_gap = (out.gap & game::kInGapFlag) == 1;
    }
    out.has_status = row(source, chunk, g_memo.status, game::kStatusRowSize, &out.status);
    if (row(source, chunk, g_memo.logic, game::kLogicDataProbe, out.logic)) {
        out.has_logic = true;
        out.in_story = out.logic[game::kLogicStoryByte] == game::kLogicStoryBlocked;
    }

    if (uint8_t r[game::kWorldTransformRowSize]; row(source, chunk, g_memo.transform, sizeof r, r)) {
        out.position_at = chunk + g_memo.transform + game::kWorldPosition;
        std::memcpy(out.position, r + game::kWorldPosition, sizeof out.position);
        std::memcpy(out.rotation, r, sizeof out.rotation);
        out.has_position = on_map(out.position[0]) && on_map(out.position[1]) && on_map(out.position[2]);
        // Read as zero once in Cheat Engine, at two decimals: logged the first time it is not, never a gate.
        const float pad = at<float>(r + game::kWorldPositionPad);
        if (pad != 0.0f && !g_pad_said) {
            char text[32];
            std::snprintf(text, sizeof text, "%.6g", pad);
            log::write("player: the transform's +0x1c reads %s, not 0 (the first time)", text);
            g_pad_said = true;
        }
    }
    if (uint8_t r[game::kMovementRowSize]; row(source, chunk, g_memo.movement, sizeof r, r)) {
        out.speed_at = chunk + g_memo.movement + game::kMovementSpeed;
        out.speed = at<float>(r + game::kMovementSpeed);
        out.has_speed = std::isfinite(out.speed) && out.speed >= 0.0f && out.speed < kSpeedBound;
    }
    if (row(source, chunk, g_memo.state, sizeof out.velocity, out.velocity)) {
        out.velocity_at = chunk + g_memo.state;
        const float x = out.velocity[0], y = out.velocity[1], z = out.velocity[2];
        out.h = std::sqrt(x * x + z * z);
        out.total = std::sqrt(x * x + y * y + z * z);
        out.has_velocity = std::isfinite(out.total) && out.total < kSpeedBound;
    }
}

void log_first_look(void *world) {
    if (!world || !g_get_player) return;
    uint64_t out[8] = {};
    g_get_player(out, world);
    log::write("get_player -> %p %p %p %p", reinterpret_cast<void *>(out[0]), reinterpret_cast<void *>(out[1]),
               reinterpret_cast<void *>(out[2]), reinterpret_cast<void *>(out[3]));
    static ecs::World w;
    if ((out[3] & 1) && w.load(mem::live(), reinterpret_cast<uint64_t>(world))) ecs::log_archetype(w, out[1], out[0]);
}

}  // namespace crsrt::player
