// spawn.h — a probe: read-only hooks on the two spawn functions of game.h (kSpawnSubmit, kSpawnBundle), logging each
// call's four register arguments, what they point at, the bundle's 0x90-byte records and the value returned. It
// changes nothing in the game. On by default while the path is being read: [probe] spawn=0 in <basename>.ini turns it
// off. Story AKTH-0119.
#pragma once

#include <windows.h>

namespace crsrt::spawn {

// MinHook must be initialised. False when neither function's first bytes are the ones game.h expects.
bool install(HMODULE exe);

}  // namespace crsrt::spawn
