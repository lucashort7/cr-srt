// snapfile.h — the file cr-probe writes on F9 (probe/snapshot.cpp) and the tests read back: the ECS as it stood inside
// one runner call. Little-endian:
//   "CRSNAP03" | u64 world | u32 runner calls | u32 slots scanned | u32 world length | the world's first bytes
//   per archetype whose tables read: u32 'ARCH' | i32 index | u64 chunk | u32 type count | u32 blob length
//     | 32 bytes of its type entry (world+8+(index+kTypeEntryBias)*32) | 32 bytes of its offsets entry
//     (world+8+index*32+kOffsetsBase) | u32 hashes[count] | u32 offsets[count] | u32 n | u32 tags[n]: the array the
//     type entry's +0 points at, n from its +0x18 | blob: the chunk's first bytes, its header included; written only
//     for an archetype holding Health
//   u32 'END!'
// Every block is kept at the address it was read from, so the file answers a copy as the game would have.
#pragma once

#include "source.h"

#include <cstdint>
#include <vector>

namespace crsrt::snapfile {

inline constexpr char kMagic[8] = {'C', 'R', 'S', 'N', 'A', 'P', '0', '3'};
inline constexpr uint32_t kArchTag = 0x48435241;  // "ARCH"
inline constexpr uint32_t kEndTag = 0x21444e45;   // "END!"

// A snapshot read whole into memory: a Source whose copies are answered from the file's blocks. A copy is answered when
// one block holds all of it. One that spans two blocks fails, stricter than the game: an archetype's tags, hashes and
// offsets are one allocation there (1145 of 1146 archetypes in the 12:47 file), three blocks here.
class Snapshot final : public mem::Source {
   public:
    // False when the file is missing, is not CRSNAP03 or ends early; the snapshot is then empty.
    bool load(const char *path);

    bool copy(uint64_t at, void *out, size_t n) const override;

    uint64_t world() const { return world_; }
    uint32_t calls() const { return calls_; }
    // The archetypes the file holds tables for, in file order: the slot index, its chunk, its column count and how many
    // of its chunk's bytes the file holds (0 for an archetype without Health).
    struct Archetype {
        int32_t index;
        uint64_t chunk;
        uint32_t count, blob;
    };
    const std::vector<Archetype> &archetypes() const { return archetypes_; }

   private:
    struct Block {
        uint64_t at;
        size_t offset, size;  // into bytes_
    };
    std::vector<uint8_t> bytes_;
    std::vector<Block> blocks_;
    std::vector<Archetype> archetypes_;
    uint64_t world_ = 0;
    uint32_t calls_ = 0;
};

}  // namespace crsrt::snapfile
