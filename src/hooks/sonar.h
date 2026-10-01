// sonar.h — the hook on heron::ui_sonar::update_exploration_sonar::system, the one system that reads the district's
// catalogue, its runtime state and the active district together (game.h, kSonarName). Every kEvery calls it resolves
// the district the player stands in, its level and its XP, and publishes them for the overlay (AKTH-0106). It changes
// nothing in the game. Like the runner, it is not called while the game is paused.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::sonar {

struct District {
    bool has = false;   // false: no active district, or a shape the reads did not recognise
    char id[32] = "";   // the catalogue's own id, "ZONE_MOLD"
    int level = 0;      // as the map shows it: levels completed plus one
    int max_level = 0;  // 15 or 10
    int xp = 0;         // into the current level
    int cost = 0;       // of the current level; 0 at the max
};

// MinHook must be initialised. False with the reason logged.
bool install(HMODULE exe, uintptr_t sonar_offset);

// The district as the hook last resolved it. Any thread.
void district(District &out);

}  // namespace crsrt::sonar
