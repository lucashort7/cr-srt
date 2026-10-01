// player_test.cpp — the player's rows over the 12:47 snapshot: slot 941, Health 1143 of 1430 (tools/snap.py `list`).
#include "player.h"
#include "check.h"
#include "ecs.h"
#include "snapfile.h"

#include <cmath>
#include <cstdint>
#include <cstring>

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

// A source over the snapshot that counts its copies, and can move one slot to another chunk: the memo's two paths.
class Counting final : public mem::Source {
   public:
    explicit Counting(const snapfile::Snapshot &snap) : snap_(snap) {}
    bool copy(uint64_t at, void *out, size_t n) const override {
        copies++;
        if (!snap_.copy(at, out, n)) return false;
        if (moved_at >= at && moved_at + 8 <= at + n)  // the moved slot, inside any copy that covers it
            std::memcpy(static_cast<uint8_t *>(out) + (moved_at - at), &moved_to, 8);
        return true;
    }
    mutable int copies = 0;
    uint64_t moved_at = 0, moved_to = 0;

   private:
    const snapfile::Snapshot &snap_;
};

// Rows the reader copies on a kept memo: the slot check, then Health and nine more, one copy each.
constexpr int kKeptCopies = 12;

TEST(a_kept_memo_reads_no_slot_table) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    const uint64_t chunk = player_chunk(snap);
    static player::State s;
    player::read_chunk(snap, snap.world(), chunk, s);  // whatever the memo held, it is this chunk's now
    Counting counting(snap);
    player::read_chunk(counting, snap.world(), chunk, s);
    CHECK(s.has_health);
    CHECK(std::lround(s.fraction * static_cast<float>(s.max)) == 1143);
    CHECK(counting.copies == kKeptCopies);
}

// The slot the memo names now holds another chunk: the reader resolves again, a slot table's worth of copies, and finds
// the player's chunk in no slot. The slot back as it was, the memo, untouched by the failure, holds again.
TEST(a_moved_slot_resolves_again) {
    snapfile::Snapshot snap;
    CHECK(snap.load(kFixture));
    const uint64_t chunk = player_chunk(snap);
    static player::State s;
    player::read_chunk(snap, snap.world(), chunk, s);
    Counting counting(snap);
    counting.moved_at = ecs::slot_address(snap.world(), kPlayerSlot);
    counting.moved_to = chunk + 0x1000;
    player::read_chunk(counting, snap.world(), chunk, s);
    CHECK(counting.copies > kKeptCopies + 10);  // the table in 17 blocks of 512 slots, to the first non-chunk
    CHECK(!s.has_health);
    counting.moved_at = 0;
    counting.copies = 0;
    player::read_chunk(counting, snap.world(), chunk, s);
    CHECK(s.has_health);
    CHECK(s.arch == kPlayerSlot);
    CHECK(counting.copies == kKeptCopies);
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
