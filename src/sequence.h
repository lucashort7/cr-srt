// sequence.h — the name of the level the player is in (AKTH-0114). A Luau call that switches level names its
// destination: zone_transition(from, to) and the *_switch_to_level family (a path, its basename kept), measured
// 2026-09-30/10-01 on the taxi, the Gap's arena and the Oldest House. The level itself is the id at GameState+0x90
// (transit::area), which changes a moment after the call; the name is bound to the first id that differs from the
// one current at the call, and from then on that id is drawn by that name. An elevator ride changes the id with no
// call, and such an id keeps its ini label or its digits. Nothing here reads the game.
#pragma once

#include <cstddef>
#include <cstdint>

namespace crsrt::sequence {

// A level switch call named `level`, with `from` the id current at the call. Any thread.
void enter(const char *level, uint64_t from);

// The name bound to `id`, false when none; binds the pending name when `id` differs from its `from`. The render
// thread, every draw.
bool name_for(uint64_t id, char *out, size_t cap);

}  // namespace crsrt::sequence
