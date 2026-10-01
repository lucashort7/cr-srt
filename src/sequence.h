// sequence.h — the sequence the player is in (AKTH-0114): a scripted level outside the open world, named by the Luau
// call that enters it and cleared by the one that leaves. Two entry families are measured (2026-09-30):
// return_point_save_then_switch_to_level(entity, "<path>\mq_02_taxi_puzzle_transparent", jump, fade) left through
// clear_return_point_then_switch_to_level, and zone_transition("zone_twisted", "boss_psychic_arena") left through
// zone_transition back to a "zone_*". save_then_switch_to_level inside one renames it (the taxi's end event). The
// name is the level's basename; the overlay draws it as `section:`. Nothing here reads the game.
#pragma once

#include <cstddef>

namespace crsrt::sequence {

// The level named by an entry call, any form: a path's basename is kept, from the last '\' or '/'. Any thread.
void enter(const char *level);

// The player left the sequence.
void leave();

// The current sequence's name into `out`; false when the player is in none.
bool current(char *out, size_t cap);

}  // namespace crsrt::sequence
