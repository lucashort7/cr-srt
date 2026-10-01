// player_test.cpp — the player's rows over the 12:47 snapshot: slot 941, Health 1143 of 1430 (tools/snap.py `list`).
#include "player.h"
#include "check.h"
#include "ecs.h"
#include "snapfile.h"

#include <cmath>
#include <cstdint>

using namespace crsrt;

namespace {

constexpr char kFixture[] = "fixtures/snap-20260928-124754.bin";
constexpr int64_t kPlayerSlot = 941;

uint64_t player_chunk(const snapfile::Snapshot &snap) {
    ecs::World world;
    return world.load(snap, snap.world()) ? world.chunk(kPlayerSlot) : 0;
}

}  // namespace

TEST(the_player_reads_its_health_and_position) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    static player::State s;
    player::read_chunk(snap, snap.world(), player_chunk(snap), s);
    CHECK(s.has_health);
    CHECK(s.arch == kPlayerSlot);
    CHECK(s.max == 1430);
    CHECK(std::lround(s.fraction * static_cast<float>(s.max)) == 1143);
    CHECK(!s.dead);
    CHECK(s.has_position);
    CHECK(s.has_speed);
    CHECK(s.has_velocity);
    CHECK(s.has_power);
    CHECK(s.has_progression);
    CHECK(s.has_logic);
    CHECK(s.health_at == player_chunk(snap) + 0x1710);
}

// A second read of the same chunk keeps the memo and reads the same values.
TEST(the_player_reads_the_same_twice) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    static player::State a, b;
    player::read_chunk(snap, snap.world(), player_chunk(snap), a);
    player::read_chunk(snap, snap.world(), player_chunk(snap), b);
    CHECK(a.has_health);
    CHECK(b.has_health);
    CHECK(a.fraction == b.fraction);
    CHECK(a.position[0] == b.position[0]);
    CHECK(a.position[2] == b.position[2]);
}

TEST(a_chunk_in_no_slot_reads_nothing) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    static player::State s;
    player::read_chunk(snap, snap.world(), 0x1000, s);
    CHECK(!s.has_health);
    CHECK(!s.has_position);
    player::read_chunk(snap, 0, player_chunk(snap), s);
    CHECK(!s.has_health);
}
