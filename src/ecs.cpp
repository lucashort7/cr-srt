// ecs.cpp
#include "ecs.h"

#include "game.h"
#include "log.h"
#include "memory.h"

#include <windows.h>

namespace crsrt::ecs {

namespace {

const uint64_t *chunk_slots(const uint8_t *world) {
    return reinterpret_cast<const uint64_t *>(world + game::kChunkSlotBias * 8);
}

// The game's own lookup (exe+kFindTypeIndex), called only by the diagnostic dump to compare with type_index.
using FindTypeFn = int (*)(const void *hashes, uint32_t count, uint32_t hash);

}  // namespace

int64_t archetype_of_chunk(const uint8_t *world, uint64_t chunk) {
    const uint64_t *slots = chunk_slots(world);
    for (int64_t a = 0; a < game::kArchetypeMax;) {
        int64_t end = a + static_cast<int64_t>(mem::readable_run(&slots[a]) / 8);
        if (end == a) return -1;
        if (end > game::kArchetypeMax) end = game::kArchetypeMax;
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
    auto *hashes = *reinterpret_cast<const uint32_t *const *>(entry + 8);
    uint32_t count = *reinterpret_cast<const uint32_t *>(entry + 0x1c);
    auto *offsets = *reinterpret_cast<const uint32_t *const *>(world + 8 + index * 32 + game::kOffsetsBase);
    if (count > 4096 || !mem::readable(hashes, count * 4) || !mem::readable(offsets, count * 4)) return false;
    out = {index, hashes, offsets, count};
    return true;
}

int type_index(const Archetype &archetype, uint32_t hash) {
    for (uint32_t i = 0; i < archetype.count; i++)
        if (archetype.hashes[i] == hash) return static_cast<int>(i);
    return -1;
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
    char line[480];
    int n = 0;
    for (uint32_t i = 0; i < a.count; i++) {
        n += wsprintfA(line + n, "%08lx@%lx ", static_cast<unsigned long>(a.hashes[i]),
                       static_cast<unsigned long>(a.offsets[i]));
        if (n > 400 || i + 1 == a.count) {
            log::write("arch %ld types: %s", static_cast<long>(index), line);
            n = 0;
        }
    }
    HMODULE exe = GetModuleHandleW(game::kExeName);
    auto find = reinterpret_cast<FindTypeFn>(reinterpret_cast<uint8_t *>(exe) + game::kFindTypeIndex);
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
        log::write("  %s: idx %d (mine %d) row %p = chunk+%lx  +0 float %d.%03d  +4 int %ld", t.name, game_i, mine,
                   row, static_cast<unsigned long>(a.offsets[game_i]), whole, frac,
                   static_cast<long>(*reinterpret_cast<const int32_t *>(row + 4)));
    }
}

}  // namespace crsrt::ecs
