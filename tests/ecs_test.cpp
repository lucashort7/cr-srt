// ecs_test.cpp — the ECS read by copy over the 12:47 snapshot. The expected rows are tools/snap.py's `list` of the same
// file: the player at slot 941 with 1143 of 1430, and a pair of 632-point enemies in the two rows of slot 669.
#include "ecs.h"
#include "check.h"
#include "game.h"
#include "snapfile.h"

#include <cmath>
#include <cstdint>
#include <cstring>

using namespace crsrt;

namespace {

constexpr char kFixture[] = "fixtures/snap-20260928-124754.bin";
constexpr int64_t kPlayerSlot = 941, kPairSlot = 669;
constexpr uint32_t kPlayerFactionTag = 0xf714ec8e;  // heron::faction_tag::component::Player

struct Loaded {
    snapfile::Snapshot snap;
    ecs::World world;
    bool ok = false;
    Loaded() { ok = snap.load(kFixture) && world.load(snap, snap.world()); }
};

int points(const uint8_t *row) {
    float fraction = 0;
    int32_t max = 0;
    std::memcpy(&fraction, row, 4);
    std::memcpy(&max, row + 4, 4);
    return static_cast<int>(std::lround(fraction * static_cast<float>(max)));
}

}  // namespace

TEST(the_world_loads_every_slot_in_use) {
    const Loaded l;
    CHECK(l.ok);
    for (const auto &a : l.snap.archetypes()) CHECK(l.world.chunk(a.index) == a.chunk);
    CHECK(l.world.slots() > kPlayerSlot);
    CHECK(l.world.chunk(l.world.slots()) == 0);
    CHECK(l.world.entry(-1).count == 0);
}

TEST(a_chunk_is_found_in_its_slot) {
    const Loaded l;
    CHECK(l.world.find(l.world.chunk(kPlayerSlot)) == kPlayerSlot);
    CHECK(l.world.find(0) == -1);
    CHECK(l.world.find(1) == -1);
}

TEST(an_unreadable_world_loads_nothing) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    ecs::World world;
    CHECK(!world.load(snap, 0x1000));
    CHECK(world.slots() == 0);
    CHECK(world.chunk(0) == 0);
}

TEST(columns_place_the_players_health) {
    const Loaded l;
    static ecs::Columns c;
    CHECK(c.read(l.world, kPlayerSlot));
    CHECK(c.capacity(game::kHealth, game::kHealthRowSize) == 1);
    CHECK(c.offset(game::kResourcing) != ecs::kAbsent);
    uint8_t row[game::kHealthRowSize];
    CHECK(ecs::copy_rows(l.snap, l.world.chunk(kPlayerSlot), c.offset(game::kHealth), 1, sizeof row, row));
    CHECK(points(row) == 1143);
    int32_t max = 0;
    std::memcpy(&max, row + 4, 4);
    CHECK(max == 1430);
}

TEST(columns_read_both_rows_of_a_two_row_chunk) {
    const Loaded l;
    static ecs::Columns c;
    CHECK(c.read(l.world, kPairSlot));
    CHECK(c.capacity(game::kHealth, game::kHealthRowSize) == 2);
    CHECK(c.offset(game::kSpawnedNpc) != ecs::kAbsent);
    CHECK(c.offset(game::kPlayerTag) == ecs::kAbsent);
    CHECK(c.width(game::kPlayerTag) == 0);
    uint8_t rows[2 * game::kHealthRowSize];
    CHECK(ecs::copy_rows(l.snap, l.world.chunk(kPairSlot), c.offset(game::kHealth), 2, game::kHealthRowSize, rows));
    CHECK(points(rows) == 632);
    CHECK(points(rows + game::kHealthRowSize) == 632);
    uint64_t handles[2] = {};
    CHECK(l.snap.copy(l.world.chunk(kPairSlot) + game::kEntityColumn, handles, sizeof handles));
    CHECK(static_cast<uint32_t>(handles[0]) == 0x1ece6);
    CHECK(static_cast<uint32_t>(handles[1]) == 0x1ec32);
}

TEST(a_tag_is_in_the_type_list_and_not_in_the_columns) {
    const Loaded l;
    static ecs::Columns c;
    CHECK(c.read(l.world, kPlayerSlot));
    CHECK(c.index(kPlayerFactionTag) < 0);
    CHECK(ecs::has_tag(l.world, kPlayerSlot, kPlayerFactionTag));
    CHECK(!ecs::has_tag(l.world, kPairSlot, kPlayerFactionTag));
}

TEST(a_missing_archetype_has_no_columns) {
    const Loaded l;
    static ecs::Columns c;
    CHECK(!c.read(l.world, l.world.slots()));
    CHECK(c.count() == 0);
    CHECK(c.offset(game::kHealth) == ecs::kAbsent);
}
