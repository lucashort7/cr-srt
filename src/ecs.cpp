// ecs.cpp
#include "ecs.h"

#include "game.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace crsrt::ecs {

namespace {

// The game's own lookup (solve::find_type), called only by the diagnostic dump to compare with type_index.
using FindTypeFn = int (*)(const void *hashes, uint32_t count, uint32_t hash);
uintptr_t g_find_type = 0;

}  // namespace

void set_find_type(uintptr_t exe_offset) { g_find_type = exe_offset; }

namespace {

constexpr int64_t kSlotBlock = 512;   // slots per copy: 4 KB
constexpr int64_t kEntryBlock = 128;  // entries per copy: 4 KB

uint64_t entry_address(uint64_t world, int64_t index) {
    return world + 8 + static_cast<uint64_t>(index + game::kTypeEntryBias) * 32;
}

const Entry g_no_entry;

// A value a chunk slot can hold: a 16-aligned address in user space. What follows the table holds small integers.
bool is_chunk(uint64_t v) { return v >= 0x10000 && v % 16 == 0 && v < 0x800000000000ull; }

}  // namespace

uint64_t slot_address(uint64_t world, int64_t index) {
    return world + static_cast<uint64_t>(index + game::kChunkSlotBias) * 8;
}

bool World::load(const mem::Source &source, uint64_t world) {
    source_ = &source;
    world_ = world;
    slots_.resize(static_cast<size_t>(game::kChunkSlots));
    int64_t read = 0;
    while (read < game::kChunkSlots) {
        const int64_t n = std::min(kSlotBlock, game::kChunkSlots - read);
        if (!world || !source.copy(slot_address(world, read), &slots_[static_cast<size_t>(read)], n * 8ull)) break;
        const auto block = std::span(slots_).subspan(static_cast<size_t>(read), static_cast<size_t>(n));
        const auto other = std::ranges::find_if(block, [](uint64_t v) { return v && !is_chunk(v); });
        read += other - block.begin();
        if (other != block.end()) break;
    }
    int64_t used = read;
    while (used > 0 && !slots_[static_cast<size_t>(used - 1)]) used--;
    slots_.resize(static_cast<size_t>(used));
    entries_.resize(static_cast<size_t>(used));
    int64_t got = 0;
    while (got < used) {
        const int64_t n = std::min(kEntryBlock, used - got);
        if (!source.copy(entry_address(world, got), &entries_[static_cast<size_t>(got)], n * sizeof(Entry))) break;
        got += n;
    }
    slots_.resize(static_cast<size_t>(got));
    entries_.resize(static_cast<size_t>(got));
    return got > 0;
}

uint64_t World::chunk(int64_t index) const {
    return index >= 0 && index < slots() ? slots_[static_cast<size_t>(index)] : 0;
}

const Entry &World::entry(int64_t index) const {
    return index >= 0 && index < slots() ? entries_[static_cast<size_t>(index)] : g_no_entry;
}

int64_t World::find(uint64_t chunk) const {
    if (!chunk) return -1;
    const auto at = std::ranges::find(slots_, chunk);
    return at == slots_.end() ? -1 : at - slots_.begin();
}

bool Columns::read(const World &world, int64_t index) { return read(world.source(), world.entry(index)); }

bool Columns::read(const mem::Source &source, const Entry &e) {
    count_ = 0;
    if (!e.count || e.count > kMaxColumns || !source.copy(e.hashes, hashes_, e.count * 4ull) ||
        !source.copy(e.offsets, offsets_, e.count * 4ull))
        return false;
    count_ = e.count;
    return true;
}

int Columns::index(uint32_t hash) const {
    for (uint32_t i = 0; i < count_; i++)
        if (hashes_[i] == hash) return static_cast<int>(i);
    return -1;
}

uint32_t Columns::offset(uint32_t hash) const {
    const int i = index(hash);
    return i < 0 ? kAbsent : offsets_[i];
}

uint32_t Columns::width(uint32_t hash) const {
    const uint32_t column = offset(hash);
    if (column == kAbsent) return 0;
    uint32_t next = UINT32_MAX;
    for (uint32_t i = 0; i < count_; i++)
        if (offsets_[i] > column && offsets_[i] < next) next = offsets_[i];
    return next == UINT32_MAX ? 0 : next - column;
}

uint32_t Columns::capacity(uint32_t hash, uint32_t row_size) const { return row_size ? width(hash) / row_size : 0; }

bool read_entry(const mem::Source &source, uint64_t world, int64_t index, Entry &out) {
    return index >= 0 && mem::read(source, entry_address(world, index), out);
}

bool type_list(const mem::Source &source, const Entry &entry, std::vector<uint32_t> &out) {
    out.clear();
    if (!entry.tag_count || entry.tag_count > kMaxColumns) return false;
    out.resize(entry.tag_count);
    if (source.copy(entry.tags, out.data(), entry.tag_count * 4ull)) return true;
    out.clear();
    return false;
}

bool has_tag(const World &world, int64_t index, uint32_t hash) {
    std::vector<uint32_t> tags;
    return type_list(world.source(), world.entry(index), tags) && std::ranges::find(tags, hash) != tags.end();
}

bool copy_rows(const mem::Source &source, uint64_t chunk, uint32_t offset, uint32_t rows, uint32_t row_size,
               void *out) {
    return chunk && offset != kAbsent && source.copy(chunk + offset, out, static_cast<size_t>(rows) * row_size);
}

void log_type_list(int64_t index, const uint32_t *hashes, uint32_t count) {
    char line[480];
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        n += wsprintfA(line + n, "%08lx ", static_cast<unsigned long>(hashes[i]));
        if (n > 440 || i + 1 == count) {
            log::write("arch %ld tags: %s", static_cast<long>(index), line);
            n = 0;
        }
    }
}

bool locate_row(const World &world, uint64_t row, uint32_t hash, uint32_t stride, RowPlace &out) {
    // A chunk more than this far below the row cannot hold it: skips the columns of every unrelated chunk.
    constexpr uint64_t kChunkSpan = 64ull << 20;
    static Columns columns;
    bool found = false;
    uint64_t best = 0;
    for (int64_t a = 0; a < world.slots(); a++) {
        const uint64_t chunk = world.chunk(a);
        if (!chunk || chunk > row || row - chunk > kChunkSpan || !columns.read(world, a)) continue;
        const uint32_t column = columns.offset(hash);
        if (column == kAbsent || row < chunk + column) continue;
        const uint64_t d = row - (chunk + column);
        if (d % stride || (found && d >= best)) continue;
        const uint32_t capacity = columns.capacity(hash, stride);
        if (capacity && d / stride >= capacity) continue;
        out = {a, chunk, column, capacity, static_cast<uint32_t>(d / stride)};
        best = d;
        found = true;
    }
    return found;
}

uint64_t row_of(const Columns &columns, uint64_t chunk, uint32_t capacity, uint32_t index, uint32_t hash,
                uint32_t *size) {
    const uint32_t column = columns.offset(hash);
    if (column == kAbsent || !capacity) return 0;
    *size = columns.width(hash) / capacity;
    return chunk + column + static_cast<uint64_t>(index) * *size;
}

void log_types(int64_t index, const Columns &columns) {
    char line[480];
    int n = 0;
    for (uint32_t i = 0; i < columns.count(); i++) {
        n += wsprintfA(line + n, "%08lx@%lx ", static_cast<unsigned long>(columns.hash(i)),
                       static_cast<unsigned long>(columns.offset_at(i)));
        if (n > 400 || i + 1 == columns.count()) {
            log::write("arch %ld types: %s", static_cast<long>(index), line);
            n = 0;
        }
    }
}

void log_archetype(const World &world, uint64_t chunk, uint64_t tag_row) {
    static const struct {
        const char *name;
        uint32_t hash;
    } kNamed[] = {
        {"Player tag (get_player's)", game::kPlayerTag},
        {"heron::health::component::Health", game::kHealth},
        {"heron::health::component::HealthLink", game::kHealthLink},
        {"heron::health::component::HealthFlags", game::kHealthFlags},
        {"heron::health::component::Dead", game::kDead},
        {"contentgame::HealthComponent (crheal h_hc)", game::kHealthComponent},
        {"heron::healing::component::Regeneration", game::kRegeneration},
        {"heron::player_status::component::CombatStatus", game::kCombatStatus},
    };
    const int64_t index = world.find(chunk);
    if (index < 0) {
        log::write("arch: chunk %p not found in the table", reinterpret_cast<void *>(chunk));
        return;
    }
    static Columns columns;
    if (!columns.read(world, index)) {
        log::write("arch %ld: tables unreadable", static_cast<long>(index));
        return;
    }
    const Entry &e = world.entry(index);
    log::write("arch %ld: chunk %p hashes %p count %u offs %p tag row %p = chunk+%lx", static_cast<long>(index),
               reinterpret_cast<void *>(chunk), reinterpret_cast<void *>(e.hashes), columns.count(),
               reinterpret_cast<void *>(e.offsets), reinterpret_cast<void *>(tag_row),
               static_cast<unsigned long>(tag_row - chunk));
    log_types(index, columns);
    if (!g_find_type) {
        log::write("  the game's lookup is unresolved: no row comparison");
        return;
    }
    HMODULE exe = GetModuleHandleW(game::kExeName);
    // The game's lookup searches the copy as it would its own array: it reads the hashes and nothing else.
    auto find = reinterpret_cast<FindTypeFn>(reinterpret_cast<uint8_t *>(exe) + g_find_type);
    for (const auto &t : kNamed) {
        const int game_i = find(columns.hashes(), columns.count(), t.hash), mine = columns.index(t.hash);
        if (game_i < 0) {
            log::write("  %s: not in this archetype (game %d, mine %d)", t.name, game_i, mine);
            continue;
        }
        const uint64_t row = chunk + columns.offset_at(static_cast<uint32_t>(game_i));
        uint8_t bytes[8];
        if (!world.source().copy(row, bytes, sizeof bytes)) {
            log::write("  %s: idx %d row %p unreadable", t.name, game_i, reinterpret_cast<void *>(row));
            continue;
        }
        float f = 0;
        int32_t i32 = 0;
        std::memcpy(&f, bytes, 4);
        std::memcpy(&i32, bytes + 4, 4);
        const int whole = static_cast<int>(f);
        int frac = static_cast<int>((f - static_cast<float>(whole)) * 1000);
        if (frac < 0) frac = -frac;
        log::write("  %s: idx %d (mine %d) row %p = chunk+%lx  +0 float %d.%03d  +4 int %ld", t.name, game_i, mine,
                   reinterpret_cast<void *>(row), static_cast<unsigned long>(columns.offset_at(game_i)), whole, frac,
                   static_cast<long>(i32));
    }
}

}  // namespace crsrt::ecs
