// sonar.h — the hook on heron::ui_sonar::update_exploration_sonar::system, the one system that reads the district's
// catalogue, its runtime state and the active district together (game.h, kSonarName). A probe for now: it logs the
// env tray, the catalogue's ids, and the active district and the runtime whenever they change, to find where the zone's
// level and XP live (AKTH-0106). It changes nothing in the game.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::sonar {

// MinHook must be initialised. False with the reason logged.
bool install(HMODULE exe, uintptr_t sonar_offset);

// Threads inside the hook right now; the unloader waits for 0 before unmapping.
long inflight();

}  // namespace crsrt::sonar
