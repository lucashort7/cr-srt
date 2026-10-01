// ecs.h — reading the game's ECS world: which archetype a chunk belongs to, and where a component's row sits in it.
// An entity lives in a chunk of its archetype; it changes archetype, and often chunk, whenever a component is added
// to it or removed, so nothing here is worth more than the call that read it. Layout constants: game.h.
#pragma once

#include "source.h"

#include <cstdint>
#include <vector>

namespace crsrt::ecs {

// --- Read by copy, through a Source: nothing below follows a pointer into the game, so a chunk freed mid-read fails a
// copy instead of faulting, and a test reads a snapshot file the same way. The pointer-following readers further down
// leave as their callers move here.

// An archetype's entry as the game lays it out, 32 bytes at world + 8 + (index + kTypeEntryBias) * 32.
struct Entry {
    uint64_t tags = 0;     // the full type list: the columns plus the tags that carry no bytes
    uint64_t hashes = 0;   // one type hash per column
    uint64_t offsets = 0;  // one u32 row offset from the chunk per column
    uint32_t tag_count = 0, count = 0;
};
static_assert(sizeof(Entry) == 32);

// The world's chunk-slot table and the entries of the slots in use, copied on load and read from the copy after.
class World {
   public:
    // Copies the slot table a block at a time up to the first block that fails, then the entries up to the highest slot
    // holding a chunk. False, and empty, when no slot reads.
    bool load(const mem::Source &source, uint64_t world);

    const mem::Source &source() const { return *source_; }
    uint64_t address() const { return world_; }
    // The slots read: the highest holding a chunk, plus one.
    int64_t slots() const { return static_cast<int64_t>(entries_.size()); }
    // The chunk in slot `index`, 0 when none or past slots().
    uint64_t chunk(int64_t index) const;
    // Slot `index`'s entry; zeros past slots().
    const Entry &entry(int64_t index) const;
    // The slot holding `chunk`, or -1.
    int64_t find(uint64_t chunk) const;

   private:
    const mem::Source *source_ = nullptr;
    uint64_t world_ = 0;
    std::vector<uint64_t> slots_;
    std::vector<Entry> entries_;
};

// The address of slot `index`, for a caller that checks one slot without loading the table.
uint64_t slot_address(uint64_t world, int64_t index);

inline constexpr uint32_t kMaxColumns = 4096;
inline constexpr uint32_t kAbsent = UINT32_MAX;

// One archetype's columns: the hashes and offsets its entry points at, copied. An archetype's types never change; its
// offsets do (a chunk reallocated from 10 rows to 2 moved every column), so they are worth only the read that took
// them.
class Columns {
   public:
    // Copies slot `index`'s hashes and offsets. False when either copy fails, or the count is 0 or past kMaxColumns.
    bool read(const World &world, int64_t index);

    uint32_t count() const { return count_; }
    uint32_t hash(uint32_t i) const { return hashes_[i]; }
    // Column i's offset from the chunk, i below count().
    uint32_t offset_at(uint32_t i) const { return offsets_[i]; }
    // The column of type `hash`, or -1.
    int index(uint32_t hash) const;
    // The column's offset from the chunk; kAbsent when the archetype lacks the type.
    uint32_t offset(uint32_t hash) const;
    // The column's bytes: up to the nearest column above it, its rows plus under 16 bytes of padding. 0 when absent or
    // the last column, which nothing above bounds.
    uint32_t width(uint32_t hash) const;
    // The rows a chunk of this archetype holds, read off the column of type `hash` and its row size: exact while the
    // row outweighs the padding. 0 when width() is.
    uint32_t capacity(uint32_t hash, uint32_t row_size) const;

   private:
    uint32_t count_ = 0;
    uint32_t hashes_[kMaxColumns] = {};
    uint32_t offsets_[kMaxColumns] = {};
};

// Slot `index`'s full type list, the columns plus the tags that carry no bytes, copied into out. False when not.
bool type_list(const World &world, int64_t index, std::vector<uint32_t> &out);

// True when slot `index`'s full type list holds `hash`: the test for a tag, which has no column.
bool has_tag(const World &world, int64_t index, uint32_t hash);

// Copies `rows` rows of `row_size` bytes from the column at `offset` of `chunk`.
bool copy_rows(const mem::Source &source, uint64_t chunk, uint32_t offset, uint32_t rows, uint32_t row_size, void *out);

// --- The pointer-following readers, checked by mem::readable first.

struct Archetype {
    int64_t index = -1;
    const uint32_t *hashes = nullptr;   // component type hash per type index
    const uint32_t *offsets = nullptr;  // row offset from the chunk per type index
    uint32_t count = 0;
};

// The archetype whose chunk-table slot holds `chunk`, or -1. Reads the table a memory region at a time.
int64_t archetype_of_chunk(const uint8_t *world, uint64_t chunk);

// True when chunk-table slot `index` still holds `chunk`: a cheap check that a remembered archetype is current.
bool chunk_is_at(const uint8_t *world, int64_t index, uint64_t chunk);

// The archetype's type and offset tables, checked readable. False when either is not.
bool archetype(const uint8_t *world, int64_t index, Archetype &out);

// The type's index in the archetype, or -1.
int type_index(const Archetype &archetype, uint32_t hash);

// The archetype's full type list, the columns plus the tags that carry no bytes, checked readable. False when not.
struct TypeList {
    const uint32_t *hashes = nullptr;
    uint32_t count = 0;
};
bool type_list(const uint8_t *world, int64_t index, TypeList &out);

// True when the type is in the list: the test for a tag, which has no column.
bool has_type(const TypeList &list, uint32_t hash);

// Logs an archetype's full type list, several hashes to a line, for tools/typenames.py.
void log_type_list(int64_t index, const uint32_t *hashes, uint32_t count);

// Where a component row sits: its archetype, that archetype's chunk, the column's offset from the chunk, how many rows
// the column holds (the next column's offset minus this one's, over the row size) and the row's index in it.
struct RowPlace {
    int64_t arch = -1;
    uint64_t chunk = 0;
    uint32_t column = 0;
    uint32_t capacity = 0;
    uint32_t index = 0;
};

// The archetype whose `hash` column holds `row`, rows `stride` bytes apart; the nearest column below the row wins.
// False when none does: the entity's chunk is not in the chunk table, or the walk is wrong.
bool locate_row(const uint8_t *world, uint64_t row, uint32_t hash, uint32_t stride, RowPlace &out);

// The row of type `hash` for the entity at `index` of a chunk whose columns hold `capacity` rows, and the column's
// width over the capacity in *size (a row's size, padding included). nullptr when the type is not in the archetype.
const uint8_t *row_of(const Archetype &archetype, uint64_t chunk, uint32_t capacity, uint32_t index, uint32_t hash,
                      uint32_t *size);

// Logs an archetype's every type hash and offset, several to a line.
void log_types(const Archetype &archetype);

// The exe offset of the game's own find-type-index function (solve::find_type), compared against type_index by
// log_archetype; 0 skips the comparison.
void set_find_type(uintptr_t exe_offset);

// Logs an archetype's every type hash and offset, and the rows of the types game.h names. For the first runner call.
void log_archetype(const uint8_t *world, uint64_t chunk, uint64_t tag_row);

}  // namespace crsrt::ecs
