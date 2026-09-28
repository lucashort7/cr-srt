// player.cpp
#include "player.h"

#include "ecs.h"
#include "game.h"
#include "log.h"
#include "memory.h"

namespace crsrt::player {

namespace {

GetPlayerFn g_get_player;

// The last resolution, reused while the world is the same, get_player hands back the same chunk and the table still
// holds that chunk at that archetype. The chunk alone is not enough: an entity can change archetype and keep its chunk.
struct Memo {
    void *world = nullptr;
    uint64_t chunk = 0;
    int64_t arch = -1;
    uint32_t offset = 0;
} g_memo;

}  // namespace

void set_get_player(GetPlayerFn fn) { g_get_player = fn; }

const float *health_row(void *world, int64_t *arch_out, uint64_t *chunk_out) {
    uint64_t out[8] = {};
    if (!g_get_player || !world) return nullptr;
    g_get_player(out, world);
    if (!(out[3] & 1) || !out[1]) return nullptr;
    const uint64_t chunk = out[1];
    const auto *w = static_cast<const uint8_t *>(world);

    if (world == g_memo.world && chunk == g_memo.chunk && ecs::chunk_is_at(w, g_memo.arch, chunk)) {
        const auto *row = reinterpret_cast<const float *>(chunk + g_memo.offset);
        if (mem::readable(row, 8)) {
            *arch_out = g_memo.arch;
            *chunk_out = chunk;
            return row;
        }
        // an unreadable row under a matching key means the memo is wrong: the scan decides
    }

    ecs::Archetype a;
    int64_t index = ecs::archetype_of_chunk(w, chunk);
    if (index < 0 || !ecs::archetype(w, index, a)) return nullptr;
    int i = ecs::type_index(a, game::kHealth);
    if (i < 0) return nullptr;
    const auto *row = reinterpret_cast<const float *>(chunk + a.offsets[i]);
    if (!mem::readable(row, 8)) return nullptr;
    g_memo = {world, chunk, index, a.offsets[i]};
    *arch_out = index;
    *chunk_out = chunk;
    return row;
}

void log_first_look(void *world) {
    if (!world || !g_get_player) return;
    uint64_t out[8] = {};
    g_get_player(out, world);
    log::write("get_player -> %p %p %p %p", reinterpret_cast<void *>(out[0]), reinterpret_cast<void *>(out[1]),
               reinterpret_cast<void *>(out[2]), reinterpret_cast<void *>(out[3]));
    if (out[3] & 1) ecs::log_archetype(static_cast<const uint8_t *>(world), out[1], out[0]);
}

}  // namespace crsrt::player
