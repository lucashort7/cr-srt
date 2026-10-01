// ecs.cpp
#include "ecs.h"

#include "game.h"
#include "log.h"
#include "memory.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace crsrt::ecs {

namespace {

const uint64_t *chunk_slots(const uint8_t *world) {
    return reinterpret_cast<const uint64_t *>(world + game::kChunkSlotBias * 8);
}

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
        read += n;
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

bool Columns::read(const World &world, int64_t index) {
    const Entry &e = world.entry(index);
    count_ = 0;
    if (!e.count || e.count > kMaxColumns || !world.source().copy(e.hashes, hashes_, e.count * 4ull) ||
        !world.source().copy(e.offsets, offsets_, e.count * 4ull))
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

bool type_list(const World &world, int64_t index, std::vector<uint32_t> &out) {
    const Entry &e = world.entry(index);
    out.clear();
    if (!e.tag_count || e.tag_count > kMaxColumns) return false;
    out.resize(e.tag_count);
    if (world.source().copy(e.tags, out.data(), e.tag_count * 4ull)) return true;
    out.clear();
    return false;
}

bool has_tag(const World &world, int64_t index, uint32_t hash) {
    std::vector<uint32_t> tags;
    return type_list(world, index, tags) && std::ranges::find(tags, hash) != tags.end();
}

bool copy_rows(const mem::Source &source, uint64_t chunk, uint32_t offset, uint32_t rows, uint32_t row_size,
               void *out) {
    return chunk && offset != kAbsent && source.copy(chunk + offset, out, static_cast<size_t>(rows) * row_size);
}

int64_t archetype_of_chunk(const uint8_t *world, uint64_t chunk) {
    const uint64_t *slots = chunk_slots(world);
    for (int64_t a = 0; a < game::kArchetypeMax;) {
        int64_t end = a + static_cast<int64_t>(mem::readable_run(&slots[a]) / 8);
        if (end == a) return -1;
        end = std::min(end, game::kArchetypeMax);
        for (; a < end; a++)
            if (slots[a] == chunk) return a;
    }
    return -1;
}

bool chunk_is_at(const uint8_t *world, int64_t index, uint64_t chunk) {
    if (index < 0) return false;
    const uint64_t *slot = &chunk_slots(world)[index];
    return mem::readable(slot, 8) && *slot == chunk;
}

bool archetype(const uint8_t *world, int64_t index, Archetype &out) {
    const uint8_t *entry = world + 8 + (index + game::kTypeEntryBias) * 32;
    if (!mem::readable(entry, 32)) return false;
    const auto *hashes = *reinterpret_cast<const uint32_t *const *>(entry + 8);
    uint32_t count = *reinterpret_cast<const uint32_t *>(entry + 0x1c);
    const auto *offsets = *reinterpret_cast<const uint32_t *const *>(world + 8 + index * 32 + game::kOffsetsBase);
    if (count > 4096 || !mem::readable(hashes, count * 4) || !mem::readable(offsets, count * 4)) return false;
    out = {index, hashes, offsets, count};
    return true;
}

int type_index(const Archetype &archetype, uint32_t hash) {
    for (uint32_t i = 0; i < archetype.count; i++)
        if (archetype.hashes[i] == hash) return static_cast<int>(i);
    return -1;
}

bool type_list(const uint8_t *world, int64_t index, TypeList &out) {
    const uint8_t *entry = world + 8 + (index + game::kTypeEntryBias) * 32;
    if (!mem::readable(entry, 32)) return false;
    const auto *hashes = *reinterpret_cast<const uint32_t *const *>(entry);
    const uint32_t count = *reinterpret_cast<const uint32_t *>(entry + 0x18);
    if (count > 4096 || !mem::readable(hashes, count * 4)) return false;
    out = {hashes, count};
    return true;
}

bool has_type(const TypeList &list, uint32_t hash) {
    for (uint32_t i = 0; i < list.count; i++)
        if (list.hashes[i] == hash) return true;
    return false;
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

bool locate_row(const uint8_t *world, uint64_t row, uint32_t hash, uint32_t stride, RowPlace &out) {
    // A chunk more than this far below the row cannot hold it: skips the archetype tables of every unrelated chunk.
    constexpr uint64_t kChunkSpan = 64ull << 20;
    const uint64_t *slots = chunk_slots(world);
    bool found = false;
    uint64_t best = 0;
    for (int64_t a = 0; a < game::kArchetypeMax;) {
        int64_t end = a + static_cast<int64_t>(mem::readable_run(&slots[a]) / 8);
        if (end == a) break;
        end = std::min(end, game::kArchetypeMax);
        for (; a < end; a++) {
            const uint64_t chunk = slots[a];
            if (!chunk || chunk > row || row - chunk > kChunkSpan) continue;
            Archetype arch;
            if (!archetype(world, a, arch)) continue;
            int i = type_index(arch, hash);
            if (i < 0) continue;
            const uint32_t column = arch.offsets[i];
            if (row < chunk + column) continue;
            const uint64_t d = row - (chunk + column);
            if (d % stride || (found && d >= best)) continue;
            uint32_t next = UINT32_MAX;  // the nearest column after this one bounds it
            for (uint32_t k = 0; k < arch.count; k++)
                if (arch.offsets[k] > column && arch.offsets[k] < next) next = arch.offsets[k];
            const uint32_t capacity = next == UINT32_MAX ? 0 : (next - column) / stride;
            if (capacity && d / stride >= capacity) continue;
            out = {a, chunk, column, capacity, static_cast<uint32_t>(d / stride)};
            best = d;
            found = true;
        }
    }
    return found;
}

const uint8_t *row_of(const Archetype &a, uint64_t chunk, uint32_t capacity, uint32_t index, uint32_t hash,
                      uint32_t *size) {
    int i = type_index(a, hash);
    if (i < 0 || !capacity) return nullptr;
    const uint32_t column = a.offsets[i];
    uint32_t next = UINT32_MAX;
    for (uint32_t k = 0; k < a.count; k++)
        if (a.offsets[k] > column && a.offsets[k] < next) next = a.offsets[k];
    *size = next == UINT32_MAX ? 0 : (next - column) / capacity;
    return reinterpret_cast<const uint8_t *>(chunk) + column + static_cast<uint64_t>(index) * *size;
}

void log_types(const Archetype &a) {
    char line[480];
    int n = 0;
    for (uint32_t i = 0; i < a.count; i++) {
        n += wsprintfA(line + n, "%08lx@%lx ", static_cast<unsigned long>(a.hashes[i]),
                       static_cast<unsigned long>(a.offsets[i]));
        if (n > 400 || i + 1 == a.count) {
            log::write("arch %ld types: %s", static_cast<long>(a.index), line);
            n = 0;
        }
    }
}

void log_archetype(const uint8_t *world, uint64_t chunk, uint64_t tag_row) {
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
    int64_t index = archetype_of_chunk(world, chunk);
    if (index < 0) {
        log::write("arch: chunk %p not found in the table", reinterpret_cast<void *>(chunk));
        return;
    }
    Archetype a;
    if (!archetype(world, index, a)) {
        log::write("arch %ld: tables unreadable", static_cast<long>(index));
        return;
    }
    log::write("arch %ld: chunk %p hashes %p count %u offs %p tag row %p = chunk+%lx", static_cast<long>(index),
               reinterpret_cast<void *>(chunk), a.hashes, a.count, a.offsets, reinterpret_cast<void *>(tag_row),
               static_cast<unsigned long>(tag_row - chunk));
    log_types(a);
    if (!g_find_type) {
        log::write("  the game's lookup is unresolved: no row comparison");
        return;
    }
    HMODULE exe = GetModuleHandleW(game::kExeName);
    auto find = reinterpret_cast<FindTypeFn>(reinterpret_cast<uint8_t *>(exe) + g_find_type);
    for (const auto &t : kNamed) {
        int game_i = find(a.hashes, a.count, t.hash), mine = type_index(a, t.hash);
        if (game_i < 0) {
            log::write("  %s: not in this archetype (game %d, mine %d)", t.name, game_i, mine);
            continue;
        }
        const uint8_t *row = reinterpret_cast<const uint8_t *>(chunk) + a.offsets[game_i];
        if (!mem::readable(row, 8)) {
            log::write("  %s: idx %d row %p unreadable", t.name, game_i, row);
            continue;
        }
        float f = *reinterpret_cast<const float *>(row);
        int whole = static_cast<int>(f), frac = static_cast<int>((f - static_cast<float>(whole)) * 1000);
        if (frac < 0) frac = -frac;
        log::write("  %s: idx %d (mine %d) row %p = chunk+%lx  +0 float %d.%03d  +4 int %ld", t.name, game_i, mine, row,
                   static_cast<unsigned long>(a.offsets[game_i]), whole, frac,
                   static_cast<long>(*reinterpret_cast<const int32_t *>(row + 4)));
    }
}

}  // namespace crsrt::ecs
