// enemies_test.cpp — the enemy walk over the 12:47 snapshot against tools/snap.py's `list` of the same file: ten live
// NPCs of faction 4aac31a8, the nearest 5.3 m from the player, then 7.4 and 8.7.
#include "enemies.h"
#include "check.h"
#include "ecs.h"
#include "game.h"
#include "snapfile.h"

#include <cmath>
#include <cstdint>

using namespace crsrt;

namespace {

constexpr char kFixture[] = "fixtures/snap-20260928-124754.bin";
constexpr int64_t kPlayerSlot = 941;

bool near(float a, float b) { return std::fabs(a - b) < 0.05f; }

// The player as the runner would hand it to the walk: its slot and its position, off the transform row.
player::State player_of(const snapfile::Snapshot &snap) {
    player::State s;
    ecs::World world;
    static ecs::Columns c;
    if (!world.load(snap, snap.world()) || !c.read(world, kPlayerSlot)) return s;
    s.arch = kPlayerSlot;
    s.has_position = snap.copy(world.chunk(kPlayerSlot) + c.offset(game::kWorldTransform) + game::kWorldPosition,
                               s.position, sizeof s.position);
    return s;
}

}  // namespace

// The list as the walk thread would publish it, the player where the runner would have read it.
static const enemies::List &walked() {
    static snapfile::Snapshot snap;
    static enemies::List list;
    static bool done = false;
    if (!done && snap.load(kFixture)) enemies::scan(snap, snap.world(), player_of(snap), list);
    done = true;
    return list;
}

TEST(the_walk_lists_the_ten_npcs) {
    const enemies::List &list = walked();
    CHECK(list.total == 10);
    CHECK(list.count == 10);
    CHECK(list.allies == 0);
    for (int i = 0; i < list.count; i++) {
        CHECK(list.rows[i].faction == 0x4aac31a8);
        CHECK(list.rows[i].level == 5);
        CHECK(list.rows[i].fraction == 1.0f);
    }
}

TEST(the_walk_lists_the_nearest_first) {
    const enemies::List &list = walked();
    CHECK(list.count >= 3);
    if (list.count < 3) return;
    CHECK(list.rows[0].id == 0x1ece6);
    CHECK(list.rows[0].max == 632);
    CHECK(near(list.rows[0].distance, 5.3f));
    CHECK(list.rows[1].id == 0x1ec8c);
    CHECK(near(list.rows[1].distance, 7.4f));
    CHECK(list.rows[2].id == 0x1ec35);
    CHECK(list.rows[2].max == 3159);
    CHECK(near(list.rows[2].distance, 8.7f));
    for (int i = 1; i < list.count; i++) CHECK(list.rows[i - 1].distance <= list.rows[i].distance);
}

// Without the player's position the rows are listed unsorted, each at distance -1.
TEST(the_walk_without_a_player_marks_every_distance_unknown) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    static enemies::List list;
    enemies::scan(snap, snap.world(), player::State{}, list);
    CHECK(list.total == 10);
    for (int i = 0; i < list.count; i++) CHECK(list.rows[i].distance < 0.0f);
}

TEST(the_walk_over_no_world_lists_nothing) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    static enemies::List list;
    enemies::scan(snap, 0x1000, player::State{}, list);
    CHECK(list.total == 0);
    CHECK(list.count == 0);
}
