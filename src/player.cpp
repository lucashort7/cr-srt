// player.cpp
#include "player.h"

#include "ecs.h"
#include "game.h"
#include "log.h"
#include "memory.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace crsrt::player {

namespace {

GetPlayerFn g_get_player;

constexpr uint32_t kAbsent = UINT32_MAX;  // the archetype lacks the column
constexpr float kMapBound = 1.0e6f;       // metres; a coordinate past it is not a position
constexpr float kSpeedBound = 1000.0f;    // metres a second
constexpr int kResolutionsLogged = 50;

// The last resolution, reused while the world is the same, get_player hands back the same chunk and the table still
// holds that chunk at that archetype. The chunk alone is not enough: an entity can change archetype and keep its chunk.
// Each column's offset from the chunk, row 0 being the player's.
struct Memo {
    void *world = nullptr;
    uint64_t chunk = 0;
    int64_t arch = -1;
    uint32_t health = kAbsent, transform = kAbsent, movement = kAbsent, state = kAbsent;
    uint32_t gap = kAbsent, status = kAbsent, logic = kAbsent;
    uint32_t resourcing = kAbsent, progression = kAbsent, upgrades = kAbsent;
    bool dead = false;
    uint32_t base = kAbsent;  // the lowest row present: where the one proving read starts
} g_memo;

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
T at(uint64_t p) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof v);
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

// The memo for this chunk: kept unless `force`, else found again by the archetype scan, and logged when that moved
// anything. kFailed when the chunk's archetype or its Health column is not found.
Resolved resolve(void *world, uint64_t chunk, bool force) {
    const auto *w = static_cast<const uint8_t *>(world);
    if (!force && world == g_memo.world && chunk == g_memo.chunk && ecs::chunk_is_at(w, g_memo.arch, chunk))
        return kKept;
    ecs::Archetype a;
    const int64_t index = ecs::archetype_of_chunk(w, chunk);
    if (index < 0 || !ecs::archetype(w, index, a)) return kFailed;
    const int h = ecs::type_index(a, game::kHealth);
    if (h < 0) return kFailed;
    const int t = ecs::type_index(a, game::kWorldTransform), m = ecs::type_index(a, game::kMovement);
    const int s = ecs::type_index(a, game::kMovementState);
    const int g = ecs::type_index(a, game::kInGap), st = ecs::type_index(a, game::kStatus);
    const int ld = ecs::type_index(a, game::kLogicData);
    const int rs = ecs::type_index(a, game::kResourcing), pp = ecs::type_index(a, game::kPlayerProgression);
    const int cu = ecs::type_index(a, game::kCoreUpgrade);
    if (ld >= 0) {  // the column's width: the next column's offset minus its own (rows times the row size)
        uint32_t next = UINT32_MAX;
        for (uint32_t i = 0; i < a.count; i++)
            if (a.offsets[i] > a.offsets[ld] && a.offsets[i] < next) next = a.offsets[i];
        log::write("player: LogicData column +%lx, width %ld", static_cast<unsigned long>(a.offsets[ld]),
                   static_cast<long>(next == UINT32_MAX ? -1 : static_cast<long>(next - a.offsets[ld])));
    }
    const Memo was = g_memo;
    g_memo = {world,
              chunk,
              index,
              a.offsets[h],
              t < 0 ? kAbsent : a.offsets[t],
              m < 0 ? kAbsent : a.offsets[m],
              s < 0 ? kAbsent : a.offsets[s],
              g < 0 ? kAbsent : a.offsets[g],
              st < 0 ? kAbsent : a.offsets[st],
              ld < 0 ? kAbsent : a.offsets[ld],
              rs < 0 ? kAbsent : a.offsets[rs],
              pp < 0 ? kAbsent : a.offsets[pp],
              cu < 0 ? kAbsent : a.offsets[cu],
              false,
              kAbsent};
    g_memo.base = g_memo.health;
    for (const uint32_t o : {g_memo.progression, g_memo.upgrades})
        if (o != kAbsent && o < g_memo.base) g_memo.base = o;
    ecs::TypeList list;
    g_memo.dead = ecs::type_list(w, index, list) && ecs::has_type(list, game::kDead);
    const bool moved = was.world != world || was.chunk != chunk || was.arch != index || was.health != g_memo.health ||
                       was.transform != g_memo.transform || was.movement != g_memo.movement ||
                       was.state != g_memo.state || was.gap != g_memo.gap || was.status != g_memo.status ||
                       was.logic != g_memo.logic || was.resourcing != g_memo.resourcing ||
                       was.progression != g_memo.progression || was.upgrades != g_memo.upgrades;
    if (moved && g_resolutions < kResolutionsLogged) {
        g_resolutions++;
        char health[16], transform[16], movement[16], state[16], gap[16], status[16], logic[16];
        char resourcing[16], progression[16], upgrades[16];
        offset_text(g_memo.health, health);
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
            static_cast<long>(index), reinterpret_cast<void *>(chunk), health, transform, movement, state, gap, status,
            logic, resourcing, progression, upgrades,
            g_resolutions == kResolutionsLogged ? " (the last such line)" : "");
        // The full type list once per archetype new to this process, tags included: a diff of two lists names the
        // component a cutscene or a death adds (AKTH-0110).
        ecs::TypeList tags;
        if (was.arch != index && !listed(index) && ecs::type_list(w, index, tags)) ecs::log_type_list(index, tags);
    }
    return kFresh;
}

// Row `offset`, `size` bytes, readable, given `run` bytes readable from the memo's base row: the rows above it, every
// row in each player archetype seen, cost no VirtualQuery of their own.
bool row_readable(uint64_t chunk, uint32_t offset, uint32_t size, size_t run) {
    if (offset == kAbsent) return false;
    if (offset >= g_memo.base) return offset - g_memo.base + size <= run;
    return mem::readable(reinterpret_cast<const void *>(chunk + offset), size);
}

// The bytes the memo's rows span from its base row, read once with ReadProcessMemory to prove them: a VirtualQuery
// here cost 0.5 ms on average under the game's spawning (2026-09-30, AKTH-0111), a queue on the address-space lock.
size_t span_readable(uint64_t chunk) {
    uint32_t end = g_memo.health + game::kHealthRowSize;
    const uint32_t rows[][2] = {{g_memo.transform, game::kWorldTransformRowSize},
                                {g_memo.movement, game::kMovementRowSize},
                                {g_memo.state, game::kMovementStateRowSize},
                                {g_memo.gap, game::kInGapRowSize},
                                {g_memo.status, game::kStatusRowSize},
                                {g_memo.logic, game::kLogicDataProbe},
                                {g_memo.resourcing, game::kResourcingRowSize},
                                {g_memo.progression, game::kPlayerProgressionRowSize},
                                {g_memo.upgrades, game::kCoreUpgradeRowSize}};
    for (const auto &r : rows)
        if (r[0] != kAbsent && r[0] >= g_memo.base && r[0] + r[1] > end) end = r[0] + r[1];
    const size_t span = end - g_memo.base;
    return mem::readable(reinterpret_cast<const void *>(chunk + g_memo.base), span) ? span : 0;
}

// The memo resolved for get_player's chunk with its Health row readable, and the bytes readable from the memo's base
// row; 0 when not. A kept memo whose row is unreadable is wrong: the scan decides.
size_t locate(void *world, uint64_t *chunk_out) {
    const uint64_t chunk = player_chunk(world);
    if (!chunk) return 0;
    Resolved r = resolve(world, chunk, false);
    size_t run = r != kFailed ? span_readable(chunk) : 0;
    if (r == kKept && !row_readable(chunk, g_memo.health, game::kHealthRowSize, run)) {
        r = resolve(world, chunk, true);
        run = r != kFailed ? span_readable(chunk) : 0;
    }
    if (r == kFailed || !row_readable(chunk, g_memo.health, game::kHealthRowSize, run)) return 0;
    *chunk_out = chunk;
    return run;
}

bool on_map(float v) { return std::isfinite(v) && std::fabs(v) < kMapBound; }

}  // namespace

void set_get_player(GetPlayerFn fn) { g_get_player = fn; }

void read(void *world, State &out) {
    out = {};
    uint64_t chunk = 0;
    const size_t run = locate(world, &chunk);
    if (!run) return;
    out.arch = g_memo.arch;
    out.dead = g_memo.dead;
    out.health_at = chunk + g_memo.health;
    out.has_health = true;
    out.fraction = at<float>(chunk + g_memo.health);
    out.max = at<int32_t>(chunk + g_memo.health + 4);
    if (row_readable(chunk, g_memo.resourcing, game::kResourcingRowSize, run)) {
        const uint64_t row = chunk + g_memo.resourcing;
        out.power = at<float>(row + game::kPower);
        out.power_max = at<float>(row + game::kPowerMax);
        out.command_max = at<float>(row + game::kCommandMax);
        out.command = at<float>(row + game::kCommandFraction) * out.command_max;
        out.has_power = std::isfinite(out.power) && std::isfinite(out.power_max) && std::isfinite(out.command) &&
                        std::isfinite(out.command_max);
    }
    if (row_readable(chunk, g_memo.progression, game::kPlayerProgressionRowSize, run)) {
        const uint64_t row = chunk + g_memo.progression;
        out.build_power = at<int32_t>(row + game::kBuildPower);
        out.aberrant = at<int32_t>(row + game::kAberrantStrength);
        out.ability = at<int32_t>(row + game::kAbilityStrength);
        out.has_progression = true;
    }
    if (row_readable(chunk, g_memo.upgrades, game::kCoreUpgradeRowSize, run)) {
        const uint64_t row = chunk + g_memo.upgrades;
        out.health_upgrades = at<int32_t>(row + game::kHealthUpgrades);
        out.power_upgrades = at<int32_t>(row + game::kPowerUpgrades);
        out.has_upgrades = true;
    }
    if (row_readable(chunk, g_memo.gap, game::kInGapRowSize, run)) {
        out.gap = at<uint64_t>(chunk + g_memo.gap);
        out.has_gap = true;
        out.in_gap = (out.gap & game::kInGapFlag) == 1;
    }
    if (row_readable(chunk, g_memo.status, game::kStatusRowSize, run)) {
        out.status = at<uint64_t>(chunk + g_memo.status);
        out.has_status = true;
    }
    if (row_readable(chunk, g_memo.logic, game::kLogicDataProbe, run)) {
        std::memcpy(out.logic, reinterpret_cast<const void *>(chunk + g_memo.logic), sizeof out.logic);
        out.has_logic = true;
        out.in_story = out.logic[game::kLogicStoryByte] == game::kLogicStoryBlocked;
    }

    if (row_readable(chunk, g_memo.transform, game::kWorldTransformRowSize, run)) {
        const uint64_t row = chunk + g_memo.transform;
        out.position_at = row + game::kWorldPosition;
        std::memcpy(out.position, reinterpret_cast<const void *>(out.position_at), sizeof out.position);
        std::memcpy(out.rotation, reinterpret_cast<const void *>(row), sizeof out.rotation);
        out.has_position = on_map(out.position[0]) && on_map(out.position[1]) && on_map(out.position[2]);
        // Read as zero once in Cheat Engine, at two decimals: logged the first time it is not, never a gate.
        const float pad = at<float>(row + game::kWorldPositionPad);
        if (pad != 0.0f && !g_pad_said) {
            char text[32];
            std::snprintf(text, sizeof text, "%.6g", pad);
            log::write("player: the transform's +0x1c reads %s, not 0 (the first time)", text);
            g_pad_said = true;
        }
    }
    if (row_readable(chunk, g_memo.movement, game::kMovementRowSize, run)) {
        out.speed_at = chunk + g_memo.movement + game::kMovementSpeed;
        out.speed = at<float>(out.speed_at);
        out.has_speed = std::isfinite(out.speed) && out.speed >= 0.0f && out.speed < kSpeedBound;
    }
    if (row_readable(chunk, g_memo.state, game::kMovementStateRowSize, run)) {
        out.velocity_at = chunk + g_memo.state;
        std::memcpy(out.velocity, reinterpret_cast<const void *>(out.velocity_at), sizeof out.velocity);
        const float x = out.velocity[0], y = out.velocity[1], z = out.velocity[2];
        out.h = std::sqrt(x * x + z * z);
        out.total = std::sqrt(x * x + y * y + z * z);
        out.has_velocity = std::isfinite(out.total) && out.total < kSpeedBound;
    }
}

const float *health_row(void *world, int64_t *arch_out, uint64_t *chunk_out) {
    uint64_t chunk = 0;
    if (!locate(world, &chunk)) return nullptr;
    *arch_out = g_memo.arch;
    *chunk_out = chunk;
    return reinterpret_cast<const float *>(chunk + g_memo.health);
}

void log_first_look(void *world) {
    if (!world || !g_get_player) return;
    uint64_t out[8] = {};
    g_get_player(out, world);
    log::write("get_player -> %p %p %p %p", reinterpret_cast<void *>(out[0]), reinterpret_cast<void *>(out[1]),
               reinterpret_cast<void *>(out[2]), reinterpret_cast<void *>(out[3]));
    if (out[3] & 1) ecs::log_archetype(static_cast<const uint8_t *>(world), out[1], out[0]);
}

}  // namespace crsrt::player
