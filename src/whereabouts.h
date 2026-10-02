// whereabouts.h — where the player is, as the overlay's zone section says it: the zone (the sonar's district, the Gap,
// a challenge), the section (the level the player is in) and the clock of seconds spent in the zone. The hooks hand in
// what they read, the overlay asks for one View a frame, and the [areas] table of the ini is read and written by
// flush(), off the game's threads and off Present. Nothing here reads the game. The vocabulary: CONTEXT.md.
//
// A level's name: a Luau call that switches level names its destination (zone_transition(from, to), the
// *_switch_to_level family by a path, its basename kept; AKTH-0114). The level itself is the id at GameState+0x90,
// which changes a moment after the call, so the name binds to the first id that differs from the one current at the
// call; an elevator changes the id with no call, and such an id keeps its ini label or its digits. A pair bound is
// written to [areas], the game's name over a hand label, so the table outlives the inject. [area_zones] is hand-typed
// only: the zone a level with no district belongs to, a Resonant's arena (02708bbb=ZONE_PATTERN).
//
// The clock: from zero at every change of zone, a return included, and still while the game is paused. Outside every
// district is a zone of its own. A district is taken once it has held kZoneSettle of running time, a stretch with no
// district once it has lasted that long, and a shorter stretch counts for the district around it: inside one district
// the game's active list empties for a few looks (2026-09-29: 16 gaps, the shortest three at most 3 s).
#pragma once

#include "hooks/sonar.h"

#include <cstdint>

namespace crsrt::whereabouts {

inline constexpr double kZoneSettle = 3.5;      // seconds of running time a new zone must hold
inline constexpr uint64_t kPendingMs = 30'000;  // a name whose level never changes within this is forgotten

// A level switch named `level` (a path or a name; the basename is kept) at `at_ms`, with `from` the level id current
// at the call. Any thread.
void level_named(const char *level, uint64_t from, uint64_t at_ms);

// What the overlay reads for one frame.
struct Inputs {
    sonar::District district;  // the sonar's last look
    uint64_t level = 0;        // GameState's level id (transit::area); 0 before the first read
    const char *point = "";    // the area the last fast travel landed in (transit::point), "" when none
    uint64_t point_level = 0;  // the level id current when it landed
    bool in_gap = false;       // the player's InGap flag, false when the runner's reading is stale
    bool in_story = false;     // the player's story-area flag, false when stale
    bool challenge = false;    // a challenge runs (uistate::challenge)
    uint32_t machine = 0;      // its machine's hash
    bool paused = false;       // a hub screen or the pause menu is open (uistate::paused)
    double run = 0.0;          // seconds of running game since the last frame: 0 while the runner is quiet
    uint64_t now_ms = 0;       // a monotonic clock, for the pending name's age
};

struct View {
    char zone[64];  // "ZONE_HISS [lv3] [story]", "THE_GAP", "CHALLENGE <machine>", an [area_zones] zone, "MINIQUEST",
                    // "--"
    char section[64];  // a fast travel's area, the level's name, its [areas] label, its id's first 8 hex digits, "--"
    long seconds;      // in the zone
    bool paused;
};

// One frame: the clock runs by in.run, a pending name binds once the level changes, and out says where the player
// is. The render thread.
void frame(const Inputs &in, View &out);

// Writes the names bound since the last flush to [areas] of the ini at `ini`, and reads the label and the
// [area_zones] zone of the level the last frame saw. Not the game's threads, not Present: the walk thread, once a
// second.
void flush(const wchar_t *ini);

// Everything forgotten: the clock, the names, the label. For the tests.
void reset();

}  // namespace crsrt::whereabouts
