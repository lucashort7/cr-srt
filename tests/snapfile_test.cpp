// snapfile_test.cpp — the snapshot answers copies at the addresses the game had, and holds what cr-probe wrote.
#include "snapfile.h"
#include "check.h"
#include "game.h"

#include <cstdint>
#include <vector>

using namespace crsrt;

namespace {

constexpr char kFixture[] = "fixtures/snap-20260928-125351.bin";

uint64_t type_entry(uint64_t world, int64_t index) { return world + 8 + (index + game::kTypeEntryBias) * 32; }
uint64_t offsets_entry(uint64_t world, int64_t index) { return world + 8 + index * 32 + game::kOffsetsBase; }

}  // namespace

// The counts a Python read of the same file gives: 1134 archetypes, 23 chunks with Health, each read whole. A chunk
// without Health can still be answered: a blob runs 0x400 bytes past its last column, into whatever follows it.
TEST(the_fixture_loads_whole) {
    snapfile::Snapshot s;
    CHECK(s.load(kFixture));
    CHECK(s.world() != 0);
    CHECK(s.calls() == 302);
    CHECK(s.archetypes().size() == 1134);
    int chunks = 0, unread = 0;
    std::vector<uint8_t> bytes;
    for (const auto &a : s.archetypes()) {
        if (!a.blob) continue;
        chunks++;
        bytes.resize(a.blob);
        if (!s.copy(a.chunk, bytes.data(), a.blob)) unread++;
    }
    CHECK(chunks == 23);
    CHECK(unread == 0);
}

TEST(a_missing_or_foreign_file_does_not_load) {
    snapfile::Snapshot s;
    CHECK(!s.load("fixtures/none.bin"));
    CHECK(!s.load("check.h"));
    uint8_t b = 0;
    CHECK(!s.copy(0x1000, &b, 1));
}

// Each archetype's chunk slot, read out of the world's bytes, names the chunk the file recorded beside it.
TEST(a_chunk_slot_reads_the_chunk_recorded) {
    snapfile::Snapshot s;
    CHECK(s.load(kFixture));
    int wrong = 0;
    for (const auto &a : s.archetypes()) {
        uint64_t chunk = 0;
        if (!mem::read(s, s.world() + (a.index + game::kChunkSlotBias) * 8, chunk) || chunk != a.chunk) wrong++;
    }
    CHECK(wrong == 0);
}

// The walk read an archetype's offsets through its type entry's +0x10, ecs.cpp through its offsets entry's +0: one
// pointer, measured on all three CRSNAP03 files on 2026-10-01 (3428 of 3428 archetypes).
TEST(both_routes_to_the_offsets_name_one_array) {
    snapfile::Snapshot s;
    CHECK(s.load(kFixture));
    int differ = 0;
    for (const auto &a : s.archetypes()) {
        uint64_t via_type = 0, via_offsets = 0;
        CHECK(mem::read(s, type_entry(s.world(), a.index) + 0x10, via_type));
        CHECK(mem::read(s, offsets_entry(s.world(), a.index), via_offsets));
        if (via_type != via_offsets) differ++;
    }
    CHECK(differ == 0);
}

// The hashes and offsets arrays read at the pointers their entries hold, whole, and not one u32 past.
TEST(an_archetypes_tables_read_at_their_pointers) {
    snapfile::Snapshot s;
    CHECK(s.load(kFixture));
    const auto &a = s.archetypes().front();
    uint64_t hashes = 0;
    uint32_t count = 0;
    CHECK(mem::read(s, type_entry(s.world(), a.index) + 8, hashes));
    CHECK(mem::read(s, type_entry(s.world(), a.index) + 0x1c, count));
    CHECK(count == a.count);
    std::vector<uint32_t> table(count + 1);
    CHECK(s.copy(hashes, table.data(), count * 4ull));
    CHECK(!s.copy(hashes, table.data(), (count + 1) * 4ull));
}
