// spawn.h — a probe: read-only hooks on the two spawn functions of game.h (kSpawnSubmit, kSpawnBundle), logging each
// call's four register arguments, what they point at, the bundle's 0x90-byte records and the value returned. It
// changes nothing in the game. With the game's column lookup (solve::find_type) it also records, per bundle call with
// records, which callers look up which columns inside it: the systems that touch the fresh entity. On by default while
// the path is being read: [probe] spawn=0 in <basename>.ini turns it off. Story AKTH-0119.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::spawn {

// MinHook must be initialised. find_type is the lookup's exe offset, 0 when unsolved (the lookup is then not hooked).
// False when neither spawn function's first bytes are the ones game.h expects.
bool install(HMODULE exe, uintptr_t find_type);

}  // namespace crsrt::spawn
