// player.h — the player's entity, through the game's own get_player, and its Health row.
#pragma once

#include <cstdint>

namespace crsrt::player {

// get_player(out, world) fills out[0] = the player tag's row, out[1] = the chunk, out[3] bit 0 = found.
using GetPlayerFn = uint64_t *(*)(uint64_t *out, void *world);

void set_get_player(GetPlayerFn fn);

// The player's Health row (+0 the fraction of max as a float, +4 max health in points as an int), or nullptr when
// anything on the way is unreadable. Cheap enough for every runner call: a repeat chunk skips the archetype scan.
// Runner thread only.
const float *health_row(void *world, int64_t *arch_out, uint64_t *chunk_out);

// get_player's answer and the player's whole archetype, to the log. Once, on the first runner call.
void log_first_look(void *world);

}  // namespace crsrt::player
